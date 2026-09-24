import { useState } from "react";
import { Editor } from "./Editor";
import { ResultView } from "./ResultView";
import { solveMessages, invalidateRequests } from "./api";
import type { FieldErrors, MessageDraft, SolveResponse } from "./types";

function initialDrafts(): MessageDraft[] {
  return [
    { id: "1", C: "1", T: "8", D: "8", J: "0" },
    { id: "2", C: "2", T: "6", D: "6", J: "0" },
    { id: "3", C: "3", T: "10", D: "10", J: "0" },
  ];
}

export default function App() {
  const [drafts, setDrafts] = useState<MessageDraft[]>(initialDrafts);
  // The result is version-bound to the exact drafts it was computed from.
  const [result, setResult] = useState<SolveResponse | null>(null);
  const [fieldErrors, setFieldErrors] = useState<FieldErrors>({});
  const [busy, setBusy] = useState(false);
  const [fatal, setFatal] = useState<string | null>(null);

  function markDirty() {
    // Editing immediately takes down the old result and invalidates the
    // in-flight request, so a late HTTP response can never overwrite the
    // new draft (the API layer also drops stale responses by sequence).
    invalidateRequests();
    setResult(null);
    setBusy(false);
    setFieldErrors({});
    setFatal(null);
  }

  async function onSolve() {
    invalidateRequests();
    setBusy(true);
    setFatal(null);
    setFieldErrors({});
    const outcome = await solveMessages(drafts);
    // Superseded by a newer edit / solve: leave all state (incl. busy)
    // to the current request; a late response never lands here.
    if (outcome.message === "aborted" || outcome.message === "stale") return;
    setBusy(false);
    if (outcome.status === "ok" && outcome.data) {
      setResult(outcome.data);
      return;
    }
    if (outcome.status === "invalid") {
      setFieldErrors(outcome.fieldErrors ?? {});
      return;
    }
    setFatal(outcome.message ?? "求解失败");
  }

  return (
    <div className="page">
      <header className="topbar">
        <h1>卫星非抢占遥测总线 · 固定优先级综合台</h1>
        <p className="subtitle">
          对 2–18 条消息精确求解全局最优固定优先级（最小化超期数，其次
          Σ&nbsp;min(R,D+1)，同分取编号字典序最小）。
        </p>
      </header>

      <Editor
        drafts={drafts}
        setDrafts={setDrafts}
        fieldErrors={fieldErrors}
        onDirty={markDirty}
        onSolve={onSolve}
        busy={busy}
      />

      {fatal && <div className="error-banner">服务错误：{fatal}</div>}

      {result ? (
        <ResultView result={result} />
      ) : (
        <section className="card placeholder">
          <h2>{busy ? "正在求解…" : "尚未求解"}</h2>
          <p className="muted">
            {busy
              ? "正在遍历全部优先级排列的子集 DP 状态，请稍候。"
              : "编辑或导入消息参数后点击「求解最优优先级」。每条消息的阻塞值、完整迭代轨迹与超期结论将在此展示；继续编辑会立即撤下当前结果。"}
          </p>
        </section>
      )}

      <footer className="footnote muted small">
        判据：r₀=J+C+B，r(k+1)=J+C+B+Σ<sub>h更高</sub>⌈(r(k)+J<sub>h</sub>)/T<sub>h</sub>⌉·C<sub>h</sub>；
        定点 R≤D 为可调度，首次 r&gt;D 为超期。B 为全部更低优先级消息 C 的最大值，
        最低优先级消息取 0。
      </footer>
    </div>
  );
}
