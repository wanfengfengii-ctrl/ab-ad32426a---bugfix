"""HTTP-level tests: valid solving, 422 field locations, response shape."""
from __future__ import annotations

from fastapi.testclient import TestClient

from app.main import app

client = TestClient(app)


def m(mid, C, T, D=None, J=0):
    return {"id": mid, "C": C, "T": T, "D": D if D is not None else T, "J": J}


def test_health():
    r = client.get("/api/health")
    assert r.status_code == 200
    assert r.json()["status"] == "ok"


def test_limits():
    r = client.get("/api/limits")
    assert r.json() == {"minMessages": 2, "maxMessages": 18, "maxValue": 10**9}


def test_solve_basic_order_and_report():
    payload = {"messages": [
        m(1, 1, 8),
        m(2, 2, 6),
        m(3, 3, 10),
    ]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 200, r.text
    data = r.json()

    assert set(data["order"]) == {1, 2, 3}
    assert data["objective"] == {"misses": 0, "score": data["objective"]["score"]}
    assert len(data["messages"]) == 3

    positions = [row["position"] for row in data["messages"]]
    assert sorted(positions) == [1, 2, 3]
    for row in data["messages"]:
        assert row["trace"], "trace must contain at least r0"
        assert row["trace"][0]["k"] == 0
        assert row["trace"][0]["r"] == row["J"] + row["C"] + row["blocking"]
        assert row["response"] == row["trace"][-1]["r"]
        # blocking = max C of strictly lower-priority messages
        lower_ids = [i for i in data["order"]
                     if data["order"].index(i) > data["order"].index(row["id"])]
        lower_c = [x["C"] for x in payload["messages"] if x["id"] in lower_ids]
        assert row["blocking"] == (max(lower_c) if lower_c else 0)


def test_solution_is_feasible_optimum_small():
    # Classic rate-monotonic instance that is schedulable in some order.
    payload = {"messages": [
        m(1, 1, 4), m(2, 1, 6), m(3, 2, 10),
    ]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 200
    assert r.json()["objective"]["misses"] == 0


def test_422_when_too_few_messages():
    r = client.post("/api/solve", json={"messages": [m(1, 1, 4)]})
    assert r.status_code == 422
    locs = [tuple(e["loc"]) for e in r.json()["detail"]]
    assert any(loc[:2] == ("body", "messages") for loc in locs)


def test_422_when_too_many_messages():
    payload = {"messages": [m(i, 1, 100) for i in range(1, 20)]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 422


def test_422_locates_specific_field_for_out_of_range_value():
    bad = m(2, 5, 4)  # C > T and C > D
    r = client.post("/api/solve", json={"messages": [m(1, 1, 4), bad]})
    assert r.status_code == 422
    fields = {e["loc"][-1] for e in r.json()["detail"]}
    assert "C" in fields


def test_422_for_d_gt_t():
    bad = {"id": 2, "C": 1, "T": 5, "D": 6, "J": 0}
    r = client.post("/api/solve", json={"messages": [m(1, 1, 4), bad]})
    assert r.status_code == 422
    assert any(e["loc"][-1] == "D" for e in r.json()["detail"])


def test_422_for_j_gt_d_minus_c():
    bad = {"id": 2, "C": 2, "T": 10, "D": 5, "J": 4}  # J > D-C = 3
    r = client.post("/api/solve", json={"messages": [m(1, 1, 4), bad]})
    assert r.status_code == 422
    assert any(e["loc"][-1] == "J" for e in r.json()["detail"])


def test_422_for_negative_j():
    bad = {"id": 2, "C": 2, "T": 10, "D": 10, "J": -1}
    r = client.post("/api/solve", json={"messages": [m(1, 1, 4), bad]})
    assert r.status_code == 422
    assert any(e["loc"][-1] == "J" for e in r.json()["detail"])


def test_422_for_value_above_1e9():
    bad = {"id": 2, "C": 10**9 + 1, "T": 10**9 + 1,
           "D": 10**9 + 1, "J": 0}
    r = client.post("/api/solve", json={"messages": [m(1, 1, 4), bad]})
    assert r.status_code == 422


def test_422_locates_message_index_in_loc():
    bad = [m(1, 1, 4), m(2, 0, 5)]  # C = 0 invalid at index 1
    r = client.post("/api/solve", json={"messages": bad})
    assert r.status_code == 422
    locs = [tuple(e["loc"]) for e in r.json()["detail"]]
    assert any(loc[:3] == ("body", "messages", 1) for loc in locs)


def test_422_for_duplicate_ids_points_at_field():
    payload = {"messages": [m(1, 1, 4), m(1, 1, 5)]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 422
    errs = r.json()["detail"]
    assert any(e["loc"] == ["body", "messages", 1, "id"] for e in errs)


def test_422_for_extra_field():
    payload = {"messages": [m(1, 1, 4), m(2, 1, 5)], "bogus": 1}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 422


def test_422_for_wrong_type():
    r = client.post("/api/solve", json={"messages": [m(1, "x", 4), m(2, 1, 5)]})
    assert r.status_code == 422


def test_422_is_independent_of_json_field_order():
    # Fields deliberately sent out of declaration order (J first, id last);
    # the cross-field checks must still point at the offending field.
    payload = {"messages": [
        {"J": 0, "C": 1, "T": 4, "D": 4, "id": 1},
        {"J": 0, "C": 9, "D": 5, "T": 5, "id": 2},
    ]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 422
    locs = [tuple(e["loc"]) for e in r.json()["detail"]]
    assert ("body", "messages", 1, "C") in locs


def test_boundary_values_accepted():
    payload = {"messages": [
        {"id": 1, "C": 1, "T": 1, "D": 1, "J": 0},
        {"id": 10**9, "C": 10**9, "T": 10**9, "D": 10**9, "J": 0},
    ]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 200, r.text


def test_j_at_boundary_d_minus_c_accepted():
    payload = {"messages": [
        {"id": 1, "C": 3, "T": 10, "D": 7, "J": 4},
        {"id": 2, "C": 2, "T": 9, "D": 8, "J": 6},
    ]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 200, r.text


def test_solve_maximum_size_18_messages():
    payload = {"messages": [
        {"id": i, "C": 1, "T": 100 + i, "D": 100 + i, "J": 0}
        for i in range(1, 19)
    ]}
    r = client.post("/api/solve", json=payload)
    assert r.status_code == 200, r.text
    data = r.json()
    assert sorted(data["order"]) == list(range(1, 19))
    assert len(data["messages"]) == 18
    positions = sorted(row["position"] for row in data["messages"])
    assert positions == list(range(1, 19))
    for row in data["messages"]:
        assert len(row["trace"]) >= 1
        # blocking = max C of strictly lower-priority messages
        pos = data["order"].index(row["id"])
        lower = data["order"][pos + 1:]
        c_by_id = {x["id"]: x["C"] for x in payload["messages"]}
        expected_blocking = max((c_by_id[i] for i in lower), default=0)
        assert row["blocking"] == expected_blocking


def test_near_saturation_acceptance_instance_via_api():
    import time

    payload = {"messages": [
        {"id": 1, "C": 332, "T": 997, "D": 997, "J": 0},
        {"id": 2, "C": 667, "T": 1000, "D": 1000, "J": 0},
    ] + [
        {"id": i, "C": 1, "T": 10**9, "D": 10**9, "J": 0}
        for i in range(3, 15)
    ]}
    start = time.time()
    r = client.post("/api/solve", json=payload)
    elapsed = time.time() - start
    assert r.status_code == 200, r.text
    assert elapsed < 5.0, elapsed
    data = r.json()
    assert data["order"] == [
        3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 1,
    ]
    assert data["objective"] == {"misses": 1, "score": 21069}
    by_id = {row["id"]: row for row in data["messages"]}
    assert by_id[1]["schedulable"] is False
    assert by_id[1]["response"] == 1011
    assert by_id[1]["responseExact"] is True
    assert by_id[1]["scoreTerm"] == 998
    assert by_id[2]["response"] == 1000 and by_id[2]["schedulable"]
    for row in data["messages"]:
        assert row["trace"], "trace must not be empty"
        assert row["response"] == row["trace"][-1]["r"]

