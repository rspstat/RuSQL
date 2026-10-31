// resultExport.ts
// A result table as JSON (the "⬇ JSON" button next to "⬇ CSV"). A result cell is text: the engine writes every value as text and NULL as the word NULL. The JSON gives
// back what a reader expects of it: NULL is null; a number is a number when a JSON number loses nothing (-12, 3.5, 0.25 read back as the same text; 10.50 and
// 12345678901234567890 do not, so they stay text); everything else stays text ('007', '2024-01-02'). Each row is an object under the column names, and a name that repeats
// gets _2, _3 ... so that no value is lost.

const PLAIN_NUMBER = /^-?(0|[1-9]\d*)(\.\d+)?$/;

export function jsonValue(cell: string): string | number | null {
  if (cell === "NULL") return null;
  if (PLAIN_NUMBER.test(cell) && String(Number(cell)) === cell) return Number(cell);
  return cell;
}

export function uniqueNames(columns: string[]): string[] {
  const seen = new Map<string, number>();
  return columns.map(name => {
    const n = (seen.get(name) ?? 0) + 1;
    seen.set(name, n);
    return n === 1 ? name : `${name}_${n}`;
  });
}

export function rowsToJson(columns: string[], rows: string[][]): string {
  const names = uniqueNames(columns);
  const objects = rows.map(row => {
    const o: Record<string, string | number | null> = {};
    names.forEach((name, i) => { o[name] = jsonValue(row[i] ?? "NULL"); });
    return o;
  });
  return JSON.stringify(objects, null, 2);
}
