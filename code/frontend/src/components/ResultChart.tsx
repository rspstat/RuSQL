// ResultChart.tsx
// "📊 Chart" under a result table: the columns that are numbers drawn as bars, a line or a pie, with the first column that is not a number naming the points. The button
// and the panel are children of the result's info row (the panel takes a row of its own), so the table below keeps its place. What is drawn is worked out in lib/chartData.ts.

import { useEffect, useMemo, useRef, useState } from "react";
import type { ReactElement } from "react";
import { MAX_SERIES, defaultSpec, formatNumber, isNumericColumn, niceScale, prepareChart, type ChartKind, type ChartSpec } from "../lib/chartData";
import "./ResultChart.css";

const COLORS = ["#4ec9b0", "#569cd6", "#d7ba7d", "#c586c0", "#ce9178", "#9cdcfe", "#b5cea8", "#d16969", "#dcdcaa", "#4fc1ff", "#c8c8c8", "#e5c07b"];
const HEIGHT = 270;
const MARGIN = { top: 14, right: 14, bottom: 58, left: 62 };

interface Props { columns: string[]; rows: string[][] }

function clip(text: string, n: number): string {
  return text.length > n ? text.slice(0, n - 1) + "…" : text;
}

export default function ResultChart({ columns, rows }: Props) {
  const initial = useMemo(() => defaultSpec(columns, rows), [columns, rows]);
  const [open, setOpen] = useState(false);
  const [spec, setSpec] = useState<ChartSpec | null>(initial);
  const [width, setWidth] = useState(640);
  const holder = useRef<HTMLDivElement | null>(null);

  useEffect(() => { setSpec(initial); }, [initial]); // (a new result is drawn from its own default)

  useEffect(() => {
    const el = holder.current;
    if (!open || !el || typeof ResizeObserver === "undefined") return;
    const watch = new ResizeObserver(() => setWidth(Math.max(360, Math.min(960, Math.floor(el.clientWidth)))));
    watch.observe(el);
    setWidth(Math.max(360, Math.min(960, Math.floor(el.clientWidth || 640))));
    return () => watch.disconnect();
  }, [open]);

  const numeric = useMemo(() => columns.map((_, i) => isNumericColumn(rows, i)), [columns, rows]);
  if (!initial || rows.length < 2) return null; // (nothing to draw: no number, or a single row)

  const current = spec ?? initial;
  const data = prepareChart(columns, rows, current);
  const set = (patch: Partial<ChartSpec>) => setSpec({ ...current, ...patch });
  const toggleY = (ci: number) => {
    const has = current.yCols.includes(ci);
    if (has && current.yCols.length === 1) return;
    if (!has && current.yCols.length >= MAX_SERIES) return;
    set({ yCols: has ? current.yCols.filter(c => c !== ci) : [...current.yCols, ci] });
  };

  const plotW = width - MARGIN.left - MARGIN.right;
  const plotH = HEIGHT - MARGIN.top - MARGIN.bottom;
  const all = data.series.flatMap(s => s.values).filter((v): v is number => v !== null);
  const scale = niceScale(Math.min(...all, 0), Math.max(...all, 0), current.kind === "bar");
  const y = (v: number) => MARGIN.top + plotH - ((v - scale.min) / (scale.max - scale.min)) * plotH;
  const n = data.labels.length;
  const band = plotW / Math.max(n, 1);
  const labelEvery = Math.max(1, Math.ceil(n / Math.max(1, Math.floor(plotW / 54))));

  const axes = (
    <>
      {scale.ticks.map(t => (
        <g key={t}>
          <line x1={MARGIN.left} x2={MARGIN.left + plotW} y1={y(t)} y2={y(t)} className={t === 0 ? "rc-zero" : "rc-grid"} />
          <text x={MARGIN.left - 6} y={y(t) + 4} textAnchor="end" className="rc-tick">{formatNumber(t)}</text>
        </g>
      ))}
      {data.labels.map((label, i) => (i % labelEvery === 0 ? (
        <text key={i} transform={`translate(${MARGIN.left + band * (i + 0.5)} ${MARGIN.top + plotH + 12}) rotate(${band < 54 || label.length > 8 ? -32 : 0})`} textAnchor={band < 54 || label.length > 8 ? "end" : "middle"} className="rc-tick">
          {clip(label, 16)}
        </text>
      ) : null))}
    </>
  );

  let pie: ReactElement | null = null;
  if (current.kind === "pie") {
    const values = (data.series[0]?.values ?? []).map(v => (v !== null && v > 0 ? v : 0));
    const total = values.reduce((a, b) => a + b, 0);
    const R = Math.min(plotH, plotW * 0.45) / 2 + 20;
    const cx = MARGIN.left + R + 8, cy = MARGIN.top + plotH / 2 + 12;
    let angle = -Math.PI / 2;
    pie = (
      <>
        {total > 0 && values.map((v, i) => {
          if (v <= 0) return null;
          const a0 = angle;
          angle += (v / total) * Math.PI * 2;
          const a1 = angle;
          const large = a1 - a0 > Math.PI ? 1 : 0;
          const d = v === total
            ? `M ${cx} ${cy - R} A ${R} ${R} 0 1 1 ${cx - 0.01} ${cy - R} Z`
            : `M ${cx} ${cy} L ${cx + R * Math.cos(a0)} ${cy + R * Math.sin(a0)} A ${R} ${R} 0 ${large} 1 ${cx + R * Math.cos(a1)} ${cy + R * Math.sin(a1)} Z`;
          return <path key={i} d={d} fill={COLORS[i % COLORS.length]} className="rc-slice"><title>{`${data.labels[i]}: ${formatNumber(v)} (${((v / total) * 100).toFixed(1)}%)`}</title></path>;
        })}
        {values.map((v, i) => (
          <g key={i} transform={`translate(${cx + R + 24} ${MARGIN.top + 8 + i * 17})`}>
            <rect width="10" height="10" y="-9" fill={COLORS[i % COLORS.length]} />
            <text x="15" className="rc-legend">{clip(data.labels[i], 22)} · {formatNumber(v)}{total > 0 ? ` (${((v / total) * 100).toFixed(1)}%)` : ""}</text>
          </g>
        ))}
      </>
    );
  }

  return (
    <>
      <button className={"csv-btn" + (open ? " rc-on" : "")} title="Draw the numbers of this result as a chart" onClick={() => setOpen(o => !o)}>📊 Chart</button>
      {open && (
        <div className="rc-panel" ref={holder}>
          <div className="rc-controls">
            <label>Type
              <select value={current.kind} onChange={e => set({ kind: e.target.value as ChartKind })}>
                <option value="bar">Bar</option>
                <option value="line">Line</option>
                <option value="pie">Pie</option>
              </select>
            </label>
            <label>Labels
              <select value={current.xCol} onChange={e => set({ xCol: Number(e.target.value) })}>
                <option value={-1}>(row number)</option>
                {columns.map((c, i) => <option key={i} value={i}>{c}</option>)}
              </select>
            </label>
            <span className="rc-values">Values
              {columns.map((c, i) => (numeric[i] && i !== current.xCol ? (
                <label key={i} className="rc-check"><input type="checkbox" checked={current.yCols.includes(i)} onChange={() => toggleY(i)} />{c}</label>
              ) : null))}
            </span>
          </div>
          <svg viewBox={`0 0 ${width} ${HEIGHT}`} width="100%" height={HEIGHT} className="rc-svg" role="img" aria-label="Chart of the result">
            {current.kind !== "pie" && axes}
            {current.kind === "bar" && data.series.map((s, si) => s.values.map((v, i) => {
              if (v === null) return null;
              const group = band * 0.78;
              const w = group / data.series.length;
              const x = MARGIN.left + band * i + (band - group) / 2 + w * si;
              const top = Math.min(y(v), y(0)), h = Math.max(1, Math.abs(y(v) - y(0)));
              return <rect key={`${si}-${i}`} x={x} y={top} width={Math.max(1, w - 1)} height={h} fill={COLORS[si % COLORS.length]} className="rc-bar"><title>{`${data.labels[i]} · ${s.name}: ${formatNumber(v)}`}</title></rect>;
            }))}
            {current.kind === "line" && data.series.map((s, si) => {
              const points = s.values.map((v, i) => (v === null ? null : [MARGIN.left + band * (i + 0.5), y(v)] as const));
              const segments: (readonly [number, number])[][] = [[]];
              points.forEach(p => { if (p) segments[segments.length - 1].push(p); else if (segments[segments.length - 1].length) segments.push([]); });
              return (
                <g key={si} stroke={COLORS[si % COLORS.length]} fill={COLORS[si % COLORS.length]}>
                  {segments.filter(seg => seg.length > 1).map((seg, k) => <polyline key={k} points={seg.map(p => p.join(",")).join(" ")} fill="none" strokeWidth="2" />)}
                  {points.map((p, i) => (p ? <circle key={i} cx={p[0]} cy={p[1]} r={n > 40 ? 2 : 3.5} className="rc-dot"><title>{`${data.labels[i]} · ${s.name}: ${formatNumber(s.values[i] as number)}`}</title></circle> : null))}
                </g>
              );
            })}
            {pie}
            {current.kind !== "pie" && data.series.length > 1 && data.series.map((s, si) => (
              <g key={si} transform={`translate(${MARGIN.left + si * 130} ${HEIGHT - 8})`}>
                <rect width="10" height="10" y="-9" fill={COLORS[si % COLORS.length]} />
                <text x="15" className="rc-legend">{clip(s.name, 16)}</text>
              </g>
            ))}
          </svg>
          {data.truncated && <div className="rc-note">The first {data.labels.length} rows are drawn ({rows.length} in the result).</div>}
        </div>
      )}
    </>
  );
}
