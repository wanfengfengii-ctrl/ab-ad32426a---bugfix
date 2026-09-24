# 卫星非抢占遥测总线 · 固定优先级综合台

工程师录入或导入 2–18 条带唯一编号的遥测消息，系统在 **全部 n! 种固定优先级
排列**中求出全局最优顺序，并展示每条消息的阻塞值、完整迭代轨迹与超期结论。

## 判据与优化目标

每条消息包含整数参数：传输时长 `C`、周期 `T`、截止期 `D`、抖动 `J`，约束

```
1 ≤ C ≤ D ≤ T ≤ 10^9,      0 ≤ J ≤ D − C
```

给定一个优先级顺序（高优先级在前），消息 `i` 的阻塞 `B_i` 为所有**更低优先级**
消息 `C` 的最大值（最低优先级取 0），响应时间迭代为

```
r0      = J_i + C_i + B_i
r(k+1)  = J_i + C_i + B_i + Σ_{h 为高优先级} ceil((r(k) + J_h) / T_h) · C_h
```

迭代在**到达定点**（`R_i = r ≤ D_i`，可调度）或**首次出现 `r > D_i`**（超期）
时停止。系统在全部顺序中依次最小化：

1. 超期消息条数；
2. `Σ_i min(R_i, D_i + 1)`；
3. 前两项相同的排列中，取消息**编号序列字典序**最小者。

n ≤ 18 时 n! 不可枚举；求解器用**子集 DP（2^n 个状态）**保证三层字典序最优，
热路径（RTA 迭代与 DP）由 C 扩展（`backend/app/native/rta_core.c`）实现，并保留
一份独立的纯 Python 参考实现用于测试交叉验证。病态的近饱和利用率实例由精确的
LCM 包络/最小不动点解析求解器处理，不会陷入百万次迭代。

## 快速开始（Docker Compose）

```bash
cp .env.example .env          # 可选：调整端口/健康检查
docker compose up --build
# 打开 http://localhost:8080
```

可配置项（均有默认值，见 `.env.example`）：

| 变量 | 含义 | 默认 |
| --- | --- | --- |
| `HOST_PORT` | 宿主机暴露端口 | `8080` |
| `APP_PORT` | 容器内 uvicorn 监听端口 | `8000` |
| `HEALTH_INTERVAL` / `HEALTH_TIMEOUT` / `HEALTH_RETRIES` / `HEALTH_START_PERIOD` / `HEALTH_PATH` | 容器健康检查 | 15s / 5s / 5 / 10s / `/api/health` |

### 一次性 verify 服务

运行代码测试、构建产物检查与对运行中服务的 API 冒烟，**自行退出并以退出码
报告结果**（0 成功，非 0 失败）：

```bash
docker compose --profile verify up --build verify
docker inspect ...   # 或直接观察命令退出码
```

## 本地开发

后端（FastAPI）：

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r backend/requirements.txt
sh scripts/build_native.sh            # 编译 C 扩展（需要 cc/gcc）
cd backend && uvicorn app.main:app --reload --port 8000
```

前端（Vite + React + TS，自动代理 `/api` 到 8000）：

```bash
cd frontend
npm install
npm run dev        # http://localhost:5173
npm run build      # 产物由 FastAPI 静态托管
```

测试：

```bash
cd backend && pytest -q
```

## HTTP API

- `GET /api/health` — 健康检查
- `GET /api/limits` — 输入上下限
- `POST /api/solve` — 请求体 `{"messages":[{id,C,T,D,J}, ...]}`

非法输入返回 **HTTP 422**，错误体为 FastAPI 标准 `{"detail":[...]}`，
`loc` 精确定位到具体消息下标与字段，例如
`["body","messages",1,"J"]`；重复编号定位到对应行的 `id` 字段。

响应中每条消息给出：优先级位序 `position`、阻塞 `blocking`、更高优先级编号
`higherPriorityIds`、响应时间 `response`、`schedulable`、判据项 `scoreTerm` 与
完整 `trace`（每行含 `r(k)`；`skipped>0` 表示该行精确折叠的初等迭代次数，
`skipped=-1` 表示轨迹过长时尾部由解析求解给出，超期结论与判据分值仍然精确）。

## 前端并发语义

- 任何输入框编辑都会立即令旧结果失效（结果区淡化并提示重新求解）；
- 每次发起/作废请求都推进请求序号并 `abort` 上一个请求，迟到的 HTTP 响应不会
  覆盖更新后的草稿或结果。
