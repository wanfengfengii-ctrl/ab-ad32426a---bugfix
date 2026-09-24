"""Tests for exact global priority optimisation."""
from __future__ import annotations

import itertools
import random
import time

import pytest

from app import scheduler


def objective(messages, order_idx):
    reports = scheduler.analyse_order(messages, list(order_idx))
    return (
        sum(not r.schedulable for r in reports),
        sum(r.score_term for r in reports),
        [messages[i]["id"] for i in order_idx],
    )


def brute_force_best(messages):
    n = len(messages)
    return min(
        itertools.permutations(range(n)),
        key=lambda order: objective(messages, order),
    )


def random_set(seed, n):
    random.seed(seed)
    msgs = []
    for k in range(n):
        t = random.choice([2, 3, 4, 5, 7, 8, 10, 13, 20, 40, 100, 200, 1000])
        c = random.randint(1, t)
        d = random.randint(c, t)
        j = random.randint(0, d - c)
        msgs.append({"id": k + 1, "C": c, "T": t, "D": d, "J": j})
    return msgs


@pytest.mark.parametrize("n", [2, 3, 4, 5, 6])
def test_dp_matches_brute_force(n):
    for seed in range(6):
        msgs = random_set(100 + n * 10 + seed, n)
        expected = list(brute_force_best(msgs))
        assert scheduler.find_best_order(msgs) == expected


def test_native_and_python_dp_agree_medium():
    for seed in range(12):
        n = random.Random(seed).randint(7, 10)
        msgs = random_set(500 + seed, n)
        assert scheduler.find_best_order_c(msgs) == \
            scheduler.find_best_order_python(msgs)


def test_lexicographic_tiebreak_on_ids():
    # Two identical messages: orders are symmetric in the first two
    # objectives, so the smallest-id-first permutation must win.
    msgs = [
        {"id": 9, "C": 1, "T": 10, "D": 10, "J": 0},
        {"id": 2, "C": 1, "T": 10, "D": 10, "J": 0},
    ]
    order = scheduler.find_best_order(msgs)
    assert [msgs[i]["id"] for i in order] == [2, 9]


def test_all_miss_case_prefers_smaller_capped_sum():
    # Both must miss under any order; optimum minimises sum min(R, D+1).
    msgs = [
        {"id": 1, "C": 5, "T": 5, "D": 5, "J": 0},
        {"id": 2, "C": 5, "T": 5, "D": 5, "J": 0},
    ]
    order = scheduler.find_best_order(msgs)
    reports = scheduler.analyse_order(msgs, order)
    assert all(not r.schedulable for r in reports)
    # score uses capped value D + 1 for misses
    assert sum(r.score_term for r in reports) == 12


def test_n18_runs_quickly():
    msgs = random_set(777, 18)
    start = time.time()
    order = scheduler.find_best_order_c(msgs)
    elapsed = time.time() - start
    assert len(order) == 18
    assert elapsed < 30.0


def test_near_saturation_bus_decides_within_window():
    # Two short-period messages plus twelve unit messages at T = 1e9 push the
    # bus to the edge of schedulability.  The exact subset DP reuses the dense
    # interference envelope across sparse tails; rebuilding it per state made
    # this instance take ~55 s instead of returning inside the 30 s window.
    msgs = [
        {"id": 1, "C": 332, "T": 997, "D": 997, "J": 0},
        {"id": 2, "C": 667, "T": 1000, "D": 1000, "J": 0},
    ]
    for kid in range(3, 15):
        msgs.append({"id": kid, "C": 1, "T": 10**9, "D": 10**9, "J": 0})

    start = time.time()
    order, reports = scheduler.solve(msgs)
    elapsed = time.time() - start

    assert [msgs[i]["id"] for i in order] == [
        3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 1,
    ]
    assert sum(not r.schedulable for r in reports) == 1
    assert sum(r.score_term for r in reports) == 21069
    assert elapsed < 30.0


def test_blocking_is_max_lower_priority_transmission():
    msgs = [
        {"id": 1, "C": 1, "T": 100, "D": 100, "J": 0},
        {"id": 2, "C": 7, "T": 100, "D": 100, "J": 0},
        {"id": 3, "C": 3, "T": 100, "D": 100, "J": 0},
    ]
    order = [0, 1, 2]  # id1 highest
    reports = scheduler.analyse_order(msgs, order)
    by_id = {msgs[r.idx]["id"]: r for r in reports}
    assert by_id[1].blocking == 7   # max C among lower priority
    assert by_id[2].blocking == 3
    assert by_id[3].blocking == 0   # lowest priority: no blocking


def test_report_lists_higher_priority_ids_in_order():
    msgs = [
        {"id": 4, "C": 1, "T": 50, "D": 50, "J": 0},
        {"id": 5, "C": 1, "T": 50, "D": 50, "J": 0},
        {"id": 6, "C": 1, "T": 50, "D": 50, "J": 0},
    ]
    reports = scheduler.analyse_order(msgs, [2, 0, 1])
    by_pos = sorted(reports, key=lambda r: r.position)
    assert by_pos[0].higher == []
    assert by_pos[1].higher == [6]
    assert by_pos[2].higher == [6, 4]
