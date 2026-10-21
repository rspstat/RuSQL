// ActivityPanel.tsx
// What the server is doing right now, under the Sessions panel of the Server Manager: the open transactions (and which one waits for which), the row locks they
// hold, the sessions with the statement each is running, and the last deadlocks. The rows come from the engine's information_schema (processlist, innodb_trx,
// innodb_locks, innodb_lock_waits, innodb_deadlocks) through the Tauri command get_activity; the panel asks again every 1.5 s while it is open.

import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";

interface Table { columns: string[]; rows: string[][] }
interface Activity { sessions: Table; transactions: Table; locks: Table; waits: Table; deadlocks: Table }

const EMPTY: Table = { columns: [], rows: [] };
const card = { marginBottom: 8, padding: "6px 10px", background: "#2d2d2d", borderRadius: 5, border: "1px solid #3a3d41", fontSize: 11, color: "#d4d4d4" } as const;
const dim = { color: "#858585" } as const;

function col(t: Table, name: string): number {
  return t.columns.findIndex(c => c.toLowerCase() === name.toLowerCase());
}

function cell(t: Table, row: string[], name: string): string {
  const i = col(t, name);
  return i < 0 ? "" : (row[i] ?? "");
}

export default function ActivityPanel() {
  const [a, setA] = useState<Activity>({ sessions: EMPTY, transactions: EMPTY, locks: EMPTY, waits: EMPTY, deadlocks: EMPTY });

  useEffect(() => {
    let alive = true;
    const poll = async () => {
      try {
        const next = await invoke<Activity>("get_activity");
        if (alive) setA(next);
      } catch { /* the server is not reachable: keep what is shown */ }
    };
    poll();
    const timer = setInterval(poll, 1500);
    return () => { alive = false; clearInterval(timer); };
  }, []);

  // the locks of each transaction, by table
  const lockedBy: Record<string, string[]> = {};
  for (const r of a.locks.rows) {
    const trx = cell(a.locks, r, "LOCK_TRX_ID");
    (lockedBy[trx] ??= []).push(`${cell(a.locks, r, "LOCK_TABLE")} #${cell(a.locks, r, "LOCK_DATA")}`);
  }

  return (
    <>
      <div className="srv-slide-section" style={{ marginTop: 14 }}>Transactions</div>
      {a.transactions.rows.length === 0 ? (
        <div className="srv-slide-text">No open transaction.</div>
      ) : a.transactions.rows.map((r, i) => {
        const id = cell(a.transactions, r, "TRX_ID");
        const waiting = cell(a.transactions, r, "TRX_STATE") === "LOCK WAIT";
        const locks = lockedBy[id] ?? [];
        return (
          <div key={i} style={{ ...card, borderColor: waiting ? "#c0392b" : "#3a3d41" }}>
            <div style={{ display: "flex", gap: 6, alignItems: "center" }}>
              <span style={{ width: 8, height: 8, borderRadius: "50%", background: waiting ? "#e74c3c" : "#4ec9b0", display: "inline-block" }} />
              <b>txn {id}</b>
              <span style={{ marginLeft: "auto", color: waiting ? "#e74c3c" : "#4ec9b0" }}>{waiting ? "LOCK WAIT" : "RUNNING"}</span>
            </div>
            <div style={dim}>{cell(a.transactions, r, "TRX_ROWS_LOCKED")} row lock(s)</div>
            {waiting && <div style={{ color: "#e74c3c" }}>waits for txn {cell(a.transactions, r, "TRX_WAITING_FOR")}</div>}
            {locks.slice(0, 6).map((l, k) => <div key={k} style={dim}>🔒 {l}</div>)}
            {locks.length > 6 && <div style={dim}>… {locks.length - 6} more</div>}
          </div>
        );
      })}

      {a.waits.rows.length > 0 && (
        <>
          <div className="srv-slide-section" style={{ marginTop: 10 }}>Waiting for a lock</div>
          {a.waits.rows.map((r, i) => (
            <div key={i} style={{ ...card, borderColor: "#c0392b" }}>
              txn {cell(a.waits, r, "REQUESTING_TRX_ID")} <span style={dim}>waits for</span> txn {cell(a.waits, r, "BLOCKING_TRX_ID")}
            </div>
          ))}
        </>
      )}

      <div className="srv-slide-section" style={{ marginTop: 10 }}>Statements</div>
      {a.sessions.rows.map((r, i) => (
        <div key={i} style={card}>
          <div style={{ display: "flex", gap: 6 }}>
            <b>#{cell(a.sessions, r, "ID")}</b> {cell(a.sessions, r, "USER")}
            <span style={{ marginLeft: "auto", ...dim }}>{cell(a.sessions, r, "DB") === "NULL" ? "" : cell(a.sessions, r, "DB")} · {cell(a.sessions, r, "COMMAND")} · {cell(a.sessions, r, "TIME")}s</span>
          </div>
          {cell(a.sessions, r, "INFO") !== "NULL" && <div style={{ ...dim, wordBreak: "break-all" }}>{cell(a.sessions, r, "INFO")}</div>}
        </div>
      ))}

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
    </>
  );
}
