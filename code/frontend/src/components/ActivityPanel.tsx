// ActivityPanel.tsx
// What the server is doing right now, under the Sessions panel of the Server Manager: the open transactions (and which one waits for which), the row locks they
// hold, the sessions with the statement each is running, the last deadlocks, and the slow query log. The rows come from the engine's information_schema (processlist,
// innodb_trx, innodb_locks, innodb_lock_waits, innodb_deadlocks, slow_log) through the Tauri command get_activity; the panel asks again every 1.5 s while it is open.
//
// R59: a session that blocks others (its open transaction holds a row lock another waits for) is outlined in red and named in a banner with a button that ends it, a
// statement that has run for long is amber (5 s) or red (30 s), a session idle with a transaction open for long says so; each session has "Stop statement"
// (KILL QUERY) and "Disconnect" (KILL CONNECTION), each asked twice before it is sent; the slow query log can be switched on and its threshold set from here.
// What is marked is worked out in lib/activityMarks.ts.

import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { DEFAULT_THRESHOLDS, duration, markSessions, ownSessionId, type SessionRow, type TrxRow, type WaitRow } from "../lib/activityMarks";

interface Table { columns: string[]; rows: string[][] }
interface Activity { sessions: Table; transactions: Table; locks: Table; waits: Table; deadlocks: Table; slow: Table; slow_settings: Table; slow_count: Table }

const EMPTY: Table = { columns: [], rows: [] };
const card = { marginBottom: 8, padding: "6px 10px", background: "#2d2d2d", borderRadius: 5, border: "1px solid #3a3d41", fontSize: 11, color: "#d4d4d4" } as const;
const dim = { color: "#858585" } as const;
const small = { background: "transparent", border: "1px solid #555", color: "#bbb", borderRadius: 3, fontSize: 10, padding: "1px 6px", cursor: "pointer" } as const;
const RED = "#e74c3c", AMBER = "#d7ba7d", GREEN = "#4ec9b0";

function col(t: Table, name: string): number {
  return t.columns.findIndex(c => c.toLowerCase() === name.toLowerCase());
}

function cell(t: Table, row: string[], name: string): string {
  const i = col(t, name);
  return i < 0 ? "" : (row[i] ?? "");
}

async function runSql(query: string): Promise<string> {
  const res = await invoke<{ results: { success: boolean; message: string }[] }>("execute_query", { query, ts: Date.now() });
  const bad = res.results.find(r => !r.success);
  return bad ? bad.message : "";
}

// a button that has to be pressed twice: the first press asks, the second one does it (it goes back after a few seconds)
function TwoStep({ label, ask, danger, onConfirm }: { label: string; ask: string; danger?: boolean; onConfirm: () => void }) {
  const [asking, setAsking] = useState(false);
  useEffect(() => {
    if (!asking) return;
    const timer = setTimeout(() => setAsking(false), 4000);
    return () => clearTimeout(timer);
  }, [asking]);
  return asking ? (
    <button style={{ ...small, borderColor: RED, color: "#fff", background: RED }} onClick={() => { setAsking(false); onConfirm(); }}>{ask}</button>
  ) : (
    <button style={{ ...small, ...(danger ? { color: "#f48771", borderColor: "#7a3b36" } : {}) }} onClick={() => setAsking(true)}>{label}</button>
  );
}

export default function ActivityPanel() {
  const [a, setA] = useState<Activity>({ sessions: EMPTY, transactions: EMPTY, locks: EMPTY, waits: EMPTY, deadlocks: EMPTY, slow: EMPTY, slow_settings: EMPTY, slow_count: EMPTY });
  const [message, setMessage] = useState("");
  const ownId = useRef<string | null>(null);
  const pollNow = useRef<() => void>(() => {});

  useEffect(() => {
    let alive = true;
    const poll = async () => {
      try {
        const next = await invoke<Activity>("get_activity");
        if (alive) setA(next);
      } catch { /* the server is not reachable: keep what is shown */ }
    };
    pollNow.current = poll;
    poll();
    const timer = setInterval(poll, 1500);
    return () => { alive = false; clearInterval(timer); };
  }, []);

  const sessions: SessionRow[] = a.sessions.rows.map(r => ({
    id: cell(a.sessions, r, "ID"), user: cell(a.sessions, r, "USER"), db: cell(a.sessions, r, "DB"), command: cell(a.sessions, r, "COMMAND"),
    seconds: Number(cell(a.sessions, r, "TIME")) || 0, info: cell(a.sessions, r, "INFO") === "NULL" ? "" : cell(a.sessions, r, "INFO"),
  }));
  const transactions: TrxRow[] = a.transactions.rows.map(r => ({
    id: cell(a.transactions, r, "TRX_ID"), state: cell(a.transactions, r, "TRX_STATE"), threadId: cell(a.transactions, r, "TRX_MYSQL_THREAD_ID"),
    rowsLocked: Number(cell(a.transactions, r, "TRX_ROWS_LOCKED")) || 0, waitingFor: cell(a.transactions, r, "TRX_WAITING_FOR"),
    ageSeconds: cell(a.transactions, r, "TRX_AGE") && cell(a.transactions, r, "TRX_AGE") !== "NULL" ? Number(cell(a.transactions, r, "TRX_AGE")) : null,
  }));
  const waits: WaitRow[] = a.waits.rows.map(r => ({ requesting: cell(a.waits, r, "REQUESTING_TRX_ID"), blocking: cell(a.waits, r, "BLOCKING_TRX_ID") }));
  ownId.current = ownSessionId(sessions) ?? ownId.current;
  const marks = markSessions(sessions, transactions, waits, DEFAULT_THRESHOLDS, ownId.current);
  const roots = sessions.filter(s => marks[s.id]?.rootBlocker);

  const kill = async (kind: "QUERY" | "CONNECTION", id: string) => {
    const error = await runSql(`KILL ${kind} ${id};`);
    setMessage(error ? `KILL ${kind} ${id}: ${error}` : kind === "QUERY" ? `Asked the statement of session #${id} to stop.` : `Session #${id} was disconnected.`);
    pollNow.current();
  };

  // the locks of each transaction, by table
  const lockedBy: Record<string, string[]> = {};
  for (const r of a.locks.rows) {
    const trx = cell(a.locks, r, "LOCK_TRX_ID");
    (lockedBy[trx] ??= []).push(`${cell(a.locks, r, "LOCK_TABLE")} #${cell(a.locks, r, "LOCK_DATA")}`);
  }
  const blockersTrx = new Set(waits.map(w => w.blocking));

  const logOn = cell(a.slow_settings, a.slow_settings.rows[0] ?? [], "LOG_ON") === "1";
  const threshold = Number(cell(a.slow_settings, a.slow_settings.rows[0] ?? [], "SECONDS")) || 10;
  const slowCount = cell(a.slow_count, a.slow_count.rows[0] ?? [], "Value");
  const setSlow = async (setting: string) => {
    const error = await runSql(`SET GLOBAL ${setting};`);
    setMessage(error ? `SET GLOBAL ${setting}: ${error}` : "");
    pollNow.current();
  };

  return (
    <>
      {roots.length > 0 && (
        <div style={{ ...card, borderColor: RED, background: "#3a2323", marginTop: 12 }}>
          {roots.map(s => (
            <div key={s.id} style={{ display: "flex", alignItems: "center", gap: 6, marginBottom: 2 }}>
              <span style={{ color: RED, fontWeight: 700 }}>⚠ #{s.id} blocks {marks[s.id].blocking} transaction{marks[s.id].blocking > 1 ? "s" : ""}</span>
              <span style={dim}>{s.command === "Sleep" ? "idle with a transaction open" : "running"}</span>
              <span style={{ marginLeft: "auto" }}>
                {!marks[s.id].own && <TwoStep label="Disconnect" ask={`Disconnect #${s.id}? yes`} danger onConfirm={() => kill("CONNECTION", s.id)} />}
              </span>
            </div>
          ))}
        </div>
      )}
      {message && <div style={{ ...card, borderColor: "#3a3d41", color: "#9cdcfe" }}>{message}</div>}

      <div className="srv-slide-section" style={{ marginTop: 14 }}>Transactions</div>
      {a.transactions.rows.length === 0 ? (
        <div className="srv-slide-text">No open transaction.</div>
      ) : transactions.map((trx, i) => {
        const waiting = trx.state === "LOCK WAIT";
        const blocker = blockersTrx.has(trx.id);
        const locks = lockedBy[trx.id] ?? [];
        const tone = waiting || blocker ? RED : GREEN;
        return (
          <div key={i} style={{ ...card, borderColor: waiting || blocker ? "#c0392b" : "#3a3d41" }}>
            <div style={{ display: "flex", gap: 6, alignItems: "center" }}>
              <span style={{ width: 8, height: 8, borderRadius: "50%", background: tone, display: "inline-block" }} />
              <b>txn {trx.id}</b>
              {trx.threadId && trx.threadId !== "0" && <span style={dim}>session #{trx.threadId}</span>}
              <span style={{ marginLeft: "auto", color: tone }}>{waiting ? "LOCK WAIT" : blocker ? "BLOCKING" : "RUNNING"}</span>
            </div>
            <div style={dim}>{trx.rowsLocked} row lock(s){trx.ageSeconds !== null ? ` · open ${duration(trx.ageSeconds)}` : ""}</div>
            {waiting && <div style={{ color: RED }}>waits for txn {trx.waitingFor}</div>}
            {locks.slice(0, 6).map((l, k) => <div key={k} style={dim}>🔒 {l}</div>)}
            {locks.length > 6 && <div style={dim}>… {locks.length - 6} more</div>}
          </div>
        );
      })}

      {a.waits.rows.length > 0 && (
        <>
          <div className="srv-slide-section" style={{ marginTop: 10 }}>Waiting for a lock</div>
          {waits.map((w, i) => (
            <div key={i} style={{ ...card, borderColor: "#c0392b" }}>
              txn {w.requesting} <span style={dim}>waits for</span> txn {w.blocking}
            </div>
          ))}
        </>
      )}

      <div className="srv-slide-section" style={{ marginTop: 10 }}>Statements</div>
      {sessions.map(s => {
        const m = marks[s.id];
        const border = m.rootBlocker ? RED : m.waitingForSession ? "#c0392b" : m.slow === "hot" ? RED : m.slow === "warn" ? AMBER : m.idleInTransaction !== null ? AMBER : "#3a3d41";
        return (
          <div key={s.id} style={{ ...card, borderColor: border, ...(m.rootBlocker ? { background: "#35282a" } : {}) }}>
            <div style={{ display: "flex", gap: 6, alignItems: "center", flexWrap: "wrap" }}>
              <b>#{s.id}</b> {s.user}
              {m.own && <span style={{ ...dim, border: "1px solid #444", borderRadius: 3, padding: "0 4px" }}>this app</span>}
              {m.rootBlocker && <span style={{ color: RED, fontWeight: 700 }}>BLOCKS {m.blocking}</span>}
              {m.waitingForSession && <span style={{ color: RED }}>waits for #{m.waitingForSession}</span>}
              {m.slow && <span style={{ color: m.slow === "hot" ? RED : AMBER, fontWeight: 700 }}>SLOW {duration(s.seconds)}</span>}
              {m.idleInTransaction !== null && <span style={{ color: AMBER }}>idle in transaction {duration(m.idleInTransaction)}</span>}
              <span style={{ marginLeft: "auto", ...dim }}>{s.db === "NULL" ? "" : s.db} · {s.command} · {duration(s.seconds)}</span>
            </div>
            {s.info && <div style={{ ...dim, wordBreak: "break-all" }}>{s.info}</div>}
            {!m.own && (
              <div style={{ display: "flex", gap: 6, marginTop: 4 }}>
                {s.command === "Query" && <TwoStep label="Stop statement" ask="Stop it? yes" onConfirm={() => kill("QUERY", s.id)} />}
                <TwoStep label="Disconnect" ask="Disconnect? yes" danger onConfirm={() => kill("CONNECTION", s.id)} />
              </div>
            )}
          </div>
        );
      })}

      {a.deadlocks.rows.length > 0 && (
        <>
          <div className="srv-slide-section" style={{ marginTop: 10 }}>Deadlocks (latest)</div>
          {a.deadlocks.rows.slice(-5).map((r, i) => (
            <div key={i} style={{ ...card, borderColor: "#c0392b" }}>
              txn {cell(a.deadlocks, r, "REQUESTING_TRX_ID")} <span style={dim}>was rolled back, waiting for</span> txn {cell(a.deadlocks, r, "BLOCKING_TRX_ID")}
            </div>
          ))}
        </>
      )}

      <div className="srv-slide-section" style={{ marginTop: 10 }}>Slow queries</div>
      <div style={card}>
        <div style={{ display: "flex", alignItems: "center", gap: 6, flexWrap: "wrap" }}>
          <span style={{ width: 8, height: 8, borderRadius: "50%", background: logOn ? GREEN : "#666", display: "inline-block" }} />
          <b>{logOn ? "log on" : "log off"}</b>
          <span style={dim}>from {threshold} s · {slowCount || "0"} counted</span>
          <span style={{ marginLeft: "auto", display: "flex", gap: 6, alignItems: "center" }}>
            <select
              value={String(threshold)}
              onChange={e => setSlow(`long_query_time = ${e.target.value}`)}
              style={{ background: "#2d2d2d", color: "#d4d4d4", border: "1px solid #444", borderRadius: 3, fontSize: 10 }}
              title="long_query_time: a statement this slow is counted and, with the log on, written down"
            >
              {[0.1, 0.5, 1, 5, 10].concat([0.1, 0.5, 1, 5, 10].includes(threshold) ? [] : [threshold]).sort((x, y) => x - y).map(v => <option key={v} value={String(v)}>{v} s</option>)}
            </select>
            <button style={small} onClick={() => setSlow(`slow_query_log = ${logOn ? 0 : 1}`)}>{logOn ? "Turn off" : "Turn on"}</button>
          </span>
        </div>
      </div>
      {a.slow.rows.length === 0 ? (
        <div className="srv-slide-text">{logOn ? "Nothing slower than the threshold yet." : "The log is off: turn it on to write slow statements down."}</div>
      ) : a.slow.rows.map((r, i) => (
        <div key={i} style={card} title={cell(a.slow, r, "SQL_TEXT")}>
          <div style={{ display: "flex", gap: 6 }}>
            <b style={{ color: AMBER }}>{Number(cell(a.slow, r, "QUERY_TIME")).toFixed(3)} s</b>
            <span style={dim}>{cell(a.slow, r, "ROWS_SENT")} row(s) · #{cell(a.slow, r, "THREAD_ID")}</span>
            <span style={{ marginLeft: "auto", ...dim }}>{cell(a.slow, r, "START_TIME").slice(0, 19)}</span>
          </div>
          <div style={{ ...dim, wordBreak: "break-all" }}>{cell(a.slow, r, "SQL_TEXT").slice(0, 160)}</div>
        </div>
      ))}
    </>
  );
}
