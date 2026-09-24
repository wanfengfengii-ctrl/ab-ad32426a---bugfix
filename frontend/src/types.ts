export interface MessageDraft {
  id: string;
  C: string;
  T: string;
  D: string;
  J: string;
}

export interface MessageInput {
  id: number;
  C: number;
  T: number;
  D: number;
  J: number;
}

export interface TraceRow {
  k: number;
  r: number;
  /** 0 = ordinary iteration; >0 = exact collapsed iteration count;
   *  -1 = tail solved analytically after the row budget. */
  skipped: number;
}

export interface SolvedMessage {
  id: number;
  C: number;
  T: number;
  D: number;
  J: number;
  position: number;
  blocking: number;
  higherPriorityIds: number[];
  response: number | null;
  responseExact: boolean;
  schedulable: boolean;
  scoreTerm: number;
  trace: TraceRow[];
}

export interface SolveResponse {
  order: number[];
  objective: { misses: number; score: number };
  messages: SolvedMessage[];
}

export interface ValidationError {
  loc: (string | number)[];
  msg: string;
  type: string;
}

export interface FieldErrors {
  /** key: `${rowIndex}.${field}` or `"messages"` / top-level shape key */
  [key: string]: string[];
}
