#!/bin/sh
# One-shot verification entrypoint for the "verify" compose service.
# Runs the backend test suite, validates build artifacts (native library
# and frontend bundle), and performs an API smoke test against the running
# web service.  Exits non-zero on the first failed stage.
set -eu

ROOT="${APP_ROOT:-/app}"
BACKEND_DIR="$ROOT/backend"
DIST_DIR="$ROOT/frontend/dist"
BASE_URL="${SMOKE_BASE_URL:-http://web:8000}"

echo "== [1/4] backend test suite =="
cd "$BACKEND_DIR"
python -m pytest tests/ -q

echo "== [2/4] build artifact checks =="
python - "$ROOT" <<'PY'
import sys
from pathlib import Path
from app import scheduler

root = Path(sys.argv[1])
lib = scheduler._load_lib()
assert lib is not None, "native accelerator library failed to load"

assert (root / "backend/app/native/librtacore.so").exists()
idx = root / "frontend/dist/index.html"
assert idx.is_file(), "frontend bundle (dist/index.html) missing"
assets = root / "frontend/dist/assets"
assert assets.is_dir() and any(assets.iterdir()), "frontend assets missing"
# Every asset referenced by index.html must exist and be non-empty.
import re
html = idx.read_text(encoding="utf-8")
refs = re.findall(r'(?:src|href)="(/assets/[^"]+)"', html)
assert refs, "index.html references no built assets"
for ref in refs:
    path = root / "frontend/dist" / ref.lstrip("/")
    assert path.is_file() and path.stat().st_size > 0, f"missing/empty asset {ref}"
print(f"artifacts OK ({len(refs)} referenced assets)")
PY

echo "== [3/4] API smoke against $BASE_URL =="
# Give the web service a moment even when dependencies are not orchestrated.
python - "$BASE_URL" <<'PY'
import json
import sys
import time

import httpx

base = sys.argv[1]
deadline = time.time() + 30
last = None
while time.time() < deadline:
    try:
        r = httpx.get(f"{base}/api/health", timeout=2)
        if r.status_code == 200 and r.json().get("status") == "ok":
            break
    except Exception as exc:  # noqa: BLE001
        last = exc
    time.sleep(1)
else:
    raise SystemExit(f"health endpoint never became ready: {last}")

payload = {
    "messages": [
        {"id": 1, "C": 1, "T": 8, "D": 8, "J": 0},
        {"id": 2, "C": 2, "T": 6, "D": 6, "J": 0},
        {"id": 3, "C": 3, "T": 10, "D": 10, "J": 0},
    ]
}
r = httpx.post(f"{base}/api/solve", json=payload, timeout=30)
assert r.status_code == 200, r.text
data = r.json()
assert sorted(data["order"]) == [1, 2, 3], data
assert data["objective"]["misses"] == 0, data
for row in data["messages"]:
    assert row["trace"], "trace must not be empty"
    assert row["response"] == row["trace"][-1]["r"]

# 422 field-location smoke
bad = {"messages": [
    {"id": 1, "C": 1, "T": 4, "D": 4, "J": 0},
    {"id": 1, "C": 9, "T": 5, "D": 5, "J": 0},
]}
r = httpx.post(f"{base}/api/solve", json=bad, timeout=10)
assert r.status_code == 422, r.text
locs = [tuple(e["loc"]) for e in r.json()["detail"]]
assert any(loc[:3] == ("body", "messages", 1) for loc in locs), locs
print("API smoke OK:", json.dumps(data["objective"]))
PY

echo "== [4/4] static frontend smoke =="
curl -fsS "$BASE_URL/" | grep -q '<div id="root">'

echo "VERIFY_OK"
