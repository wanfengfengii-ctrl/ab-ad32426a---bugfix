import { useState } from "react";
import type { SolvedMessage, SolveResponse, TraceRow } from "./types";

interface ResultProps {
  result: SolveResponse;
}

function TraceTable({ trace }: { trace: TraceRow[] }) {
  const [open, setOpen] = useState(false);
  const shown = open ? trace : trace.slice(0, 8);
  const hidden = trace.length - shown.length;
  return (
    <div className="trace">
      <table className="trace-grid">
        <thead>
          <tr>
            <th>k</th>
            <th>r(k)</th>
            <th>说明</th>
          </tr>
        </thead>
        <tbody>
          {shown.map((row, i) => (
            <tr key={i} className={row.skipped < 0 ? "elided" : ""}>
              <td>{row.k}</td>
              <td>{row.r}</td>
              <td className="muted">
                {row.skipped === 0
                  ? row.k === 0
                    ? "r₀ = J + C + B"
                    : "一次初等迭代"
                  : row.skipped > 0
                    ? `精确折叠了 ${row.skipped} 次恒定增量迭代`
                    : "轨迹过长，尾部经精确解析求解，结论有效；展示值在超期时封顶为 D+1"}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      {trace.length > 8 && (
        <button type="button" className="link-btn" onClick={() => setOpen((v) => !v)}>
          {open ? "收起轨迹" : `展开全部 ${trace.length} 行（另有 ${hidden} 行）`}
        </button>
      )}
    </div>
  );
}

function MessageCard({ msg, position, total }: { msg: SolvedMessage; position: number; total: number }) {
  const [open, setOpen] = useState(position <= 2);
  return (
    <div className={`msg-card ${msg.schedulable ? "ok" : "miss"}`}>
      <button type="button" className="msg-head" onClick={() => setOpen((v) => !v)}>
        <span className="pos">#{position}</span>
        <span className="mid">消息 {msg.id}</span>
        <span className={`badge ${msg.schedulable ? "ok" : "miss"}`}>
          {msg.schedulable ? "满足截止期" : "超期"}
        </span>
        <span className="muted small">
          R = {msg.responseExact || msg.schedulable ? msg.response : `>${msg.D}`}
        </span>
        <span className="caret">{open ? "▾" : "▸"}</span>
      </button>
      {open && (
        <div className="msg-body">
          <dl className="facts">
            <div>
              <dt>参数</dt>
              <dd>C={msg.C}, T={msg.T}, D={msg.D}, J={msg.J}</dd>
            </div>
            <div>
              <dt>阻塞 B</dt>
              <dd>
                {msg.blocking}
                <span className="muted small">
                  （所有更低优先级消息传输时长 C 的最大值
                  {msg.position === total ? "；最低优先级无阻塞，取 0" : ""}）
                </span>
              </dd>
            </div>
            <div>
              <dt>高优先级消息</dt>
              <dd>{msg.higherPriorityIds.length ? msg.higherPriorityIds.join(", ") : "无"}</dd>
            </div>
            <div>
              <dt>响应时间 R</dt>
              <dd>
                {msg.response}
                {!msg.responseExact && !msg.schedulable && (
                  <span className="muted small">（精确值未枚举，超期结论确定）</span>
                )}
              </dd>
            </div>
            <div>
              <dt>判据项 min(R, D+1)</dt>
              <dd>{msg.scoreTerm}</dd>
            </div>
          </dl>
          <TraceTable trace={msg.trace} />
        </div>
      )}
    </div>
  );
}

export function ResultView({ result }: ResultProps) {
  const byPosition = [...result.messages].sort((a, b) => a.position - b.position);
  return (
    <section className="result card">
      <div className="result-head">
        <h2>最优固定优先级顺序</h2>
        <div className="objective">
          <span className="stat">
            超期数 <strong>{result.objective.misses}</strong>
          </span>
          <span className="stat">
            Σ min(R, D+1) <strong>{result.objective.score}</strong>
          </span>
        </div>
      </div>

      <ol className="order-chain" aria-label="优先级从高到低">
        {result.order.map((id, i) => (
          <li key={id}>
            <span className="chain-id">{id}</span>
            {i < result.order.length - 1 && <span className="arrow">→</span>}
          </li>
        ))}
      </ol>
      <p className="muted small">
        顺序按优先级自高到低列出；该排列在全部 {result.order.length}! 种可能中依次最小化
        「超期数、Σmin(R,D+1)」，同分时取编号字典序最小者。
      </p>

      <div className="msg-list">
        {byPosition.map((m) => (
          <MessageCard key={m.id} msg={m} position={m.position} total={byPosition.length} />
        ))}
      </div>
    </section>
  );
}
