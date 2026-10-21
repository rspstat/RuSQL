// JourneyModal.tsx
// The journey of one SQL statement. The statement is run once with TRACE (the engine answers with the steps of its path: parse, prepare, locks, the SELECT's own
// steps, execute, commit, with the rows each step left and the time it took), and the recorded steps are then replayed here at a speed a person can follow --
// a statement takes milliseconds, so the replay is slowed down; the bars and the times stay the real ones.

import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { MultiQueryResult } from "../types";
import "./JourneyModal.css";

interface Step { n: number; what: string; rows: number | null; ms: number; share: number }
interface Trace { steps: Step[]; total: number; outcome: string; failed: boolean }

const STEP_MS = 1100; // how long the replay stays on a step at 1x

// What a step is, for the picture and the one-line explanation
function kindOf(what: string): { icon: string; text: string } {
  const w = what.toLowerCase();
  if (w.startsWith("parse")) return { icon: "Aa", text: "The SQL text is read and turned into a statement the engine can run." };
  if (w.startsWith("prepare")) return { icon: "◇", text: "The statement is checked against the cache and the locks it needs are worked out." };
  if (w.includes("lock")) return { icon: "🔒", text: "The tables are locked: shared for a read, exclusive for a write, so other sessions cannot see half a change." };
  if (w.startsWith("scan")) return { icon: "▤", text: "The rows are read from the table (an index when there is one) and joined; the WHERE filter leaves the rows shown in Rows." };
  if (w.startsWith("sort")) return { icon: "⇅", text: "The rows are put in the order ORDER BY asks for." };
  if (w.startsWith("group") || w.startsWith("aggregate")) return { icon: "Σ", text: "The rows are folded into groups and the aggregates (COUNT, SUM, ...) are worked out." };
  if (w.startsWith("commit")) return { icon: "💾", text: "The change is written to the redo log and flushed to the disk (fsync), so it survives a crash." };
  if (w.startsWith("return")) return { icon: "↩", text: "The answer is formatted and handed back to the client." };
  if (w.startsWith("execute")) return { icon: "▶", text: "The statement runs and its answer is built." };
  return { icon: "•", text: "" };
}

function parseTrace(res: MultiQueryResult): Trace | string {
  const r = res.results[0];
  if (!r) return "No answer.";
  if (!r.success) return r.message || "The statement failed.";
  const idx = (name: string) => r.columns.findIndex(c => c.toLowerCase() === name);
  const iStep = idx("step"), iWhat = idx("what"), iRows = idx("rows"), iMs = idx("ms"), iShare = idx("share %");
  if (iStep < 0 || iWhat < 0 || iMs < 0) return "The engine did not answer with a trace. Is this build too old for TRACE?";
  const steps: Step[] = [];
  let total = 0, outcome = "", failed = false;
  for (const row of r.rows) {
    if (row[iStep] === "Total") {
      total = Number(row[iMs]) || 0;
      outcome = row[iWhat] ?? "";
      failed = outcome.startsWith("Statement failed");
      continue;
    }
    steps.push({
      n: Number(row[iStep]) || steps.length + 1,
      what: row[iWhat] ?? "",
      rows: iRows >= 0 && row[iRows] !== "" ? Number(row[iRows]) : null,
      ms: Number(row[iMs]) || 0,
      share: iShare >= 0 ? Number(row[iShare]) || 0 : 0,
    });
  }
  return { steps, total, outcome, failed };
}

const isRead = (sql: string) => /^\s*(\(|SELECT\b|WITH\b)/i.test(sql);

export default function JourneyModal({ sql, onClose }: { sql: string; onClose: () => void }) {
  const [phase, setPhase] = useState<"ask" | "loading" | "ready" | "error">(isRead(sql) ? "loading" : "ask");
  const [trace, setTrace] = useState<Trace | null>(null);
  const [error, setError] = useState("");
  const [cur, setCur] = useState(-1);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(1);
  const started = useRef(false);

  const run = async () => {
    setPhase("loading");
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: `TRACE ${sql.replace(/;\s*$/, "")};`, ts: Date.now() });
      const t = parseTrace(res);
      if (typeof t === "string") { setError(t); setPhase("error"); return; }
      setTrace(t);
      setCur(-1);
      setPhase("ready");
      setPlaying(true);
    } catch (e) {
      setError(String(e));
      setPhase("error");
    }
  };

  useEffect(() => {
    if (started.current) return;
    started.current = true;
    if (isRead(sql)) run();
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!playing || !trace) return;
    if (cur >= trace.steps.length - 1) { setPlaying(false); return; }
    const t = setTimeout(() => setCur(c => c + 1), (cur < 0 ? 350 : STEP_MS) / speed);
    return () => clearTimeout(t);
  }, [playing, cur, trace, speed]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if (e.key === "Escape") onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  const steps = trace?.steps ?? [];
  const done = steps.length > 0 && cur >= steps.length - 1;
  const step = cur >= 0 ? steps[cur] : null;
  const maxMs = Math.max(...steps.map(s => s.ms), 1e-9);

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="jn-modal" onClick={e => e.stopPropagation()}>
        <div className="jn-header">
          <span>Journey of a statement</span>
          <button className="jn-close" onClick={onClose}>✕</button>
        </div>
        <pre className="jn-sql">{sql}</pre>

        {phase === "ask" && (
          <div className="jn-body">
            <p className="jn-warn">The journey is recorded by running the statement for real, so this one changes data.</p>
            <button className="jn-btn jn-primary" onClick={run}>Run it and show the journey</button>
          </div>
        )}
        {phase === "loading" && <div className="jn-body jn-dim">Running the statement...</div>}
        {phase === "error" && <div className="jn-body jn-err">{error}</div>}

        {phase === "ready" && trace && (
          <div className="jn-body">
            <div className="jn-track">
              {steps.map((s, i) => (
                <div key={i} className="jn-track-item">
                  <div className={`jn-node${i === cur ? " jn-now" : i < cur || done ? " jn-past" : ""}`} onClick={() => { setPlaying(false); setCur(i); }}>
                    <div className="jn-icon">{kindOf(s.what).icon}</div>
                    <div className="jn-node-name">{s.what}</div>
                  </div>
                  {i < steps.length - 1 && <div className={`jn-link${i < cur ? " jn-link-on" : ""}`} />}
                </div>
              ))}
            </div>

            <div className="jn-detail">
              {step ? (
                <>
                  <div className="jn-detail-title">Step {cur + 1} of {steps.length} — {step.what}</div>
                  <div className="jn-detail-text">{kindOf(step.what).text}</div>
                  <div className="jn-facts">
                    {step.rows !== null && step.rows > 0 && <span>rows left: <b>{step.rows}</b></span>}
                    <span>took: <b>{step.ms.toFixed(3)} ms</b> ({step.share}% of {trace.total.toFixed(3)} ms)</span>
                  </div>
                </>
              ) : (
                <div className="jn-detail-text jn-dim">The statement is about to start...</div>
              )}
            </div>

            <div className="jn-bars">
              {steps.map((s, i) => (
                <div key={i} className={`jn-bar-row${i === cur ? " jn-bar-now" : ""}`}>
                  <span className="jn-bar-name">{s.what}</span>
                  <span className="jn-bar-track"><span className="jn-bar-fill" style={{ width: `${(s.ms / maxMs) * 100}%`, opacity: i <= cur ? 1 : 0.28 }} /></span>
                  <span className="jn-bar-ms">{s.ms.toFixed(3)} ms</span>
                </div>
              ))}
            </div>

            {done && <div className={`jn-outcome${trace.failed ? " jn-err" : ""}`}>{trace.outcome} · total {trace.total.toFixed(3)} ms</div>}

            <div className="jn-controls">
              <button className="jn-btn" disabled={cur <= 0} onClick={() => { setPlaying(false); setCur(c => Math.max(0, c - 1)); }}>◀</button>
              <button className="jn-btn jn-primary" onClick={() => { if (done) setCur(-1); setPlaying(p => (done ? true : !p)); }}>
                {done ? "↻ Replay" : playing ? "❚❚ Pause" : "▶ Play"}
              </button>
              <button className="jn-btn" disabled={cur >= steps.length - 1} onClick={() => { setPlaying(false); setCur(c => Math.min(steps.length - 1, c + 1)); }}>▶</button>
              <select className="jn-speed" value={speed} onChange={e => setSpeed(Number(e.target.value))}>
                <option value={0.5}>0.5x</option>
                <option value={1}>1x</option>
                <option value={2}>2x</option>
                <option value={4}>4x</option>
              </select>
              <span className="jn-dim jn-note">The replay is slowed down; the times are the real ones.</span>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
