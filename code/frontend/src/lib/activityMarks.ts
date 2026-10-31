// activityMarks.ts
// What the Sessions panel marks: which session blocks others (its open transaction holds the row lock that another transaction waits for), which one is waiting, which
// statement has been running for long, and which session sits idle with a transaction open. The rows are the ones information_schema gives (processlist, innodb_trx,
// innodb_lock_waits); innodb_trx.TRX_MYSQL_THREAD_ID is what ties a transaction to its session. No DOM here, so that the marking can be tested on its own.

export interface SessionRow { id: string; user: string; db: string; command: string; seconds: number; info: string }
export interface TrxRow { id: string; state: string; threadId: string; rowsLocked: number; waitingFor: string; ageSeconds: number | null }
export interface WaitRow { requesting: string; blocking: string }

export interface SessionMarks {
  blocking: number;               // how many transactions wait, directly, for this session's transaction
  rootBlocker: boolean;           // it blocks and does not wait itself: ending it frees the others
  waitingForSession: string | null; // the session whose transaction this session waits for
  slow: "warn" | "hot" | null;    // a statement that has run for long
  idleInTransaction: number | null; // seconds a transaction has been open while the session does nothing
  own: boolean;                   // the statement the panel itself asks every 1.5 s: not for KILL
}

export interface Thresholds { slowSeconds: number; verySlowSeconds: number; idleTransactionSeconds: number }
export const DEFAULT_THRESHOLDS: Thresholds = { slowSeconds: 5, verySlowSeconds: 30, idleTransactionSeconds: 30 };

// the statement the panel asks for the list of sessions: the session that is running it, while the list is made, is the app's own connection (the panel finds it that way,
// each time, and keeps its id: KILL is not offered for it)
export function isOwnStatement(info: string): boolean {
  return /^SELECT ID, USER, HOST/i.test(info.trim());
}

export function ownSessionId(sessions: SessionRow[]): string | null {
  return sessions.find(s => isOwnStatement(s.info))?.id ?? null;
}

export function markSessions(sessions: SessionRow[], transactions: TrxRow[], waits: WaitRow[], t: Thresholds = DEFAULT_THRESHOLDS, ownId: string | null = null): Record<string, SessionMarks> {
  const sessionOfTrx = new Map<string, string>();
  for (const trx of transactions) if (trx.threadId && trx.threadId !== "0") sessionOfTrx.set(trx.id, trx.threadId);
  const marks: Record<string, SessionMarks> = {};
  for (const s of sessions) {
    marks[s.id] = { blocking: 0, rootBlocker: false, waitingForSession: null, slow: null, idleInTransaction: null, own: s.id === ownId || isOwnStatement(s.info) };
    if (s.command === "Query" && !marks[s.id].own) marks[s.id].slow = s.seconds >= t.verySlowSeconds ? "hot" : s.seconds >= t.slowSeconds ? "warn" : null;
  }
  for (const w of waits) {
    const blocker = sessionOfTrx.get(w.blocking), waiter = sessionOfTrx.get(w.requesting);
    if (blocker && marks[blocker]) marks[blocker].blocking++;
    if (waiter && marks[waiter] && blocker) marks[waiter].waitingForSession = blocker;
  }
  for (const m of Object.values(marks)) m.rootBlocker = m.blocking > 0 && m.waitingForSession === null;
  for (const trx of transactions) {
    const owner = sessionOfTrx.get(trx.id);
    const session = sessions.find(s => s.id === owner);
    if (owner && marks[owner] && session && session.command === "Sleep" && trx.ageSeconds !== null && trx.ageSeconds >= t.idleTransactionSeconds) {
      marks[owner].idleInTransaction = trx.ageSeconds;
    }
  }
  return marks;
}

export function duration(seconds: number): string {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`;
  return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
}
