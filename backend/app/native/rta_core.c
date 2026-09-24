/*
 * rta_core.c -- fixed-priority response-time analysis and exact priority
 * optimisation for the satellite telemetry bus.
 *
 * RTA for message i with blocking B and higher-priority set hset:
 *   r0 = J_i + C_i + B
 *   r(k+1) = J_i + C_i + B + sum_h ceil((r(k) + J_h) / T_h) * C_h
 * Iteration stops at the first fixed point (schedulable, R = r) or the
 * first value strictly greater than D_i (deadline miss).
 *
 * Exact optimum: minimise over all permutations
 *   (miss count, sum min(R_i, D_i + 1), lexicographic id sequence)
 * with subset DP over the 2^n higher-priority subsets (n <= 18).
 *
 * RTA acceleration
 *   phase 1  elementary recurrence with provable constant-increment run
 *            collapsing (ordinary instances finish in a few rows)
 *   cert     long-double utilisation certificate that proves a miss for
 *            the DP path without iterating (conservative slack)
 *   phase 2  exact least-fixed-point solver: dense messages are folded
 *            into an interference envelope over their period LCM; sparse
 *            messages delimit intervals inside which the envelope is
 *            periodic, and the least fixed point is located with suffix
 *            minima and binary searches (handles near-saturation cases
 *            whose elementary walk would have millions of rows)
 *
 *   The phase-2 envelope depends only on the dense-message subset (and the
 *   fixed message parameters), never on base/D/sparse messages.  A single
 *   subset DP revisits the same dense subset tens of thousands of times
 *   (near-saturation instances keep one large ~10^6-cell envelope hot), so
 *   solve_dp owns a FIFO, footprint-bounded envelope cache shared by all
 *   its decisions; cache entries are stack-local to one solve, hence safe
 *   under concurrent solves and immutable in every decision.
 *
 * Trace return codes: 1 fixed point, 0 crossed D, -1 guard tripped.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>

#define MAX_N 18
#define SWITCH_STEPS 600L         /* phase 1 rows before trying phase 2 */
#define TRACE_BUDGET  20000000L   /* detailed-trace elementary step cap */
#define LCM_CAP       1000000LL   /* dense-envelope window for DP path */
#define LCM_CAP_TRACE 5000000LL   /* larger window for final reporting */
#define CERT_SLACK   1e-5L        /* conservative long-double slack */

/* Bounds on the shared dense-envelope cache (one cache per DP run).
 * Envelopes depend only on the dense subset (and the fixed message
 * parameters), so the ~n 2^(n-1) DP decisions hit a tiny number of
 * distinct envelopes -- in near-saturation instances the SAME large
 * envelope is reused tens of thousands of times.  Footprint is bounded
 * by both entry count and total cell count; FIFO eviction only costs a
 * rebuild and never changes the exact result. */
#define ENV_MAX_ENTRIES 256
#define ENV_MAX_CELLS   6000000L

/* ceil((p + J) / T) for non-negative p */
static inline int64_t relq(int64_t p, int64_t J, int64_t T) {
    return (p + J + T - 1) / T;
}

/* Distance from p to the next point where ceil((x+J)/T) grows (>= 1). */
static inline int64_t next_break(int64_t p, int64_t J, int64_t T) {
    int64_t q = relq(p, J, T);
    return q * T + 1 - J - p;
}

/* Interference of the higher-priority bit set at time p. */
static inline int64_t interf_mask(uint32_t hset, int64_t p,
                                  const int64_t *J, const int64_t *C,
                                  const int64_t *T) {
    int64_t sum = 0;
    uint32_t m = hset;
    while (m) {
        uint32_t bit = m & (0u - m);
        int h = __builtin_ctz(bit);
        m ^= bit;
        sum += relq(p, J[h], T[h]) * C[h];
    }
    return sum;
}

/* Conservative miss certificate: base + sum C(D+J)/T > D.
 * Only answers "yes" when the slack dominates floating-point error. */
static int cert_miss(int64_t base, int64_t D, uint32_t hset,
                     const int64_t *J, const int64_t *C,
                     const int64_t *T) {
    long double lhs = (long double)base;
    uint32_t m = hset;
    while (m) {
        uint32_t bit = m & (0u - m);
        int h = __builtin_ctz(bit);
        m ^= bit;
        long double u = (long double)C[h] / (long double)T[h];
        lhs += u * ((long double)D + (long double)J[h]);
    }
    return lhs > (long double)D + CERT_SLACK;
}

/* ---- Phase 1: elementary recurrence, constant-run collapsing --------- */

/* Phase 1: elementary recurrence with provable constant-run collapsing.
 *
 * Return codes: 0 miss, 1 fixed point, 2 row budget spent (caller moves
 * to the phase-2 decision solver).
 *
 * Every non-collapsed row is a fresh strictly increasing integer and all
 * values before the terminal row are <= Di, so a legal instance has at
 * most Di + 2 such rows; the trace caller passes a budget of that size.
 *
 * When the trace buffer is full the walk continues buffer-less and keeps
 * repurposing the final slot with the latest value, annotated with the
 * number of elementary iterations it compresses relative to the previous
 * stored row; the terminal row therefore always carries the exact R. */
static int phase1(int64_t Di, int64_t base, uint32_t hset,
                  const int64_t *J, const int64_t *C, const int64_t *T,
                  int64_t x, long row_budget, long step_budget,
                  int64_t *trace, int64_t *skipped, int *np, int cap,
                  int *out_sched, int64_t *out_resp) {
    int n = *np;
    long rows = 0;
    long step = 0;          /* elementary iterations reaching x so far */
    long anchor_step = 0;   /* step carried by stored row at cap - 2 */

    #define EMIT(val, skip) do {                                            \
        long _ds = (long)(skip) + 1;                                        \
        step += _ds;                                                        \
        if (cap > 0) {                                                      \
            if (n < cap) {                                                  \
                if (n == cap - 2 && cap >= 2) anchor_step = step;           \
                trace[n] = (val); skipped[n] = (skip);                      \
                n++;                                                        \
            } else {                                                        \
                trace[cap - 1] = (val);                                     \
                skipped[cap - 1] = step - anchor_step - 1;                  \
            }                                                               \
        }                                                                   \
    } while (0)

    for (;;) {
        if (++rows > row_budget || step > step_budget) {
            *np = n;
            return 2;
        }

        int64_t fx = interf_mask(hset, x, J, C, T);
        int64_t y = base + fx;
        EMIT(y, 0);
        if (y > Di) {
            *np = cap > 0 ? (n < cap ? n : cap) : n;
            *out_sched = 0; *out_resp = y;
            return 0;
        }
        if (y == x) {
            *np = cap > 0 ? (n < cap ? n : cap) : n;
            *out_sched = 1; *out_resp = y;
            return 1;
        }
        int64_t delta = y - x;

        int64_t fy = interf_mask(hset, y, J, C, T);
        if (fy - fx != delta) { x = y; continue; }

        /* Regular messages (T_h | delta) put identical breakpoints in
         * every delta-length arc and contribute a constant reg per arc.
         * Irregular messages may have no breakpoint inside crossed arcs. */
        int64_t reg = 0;
        int64_t d_irreg = INT64_MAX;
        int any_irreg = 0;
        int bad = 0;
        uint32_t mm = hset;
        while (mm) {
            uint32_t bit = mm & (0u - mm);
            int h = __builtin_ctz(bit);
            mm ^= bit;
            if (delta % T[h] == 0) {
                reg += (delta / T[h]) * C[h];
            } else {
                any_irreg = 1;
                int64_t d = next_break(y, J[h], T[h]);
                if (d < d_irreg) d_irreg = d;
                if (next_break(x, J[h], T[h]) <= delta) { bad = 1; break; }
            }
        }
        if (bad || reg != delta) { x = y; continue; }

        int64_t to_miss = (y >= Di)
            ? 1 : (Di + 1 - y + delta - 1) / delta;
        int64_t s_clean;
        if (!any_irreg) {
            s_clean = to_miss;
        } else {
            int64_t s_before = (d_irreg - 1) / delta;
            s_clean = s_before < to_miss ? s_before : to_miss;
        }
        if (s_clean < 2) { x = y; continue; }

        int64_t z = y + s_clean * delta;
        EMIT(z, s_clean - 1);
        if (z > Di) {
            *np = cap > 0 ? (n < cap ? n : cap) : n;
            *out_sched = 0; *out_resp = z;
            return 0;
        }
        x = z;
    }
    #undef EMIT
}

/* ---- Phase 2: dense envelope + sparse interval sweep ------------------ */

typedef struct { int h; int64_t T, C, J; } hmsg;

static int by_T(const void *a, const void *b) {
    const hmsg *x = a, *y = b;
    if (x->T < y->T) return -1;
    if (x->T > y->T) return 1;
    return x->h - y->h;
}

static inline int64_t gcd64(int64_t a, int64_t b) {
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}

/* A built dense envelope.  Its contents depend ONLY on the subset of dense
 * (short-period, mutually-LCM-bounded) higher-priority messages, never on
 * base / D / the sparse messages, so one envelope is shared by every DP
 * state whose dense subset is equal. */
typedef struct {
    int32_t *F;
    int32_t *ev;
    int32_t *tree;
    int32_t L, E, P2;
    int64_t S, G, dense_const, win_min;
    uint32_t key;          /* dense-subset bit mask */
} envelope;

static void envelope_free(envelope *e) {
    free(e->tree); free(e->ev); free(e->F);
    e->F = NULL; e->ev = NULL; e->tree = NULL;
}

static int64_t envelope_cells(const envelope *e) {
    return (int64_t)(e->L + 1) + (e->L + 2) + 2 * (int64_t)e->P2;
}

/* Collect the higher-priority messages of hset, sorted by period. */
static int collect_hmsgs(uint32_t hset,
                         const int64_t *J, const int64_t *C,
                         const int64_t *T, hmsg *hs, int *pnh) {
    int nh = (int)__builtin_popcount(hset);
    int k = 0;
    uint32_t m = hset;
    while (m) {
        uint32_t bit = m & (0u - m);
        int h = __builtin_ctz(bit);
        m ^= bit;
        hs[k++] = (hmsg){h, T[h], C[h], J[h]};
    }
    qsort(hs, (size_t)nh, sizeof(hmsg), by_T);
    *pnh = nh;
    return nh;
}

/* Greedy maximal dense prefix of the period-sorted list; sets *pnd and the
 * dense-subset key.  Periods past the first LCM overflow are >= the failing
 * period (the list is sorted), so they could never fit either. */
static int dense_prefix(const hmsg *hs, int nh, int64_t lcm_cap,
                        int *pnd, uint32_t *pkey, int64_t *pL) {
    int nd = 0;
    int64_t L = 1;
    uint32_t key = 0;
    for (int a = 0; a < nh; a++) {
        int64_t g = gcd64(L, hs[a].T);
        int64_t nl;
        if (__builtin_mul_overflow(L, hs[a].T / g, &nl) || nl > lcm_cap) break;
        L = nl;
        nd++;
        key |= (uint32_t)1 << hs[a].h;
    }
    *pnd = nd; *pkey = key; *pL = L;
    return nd;
}

/* Build the exact dense envelope for hs[0..nd).  Return codes:
 *   1  envelope populated in *env
 *   0  dense utilisation >= 1: recurrence strictly increases, certain miss
 *  -1  resource exhaustion */
static int build_envelope(const hmsg *hs, int nd, int64_t L, uint32_t key,
                          envelope *env) {
    env->F = NULL; env->ev = NULL; env->tree = NULL;
    env->L = (int32_t)L; env->key = key;

    /* Dense periods satisfy T <= L <= lcm_cap (~5e6).  Writing each
     * jitter as J = a*T + j0 (0 <= j0 < T) extracts a large constant
     * a*C (up to ~1e9/T * C) from the window envelope: F then only carries
     * the intra-window variation, whose value is below L, so int32 storage
     * is exact and halves the peak footprint. */
    int32_t *F = (int32_t *)calloc((size_t)L + 1, sizeof(int32_t));
    if (!F) return -1;

    int64_t dense_const = 0;   /* sum a*C over the dense messages */
    for (int a = 0; a < nd; a++) {
        int64_t t = hs[a].T, c = hs[a].C, j = hs[a].J;
        int64_t whole = j / t;
        int64_t j0 = j - whole * t;
        dense_const += whole * c;
        /* ceil((q+j0)/t) at q=0 is 1 for j0 > 0; first jump to the next
         * quotient is at q = 1 (j0=0) or t-j0+1 (j0>0), then every t. */
        if (j0 > 0) {
            F[0] += (int32_t)c;
            for (int64_t q = t - j0 + 1; q <= L; q += t)
                F[q] += (int32_t)c;
        } else {
            for (int64_t q = 1; q <= L; q += t)
                F[q] += (int32_t)c;
        }
    }
    /* F[q] = sum C ceil((q+j0)/t): accumulate within-window prefix sums. */
    for (int64_t q = 1; q <= L; q++) F[q] += F[q - 1];

    int64_t S = F[L] - F[0];        /* dense within-window growth per L */
    int64_t G = L - S;              /* net slack per window (> 0: U < 1) */
    if (G <= 0) {
        /* Dense utilisation >= 1 (jitter does not change the asymptotic
         * rate): the recurrence is strictly increasing -> certain miss. */
        free(F);
        return 0;
    }

    /* Event offsets at which F jumps; segment j is [ev[j], ev[j+1]) with F
     * constant, so A(q)=F[q]-q slopes -1 and its minimum within the
     * segment is M[j] = F[ev[j]] - (ev[j+1] - 1).  The segment minima are
     * not monotone, so a range-min segment tree answers "first segment
     * after j0 with M <= tgt" in O(log E).  L <= ~5e6 fits int32. */
    int32_t *ev = (int32_t *)malloc(((size_t)L + 2) * sizeof(int32_t));
    if (!ev) { free(F); return -1; }
    int E = 1;
    ev[0] = 0;
    for (int64_t a = 1; a <= L; a++) {
        if (F[a] > F[a - 1]) ev[E++] = (int32_t)a;
    }

    int P2 = 1;
    while (P2 < E) P2 <<= 1;
    int32_t *tree = (int32_t *)malloc(2 * (size_t)P2 * sizeof(int32_t));
    if (!tree) { free(F); free(ev); return -1; }
    for (int i = 0; i < 2 * P2; i++) tree[i] = INT32_MAX;
    for (int j = 0; j < E; j++) {
        int64_t end = j + 1 < E ? (int64_t)ev[j + 1] - 1 : L;
        tree[P2 + j] = (int32_t)((int64_t)F[ev[j]] - end); /* segment min M[j] */
    }
    for (int i = P2 - 1; i >= 1; i--) {
        int64_t a = tree[2 * i], b = tree[2 * i + 1];
        tree[i] = a < b ? a : b;
    }

    env->F = F; env->ev = ev; env->tree = tree;
    env->E = E; env->P2 = P2;
    env->S = S; env->G = G; env->dense_const = dense_const;
    env->win_min = tree[1];
    return 1;
}

/* FIFO cache of dense envelopes shared by all DP decisions in one solve.
 * Both kinds of build result are remembered: a built envelope and a
 * "saturated" marker (dense utilisation >= 1 -> certain miss), so neither
 * is ever rebuilt.  Ring-buffer FIFO keeps the footprint bounded; evicting
 * only costs a rebuild and never changes the exact result. */
typedef struct {
    uint32_t keys[ENV_MAX_ENTRIES];
    int      status[ENV_MAX_ENTRIES]; /* 1 built, 0 saturated (certain miss) */
    envelope envs[ENV_MAX_ENTRIES];
    int      count;                   /* occupied slots */
    int      head;                    /* oldest occupied slot (eviction) */
    int64_t  cells;
} env_cache;

static void env_cache_init(env_cache *c) {
    c->count = 0; c->head = 0; c->cells = 0;
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) c->keys[i] = UINT32_MAX;
}

static void env_cache_destroy(env_cache *c) {
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) {
        if (c->keys[i] != UINT32_MAX && c->status[i] == 1)
            envelope_free(&c->envs[i]);
        c->keys[i] = UINT32_MAX;
    }
    c->count = c->head = 0; c->cells = 0;
}

/* Look up key; returns slot index or -1. */
static int env_cache_find(const env_cache *c, uint32_t key) {
    for (int n = 0, i = c->head; n < c->count;
         n++, i = (i + 1) % ENV_MAX_ENTRIES) {
        if (c->keys[i] == key) return i;
    }
    return -1;
}

static void env_cache_evict_oldest(env_cache *c) {
    int slot = c->head;
    c->head = (c->head + 1) % ENV_MAX_ENTRIES;
    if (c->keys[slot] == UINT32_MAX) return;
    if (c->status[slot] == 1) {
        c->cells -= envelope_cells(&c->envs[slot]);
        envelope_free(&c->envs[slot]);
    }
    c->keys[slot] = UINT32_MAX;
    c->count--;
}

/* Make room for a freshly built envelope of `need` cells: free a slot and
 * stay under the cell budget.  Returns 0 only for a single envelope that
 * alone exceeds the whole budget (caller bypasses the cache). */
static int env_cache_make_room(env_cache *c, int64_t need) {
    if (need > ENV_MAX_CELLS) return 0;
    while (c->count == ENV_MAX_ENTRIES ||
           (c->count > 0 && c->cells + need > ENV_MAX_CELLS)) {
        env_cache_evict_oldest(c);
    }
    return 1;
}

/* Adopt an already-built envelope (or a saturated marker) into the cache.
 * Ownership of built's malloc arrays transfers on adoption; on bypass the
 * envelope stays with the caller, which must free it. */
static envelope *env_cache_adopt(env_cache *c, uint32_t key, int st,
                                 envelope *built) {
    int64_t need = st == 1 ? envelope_cells(built) : 0;
    if (!env_cache_make_room(c, need)) return built;   /* oversized: bypass */

    int slot = (c->head + c->count) % ENV_MAX_ENTRIES;
    if (st == 1) {
        c->envs[slot] = *built;   /* transfer ownership of malloc arrays */
        c->cells += need;
    }
    c->keys[slot] = key;
    c->status[slot] = st;
    c->count++;
    return st == 1 ? &c->envs[slot] : NULL;
}

/* Within the level-base segment tree rooted at node, covering leaf range
 * [L0, R0), return the smallest leaf index in [ql, qr) whose value <= tgt,
 * or qr if none.  O(log^2 P2) with the tail recursion below. */
static int segtree_first(const int32_t *tree, int P2, int node,
                         int L0, int R0, int ql, int qr, int64_t tgt) {
    if (R0 <= ql || L0 >= qr || tree[node] > tgt) return qr;
    if (R0 - L0 == 1) return L0;
    int M = L0 + (R0 - L0) / 2;
    int res = segtree_first(tree, P2, 2 * node, L0, M, ql, qr, tgt);
    if (res != qr) return res;
    return segtree_first(tree, P2, 2 * node + 1, M, R0, ql, qr, tgt);
}

/* Least fixed point of f(r) = base + dense interference (prebuilt envelope)
 * + sparse interference ceil((r+J)/T) C for the messages in sparse[].
 * Returns 1 with *out_resp = R when R <= D, 0 when no fixed point <= D,
 * -1 on internal resource limits. */
static int envelope_fixed_point(const envelope *e,
                                const hmsg *sparse, int ns,
                                int64_t base, int64_t Di,
                                int64_t *out_resp) {
    const int32_t *F = e->F;
    const int32_t *ev = e->ev;
    const int32_t *tree = e->tree;
    const int32_t L = e->L, E = e->E, P2 = e->P2;
    const int64_t S = e->S, G = e->G, dense_const = e->dense_const,
                  win_min = e->win_min;

    /* Smallest q >= q0 with F[q] - q <= tgt; L+1 when none in window. */
    #define FIRST_Q(q0, tgt, out_q) do {                                      \
        int64_t _q0 = (q0);                                                  \
        (out_q) = L + 1;                                                     \
        int64_t _v0 = F[_q0] - _q0;                                         \
        if (_v0 <= (tgt)) { (out_q) = _q0; }                                 \
        else {                                                              \
            int _a = 0, _b = E - 1, _j0 = 0;                                \
            while (_a <= _b) {                                              \
                int _m = (_a + _b) / 2;                                     \
                if (ev[_m] <= _q0) { _j0 = _m; _a = _m + 1; }               \
                else _b = _m - 1;                                           \
            }                                                              \
            int64_t _end0 = _j0 + 1 < E ? (int64_t)ev[_j0 + 1] - 1 : L;    \
            int64_t _land = _q0 + (_v0 - (tgt));                            \
            if (_land <= _end0) { (out_q) = _land; }                       \
            else {                                                         \
                int _js = segtree_first(tree, P2, 1, 0, P2,               \
                                        _j0 + 1, E, (tgt));                \
                if (_js < E) {                                             \
                    int64_t _s = ev[_js];                                   \
                    int64_t _av = F[_s] - _s;                              \
                    (out_q) = _s + (_av > (tgt) ? _av - (tgt) : 0);        \
                }                                                          \
            }                                                              \
        }                                                                  \
    } while (0)

    int rc_status = 0; /* 0 = miss */
    int64_t start = base;
    long evguard = 0;
    int64_t kwin_max = (Di + 1) / L;

    for (;;) {
        if (++evguard > 30000000L) { rc_status = -1; break; }
        if (start > Di) break;

        /* Sparse interference at the current frontier; it is non-decreasing,
         * so sparse(r) >= rc for every r >= start. */
        int64_t rc = 0;
        for (int a = 0; a < ns; a++)
            rc += relq(start, sparse[a].J, sparse[a].T) * sparse[a].C;

        int64_t k0 = start / L;
        int64_t q0 = start - k0 * L;
        /* Crossing: F[q]-q <= k*G - sparse - base - dense_const. */
        int64_t rhs0 = k0 * G - rc - base - dense_const;
        int64_t q;
        int64_t k;
        if (F[q0] - q0 <= rhs0) {
            q = q0; k = k0;
        } else {
            FIRST_Q(q0, rhs0, q);
            if (q <= L) {
                k = k0;
            } else {
                /* Smallest later window whose full range admits crossing. */
                int64_t need = rc + base + dense_const + win_min;
                int64_t kc = need <= 0 ? 0 : (need + G - 1) / G;
                if (kc <= k0) kc = k0 + 1;
                if (kc > kwin_max) break;
                k = kc;
                FIRST_Q(0, kc * G - rc - base - dense_const, q);
                if (q > L) break;
            }
        }
        int64_t z = k * L + q;
        if (z > Di) break;

        /* z is the least r >= start with base + dense(r) + rc <= r.  For
         * every r in [start, z) the true map g(r) >= that bound > r, so no
         * fixed point hides before z.  At z, recompute the true sparse
         * interference: equality confirms the global least fixed point. */
        int64_t rc_z = 0;
        for (int a = 0; a < ns; a++)
            rc_z += relq(z, sparse[a].J, sparse[a].T) * sparse[a].C;
        int64_t kz = z / L, qz = z - kz * L;
        if (base + dense_const + F[qz] + kz * S + rc_z <= z) {
            *out_resp = z;
            rc_status = 1;
            break;
        }
        start = z;
    }

    #undef FIRST_Q
    return rc_status;
}

/* Decide one RTA once phase 1 has spent its row budget: build (or fetch
 * from cache) the dense envelope, then run the sparse interval sweep.
 * Returns 1 schedulable, 0 miss, -1 resource limit. */
static int phase2_decide(uint32_t hset, int64_t base, int64_t Di,
                         const int64_t *J, const int64_t *C,
                         const int64_t *T, int64_t lcm_cap,
                         env_cache *cache, int64_t *out_resp) {
    hmsg hs[MAX_N];
    int nh;
    collect_hmsgs(hset, J, C, T, hs, &nh);

    int nd;
    uint32_t key;
    int64_t L;
    dense_prefix(hs, nh, lcm_cap, &nd, &key, &L);

    if (cache != NULL) {
        int slot = env_cache_find(cache, key);
        if (slot >= 0) {
            if (cache->status[slot] == 0) return 0;   /* saturated: miss */
            return envelope_fixed_point(&cache->envs[slot],
                                        hs + nd, nh - nd, base, Di, out_resp);
        }
    }

    /* First sight of this dense subset: build once, adopt for sharing. */
    envelope local_env;
    int st = build_envelope(hs, nd, L, key, &local_env);
    if (st == -1) return -1;
    if (st == 0) {
        /* Remember the certain-miss conclusion to avoid rebuilding. */
        if (cache != NULL) env_cache_adopt(cache, key, 0, NULL);
        return 0;
    }

    envelope *env;
    if (cache != NULL) {
        envelope *cached = env_cache_adopt(cache, key, 1, &local_env);
        if (cached == &local_env) {
            /* Single envelope exceeds the whole cell budget: bypass cache. */
            int rc = envelope_fixed_point(&local_env, hs + nd, nh - nd,
                                          base, Di, out_resp);
            envelope_free(&local_env);
            return rc;
        }
        env = cached;
    } else {
        env = &local_env;
    }
    int rc = envelope_fixed_point(env, hs + nd, nh - nd, base, Di, out_resp);
    if (cache == NULL) envelope_free(&local_env);
    return rc;
}

/* ---- Entries ---------------------------------------------------------- */

/* Decision engine (DP inner loop): exact schedulability and, on success,
 * the exact least fixed point.  Misses return Di + 1 (the capped value the
 * objective uses).  cache (may be NULL) shares dense envelopes across the
 * many DP decisions of one solve. */
int rta_light(int64_t Ji, int64_t Ci, int64_t Di, int64_t B,
              const int64_t *J, const int64_t *C, const int64_t *T,
              uint32_t hset, env_cache *cache,
              int *out_sched, int64_t *out_resp) {
    int64_t base = Ji + Ci + B;
    if (base > Di) { *out_sched = 0; *out_resp = base; return 0; }
    if (cert_miss(base, Di, hset, J, C, T)) {
        *out_sched = 0; *out_resp = Di + 1; return 0;
    }

    int sched = 1;
    int64_t resp = 0;
    int n = 0;
    int st = phase1(Di, base, hset, J, C, T, base, SWITCH_STEPS,
                    LONG_MAX / 2,
                    NULL, NULL, &n, 0, &sched, &resp);
    if (st == 2) st = phase2_decide(hset, base, Di, J, C, T,
                                    LCM_CAP, cache, &resp);
    if (st == -1) return -1;
    *out_sched = st;
    *out_resp = st ? resp : Di + 1;
    return st;
}

/* Detailed trajectory (final reporting).  Up to TRACE_BUDGET elementary
 * iterations are materialised (constant runs collapsed exactly).  If a
 * pathological walk exhausts that budget the phase-2 solver supplies the
 * exact decision and the final row is emitted with skipped == -1
 * ("trajectory elided, conclusion computed analytically"); in that case a
 * miss reports Di + 1, identical to the objective's capped value. */
int rta_trace(int64_t Ji, int64_t Ci, int64_t Di, int64_t B,
              const int64_t *J, const int64_t *C, const int64_t *T,
              const int *higher, int nh,
              int64_t *trace, int64_t *skipped, int *out_n, int cap) {
    uint32_t mask = 0;
    for (int idx = 0; idx < nh; idx++) mask |= (1u << higher[idx]);
    int64_t base = Ji + Ci + B;
    int n = 0;
    if (cap > 0) { trace[0] = base; skipped[0] = 0; }
    n++;
    if (base > Di) { *out_n = n; return 0; }

    int sched = 1;
    int64_t resp = 0;
    int st = phase1(Di, base, mask, J, C, T, base,
                    LONG_MAX / 2, TRACE_BUDGET,
                    trace, skipped, &n, cap, &sched, &resp);
    if (st == 0 || st == 1) { *out_n = n; return st; }

    /* Budget exhausted: exact analytical decision.  Reporting runs only n
     * times (once per message) and uses the larger window, so it builds a
     * transient envelope rather than touching the DP-shared cache. */
    int64_t z = 0;
    int st2 = phase2_decide(mask, base, Di, J, C, T,
                            LCM_CAP_TRACE, NULL, &z);
    if (st2 == -1) return -1;
    int64_t val = st2 ? z : Di + 1;
    if (cap > 0) {
        int slot = n < cap ? n : cap - 1;
        trace[slot] = val;
        skipped[slot] = -1;
        n = slot + 1;
    }
    *out_n = n;
    return st2;
}

/* ---- Exact subset DP --------------------------------------------------- */

static void build_seq(int mask, const int *parent, const int32_t *ids, int k,
                      int32_t *out) {
    for (int p = k - 1; p >= 0; p--) {
        int i = parent[mask];
        out[p] = ids[i];
        mask ^= 1 << i;
    }
}

static int lex_compare(int i_c, int prev_c, int mask_b, const int *parent,
                       const int32_t *ids, int k,
                       int32_t *sa, int32_t *sb) {
    build_seq(prev_c, parent, ids, k - 1, sa);
    sa[k - 1] = ids[i_c];
    build_seq(mask_b, parent, ids, k, sb);
    for (int p = 0; p < k; p++) {
        if (sa[p] != sb[p]) return sa[p] < sb[p] ? -1 : 1;
    }
    return 0;
}

int solve_dp(int n, const int64_t *J, const int64_t *C,
             const int64_t *T, const int64_t *D, const int32_t *ids,
             int *out_order) {
    int size = 1 << n;
    uint32_t full = (uint32_t)(size - 1);

    env_cache cache;
    env_cache_init(&cache);

    int *miss = (int *)calloc((size_t)size, sizeof(int));
    int64_t *score = (int64_t *)calloc((size_t)size, sizeof(int64_t));
    int *parent = (int *)malloc((size_t)size * sizeof(int));
    int64_t *bmax = (int64_t *)malloc((size_t)size * sizeof(int64_t));
    int status = -2;
    if (!miss || !score || !parent || !bmax) goto cleanup;
    parent[0] = -1;

    bmax[0] = 0;
    for (int mask = 1; mask < size; mask++) {
        int bit = mask & -mask;
        int i = __builtin_ctz((unsigned)bit);
        int64_t v = bmax[mask ^ bit];
        bmax[mask] = C[i] > v ? C[i] : v;
    }

    int32_t sa[MAX_N], sb[MAX_N];

    for (int mask = 1; mask < size; mask++) {
        int have_best = 0;
        int b_miss = 0;
        int64_t b_score = 0;
        int k = __builtin_popcount((unsigned)mask);
        int m = mask;
        while (m) {
            int bit = m & -m;
            int i = __builtin_ctz((unsigned)bit);
            m ^= bit;
            int prev = mask ^ bit;
            int64_t B = bmax[full ^ (uint32_t)mask];

            int sched = 1;
            int64_t resp = 0;
            int rc = rta_light(J[i], C[i], D[i], B, J, C, T,
                               (uint32_t)prev, &cache, &sched, &resp);
            if (rc == -1) { status = -1; goto cleanup; }

            int c_miss = miss[prev] + (sched ? 0 : 1);
            int64_t term = sched ? (resp < D[i] + 1 ? resp : D[i] + 1)
                                 : D[i] + 1;
            int64_t c_score = score[prev] + term;

            int better = 0;
            if (!have_best) {
                better = 1;
            } else if (c_miss != b_miss) {
                better = c_miss < b_miss;
            } else if (c_score != b_score) {
                better = c_score < b_score;
            } else {
                better = lex_compare(i, prev, mask, parent, ids, k,
                                     sa, sb) < 0;
            }
            if (better) {
                b_miss = c_miss;
                b_score = c_score;
                parent[mask] = i;
                have_best = 1;
            }
        }
        miss[mask] = b_miss;
        score[mask] = b_score;
    }

    {
        int mask = size - 1;
        for (int p = n - 1; p >= 0; p--) {
            int i = parent[mask];
            out_order[p] = i;
            mask ^= 1 << i;
        }
    }
    status = 0;
cleanup:
    env_cache_destroy(&cache);
    free(miss); free(score); free(parent); free(bmax);
    return status;
}
