// indexAdvice.ts
// The logic behind the index advice under the plan of an EXPLAIN (no DOM, no Tauri; tested by code/test/frontend/test_libs.mjs). The engine does the reading
// (`SUGGEST INDEX <statement>`, executor_advice.cpp); this file finds the statement an EXPLAIN is about in the text that was run, reads the answer, and works out
// the temporary index a trial of a suggestion builds and the time a plan says it took.

export interface Suggestion {
  table: string;
  columns: string;
  why: string;
  create: string; // CREATE INDEX idx_t_a ON t (a);
  rowsInTable: number;
  rowsRead: number | null; // how many rows a search through it is expected to read (null: not known)
}
export interface Advice {
  suggestions: Suggestion[];
  already: { table: string; columns: string; detail: string }[];
  notes: string[];
}

// The statements of a text, cut at the `;` that is not inside a string, a quoted name or a comment
export function splitStatements(text: string): string[] {
  const out: string[] = [];
  let cur = "";
  let quote = "";
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quote) {
      cur += c;
      if (c === "\\" && quote !== "`" && i + 1 < text.length) cur += text[++i];
      else if (c === quote) {
        if (text[i + 1] === quote) cur += text[++i];
        else quote = "";
      }
      continue;
    }
    if (c === "'" || c === '"' || c === "`") { quote = c; cur += c; continue; }
    if (c === "-" && text[i + 1] === "-") { while (i < text.length && text[i] !== "\n") i++; continue; }
    if (c === "/" && text[i + 1] === "*") { const end = text.indexOf("*/", i + 2); i = end < 0 ? text.length : end + 1; continue; }
    if (c === ";") { if (cur.trim()) out.push(cur.trim()); cur = ""; continue; }
    cur += c;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

// For each EXPLAIN statement of a text, in order: the statement it explains when that is a SELECT, UPDATE or DELETE ("" for one the advice does not read). The nth plan on the
// screen is the plan of the nth of them.
export function explainTargets(text: string): string[] {
  const out: string[] = [];
  for (const statement of splitStatements(text)) {
    const m = /^EXPLAIN\s+(?:ANALYZE\s+)?([\s\S]+)$/i.exec(statement);
    if (!m) continue;
    out.push(/^(SELECT|UPDATE|DELETE)\b/i.test(m[1].trim()) ? m[1].trim() : "");
  }
  return out;
}

// The answer of SUGGEST INDEX (columns kind, table_name, columns, detail, create_index, rows_in_table, rows_read_with_index)
export function parseAdvice(columns: string[], rows: string[][]): Advice {
  const at = (name: string) => columns.indexOf(name);
  const [kind, table, cols, detail, create, inTable, readWith] = ["kind", "table_name", "columns", "detail", "create_index", "rows_in_table", "rows_read_with_index"].map(at);
  const advice: Advice = { suggestions: [], already: [], notes: [] };
  if (kind < 0) return advice;
  for (const row of rows) {
    const cell = (i: number) => (i >= 0 ? (row[i] ?? "") : "");
    const number = (i: number) => (cell(i) === "" ? null : Number(cell(i)));
    if (cell(kind) === "suggest") {
      advice.suggestions.push({ table: cell(table), columns: cell(cols), why: cell(detail), create: cell(create), rowsInTable: number(inTable) ?? 0, rowsRead: number(readWith) });
    } else if (cell(kind) === "already") {
      advice.already.push({ table: cell(table), columns: cell(cols), detail: cell(detail) });
    } else if (cell(kind) === "note") {
      advice.notes.push(cell(detail));
    }
  }
  return advice;
}

// "Execution time: 0.123 ms" / "1.5 sec" of an EXPLAIN ANALYZE, in milliseconds (null: the plan has no such line)
export function executionMs(lines: string[]): number | null {
  for (const line of lines) {
    const m = /^Execution time\s*:\s*([\d.]+)\s*(ms|sec)/i.exec(line.trim());
    if (m) return Number(m[1]) * (m[2].toLowerCase() === "sec" ? 1000 : 1);
  }
  return null;
}

export function median(values: number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

// The index a trial builds for a suggestion (under a name of its own, dropped again when the measuring is done) and the statement that removes it
export function trialIndex(s: Suggestion, tag: number): { create: string; drop: string; name: string } {
  const name = `__try_${tag}_${s.table}`;
  return { name, create: `CREATE INDEX ${name} ON ${s.table} (${s.columns});`, drop: `DROP INDEX ${name} ON ${s.table};` };
}

// What a trial shows: "0.80 ms → 0.05 ms (16x faster)" -- or that the index changed nothing
export function describeTrial(before: number, after: number): string {
  const fmt = (v: number) => (v >= 100 ? v.toFixed(0) : v >= 1 ? v.toFixed(2) : v.toFixed(3)) + " ms";
  if (!(after > 0)) return `${fmt(before)} → ${fmt(after)}`;
  const speedup = before / after;
  const verdict = speedup >= 1.25 ? `${speedup >= 10 ? speedup.toFixed(0) : speedup.toFixed(1)}x faster` : speedup <= 0.8 ? "slower" : "no change";
  return `${fmt(before)} → ${fmt(after)} (${verdict})`;
}
