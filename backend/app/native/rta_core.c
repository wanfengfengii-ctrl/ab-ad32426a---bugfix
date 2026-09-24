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
 * Trace return codes: 1 fixed point, 0 crossed D, -1 guard tripped.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define MAX_N 18
#define SWITCH_STEPS 600L         /* phase 1 rows before trying phase 2 */
#define TRACE_BUDGET  20000000L   /* detailed-trace elementary step cap */
#define LCM_CAP       1000000LL   /* dense-envelope window for DP path */
#define LCM_CAP_TRACE 5000000LL   /* larger window for final reporting */
#define CERT_SLACK   1e-5L        /* conservative long-double slack */

/* The dense interference envelope (period-LCM window F, its jump events and
 * the range-min segment tree) depends only on the set of dense higher-
 * priority messages, not on the sparse tail.  A subset-DP run asks the same
 * few envelopes thousands of times (once per sparse subset); rebuilding an
 * O(L) window each time dominated near-saturation instances.  Envelopes are
 * therefore built once per solve and reused through this small LRU cache. */
#define ENV_SLOTS    64
#define ENV_BUDGET   (1LL << 28)  /* 256 MiB cap on live envelope storage */

typedef struct {
    int valid;
    uint32_t key;                 /* dense-set bitmask */
    unsigned long stamp;
    int64_t L, S, G;
    int64_t dense_const;
    int64_t win_min;
    int32_t *F;                   /* NULL for a retained G <= 0 marker */
    int32_t *ev;
    int32_t *tree;
    int E, P2;
    size_t bytes;
} env_entry;

typedef struct {
    env_entry slots[ENV_SLOTS];
    size_t total_bytes;
    unsigned long clock;
} env_cache;

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

/* ---- Dense-envelope cache --------------------------------------------- */

static void env_release(env_entry *e) {
    free(e->F);
    free(e->ev);
    free(e->tree);
    memset(e, 0, sizeof(*e));
}

static void env_cache_clear(env_cache *ec) {
    for (int s = 0; s < ENV_SLOTS; s++) env_release(&ec->slots[s]);
    ec->total_bytes = 0;
}

/* Build the exact interference envelope of the dense set ``key``.
 * Returns 0 on success, -1 on allocation failure.  A saturated dense set
 * (utilisation >= 1) is retained as a F == NULL marker carrying G <= 0. */
static int env_build(env_entry *e, uint32_t key,
                     const int64_t *J, const int64_t *C,
                     const int64_t *T, int64_t lcm_cap) {
    memset(e, 0, sizeof(*e));
    e->valid = 1;
    e->key = key;

    int nd = (int)__builtin_popcount((unsigned)key);
    hmsg hs[MAX_N];
    int k = 0;
    uint32_t mm = key;
    while (mm) {
        uint32_t bit = mm & (0u - mm);
        int h = __builtin_ctz(bit);
        mm ^= bit;
        hs[k++] = (hmsg){h, T[h], C[h], J[h]};
    }
    qsort(hs, (size_t)nd, sizeof(hmsg), by_T);

    int64_t L = 1;
    for (int a = 0; a < nd; a++) {
        int64_t g = gcd64(L, hs[a].T);
        int64_t nl;
        if (__builtin_mul_overflow(L, hs[a].T / g, &nl) || nl > lcm_cap) {
            /* The key is the greedy-prefix result of its caller, so every
             * member fits; tolerate a mismatch by truncating the key. */
            nd = a;
            key = 0;
            for (int b = 0; b < a; b++) key |= (uint32_t)1 << hs[b].h;
            e->key = key;
            break;
        }
        L = nl;
    }
    e->L = L;

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
    e->S = S;
    e->G = G;
    e->dense_const = dense_const;
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
        tree[P2 + j] = (int32_t)((int64_t)F[ev[j]] - end); /* segment min */
    }
    for (int i = P2 - 1; i >= 1; i--) {
        int64_t a = tree[2 * i], b = tree[2 * i + 1];
        tree[i] = a < b ? a : b;
    }

    e->F = F;
    e->ev = ev;
    e->tree = tree;
    e->E = E;
    e->P2 = P2;
    e->win_min = tree[1];
    e->bytes = ((size_t)L + 1) * sizeof(int32_t)
             + ((size_t)L + 2) * sizeof(int32_t)
             + 2 * (size_t)P2 * sizeof(int32_t);
    return 0;
}

/* Return the cached envelope for ``key``, building it on a miss.  LRU
 * eviction keeps the live footprint under ENV_BUDGET.  NULL on failure. */
static env_entry *env_get(env_cache *ec, uint32_t key,
                          const int64_t *J, const int64_t *C,
                          const int64_t *T, int64_t lcm_cap) {
    for (int s = 0; s < ENV_SLOTS; s++) {
        env_entry *e = &ec->slots[s];
        if (e->valid && e->key == key) {
            e->stamp = ++ec->clock;
            return e;
        }
    }

    int victim = -1;
    unsigned long oldest = 0;
    for (int s = 0; s < ENV_SLOTS; s++) {
        if (!ec->slots[s].valid) { victim = s; break; }
        if (victim < 0 || ec->slots[s].stamp < oldest) {
            victim = s;
            oldest = ec->slots[s].stamp;
        }
    }
    env_entry *e = &ec->slots[victim];
    if (e->valid) {
        ec->total_bytes -= e->bytes;
        env_release(e);
    }
    if (env_build(e, key, J, C, T, lcm_cap) != 0) {
        env_release(e);
        return NULL;
    }
    e->stamp = ++ec->clock;
    ec->total_bytes += e->bytes;

    while (ec->total_bytes > ENV_BUDGET) {
        int old = -1;
        unsigned long ost = 0;
        for (int s = 0; s < ENV_SLOTS; s++) {
            env_entry *g = &ec->slots[s];
            if (!g->valid || g == e || g->bytes == 0) continue;
            if (old < 0 || g->stamp < ost) { old = s; ost = g->stamp; }
        }
        if (old < 0) break;
        ec->total_bytes -= ec->slots[old].bytes;
        env_release(&ec->slots[old]);
    }
    return e;
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

/* Smallest q >= q0 with F[q] - q <= tgt; L+1 when none in window. */
static inline int64_t first_q(const env_entry *e, int64_t q0, int64_t tgt) {
    const int32_t *F = e->F, *ev = e->ev, *tree = e->tree;
    int64_t L = e->L;
    int E = e->E, P2 = e->P2;
    int64_t out = L + 1;
    int64_t v0 = F[q0] - q0;
    if (v0 <= tgt) { out = q0; }
    else {
        int a = 0, b = E - 1, j0 = 0;
        while (a <= b) {
            int mm = (a + b) / 2;
            if (ev[mm] <= q0) { j0 = mm; a = mm + 1; }
            else b = mm - 1;
        }
        int64_t end0 = j0 + 1 < E ? (int64_t)ev[j0 + 1] - 1 : L;
        int64_t land = q0 + (v0 - tgt);
        if (land <= end0) { out = land; }
        else {
            int js = segtree_first(tree, P2, 1, 0, P2, j0 + 1, E, tgt);
            if (js < E) {
                int64_t s = ev[js];
                int64_t av = F[s] - s;
                out = s + (av > tgt ? av - tgt : 0);
            }
        }
    }
    return out;
}

/* Least fixed point of f(r) = base + sum_h C_h ceil((r+J_h)/T_h).
 * Returns 1 with *out_resp = R when R <= D, 0 when no fixed point <= D,
 * -1 on internal resource limits.  lcm_cap bounds the dense envelope. */
static int least_fixed_point(int64_t base, int64_t Di,
                             uint32_t hset,
                             const int64_t *J, const int64_t *C,
                             const int64_t *T,
                             int64_t lcm_cap, env_cache *ec,
                             int64_t *out_resp) {
    int nh = (int)__builtin_popcount((unsigned)hset);
    hmsg hs[MAX_N];
    int k = 0;
    uint32_t m = hset;
    while (m) {
        uint32_t bit = m & (0u - m);
        int h = __builtin_ctz(bit);
        m ^= bit;
        hs[k++] = (hmsg){h, T[h], C[h], J[h]};
    }
    qsort(hs, (size_t)nh, sizeof(hmsg), by_T);

    /* Greedy dense prefix by LCM window only; the positive-slack check
     * happens after the exact envelope is built (jitter shifts breakpoints
     * at window boundaries, so utilisation cannot be decided from the
     * periods alone). */
    int nd = 0;
    int64_t L = 1;
    uint32_t dkey = 0;
    for (int a = 0; a < nh; a++) {
        int64_t g = gcd64(L, hs[a].T);
        int64_t nl;
        if (__builtin_mul_overflow(L, hs[a].T / g, &nl) || nl > lcm_cap) break;
        L = nl;
        dkey |= (uint32_t)1 << hs[a].h;
        nd++;
    }

    env_entry *e = env_get(ec, dkey, J, C, T, lcm_cap);
    if (e == NULL) return -1;
    /* env_build may conservatively truncate a pathological key; align the
     * sparse boundary with what the envelope actually covers. */
    nd = (int)__builtin_popcount((unsigned)e->key);

    int64_t dense_const = e->dense_const;
    int64_t G = e->G;
    int64_t win_min = e->win_min;
    L = e->L;
    int64_t S = e->S;
    const int32_t *F = e->F;

    if (G <= 0) {
        /* Dense utilisation >= 1 (jitter does not change the asymptotic
         * rate): the recurrence is strictly increasing -> certain miss. */
        return 0;
    }

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
        for (int a = nd; a < nh; a++)
            rc += relq(start, hs[a].J, hs[a].T) * hs[a].C;

        int64_t k0 = start / L;
        int64_t q0 = start - k0 * L;
        /* Crossing: F[q]-q <= k*G - sparse - base - dense_const. */
        int64_t rhs0 = k0 * G - rc - base - dense_const;
        int64_t q;
        int64_t k;
        if (F[q0] - q0 <= rhs0) {
            q = q0; k = k0;
        } else {
            q = first_q(e, q0, rhs0);
            if (q <= L) {
                k = k0;
            } else {
                /* Smallest later window whose full range admits crossing. */
                int64_t need = rc + base + dense_const + win_min;
                int64_t kc = need <= 0 ? 0 : (need + G - 1) / G;
                if (kc <= k0) kc = k0 + 1;
                if (kc > kwin_max) break;
                k = kc;
                q = first_q(e, 0, kc * G - rc - base - dense_const);
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
        for (int a = nd; a < nh; a++)
            rc_z += relq(z, hs[a].J, hs[a].T) * hs[a].C;
        int64_t kz = z / L, qz = z - kz * L;
        if (base + dense_const + F[qz] + kz * S + rc_z <= z) {
            *out_resp = z;
            rc_status = 1;
            break;
        }
        start = z;
    }

    return rc_status;
}

/* ---- Entries ---------------------------------------------------------- */

/* Decision engine (DP inner loop): exact schedulability and, on success,
 * the exact least fixed point.  Misses return Di + 1 (the capped value the
 * objective uses). */
static int rta_light_cached(int64_t Ji, int64_t Ci, int64_t Di, int64_t B,
                            const int64_t *J, const int64_t *C,
                            const int64_t *T, uint32_t hset,
                            env_cache *ec,
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
    if (st == 2) st = least_fixed_point(base, Di, hset, J, C, T,
                                        LCM_CAP, ec, &resp);
    if (st == -1) return -1;
    *out_sched = st;
    *out_resp = st ? resp : Di + 1;
    return st;
}

/* Public single-query entry: owns a one-shot envelope cache. */
int rta_light(int64_t Ji, int64_t Ci, int64_t Di, int64_t B,
              const int64_t *J, const int64_t *C, const int64_t *T,
              uint32_t hset, int *out_sched, int64_t *out_resp) {
    env_cache ec;
    memset(&ec, 0, sizeof(ec));
    int st = rta_light_cached(Ji, Ci, Di, B, J, C, T, hset, &ec,
                              out_sched, out_resp);
    env_cache_clear(&ec);
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

    /* Budget exhausted: exact analytical decision. */
    int64_t z = 0;
    env_cache ec;
    memset(&ec, 0, sizeof(ec));
    int st2 = least_fixed_point(base, Di, mask, J, C, T,
                                LCM_CAP_TRACE, &ec, &z);
    env_cache_clear(&ec);
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

    /* One cache for the whole DP: the dense envelope queried by a state
     * depends only on the dense subset, which recurs across thousands of
     * distinct sparse subsets. */
    env_cache ec;
    memset(&ec, 0, sizeof(ec));

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
            int rc = rta_light_cached(J[i], C[i], D[i], B, J, C, T,
                                      (uint32_t)prev, &ec, &sched, &resp);
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
    env_cache_clear(&ec);
    free(miss); free(score); free(parent); free(bmax);
    return status;
}
