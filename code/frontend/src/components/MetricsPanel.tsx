// MetricsPanel.tsx
// The trend of what the server counts, in the Server Manager: statements per second (all, reads, writes), sessions, row-lock waits, the size of the redo log and the buffer pool.
// The numbers are the engine's own counters (SHOW GLOBAL STATUS, through the Tauri command get_status_counters), read once a second; a rate is the change between two readings
// divided by the time between them. The readings are kept while the app runs, so closing the panel does not lose the trend.

import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./MetricsPanel.css";

interface Table { columns: string[]; rows: string[][] }
interface Sample { t: number; v: Record<string, number> }

const POLL_MS = 1000;
const WINDOW_MS = 2 * 60 * 1000;   // the span the charts show
const KEEP_MS = 10 * 60 * 1000;    // the readings kept
const GAP_MS = 5000;               // no reading for this long: the line breaks instead of joining the two sides

let history: Sample[] = [];

function readCounters(t: Table): Record<string, number> | null {
  const name = t.columns.findIndex(c => c.toLowerCase() === "variable_name");
  const value = t.columns.findIndex(c => c.toLowerCase() === "value");
  if (name < 0 || value < 0 || t.rows.length === 0) return null;
  const v: Record<string, number> = {};
  for (const r of t.rows) {
    const n = Number(r[value]);
    if (!Number.isNaN(n)) v[r[name]] = n;
  }
  return v;
}

type Point = [number, number];

// the readings of one counter as a line of points: a rate (change per second) or the value itself
function line(samples: Sample[], names: string | string[], mode: "rate" | "value"): Point[] {
  const list = Array.isArray(names) ? names : [names];
  const total = (s: Sample) => list.reduce((sum, n) => sum + (s.v[n] ?? 0), 0);
  const points: Point[] = [];
  if (mode === "value") {
    for (const s of samples) points.push([s.t, total(s)]);
    return points;
  }
  for (let i = 1; i < samples.length; i++) {
    const a = samples[i - 1], b = samples[i];
    const seconds = (b.t - a.t) / 1000;
    if (seconds <= 0 || b.t - a.t > GAP_MS) continue;
    // (every reading is itself a statement, SHOW GLOBAL STATUS, and Questions counts it: it is not what the server was asked by anyone else)
    const own = list.includes("Questions") ? 1 : 0;
    points.push([b.t, Math.max(0, total(b) - total(a) - own) / seconds]);
  }
  return points;
}

function niceMax(value: number): number {
  if (value <= 1) return 1;
  const pow = Math.pow(10, Math.floor(Math.log10(value)));
  for (const step of [1, 2, 5, 10]) if (value <= step * pow) return step * pow;
  return 10 * pow;
}

function fmt(n: number | undefined, digits = 1): string {
  if (n === undefined || Number.isNaN(n)) return "–";
  if (Math.abs(n) >= 1e6) return (n / 1e6).toFixed(1) + "M";
  if (Math.abs(n) >= 1e4) return (n / 1e3).toFixed(1) + "k";
  return Number.isInteger(n) ? String(n) : n.toFixed(digits);
}

function fmtBytes(n: number | undefined): string {
  if (n === undefined) return "–";
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + " MB";
  if (n >= 1 << 10) return (n / (1 << 10)).toFixed(1) + " KB";
  return n + " B";
}

function fmtDuration(secs: number): string {
  if (secs < 60) return `${secs}s`;
  if (secs < 3600) return `${Math.floor(secs / 60)}m ${secs % 60}s`;
  return `${Math.floor(secs / 3600)}h ${Math.floor((secs % 3600) / 60)}m`;
}

interface Series { label: string; color: string; pts: Point[]; format?: (n: number) => string }

function Chart({ title, series, now, note }: { title: string; series: Series[]; now: number; note?: string }) {
  const W = 330, H = 76, left = 34, right = 6, top = 6, bottom = 14;
  const t0 = now - WINDOW_MS;
  const visible = series.map(s => ({ ...s, pts: s.pts.filter(p => p[0] >= t0) }));
  const max = niceMax(Math.max(0, ...visible.flatMap(s => s.pts.map(p => p[1]))));
  const x = (t: number) => left + ((t - t0) / WINDOW_MS) * (W - left - right);
  const y = (v: number) => top + (1 - v / max) * (H - top - bottom);
  // the points of a series in runs: a gap in the readings ends a run
  const runs = (pts: Point[]) => {
    const out: Point[][] = [];
    for (const p of pts) {
      const last = out[out.length - 1];
      if (last && p[0] - last[last.length - 1][0] <= GAP_MS * 1.5) last.push(p); else out.push([p]);
    }
    return out;
  };
  return (
    <div className="mt-chart">
      <div className="mt-chart-head">
        <span className="mt-chart-title">{title}</span>
        <span className="mt-chart-values">
          {visible.map(s => {
            const last = s.pts.length ? s.pts[s.pts.length - 1][1] : undefined;
            return <span key={s.label} className="mt-value"><i style={{ background: s.color }} />{s.label} <b>{last === undefined ? "–" : (s.format ?? fmt)(last)}</b></span>;
          })}
        </span>
      </div>
      <svg viewBox={`0 0 ${W} ${H}`} className="mt-svg" role="img" aria-label={title}>
        {[0, 0.5, 1].map(f => <line key={f} x1={left} x2={W - right} y1={y(max * f)} y2={y(max * f)} className="mt-grid" />)}
        <text x={left - 4} y={y(max) + 3} className="mt-axis" textAnchor="end">{(visible[0]?.format ?? fmt)(max)}</text>
        <text x={left - 4} y={y(0) + 3} className="mt-axis" textAnchor="end">0</text>
        <text x={left} y={H - 2} className="mt-axis">−2 min</text>
        <text x={W - right} y={H - 2} className="mt-axis" textAnchor="end">now</text>
        {visible.map(s => runs(s.pts).map((run, i) => run.length === 1
          ? <circle key={s.label + i} cx={x(run[0][0])} cy={y(run[0][1])} r={1.6} fill={s.color} />
          : <polyline key={s.label + i} points={run.map(p => `${x(p[0]).toFixed(1)},${y(p[1]).toFixed(1)}`).join(" ")} fill="none" stroke={s.color} strokeWidth={1.6} strokeLinejoin="round" />))}
      </svg>
      {note && <div className="mt-note">{note}</div>}
    </div>
  );
}

export default function MetricsPanel() {
  const [, setTick] = useState(0);

  useEffect(() => {
    let alive = true;
    const poll = async () => {
      try {
        const v = readCounters(await invoke<Table>("get_status_counters"));
        if (!v) return;
        const now = Date.now();
        const last = history[history.length - 1];
        // (the engine was restarted, Stop in the Server Manager does it: its counters began again, so the old readings are another run)
        if (last && v.Uptime < last.v.Uptime) history = [];
        history = [...history.filter(s => now - s.t < KEEP_MS), { t: now, v }];
        if (alive) setTick(n => n + 1);
      } catch { /* the server is not reachable: keep what is shown */ }
    };
    poll();
    const timer = setInterval(poll, POLL_MS);
    return () => { alive = false; clearInterval(timer); };
  }, []);

  const samples = history;
  const now = samples.length ? samples[samples.length - 1].t : Date.now();
  const last = samples[samples.length - 1]?.v;
  if (!last) return <div className="srv-slide-text">Waiting for the first reading…</div>;

  const used = last.Innodb_buffer_pool_pages_data ?? 0, total = last.Innodb_buffer_pool_pages_total ?? 0;
  const requests = last.Innodb_buffer_pool_read_requests ?? 0, reads = last.Innodb_buffer_pool_reads ?? 0;
  return (
    <div className="mt-panel">
      <div className="mt-tiles">
        <div><span>Uptime</span><b>{fmtDuration(last.Uptime ?? 0)}</b></div>
        <div><span>Statements</span><b>{fmt(last.Questions)}</b></div>
        <div><span>Connections</span><b>{fmt(last.Connections)}</b></div>
        <div><span>Failed logins</span><b>{fmt(last.Aborted_connects)}</b></div>
      </div>

      <Chart title="Statements per second" now={now} series={[
        { label: "all", color: "#d4d4d4", pts: line(samples, "Questions", "rate") },
        { label: "reads", color: "#569cd6", pts: line(samples, ["Com_select"], "rate") },
        { label: "writes", color: "#ce9178", pts: line(samples, ["Com_insert", "Com_update", "Com_delete", "Com_replace"], "rate") },
      ]} note="The panel's own reading (one statement a second) is left out." />

      <Chart title="Sessions" now={now} series={[
        { label: "connected", color: "#4ec9b0", pts: line(samples, "Threads_connected", "value") },
        { label: "running", color: "#dcdcaa", pts: line(samples, "Threads_running", "value") },
      ]} note={`Most at once since the start: ${fmt(last.Max_used_connections)}`} />

      <Chart title="Row locks" now={now} series={[
        { label: "waiting now", color: "#e74c3c", pts: line(samples, "Innodb_row_lock_current_waits", "value") },
        { label: "waits/s", color: "#ce9178", pts: line(samples, "Innodb_row_lock_waits", "rate") },
      ]} note={`Waits since the start: ${fmt(last.Innodb_row_lock_waits)} · deadlocks: ${fmt(last.Innodb_deadlocks)}`} />

      <Chart title="Redo log" now={now} series={[
        { label: "size", color: "#4ec9b0", pts: line(samples, "Innodb_redo_log_bytes", "value"), format: n => fmtBytes(n) },
      ]} note="Grows with every committed change and drops to 0 at a checkpoint." />

      <div className="mt-chart">
        <div className="mt-chart-head">
          <span className="mt-chart-title">Buffer pool</span>
          <span className="mt-chart-values"><span className="mt-value">pages <b>{fmt(used)} / {fmt(total)}</b></span></span>
        </div>
        <div className="mt-bar"><div style={{ width: `${total ? Math.min(100, (used / total) * 100) : 0}%` }} /></div>
        <div className="mt-note">
          {requests > 0 ? `Hit ratio since the start: ${(100 * (1 - reads / requests)).toFixed(1)}% (${fmt(requests)} page reads, ${fmt(reads)} from disk)`
            : "No page was read through the pool yet (a table's rows are served from memory; the pool carries writes)."}
        </div>
      </div>
    </div>
  );
}
