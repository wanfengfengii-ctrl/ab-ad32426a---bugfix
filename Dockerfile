# syntax=docker/dockerfile:1

# ---- Stage 1: build the React frontend ----
FROM node:20-bookworm-slim AS frontend
WORKDIR /app/frontend
# Use a project-local cache to avoid permission issues with ~/.npm.
RUN npm config set cache /tmp/.npm-cache
COPY frontend/package.json frontend/package-lock.json* ./
RUN npm ci --no-audit --no-fund
COPY frontend/ ./
RUN npm run build

# ---- Stage 2: Python API serving the built frontend ----
FROM python:3.11-slim-bookworm AS runtime
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1

# gcc is needed to build the native RTA/DP accelerator.
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY backend/requirements.txt /app/backend/requirements.txt
RUN pip install --upgrade pip && pip install -r /app/backend/requirements.txt

COPY backend/ /app/backend/
COPY scripts/ /app/scripts/

# Compile the native core into the package directory.
RUN sh /app/scripts/build_native.sh

# Built frontend is served statically by FastAPI.
COPY --from=frontend /app/frontend/dist /app/frontend/dist

# Configurable host port (compose maps this to a configurable host port).
ENV APP_HOST=0.0.0.0 \
    APP_PORT=8000
EXPOSE 8000

# Container-level health check; the path/interval can be overridden.
HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=5 \
    CMD curl -fsS "http://127.0.0.1:${APP_PORT}/api/health" || exit 1

WORKDIR /app/backend
CMD ["sh", "-c", "exec python -m uvicorn app.main:app --host ${APP_HOST} --port ${APP_PORT}"]
