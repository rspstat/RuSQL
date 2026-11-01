// IndexAdvice.tsx
// The index advice under the plan of an EXPLAIN: which index would let the statement stop scanning, why, how many rows it would read with it. The engine does the reading
// (`SUGGEST INDEX <statement>`); a SELECT can also be tried -- the index is built under a temporary name, the plan is timed before and after, and the index is dropped again.

import { useEffect, useRef, useState } from "react";
import type { QueryResult } from "../types";
import type { Advice } from "../lib/indexAdvice";
import { describeTrial, executionMs, median, parseAdvice, trialIndex } from "../lib/indexAdvice";
import "./IndexAdvice.css";

const TRIAL_RUNS = 3;       // plans timed before and after (the median is kept)
const TRIAL_MAX_ROWS = 200000; // a table above this is not worth an index built for a trial

export default function IndexAdvice({ sql, run, onInsert }: {
  sql: string;                                       // the statement the plan is of
  run: (query: string) => Promise<QueryResult[]>;    // runs statements; the answer of each
  onInsert: (text: string) => void;                  // puts a statement into the editor
}) {
  const [advice, setAdvice] = useState<Advice | null>(null);
  const [error, setError] = useState("");
  const [trial, setTrial] = useState<Record<number, string>>({});
  const [busy, setBusy] = useState(false);
  const runRef = useRef(run);
  runRef.current = run;
  const isSelect = /^SELECT\b/i.test(sql.trim());

  useEffect(() => {
    let cancelled = false;
    setAdvice(null);
    setError("");
    setTrial({});
    runRef.current(`SUGGEST INDEX ${sql}`)
      .then(results => {
        if (cancelled) return;
        const r = results[0];
        if (!r || !r.success) setError(r?.message ?? "No answer");
        else setAdvice(parseAdvice(r.columns, r.rows));
      })
      .catch(e => { if (!cancelled) setError(String(e)); });
    return () => { cancelled = true; };
  }, [sql]);

  // the time of the plan of the statement, as the median of a few runs (a comment makes each its own statement, so that no cached answer is timed)
  const timed = async (): Promise<number | null> => {
    const times: number[] = [];
    for (let k = 0; k < TRIAL_RUNS; k++) {
      const [plan] = await runRef.current(`EXPLAIN ANALYZE ${sql} /* try ${k} ${Date.now()} */`);
      if (!plan || !plan.success) return null;
      const ms = executionMs(plan.rows.map(row => (row[0] ?? "").trim()));
      if (ms === null) return null;
      times.push(ms);
    }
    return median(times);
  };

  const tryIt = async (i: number) => {
    if (!advice) return;
    const s = advice.suggestions[i];
    const index = trialIndex(s, i + 1);
    setBusy(true);
    setTrial(t => ({ ...t, [i]: "Trying…" }));
    let built = false;
    try {
      const before = await timed();
      const [made] = await runRef.current(index.create);
      if (!made?.success) { setTrial(t => ({ ...t, [i]: `Could not build the index: ${made?.message ?? ""}` })); return; }
      built = true;
      const after = await timed();
      setTrial(t => ({ ...t, [i]: before !== null && after !== null ? describeTrial(before, after) : "Could not time the plan" }));
    } catch (e) {
      setTrial(t => ({ ...t, [i]: String(e) }));
    } finally {
      if (built) await runRef.current(index.drop).catch(() => undefined);
      setBusy(false);
    }
  };

  if (error) return <div className="ia"><div className="ia-title">INDEX ADVICE</div><div className="ia-note">{error}</div></div>;
  if (!advice) return <div className="ia"><div className="ia-title">INDEX ADVICE</div><div className="ia-note">Reading the statement…</div></div>;
  return (
    <div className="ia">
      <div className="ia-title">INDEX ADVICE</div>
      {advice.suggestions.map((s, i) => (
        <div key={s.create} className="ia-card">
          <div className="ia-sql">{s.create}</div>
          <div className="ia-why">
            {s.why} — {s.table}: {s.rowsInTable.toLocaleString()} rows{s.rowsRead !== null ? `, about ${s.rowsRead.toLocaleString()} read with the index` : ""}
          </div>
          <div className="ia-actions">
            <button className="ia-btn" onClick={() => onInsert(s.create)}>Insert into editor</button>
            <button className="ia-btn" onClick={() => { navigator.clipboard?.writeText(s.create).catch(() => undefined); }}>Copy</button>
            {isSelect && s.rowsInTable <= TRIAL_MAX_ROWS && (
              <button className="ia-btn primary" disabled={busy} onClick={() => tryIt(i)}>Try it</button>
            )}
            {trial[i] && <span className="ia-trial">{trial[i]}</span>}
          </div>
        </div>
      ))}
      {advice.already.map(a => (
        <div key={a.table + a.columns} className="ia-line ok">✓ {a.table}({a.columns}): {a.detail}</div>
      ))}
      {advice.notes.map(n => <div key={n} className="ia-line">{n}</div>)}
    </div>
  );
}
