"""FastAPI application: exact fixed-priority telemetry-bus optimisation."""
from __future__ import annotations

from pathlib import Path

from fastapi import FastAPI
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from .models import MAX_MESSAGES, MIN_MESSAGES, MessageSet
from .scheduler import MessageReport, solve

app = FastAPI(
    title="Satellite Telemetry Bus Priority Analyser",
    version="1.0.0",
)


@app.get("/api/health")
async def health():
    return {"status": "ok"}


@app.get("/api/limits")
async def limits():
    return {"minMessages": MIN_MESSAGES, "maxMessages": MAX_MESSAGES, "maxValue": 10**9}


def _trace_payload(report: MessageReport) -> list[dict]:
    rows = []
    for k, (value, skipped) in enumerate(zip(report.trace, report.skipped)):
        rows.append({
            "k": k,
            "r": value,
            # skipped == 0: ordinary elementary row; >0: exact number of
            # collapsed iterations; -1: remainder solved analytically.
            "skipped": int(skipped),
        })
    return rows


@app.post("/api/solve")
def solve_endpoint(payload: MessageSet):
    # A plain (non-async) def runs in the worker thread pool, so the exact
    # solver (up to a few seconds on adversarial instances) never blocks the
    # event loop or the health endpoint.
    raw = payload.messages

    # Unique-id validation with field-level 422 locations.
    seen: dict[int, int] = {}
    errors = []
    for i, msg in enumerate(raw):
        if msg.id in seen:
            errors.append({
                "loc": ["body", "messages", i, "id"],
                "msg": f"duplicate id {msg.id}; first seen at index {seen[msg.id]}",
                "type": "value_error.unique",
            })
        else:
            seen[msg.id] = i
    if errors:
        raise RequestValidationError(errors)

    messages = [m.model_dump() for m in raw]
    order, reports = solve(messages)

    result_messages = []
    for i in order:
        r: MessageReport = reports[i]
        m = messages[i]
        result_messages.append({
            "id": m["id"],
            "C": m["C"],
            "T": m["T"],
            "D": m["D"],
            "J": m["J"],
            "position": r.position,
            "blocking": r.blocking,
            "higherPriorityIds": r.higher,
            "response": r.response,
            "responseExact": r.response_exact,
            "schedulable": r.schedulable,
            "scoreTerm": r.score_term,
            "trace": _trace_payload(r),
        })

    misses = sum(1 for r in reports if not r.schedulable)
    score = sum(r.score_term for r in reports)
    return {
        "order": [messages[i]["id"] for i in order],
        "objective": {"misses": misses, "score": score},
        "messages": result_messages,
    }


# ---- Production static hosting (Vite build) -----------------------------

_DIST = Path(__file__).resolve().parents[2] / "frontend" / "dist"
if _DIST.is_dir():
    app.mount("/assets", StaticFiles(directory=_DIST / "assets"), name="assets")

    def _serve_index():
        return FileResponse(_DIST / "index.html")

    @app.get("/", include_in_schema=False)
    async def index():
        return _serve_index()

    @app.get("/{full_path:path}", include_in_schema=False)
    async def spa(full_path: str):
        if full_path.startswith("api/"):
            return JSONResponse(status_code=404, content={"detail": "Not Found"})
        return _serve_index()
