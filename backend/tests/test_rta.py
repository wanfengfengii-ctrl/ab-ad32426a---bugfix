"""Tests for the response-time analysis recurrence (C core vs reference)."""
from __future__ import annotations

import itertools
import random

import pytest

from app import scheduler


def _run_both(J, C, T, D, i, B, higher):
    a = scheduler.rta(J[i], C[i], D[i], B, J, C, T, higher)
    b = scheduler.rta_python(J[i], C[i], D[i], B, J, C, T, higher)
    return a, b


def test_hand_computed_trace_no_interference():
    # Lone message: r0 = J + C + B = 3; no higher -> r1 = r0 fixed point.
    res = scheduler.rta(Ji=2, Ci=1, Di=5, B=0,
                        J=[2], C=[1], T=[7], higher=[])
    assert res.trace == [3, 3]
    assert res.schedulable and res.response == 3


def test_hand_computed_trace_with_interference():
    # i: C=1,D=6; higher h: C=2,T=4.
    # r0=1; r1=1+ceil(1/4)*2=3; r2=1+ceil(3/4)*2=3 -> fixed point.
    res = scheduler.rta(0, 1, 6, 0, [0, 0], [1, 2], [6, 4], [1])
    assert res.trace == [1, 3, 3]
    assert res.schedulable and res.response == 3


def test_miss_stops_on_first_value_above_deadline():
    # i: C=3, D=3; h: C=2, T=4 -> r0=3, r1=3+2=5 > 3: miss, trace ends.
    res = scheduler.rta(0, 3, 3, 0, [0, 0], [3, 2], [5, 4], [1])
    assert res.trace == [3, 5]
    assert not res.schedulable and res.response == 5


def test_blocking_enters_r0():
    res = scheduler.rta(0, 2, 10, 4, [0, 0], [2, 1], [10, 3], [1])
    assert res.trace[0] == 6  # J + C + B


def test_jitter_in_ceil_terms():
    # h has J=1: interference ceil((r+1)/3)
    res = scheduler.rta(0, 1, 9, 0, [0, 1], [1, 1], [9, 3], [1])
    assert res.trace == [1, 2, 2]  # 1+ceil(2/3)=2, 1+ceil(3/3)=2


def test_r0_above_d_immediate_miss():
    res = scheduler.rta(2, 4, 5, 1, [2], [4], [9], [])
    assert res.trace == [7] and not res.schedulable


def test_c_core_matches_python_reference_random():
    random.seed(1234)
    for _ in range(500):
        n = random.randint(1, 8)
        J, C, T, D = [], [], [], []
        for _ in range(n):
            t = random.choice([1, 2, 3, 4, 5, 7, 8, 10, 13, 20, 100, 1000])
            c = random.randint(1, t)
            d = random.randint(c, t)
            j = random.randint(0, d - c)
            J.append(j); C.append(c); T.append(t); D.append(d)
        i = random.randrange(n)
        others = [h for h in range(n) if h != i]
        random.shuffle(others)
        B = random.randint(0, 7)
        a, b = _run_both(J, C, T, D, i, B, others)
        assert a.trace == b.trace, (J, C, T, D, i, B, others)
        assert a.response == b.response
        assert a.schedulable == b.schedulable


def test_constant_run_collapse_is_exact():
    """A regular higher message (T divides the constant increment) lets the
    solver collapse a run; the emitted landing row matches the orbit."""
    # i: base 1, D 20; h: C=5, T=5. Elementary orbit: 1,6,11,16,21(miss).
    res = scheduler.rta(0, 1, 20, 0, [0, 0], [1, 5], [20, 5], [1])
    ref = scheduler.rta_python(0, 1, 20, 0, [0, 0], [1, 5], [20, 5], [1])
    assert not res.schedulable and res.response == 21
    assert res.response == ref.response
    # collapsed trace: r0, r1, then the landing row compressing 2 steps
    assert res.trace == [1, 6, 21]
    assert res.skipped == [0, 0, 2]


@pytest.mark.parametrize("period,c_high,n_high,deadline", [
    (5, 5, 1, 20),      # single saturating regular message
    (10, 5, 2, 100),    # two half-utilisation messages, U = 1 combined
    (4, 2, 2, 40),      # each contributes half the increment
    (8, 8, 1, 200),     # long miss run
])
def test_collapsed_rows_match_reference_families(period, c_high, n_high, deadline):
    """Families where every iteration increment is a multiple of each
    higher period collapse exactly; conclusions match the orbit."""
    J = [0] * (n_high + 1)
    C = [1] + [c_high] * n_high
    T = [deadline] + [period] * n_high
    D = [deadline] + [period] * n_high
    higher = list(range(1, n_high + 1))
    res = scheduler.rta(0, 1, deadline, 0, J, C, T, higher)
    ref = scheduler.rta_python(0, 1, deadline, 0, J, C, T, higher)
    assert res.response == ref.response
    assert res.schedulable == ref.schedulable
    assert any(s > 0 for s in res.skipped), "collapse expected for this family"


def test_collapsed_rows_match_reference_on_random_inputs():
    """Random sweep: response/conclusion always match the reference orbit;
    the collapse path itself is exercised deterministically by the family
    test above."""
    random.seed(99)
    for _ in range(600):
        n = random.randint(1, 6)
        J, C, T, D = [], [], [], []
        for _ in range(n):
            t = random.choice([2, 3, 4, 5, 7, 8, 10, 20, 100, 1000, 10000])
            c = random.randint(1, max(1, int(t * 0.9)))
            d = random.randint(c, t)
            j = random.randint(0, d - c)
            J.append(j); C.append(c); T.append(t); D.append(d)
        res = scheduler.rta(J[0], C[0], D[0], 0, J, C, T, list(range(1, n)))
        ref = scheduler.rta_python(J[0], C[0], D[0], 0, J, C, T,
                                    list(range(1, n)))
        assert res.response == ref.response
        assert res.schedulable == ref.schedulable


def test_near_saturation_sylvester_set_decides_fast_and_exactly():
    # Pairwise-coprime periods whose density approaches 1 from below:
    # the elementary orbit has millions of rows; the analytical solver
    # must return the exact fixed point quickly.
    T = [2, 3, 7, 43, 1807]
    C = [1] * 5
    J = [0] * 5
    res = scheduler.rta(0, 1, 10**9, 0, J, C, T, list(range(5)))
    assert res.schedulable
    assert res.response == 3263442  # 1806 * 1807


def test_inevitable_miss_with_ultra_dense_periods_uses_analytic_tail():
    # Six Sylvester periods reach density 1 - ~3e-13: the true R exceeds
    # 1e13, far beyond D = 1e9.  Enumerating the miss orbit is impossible,
    # so the trace row budget folds the tail; the miss conclusion and the
    # capped objective value stay exact while responseExact is False.
    periods = [2, 3, 7, 43, 1807, 3263443]
    nh = len(periods)
    J = [0] * (nh + 1)
    C = [1] * (nh + 1)
    T = [10**9] + periods
    res = scheduler.rta(0, 1, 10**9, 0, J, C, T, list(range(1, nh + 1)))
    assert not res.schedulable
    assert res.response_exact is False
    assert res.response == 10**9 + 1  # capped value used by the objective
    assert res.skipped[-1] == -1      # analytically folded tail marker
    assert len(res.trace) <= 2_000
