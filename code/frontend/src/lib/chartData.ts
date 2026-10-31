// chartData.ts
// What the chart of a result table (the "📊 Chart" button under a result) draws, worked out from the text cells: which column is the label axis, which columns are
// numbers, the points of each series and a scale with round ticks. No DOM here, so that the rules can be tested on their own.

export type ChartKind = "bar" | "line" | "pie";

export interface ChartSpec {
  kind: ChartKind;
  xCol: number;      // the column that names each point (or each bar, each slice)
  yCols: number[];   // the numeric columns that are drawn
}

export interface ChartData {
  labels: string[];
  series: { name: string; values: (number | null)[] }[];
  truncated: boolean; // only the first MAX_POINTS rows are drawn
}

export const MAX_POINTS = 60;
export const MAX_SERIES = 4;
export const MAX_SLICES = 12;

const NUMBER = /^-?\d+(\.\d+)?([eE][-+]?\d+)?$/;

export function toNumber(cell: string): number | null {
  const t = cell.trim();
  return NUMBER.test(t) ? Number(t) : null;
}

// a column of numbers: every cell that is not NULL is one, and at least one cell is not NULL
export function isNumericColumn(rows: string[][], ci: number): boolean {
  let seen = false;
  for (const row of rows) {
    const cell = row[ci] ?? "NULL";
    if (cell === "NULL") continue;
    if (toNumber(cell) === null) return false;
    seen = true;
  }
  return seen;
}

const LOOKS_LIKE_DATE = /^\d{4}-\d{2}(-\d{2})?/;

// the biggest absolute value of a column of numbers (what decides whether two columns can share a scale)
function magnitude(rows: string[][], ci: number): number {
  let m = 0;
  for (const row of rows) {
    const v = toNumber(row[ci] ?? "NULL");
    if (v !== null) m = Math.max(m, Math.abs(v));
  }
  return m;
}

// the chart that is drawn first: the first column that is not a number names the points (a date column makes it a line); the first number column is a series, and so is
// every other one whose biggest value is within a factor of 10 of it (a column of hundreds beside a column of millions would be a line along the bottom: the person ticks
// it, and sees it, when they want it); at most MAX_SERIES. A table of numbers only is drawn by its first column. null: there is no number to draw
export function defaultSpec(columns: string[], rows: string[][]): ChartSpec | null {
  const numeric = columns.map((_, i) => isNumericColumn(rows, i));
  if (!numeric.some(Boolean) || rows.length === 0) return null;
  let xCol = numeric.findIndex(n => !n);
  if (xCol < 0) xCol = 0;
  const candidates = numeric.map((n, i) => (n && i !== xCol ? i : -1)).filter(i => i >= 0);
  const lead = candidates.length ? magnitude(rows, candidates[0]) : 0;
  const yCols = candidates.filter(i => {
    const m = magnitude(rows, i);
    return i === candidates[0] || (lead > 0 && m > 0 && m / lead <= 10 && lead / m <= 10) || (lead === 0 && m === 0);
  }).slice(0, MAX_SERIES);
  if (yCols.length === 0) return { kind: "bar", xCol: -1, yCols: [numeric.findIndex(Boolean)] }; // (a single column of numbers: the row number names the points)
  const dated = rows.slice(0, 5).every(r => LOOKS_LIKE_DATE.test(r[xCol] ?? ""));
  return { kind: dated && rows.length > 2 ? "line" : "bar", xCol, yCols };
}

export function prepareChart(columns: string[], rows: string[][], spec: ChartSpec): ChartData {
  const limit = spec.kind === "pie" ? MAX_SLICES : MAX_POINTS;
  const used = rows.slice(0, limit);
  const yCols = spec.kind === "pie" ? spec.yCols.slice(0, 1) : spec.yCols;
  return {
    labels: used.map((r, i) => (spec.xCol < 0 ? String(i + 1) : r[spec.xCol] ?? "")),
    series: yCols.map(ci => ({ name: columns[ci], values: used.map(r => toNumber(r[ci] ?? "NULL")) })),
    truncated: rows.length > limit,
  };
}

// a scale with round ticks that holds [lo, hi]; a bar chart includes zero (a bar that does not start at zero lies about its size)
export function niceScale(lo: number, hi: number, includeZero: boolean, wanted = 5): { min: number; max: number; step: number; ticks: number[] } {
  if (includeZero) {
    lo = Math.min(lo, 0);
    hi = Math.max(hi, 0);
  }
  if (!(hi > lo)) {
    const pad = Math.abs(hi) > 0 ? Math.abs(hi) * 0.1 : 1;
    lo -= pad;
    hi += pad;
  }
  const rough = (hi - lo) / wanted;
  const power = Math.pow(10, Math.floor(Math.log10(rough)));
  const fraction = rough / power;
  const step = (fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10) * power;
  const min = Math.floor(lo / step) * step;
  const max = Math.ceil(hi / step) * step;
  const ticks: number[] = [];
  for (let v = min; v <= max + step / 2; v += step) ticks.push(Math.round(v / step) * step);
  return { min, max, step, ticks };
}

export function formatNumber(v: number): string {
  if (Math.abs(v) >= 1e15 || (v !== 0 && Math.abs(v) < 1e-6)) return v.toExponential(3);
  return Number(v.toPrecision(10)).toLocaleString("en-US", { maximumFractionDigits: 6 });
}
