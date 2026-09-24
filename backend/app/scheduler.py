"""Fixed-priority response-time analysis and global priority search.

Given a priority order (highest priority first), message *i* experiences
blocking ``B_i`` from the longest lower-priority message and interference
from all higher-priority messages:

    r0 = J_i + C_i + B_i
    r(k+1) = J_i + C_i + B_i + sum_h ceil((r(k) + J_h) / T_h) * C_h

Iteration stops at the fixed point (R_i = r, schedulable) or at the first
value strictly greater than D_i (deadline miss).

The best order minimises, lexicographically over all permutations:
  1. number of deadline misses
  2. sum of min(R_i, D_i + 1)
  3. the id sequence itself (lexicographic)

The exact optimum is found with subset DP over the 2^n higher-priority
subsets (n <= 18).  A compiled C accelerator does the RTA inner loop and
the full DP; an independent pure-python implementation is kept for
cross-validation in the test suite.
"""
from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass, field
from typing import Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
# Maximum rows materialised per message in the API trace.  Longer walks
# keep the first cap-1 rows and a terminal row whose "skipped" annotation
# gives the exact number of compressed elementary iterations.
_TRACE_CAP = 2_000

_lib: Optional[ctypes.CDLL] = None
_load_attempted = False


def _load_lib() -> Optional[ctypes.CDLL]:
    global _lib, _load_attempted
    if _load_attempted:
        return _lib
    _load_attempted = True
    for name in ("librtacore.so", "librtacore.dylib"):
        path = os.path.join(_HERE, "native", name)
        if os.path.exists(path):
            try:
                lib = ctypes.CDLL(path)
            except OSError:
                continue
            _i64 = ctypes.c_int64
            _p_i64 = ctypes.POINTER(_i64)
            _p_i32 = ctypes.POINTER(ctypes.c_int)
            lib.rta_trace.argtypes = [
                _i64, _i64, _i64, _i64,
                _p_i64, _p_i64, _p_i64, _p_i32, ctypes.c_int,
                _p_i64, _p_i64, _p_i32, ctypes.c_int,
            ]
            lib.rta_trace.restype = ctypes.c_int
            lib.solve_dp.argtypes = [
                ctypes.c_int, _p_i64, _p_i64, _p_i64, _p_i64,
                ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int),
            ]
            lib.solve_dp.restype = ctypes.c_int
            _lib = lib
            return _lib
    return None


@dataclass
class RtaResult:
    trace: list[int]
    response: Optional[int]        # None when only the miss conclusion is exact
    schedulable: bool
    skipped: list[int] = field(default_factory=list)
    response_exact: bool = True    # False: orbit elided past the row budget


def rta_python(Ji, Ci, Di, B, J, C, T, higher) -> RtaResult:
    """Independent pure-python reference implementation."""
    base = Ji + Ci + B
    trace = [base]
    skipped = [0]
    if base > Di:
        return RtaResult(trace, base, False, skipped)
    r = base
    guard = 0
    while True:
        guard += 1
        nxt = base + sum(-((-(r + J[h]) // T[h])) * C[h] for h in higher)
        trace.append(nxt)
        skipped.append(0)
        if nxt > Di:
            return RtaResult(trace, nxt, False, skipped)
        if nxt == r:
            return RtaResult(trace, nxt, True, skipped)
        r = nxt
        if guard > 5_000_000:  # pragma: no cover - defensive
            raise RuntimeError("python RTA did not converge")

def rta(Ji, Ci, Di, B, J, C, T, higher) -> RtaResult:
    lib = _load_lib()
    if lib is None:
        return rta_python(Ji, Ci, Di, B, J, C, T, higher)
    n = len(J)
    nh = len(higher)
    jarr = (ctypes.c_int64 * n)(*J)
    carr = (ctypes.c_int64 * n)(*C)
    tarr = (ctypes.c_int64 * n)(*T)
    harr = (ctypes.c_int * max(nh, 1))(*higher) if higher else (ctypes.c_int * 1)()
    trace = (ctypes.c_int64 * _TRACE_CAP)()
    skipped = (ctypes.c_int64 * _TRACE_CAP)()
    out_n = ctypes.c_int(0)
    rc = lib.rta_trace(
        Ji, Ci, Di, B, jarr, carr, tarr, harr, nh,
        trace, skipped, ctypes.byref(out_n), _TRACE_CAP,
    )
    if rc == -1:
        raise RuntimeError("RTA iteration guard tripped")
    m = out_n.value
    sk = [skipped[i] for i in range(m)]
    # skipped == -1 marks the analytically folded tail: the decision is
    # exact but the displayed terminal value is capped at D + 1 on a miss.
    exact = -1 not in sk
    return RtaResult(
        [trace[i] for i in range(m)],
        trace[m - 1],
        rc == 1,
        sk,
        response_exact=exact or (rc == 1),
    )


# ---------------------------------------------------------------------------
# Per-order analysis
# ---------------------------------------------------------------------------

@dataclass
class MessageReport:
    idx: int                         # position in the submitted array
    position: int                    # 1-based priority position (1 = highest)
    blocking: int
    higher: list[int]                # ids of higher-priority messages
    trace: list[int]
    skipped: list[int]
    response: Optional[int]
    deadline: int
    schedulable: bool
    response_exact: bool = True

    @property
    def score_term(self) -> int:
        # The objective always caps a miss at D + 1; a schedulable message
        # contributes its exact R (an elided tail only occurs on misses).
        if not self.schedulable:
            return self.deadline + 1
        assert self.response is not None
        return min(self.response, self.deadline + 1)


def analyse_order(messages: list[dict], order: list[int]) -> list[MessageReport]:
    """messages: list of {id,C,T,D,J}; order: indices, highest priority first."""
    n = len(messages)
    J = [m["J"] for m in messages]
    C = [m["C"] for m in messages]
    T = [m["T"] for m in messages]
    D = [m["D"] for m in messages]
    reports: list[Optional[MessageReport]] = [None] * n
    for pos, i in enumerate(order):
        higher = list(order[:pos])
        B = max((C[k] for k in order[pos + 1:]), default=0)
        res = rta(J[i], C[i], D[i], B, J, C, T, higher)
        reports[i] = MessageReport(
            idx=i,
            position=pos + 1,
            blocking=B,
            higher=[messages[h]["id"] for h in higher],
            trace=res.trace,
            skipped=res.skipped,
            response=res.response,
            deadline=D[i],
            schedulable=res.schedulable,
            response_exact=res.response_exact,
        )
    return reports  # type: ignore[return-value]


# ---------------------------------------------------------------------------
# Exact global optimum
# ---------------------------------------------------------------------------

def find_best_order_c(messages: list[dict]) -> Optional[list[int]]:
    lib = _load_lib()
    if lib is None:
        return None
    n = len(messages)
    jarr = (ctypes.c_int64 * n)(*[m["J"] for m in messages])
    carr = (ctypes.c_int64 * n)(*[m["C"] for m in messages])
    tarr = (ctypes.c_int64 * n)(*[m["T"] for m in messages])
    darr = (ctypes.c_int64 * n)(*[m["D"] for m in messages])
    idarr = (ctypes.c_int32 * n)(*[m["id"] for m in messages])
    out = (ctypes.c_int * n)()
    rc = lib.solve_dp(n, jarr, carr, tarr, darr, idarr, out)
    if rc != 0:
        raise RuntimeError(f"native solver failed (code {rc})")
    return [out[i] for i in range(n)]


def find_best_order_python(messages: list[dict]) -> list[int]:
    """Independent subset-DP reference (used in tests)."""
    n = len(messages)
    J = [m["J"] for m in messages]
    C = [m["C"] for m in messages]
    T = [m["T"] for m in messages]
    D = [m["D"] for m in messages]
    ids = [m["id"] for m in messages]
    size = 1 << n
    full = size - 1

    bmax = [0] * size
    for mask in range(1, size):
        bit = mask & -mask
        i = bit.bit_length() - 1
        bmax[mask] = max(C[i], bmax[mask ^ bit])

    miss = [0] * size
    score = [0] * size
    parent = [-1] * size

    def seq(mask: int) -> list[int]:
        out = []
        while mask:
            i = parent[mask]
            out.append(ids[i])
            mask ^= 1 << i
        out.reverse()
        return out

    for mask in range(1, size):
        best = None
        k = mask.bit_count()
        m = mask
        while m:
            bit = m & -m
            i = bit.bit_length() - 1
            m ^= bit
            prev = mask ^ bit
            B = bmax[full ^ mask]
            higher = [h for h in range(n) if (prev >> h) & 1]
            res = rta_python(J[i], C[i], D[i], B, J, C, T, higher)
            c_miss = miss[prev] + (0 if res.schedulable else 1)
            c_score = score[prev] + min(res.response, D[i] + 1)
            cand_ids = seq(prev) + [ids[i]]
            key = (c_miss, c_score, cand_ids)
            if best is None or key < best[0]:
                best = (key, i, c_miss, c_score)
        assert best is not None
        _, parent[mask], miss[mask], score[mask] = best

    order = []
    mask = full
    while mask:
        i = parent[mask]
        order.append(i)
        mask ^= 1 << i
    order.reverse()
    return order


def find_best_order(messages: list[dict]) -> list[int]:
    order = find_best_order_c(messages)
    if order is not None:
        return order
    return find_best_order_python(messages)  # pragma: no cover


def solve(messages: list[dict]) -> tuple[list[int], list[MessageReport]]:
    order = find_best_order(messages)
    return order, analyse_order(messages, order)
