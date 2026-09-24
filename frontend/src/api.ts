import type {
  FieldErrors,
  MessageDraft,
  MessageInput,
  SolveResponse,
  ValidationError,
} from "./types";

/**
 * Stale-response guard.  Each call to solve() aborts the in-flight request
 * and advances a sequence counter; a late HTTP response whose sequence is
 * no longer current can never overwrite a newer draft or result.
 */
let activeSeq = 0;
let activeController: AbortController | null = null;

export interface SolveOutcome {
  status: "ok" | "invalid" | "error";
  data?: SolveResponse;
  fieldErrors?: FieldErrors;
  message?: string;
}

function draftToInput(draft: MessageDraft): Record<string, number | string> {
  const num = (s: string): number | string =>
    /^[+-]?\d+$/.test(s.trim()) ? Number(s.trim()) : s;
  return {
    id: num(draft.id),
    C: num(draft.C),
    T: num(draft.T),
    D: num(draft.D),
    J: num(draft.J),
  };
}

/** Collapse FastAPI 422 detail entries into per-row/per-field messages. */
function mapValidationErrors(detail: ValidationError[]): FieldErrors {
  const out: FieldErrors = {};
  for (const err of detail) {
    const loc = err.loc ?? [];
    // Expected shapes:
    //   ["body","messages", i, field]  -> `${i}.${field}`
    //   ["body","messages"]            -> "messages"
    //   other                          -> loc joined by "."
    let key: string;
    if (loc[0] === "body" && loc[1] === "messages" && loc.length >= 4) {
      key = `${loc[2]}.${String(loc[3])}`;
    } else if (loc[0] === "body" && loc[1] === "messages") {
      key = "messages";
    } else {
      key = loc.slice(1).map(String).join(".") || "body";
    }
    (out[key] ??= []).push(err.msg);
  }
  return out;
}

export async function solveMessages(drafts: MessageDraft[]): Promise<SolveOutcome> {
  activeSeq += 1;
  const seq = activeSeq;
  activeController?.abort();
  const controller = new AbortController();
  activeController = controller;

  const messages: MessageInput[] = drafts.map((d) =>
    draftToInput(d) as unknown as MessageInput,
  );

  let response: Response;
  try {
    response = await fetch("/api/solve", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ messages }),
      signal: controller.signal,
    });
  } catch (err) {
    if ((err as Error).name === "AbortError") {
      return { status: "error", message: "aborted" };
    }
    return { status: "error", message: (err as Error).message };
  }

  // A newer solve() call supersedes this response: never deliver it.
  if (seq !== activeSeq) {
    return { status: "error", message: "stale" };
  }

  if (response.ok) {
    const data = (await response.json()) as SolveResponse;
    return { status: "ok", data };
  }
  if (response.status === 422) {
    const body = (await response.json()) as { detail: ValidationError[] };
    return { status: "invalid", fieldErrors: mapValidationErrors(body.detail ?? []) };
  }
  let text = "";
  try {
    text = await response.text();
  } catch {
    /* ignore */
  }
  return { status: "error", message: `HTTP ${response.status}: ${text}` };
}

/** Invalidate any pending request (e.g. while the user is editing). */
export function invalidateRequests(): void {
  activeSeq += 1;
  activeController?.abort();
  activeController = null;
}
