// MvccView.tsx
// "MVCC" activity-bar page: two sessions side by side and the versions of the rows between them. Session A and session B are two connections to the engine server (Tauri
// commands mvcc_exec / mvcc_reset); the middle column is information_schema.row_versions, read through each of them, so every version shows who made it (xmin), who
// ended it (xmax), what that makes it (live / uncommitted / being ended / dead) and whether A and B each see it. A scenario is a list of steps run one at a time
// (or all at once): an UPDATE that adds a version, a snapshot that keeps answering the same, a dirty read, a write that waits for a lock. Every visible string is an
// English/Korean pair with the same EN/KO toggle as the Diagram page.

import { useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { QueryResult } from "../types";
import { L, LangContext, useT } from "./diagramShared";
import type { Lang, Text } from "./diagramShared";
import "./MvccView.css";

const LANG_KEY = "rusql_diagram_lang";
function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

type Who = "A" | "B";
interface Step { s: Who; sql: string; note: Text }
interface Entry { sql: string; ok: boolean; text: string; table?: { columns: string[]; rows: string[][] } }
interface Version { key: string; data: string; xmin: string; xmax: string; state: string; vis: Record<Who, boolean> }
interface Txn { id: string; iso: string }

const TABLE = "mvcc_demo";
const SETUP = [
  `DROP TABLE IF EXISTS ${TABLE}`,
  `CREATE TABLE ${TABLE} (id INT PRIMARY KEY, name VARCHAR(20), balance INT)`,
  `INSERT INTO ${TABLE} VALUES (1, 'alice', 100), (2, 'bob', 200), (3, 'carol', 300)`,
];
const VERSIONS_SQL = `SELECT ROW_KEY, ROW_DATA, XMIN, XMAX, STATE, VISIBLE_TO_ME FROM information_schema.row_versions WHERE TABLE_NAME = '${TABLE}'`;

interface Scenario { id: string; title: Text; about: Text; steps: (iso: string) => Step[] }

const SCENARIOS: Scenario[] = [
  {
    id: "version",
    title: L("An UPDATE adds a version", "UPDATE는 덮어쓰지 않고 버전을 더한다"),
    about: L("The old version stays until nobody can need it; a delete only marks the row as ended; VACUUM removes what is dead.", "옛 버전은 아무도 필요로 하지 않을 때까지 남고, 삭제는 행에 끝났다는 표시만 하며, VACUUM이 죽은 버전을 치웁니다."),
    steps: () => [
      { s: "A", sql: "BEGIN", note: L("A opens a transaction.", "A가 트랜잭션을 엽니다.") },
      { s: "A", sql: `UPDATE ${TABLE} SET balance = balance + 50 WHERE id = 1`, note: L("A adds a new version of row 1 (UNCOMMITTED) and marks the old one as ended by A (ENDING). A sees the new one, B still sees the old one.", "A가 1번 행의 새 버전을 더하고(UNCOMMITTED) 옛 버전에 A가 끝냈다고 표시합니다(ENDING). A에게는 새 버전이, B에게는 아직 옛 버전이 보입니다.") },
      { s: "B", sql: `SELECT * FROM ${TABLE} WHERE id = 1`, note: L("B reads 100: the version A has not committed does not exist for B.", "B는 100을 읽습니다: A가 커밋하지 않은 버전은 B에게 없는 것과 같습니다.") },
      { s: "A", sql: "COMMIT", note: L("A commits: the new version is live, the old one is dead but kept.", "A가 커밋: 새 버전이 살아 있고 옛 버전은 죽었지만 남아 있습니다.") },
      { s: "B", sql: `SELECT * FROM ${TABLE} WHERE id = 1`, note: L("Now B reads 150.", "이제 B는 150을 읽습니다.") },
      { s: "A", sql: "BEGIN", note: L("A opens another transaction.", "A가 트랜잭션을 다시 엽니다.") },
      { s: "A", sql: `DELETE FROM ${TABLE} WHERE id = 3`, note: L("A delete does not remove the row: its version gets an end (xmax). B still sees it until A commits.", "삭제는 행을 지우지 않습니다: 버전에 끝(xmax)이 붙을 뿐이고, A가 커밋하기 전까지 B에게는 그대로 보입니다.") },
      { s: "A", sql: "COMMIT", note: L("Committed: the version of row 3 is dead and kept for any snapshot that may still need it.", "커밋: 3번 행의 버전은 죽었고, 아직 필요로 할 수 있는 스냅샷을 위해 남아 있습니다.") },
      { s: "A", sql: `VACUUM ${TABLE}`, note: L("VACUUM removes the dead versions nobody can still see.", "VACUUM이 더는 아무도 볼 수 없는 죽은 버전을 치웁니다.") },
    ],
  },
  {
    id: "snapshot",
    title: L("The same question, a different answer", "같은 질문, 다른 답 (스냅샷)"),
    about: L("Under REPEATABLE READ a transaction keeps reading the world as it was at BEGIN; under READ COMMITTED every statement sees what has been committed by then. Pick the level of A below.", "REPEATABLE READ에서는 트랜잭션이 BEGIN 때의 세상을 계속 읽고, READ COMMITTED에서는 문장마다 그때까지 커밋된 것을 봅니다. 아래에서 A의 격리 수준을 고르세요."),
    steps: iso => [
      { s: "A", sql: `SET ISOLATION LEVEL ${iso}`, note: L("A picks its isolation level.", "A가 격리 수준을 고릅니다.") },
      { s: "A", sql: "BEGIN", note: L("A's transaction starts; under REPEATABLE READ its snapshot is taken now.", "A의 트랜잭션이 시작됩니다. REPEATABLE READ라면 이 순간 스냅샷이 만들어집니다.") },
      { s: "A", sql: `SELECT * FROM ${TABLE} WHERE id = 1`, note: L("A reads 100.", "A는 100을 읽습니다.") },
      { s: "B", sql: `UPDATE ${TABLE} SET balance = 999 WHERE id = 1`, note: L("B updates row 1 and commits at once (autocommit): a new live version, the old one dead.", "B가 1번 행을 바꾸고 곧바로 커밋합니다(autocommit): 새 버전이 살아 있고 옛 버전은 죽었습니다.") },
      { s: "A", sql: `SELECT * FROM ${TABLE} WHERE id = 1`, note: L("REPEATABLE READ: still 100, A's snapshot does not see the new version. READ COMMITTED: 999.", "REPEATABLE READ라면 여전히 100: A의 스냅샷에는 새 버전이 안 보입니다. READ COMMITTED라면 999.") },
      { s: "A", sql: "COMMIT", note: L("A's transaction ends, and with it the snapshot.", "A의 트랜잭션이 끝나고 스냅샷도 사라집니다.") },
      { s: "A", sql: `SELECT * FROM ${TABLE} WHERE id = 1`, note: L("A reads 999 now.", "이제 A도 999를 읽습니다.") },
    ],
  },
  {
    id: "dirty",
    title: L("A dirty read", "더티 리드 (READ UNCOMMITTED)"),
    about: L("READ UNCOMMITTED reads versions nobody has committed, and they can vanish.", "READ UNCOMMITTED는 아무도 커밋하지 않은 버전을 읽고, 그 버전은 사라질 수 있습니다."),
    steps: () => [
      { s: "A", sql: "BEGIN", note: L("A opens a transaction.", "A가 트랜잭션을 엽니다.") },
      { s: "A", sql: `UPDATE ${TABLE} SET balance = 0 WHERE id = 2`, note: L("A sets bob's balance to 0, not committed.", "A가 bob의 잔액을 0으로 바꿉니다. 커밋하지 않았습니다.") },
      { s: "B", sql: "SET ISOLATION LEVEL READ UNCOMMITTED", note: L("B chooses to read uncommitted versions.", "B가 커밋되지 않은 버전도 읽기로 합니다.") },
      { s: "B", sql: `SELECT * FROM ${TABLE} WHERE id = 2`, note: L("B reads 0 -- a version that has not been committed (a dirty read).", "B는 0을 읽습니다. 커밋되지 않은 버전입니다(더티 리드).") },
      { s: "A", sql: "ROLLBACK", note: L("A rolls back: the uncommitted version disappears and the old one is live again.", "A가 롤백: 커밋되지 않은 버전이 사라지고 옛 버전이 다시 살아납니다.") },
      { s: "B", sql: `SELECT * FROM ${TABLE} WHERE id = 2`, note: L("B reads 200: the 0 it saw never existed.", "B는 200을 읽습니다. 앞에서 본 0은 처음부터 없던 값입니다.") },
      { s: "B", sql: "SET ISOLATION LEVEL READ COMMITTED", note: L("B goes back to the default level.", "B가 기본 수준으로 돌아갑니다.") },
    ],
  },
  {
    id: "wait",
    title: L("A write that waits", "기다리는 쓰기 (행 락)"),
    about: L("Two sessions cannot change the same row at once: the second waits until the first ends.", "두 세션은 같은 행을 동시에 바꿀 수 없습니다: 두 번째는 첫 번째가 끝날 때까지 기다립니다."),
    steps: () => [
      { s: "A", sql: "BEGIN", note: L("A opens a transaction.", "A가 트랜잭션을 엽니다.") },
      { s: "A", sql: `UPDATE ${TABLE} SET balance = balance + 1 WHERE id = 1`, note: L("A changes row 1 and holds its lock.", "A가 1번 행을 바꾸고 그 락을 쥡니다.") },
      { s: "B", sql: `UPDATE ${TABLE} SET balance = balance + 10 WHERE id = 1`, note: L("B tries the same row and waits: its statement does not come back.", "B가 같은 행을 바꾸려다 기다립니다: 이 문장은 돌아오지 않습니다.") },
      { s: "A", sql: "COMMIT", note: L("A commits and releases the lock: B's statement goes on.", "A가 커밋해 락을 놓으면 B의 문장이 이어서 실행됩니다.") },
      { s: "B", sql: `SELECT * FROM ${TABLE} WHERE id = 1`, note: L("B's change was made on top of A's: 111.", "B의 변경이 A의 변경 위에 쌓였습니다: 111.") },
    ],
  },
];

const sleep = (ms: number) => new Promise<void>(r => setTimeout(r, ms));

function Body() {
  const t = useT();
  const [scenarioId, setScenarioId] = useState("version");
  const [iso, setIso] = useState("REPEATABLE READ");
  const [next, setNext] = useState(0);
  const [log, setLog] = useState<Record<Who, Entry[]>>({ A: [], B: [] });
  const [txn, setTxn] = useState<Record<Who, Txn | null>>({ A: null, B: null });
  const [busy, setBusy] = useState<Record<Who, boolean>>({ A: false, B: false });
  const [versions, setVersions] = useState<Version[]>([]);
  const [note, setNote] = useState<Text | null>(null);
  const [error, setError] = useState("");
  const [ready, setReady] = useState(false);
  const [typed, setTyped] = useState<Record<Who, string>>({ A: "", B: "" });
  const [auto, setAuto] = useState(false);
  const busyRef = useRef<Record<Who, boolean>>({ A: false, B: false });
  const seen = useRef<Record<Who, Record<string, boolean>>>({ A: {}, B: {} });
  const rowsOf = useRef<Record<string, Version>>({});
  const stop = useRef(false);

  const scenario = SCENARIOS.find(s => s.id === scenarioId)!;
  const steps = scenario.steps(iso);

  const setBusyOf = (who: Who, on: boolean) => {
    busyRef.current = { ...busyRef.current, [who]: on };
    setBusy(busyRef.current);
  };

  // the versions as each session sees them (a session that is waiting for a lock cannot be asked: its last answer stays)
  const refresh = async () => {
    for (const who of ["A", "B"] as Who[]) {
      if (busyRef.current[who]) continue;
      const res = await invoke<QueryResult>("mvcc_exec", { session: who, query: VERSIONS_SQL });
      if (!res.success) continue;
      const visible: Record<string, boolean> = {};
      for (const r of res.rows) {
        const key = `${r[0]}|${r[2]}`;
        visible[key] = r[5] === "YES";
        rowsOf.current[key] = { key: r[0], data: r[1], xmin: r[2], xmax: r[3], state: r[4], vis: { A: false, B: false } };
      }
      seen.current[who] = visible;
    }
    // a version that neither session lists any more is gone (rolled back, vacuumed)
    const all = new Set([...Object.keys(seen.current.A), ...Object.keys(seen.current.B)]);
    const out: Version[] = [];
    for (const key of all) {
      const v = rowsOf.current[key];
      if (v) out.push({ ...v, vis: { A: !!seen.current.A[key], B: !!seen.current.B[key] } });
    }
    out.sort((a, b) => Number(a.key) - Number(b.key) || Number(a.xmin) - Number(b.xmin));
    setVersions(out);
  };

  const record = (who: Who, sql: string, res: QueryResult) => {
    const entry: Entry = { sql, ok: res.success, text: res.columns.length ? `${res.rows.length} row(s)` : res.message, table: res.columns.length ? { columns: res.columns, rows: res.rows.slice(0, 6) } : undefined };
    setLog(prev => ({ ...prev, [who]: [entry, ...prev[who]].slice(0, 5) }));
    const begun = /Transaction (\d+) started\. \(isolation: (\w+)\)/.exec(res.message);
    if (begun) setTxn(prev => ({ ...prev, [who]: { id: begun[1], iso: begun[2] } }));
    else if (res.success && /^\s*(COMMIT|ROLLBACK)/i.test(sql)) setTxn(prev => ({ ...prev, [who]: null }));
  };

  const exec = async (who: Who, sql: string) => {
    if (busyRef.current[who]) return;
    setBusyOf(who, true);
    setError("");
    try {
      const res = await invoke<QueryResult>("mvcc_exec", { session: who, query: sql });
      record(who, sql, res);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusyOf(who, false);
    }
    await refresh();
  };

  const reset = async () => {
    stop.current = true;
    setAuto(false);
    setError("");
    setLog({ A: [], B: [] });
    setTxn({ A: null, B: null });
    setNext(0);
    setNote(null);
    rowsOf.current = {};
    seen.current = { A: {}, B: {} };
    busyRef.current = { A: false, B: false };
    setBusy({ A: false, B: false });
    await invoke("mvcc_reset");
    const db = await invoke<string>("get_current_db");
    if (!db) { setError(t(L("No database is selected: run USE <database> in the editor (or click a database in the sidebar) first. The demo table is created there.", "선택된 데이터베이스가 없습니다. 에디터에서 USE <데이터베이스>를 실행하거나 사이드바에서 데이터베이스를 눌러 먼저 고르세요. 실습 표는 거기에 만들어집니다."))); setReady(false); return; }
    for (const sql of SETUP) {
      const res = await invoke<QueryResult>("mvcc_exec", { session: "S", query: sql });
      if (!res.success) { setError(res.message); setReady(false); return; }
    }
    setReady(true);
    await refresh();
  };

  const runStep = async (i: number) => {
    const st = steps[i];
    setNote(st.note);
    setNext(i + 1);
    const p = exec(st.s, st.sql);
    await Promise.race([p, sleep(900)]); // (a statement that waits for a lock does not hold the scenario up)
  };

  const playAll = async () => {
    stop.current = false;
    setAuto(true);
    for (let i = next; i < steps.length && !stop.current; i++) {
      await runStep(i);
      await sleep(1600);
    }
    setAuto(false);
  };

  const session = (who: Who) => (
    <div className="mv-session">
      <div className="mv-session-head">
        <b>{t(L(`Session ${who}`, `세션 ${who}`))}</b>
        <span className={`mv-chip ${txn[who] ? "mv-on" : ""}`}>{txn[who] ? t(L(`txn #${txn[who]!.id} · ${txn[who]!.iso}`, `트랜잭션 #${txn[who]!.id} · ${txn[who]!.iso}`)) : t(L("autocommit", "autocommit"))}</span>
        {busy[who] && <span className="mv-chip mv-wait">{t(L("waiting…", "기다리는 중…"))}</span>}
      </div>
      <div className="mv-run">
        <input className="mv-input" value={typed[who]} placeholder="SQL" disabled={!ready}
          onChange={e => setTyped(p => ({ ...p, [who]: e.target.value }))}
          onKeyDown={e => { if (e.key === "Enter" && typed[who].trim()) { exec(who, typed[who]); setTyped(p => ({ ...p, [who]: "" })); } }} />
        <button className="mv-btn" disabled={!ready || !typed[who].trim()} onClick={() => { exec(who, typed[who]); setTyped(p => ({ ...p, [who]: "" })); }}>{t(L("Run", "실행"))}</button>
      </div>
      <div className="mv-quick">
        {["BEGIN", "COMMIT", "ROLLBACK"].map(q => <button key={q} className="mv-btn mv-small" disabled={!ready} onClick={() => exec(who, q)}>{q}</button>)}
      </div>
      <div className="mv-log">
        {log[who].length === 0 && <div className="mv-dim">{t(L("nothing run yet", "아직 실행한 문장이 없습니다"))}</div>}
        {log[who].map((e, i) => (
          <div key={i} className={`mv-entry ${e.ok ? "" : "mv-bad"}`}>
            <code>{e.sql}</code>
            <span>{e.text}</span>
            {e.table && (
              <table className="mv-mini"><thead><tr>{e.table.columns.map(c => <th key={c}>{c}</th>)}</tr></thead>
                <tbody>{e.table.rows.map((r, k) => <tr key={k}>{r.map((c, j) => <td key={j}>{c}</td>)}</tr>)}</tbody></table>
            )}
          </div>
        ))}
      </div>
    </div>
  );

  return (
    <div className="mv-page">
      <h1>{t(L("MVCC: two sessions and the versions between them", "MVCC: 두 세션과 그 사이의 행 버전"))}</h1>
      <p className="mv-sub">{t(L("Every change makes a new version of a row instead of overwriting it. Each version knows who made it (xmin) and who ended it (xmax); what a session sees is decided by its snapshot. Reset creates the table mvcc_demo in the database you are using, then step through a scenario or type SQL in either session.", "변경은 행을 덮어쓰지 않고 새 버전을 만듭니다. 버전마다 누가 만들었는지(xmin)와 누가 끝냈는지(xmax)가 적혀 있고, 세션에게 무엇이 보이는지는 그 세션의 스냅샷이 정합니다. 초기화는 지금 쓰는 데이터베이스에 mvcc_demo 표를 만듭니다. 시나리오를 한 단계씩 따라가거나 두 세션에 직접 SQL을 입력해 보세요."))}</p>

      <div className="mv-bar">
        <select className="mv-select" value={scenarioId} onChange={e => { setScenarioId(e.target.value); if (ready) reset(); }}>
          {SCENARIOS.map(s => <option key={s.id} value={s.id}>{t(s.title)}</option>)}
        </select>
        {scenarioId === "snapshot" && (
          <label className="mv-label">{t(L("A's level", "A의 수준"))}
            <select className="mv-select" value={iso} onChange={e => { setIso(e.target.value); if (ready) reset(); }}>
              <option>REPEATABLE READ</option><option>READ COMMITTED</option>
            </select>
          </label>
        )}
        <button className="mv-btn mv-primary" onClick={reset}>{ready ? t(L("↻ Reset demo", "↻ 처음부터")) : t(L("Create demo table", "실습 표 만들기"))}</button>
        <button className="mv-btn mv-primary" disabled={!ready || next >= steps.length || auto} onClick={() => runStep(next)}>{t(L("Next step ▶", "다음 단계 ▶"))}</button>
        <button className="mv-btn" disabled={!ready || next >= steps.length || auto} onClick={playAll}>{t(L("Run all", "끝까지"))}</button>
        <span className="mv-dim">{ready ? `${Math.min(next, steps.length)} / ${steps.length}` : ""}</span>
      </div>
      <p className="mv-about">{t(scenario.about)}</p>
      {error && <div className="mv-error">{error}</div>}
      {note && <div className="mv-note">{t(note)}</div>}
      {ready && next < steps.length && <div className="mv-next">{t(L("next:", "다음:"))} <b>{steps[next].s}</b> <code>{steps[next].sql}</code></div>}

      <div className="mv-grid">
        {session("A")}
        <div className="mv-store">
          <div className="mv-session-head"><b>{t(L("Versions of the rows", "행의 버전들"))}</b><span className="mv-dim">{TABLE}</span></div>
          <table className="mv-table">
            <thead><tr><th>id</th><th>{t(L("version", "버전"))}</th><th>xmin</th><th>xmax</th><th>{t(L("state", "상태"))}</th><th>A</th><th>B</th></tr></thead>
            <tbody>
              {versions.length === 0 && <tr><td colSpan={7} className="mv-dim">{t(L("Create the demo table to begin", "실습 표를 만들면 시작합니다"))}</td></tr>}
              {versions.map(v => (
                <tr key={`${v.key}|${v.xmin}`} className={`mv-st-${v.state}`}>
                  <td>{v.key}</td><td className="mv-data">{v.data.replace(/^id=\d+, /, "")}</td><td>{v.xmin}</td><td>{v.xmax === "0" ? "–" : v.xmax}</td>
                  <td><span className={`mv-state mv-${v.state}`}>{v.state}</span></td>
                  <td className={v.vis.A ? "mv-yes" : "mv-no"}>{v.vis.A ? "●" : "○"}</td>
                  <td className={v.vis.B ? "mv-yes" : "mv-no"}>{v.vis.B ? "●" : "○"}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <div className="mv-legend">
            <span><i className="mv-state mv-LIVE" />LIVE {t(L("the newest, committed", "가장 새롭고 커밋됨"))}</span>
            <span><i className="mv-state mv-UNCOMMITTED" />UNCOMMITTED {t(L("made by an open transaction", "열린 트랜잭션이 만듦"))}</span>
            <span><i className="mv-state mv-ENDING" />ENDING {t(L("an open transaction is ending it", "열린 트랜잭션이 끝내는 중"))}</span>
            <span><i className="mv-state mv-DEAD" />DEAD {t(L("ended and committed, kept for old snapshots until VACUUM", "끝났고 커밋됨, 옛 스냅샷을 위해 VACUUM 전까지 남음"))}</span>
            <span>● {t(L("the session sees this version", "그 세션에 보이는 버전"))}</span>
          </div>
        </div>
        {session("B")}
      </div>
    </div>
  );
}

export default function MvccView() {
  const [lang, setLang] = useState<Lang>(loadLang);
  const pick = (next: Lang) => {
    setLang(next);
    try { localStorage.setItem(LANG_KEY, next); } catch { /* not saved */ }
  };
  return (
    <LangContext.Provider value={lang}>
      <div className="mv-scroll">
        <div className="mv-lang">
          <button className={lang === "ko" ? "on" : ""} onClick={() => pick("ko")}>KO</button>
          <button className={lang === "en" ? "on" : ""} onClick={() => pick("en")}>EN</button>
        </div>
        <Body />
      </div>
    </LangContext.Provider>
  );
}
