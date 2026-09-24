import { useRef, useState } from "react";
import type { MessageDraft } from "./types";
import { invalidateRequests } from "./api";

const MIN_ROWS = 2;
const MAX_ROWS = 18;

let nextClientId = 1000;
function emptyRow(): MessageDraft {
  return { id: String(nextClientId++), C: "", T: "", D: "", J: "" };
}

interface EditorProps {
  drafts: MessageDraft[];
  setDrafts: (d: MessageDraft[]) => void;
  fieldErrors: Record<string, string[]>;
  onDirty: () => void;
  onSolve: () => void;
  busy: boolean;
}

const FIELDS: { key: keyof MessageDraft; label: string; hint: string }[] = [
  { key: "id", label: "编号 id", hint: "1..10⁹，唯一" },
  { key: "C", label: "传输时长 C", hint: "1..10⁹" },
  { key: "T", label: "周期 T", hint: "C ≤ D ≤ T" },
  { key: "D", label: "截止期 D", hint: "≤ T" },
  { key: "J", label: "抖动 J", hint: "0 ≤ J ≤ D−C" },
];

export function Editor({
  drafts,
  setDrafts,
  fieldErrors,
  onDirty,
  onSolve,
  busy,
}: EditorProps) {
  const [importOpen, setImportOpen] = useState(false);
  const [importText, setImportText] = useState("");
  const [importError, setImportError] = useState<string | null>(null);
  const fileRef = useRef<HTMLInputElement>(null);

  const canAdd = drafts.length < MAX_ROWS;
  const canRemove = drafts.length > MIN_ROWS;

  function update(row: number, key: keyof MessageDraft, value: string) {
    invalidateRequests();
    const copy = drafts.map((d, i) => (i === row ? { ...d, [key]: value } : d));
    setDrafts(copy);
    onDirty();
  }

  function addRow() {
    invalidateRequests();
    if (drafts.length >= MAX_ROWS) return;
    setDrafts([...drafts, emptyRow()]);
    onDirty();
  }

  function removeRow(row: number) {
    invalidateRequests();
    if (drafts.length <= MIN_ROWS) return;
    setDrafts(drafts.filter((_, i) => i !== row));
    onDirty();
  }

  function parseImport(text: string): MessageDraft[] | string {
    const trimmed = text.trim();
    if (!trimmed) return "导入内容为空";
    // JSON array form
    if (trimmed.startsWith("[")) {
      let arr: unknown;
      try {
        arr = JSON.parse(trimmed);
      } catch (e) {
        return `JSON 解析失败: ${(e as Error).message}`;
      }
      if (!Array.isArray(arr)) return "JSON 顶层必须是数组";
      if (arr.length < MIN_ROWS || arr.length > MAX_ROWS)
        return `消息数量须在 ${MIN_ROWS}–${MAX_ROWS} 之间`;
      return arr.map((raw) => {
        const o = raw as Record<string, unknown>;
        return {
          id: String(o.id ?? ""),
          C: String(o.C ?? ""),
          T: String(o.T ?? ""),
          D: String(o.D ?? ""),
          J: String(o.J ?? ""),
        };
      });
    }
    // Whitespace-separated form: id C T D J per line.
    const rows = trimmed.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
    if (rows.length < MIN_ROWS || rows.length > MAX_ROWS)
      return `消息数量须在 ${MIN_ROWS}–${MAX_ROWS} 之间`;
    const parsed: MessageDraft[] = [];
    for (const line of rows) {
      const parts = line.split(/[\s,;]+/);
      if (parts.length !== 5)
        return `行 "${line}" 必须恰好含 5 列: id C T D J`;
      parsed.push({ id: parts[0], C: parts[1], T: parts[2], D: parts[3], J: parts[4] });
    }
    return parsed;
  }

  function doImport() {
    const result = parseImport(importText);
    if (typeof result === "string") {
      setImportError(result);
      return;
    }
    invalidateRequests();
    setDrafts(result);
    setImportOpen(false);
    setImportText("");
    setImportError(null);
    onDirty();
  }

  function onFile(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      setImportText(String(reader.result ?? ""));
      setImportError(null);
    };
    reader.readAsText(file);
  }

  const rowCountError = fieldErrors["messages"];

  return (
    <section className="editor card">
      <div className="editor-head">
        <h2>消息集编辑（{drafts.length}/{MAX_ROWS}）</h2>
        <div className="btn-row">
          <button type="button" onClick={() => setImportOpen((v) => !v)}>
            导入
          </button>
          <button type="button" onClick={addRow} disabled={!canAdd}>
            + 新增一条
          </button>
          <button type="button" className="primary" onClick={onSolve} disabled={busy}>
            {busy ? "求解中…" : "求解最优优先级"}
          </button>
        </div>
      </div>

      {rowCountError && (
        <div className="error-banner">{rowCountError.join("；")}</div>
      )}

      <div className="table-wrap">
        <table className="grid">
          <thead>
            <tr>
              <th>#</th>
              {FIELDS.map((f) => (
                <th key={f.key} title={f.hint}>
                  {f.label}
                </th>
              ))}
              <th aria-label="操作" />
            </tr>
          </thead>
          <tbody>
            {drafts.map((row, ri) => (
              <tr key={row.id}>
                <td className="rowidx">{ri + 1}</td>
                {FIELDS.map((f) => {
                  const errs = fieldErrors[`${ri}.${f.key}`];
                  return (
                    <td key={f.key}>
                      <input
                        className={errs ? "invalid" : ""}
                        value={row[f.key]}
                        onChange={(e) => update(ri, f.key, e.target.value)}
                        inputMode="numeric"
                        aria-invalid={!!errs}
                        title={errs?.join("；")}
                      />
                    </td>
                  );
                })}
                <td>
                  <button
                    type="button"
                    className="icon-btn"
                    onClick={() => removeRow(ri)}
                    disabled={!canRemove}
                    title="删除该行"
                  >
                    ✕
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {importOpen && (
        <div className="import-panel">
          <p className="muted">
            粘贴 JSON 数组（<code>[{"{"}id,C,T,D,J{"}"}]</code>…），或每行一条、以空格/逗号分隔的
            <code> id C T D J</code>。也可选择文件：
          </p>
          <input ref={fileRef} type="file" accept=".json,.txt,.csv" onChange={onFile} />
          <textarea
            rows={8}
            value={importText}
            placeholder={'[\n  {"id":1,"C":1,"T":8,"D":8,"J":0}\n]'}
            onChange={(e) => setImportText(e.target.value)}
          />
          {importError && <div className="error-banner">{importError}</div>}
          <div className="btn-row">
            <button type="button" onClick={doImport} className="primary">
              应用导入
            </button>
            <button
              type="button"
              onClick={() => {
                setImportOpen(false);
                setImportError(null);
              }}
            >
              取消
            </button>
          </div>
        </div>
      )}
    </section>
  );
}

export { emptyRow };
