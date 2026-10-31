// MvccView.tsx
// "MVCC" activity-bar page: one short story that plays by itself, over and over -- session A changes a value, session B reads it before and after A commits, and the old
// version is cleaned up. Three boxes (session A, the versions of the row, session B) joined by arrows; each step lights the arrows it is about and says one sentence. Nothing
// to type: only pause / play and start over. The story is written out below (no engine is involved); every visible string is an English/Korean pair with the same EN/KO
// toggle as the Diagram page. The look is PageKit.css + MvccView.css.

import { Fragment, useMemo, useState } from "react";
import { ArrowDefs, C, FlowArrow, L, LangContext, tint, useT } from "./diagramShared";
import type { Lang, Text } from "./diagramShared";
import { PageHero, PlayBar, rise, usePlayer } from "./PageKit";
import "./MvccView.css";

const LANG_KEY = "rusql_diagram_lang";
function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

type Who = "A" | "B";
type State = "off" | "live" | "uncommitted" | "ending" | "dead";
interface Version { value: number; state: State; seen: Record<Who, boolean> }
// who acts (none for the first picture), its statement and what it answers, the sentence, how long it stays, the two versions of the row, and the rows of arrows that light up
// (the arrows from A point at the versions, the arrows to B point at B: A only writes here and B only reads)
interface Step { who: Who | null; sql?: string; answer?: string; caption: Text; ms: number; versions: [Version, Version]; wireA: number[]; wireB: number[] }

const gone: Version = { value: 150, state: "off", seen: { A: false, B: false } };
const both = { A: true, B: true }, onlyA = { A: true, B: false }, onlyB = { A: false, B: true }, nobody = { A: false, B: false };

const STEPS: Step[] = [
  { who: null, caption: L("To begin, the row has one version: balance = 100", "처음에는 행 하나에 버전 하나가 있습니다 (balance = 100)"), ms: 2400,
    versions: [{ value: 100, state: "live", seen: both }, gone], wireA: [], wireB: [] },
  { who: "A", sql: "BEGIN", caption: L("A opens a transaction", "A가 트랜잭션을 엽니다"), ms: 1700,
    versions: [{ value: 100, state: "live", seen: both }, gone], wireA: [], wireB: [] },
  { who: "A", sql: "UPDATE accounts SET balance = 150", caption: L("A does not overwrite: it adds a new version, and the old one stays", "A는 덮어쓰지 않고 새 버전을 만듭니다. 옛 버전은 그대로 남습니다"), ms: 3400,
    versions: [{ value: 100, state: "ending", seen: onlyB }, { value: 150, state: "uncommitted", seen: onlyA }], wireA: [0, 1], wireB: [] },
  { who: "B", sql: "SELECT balance FROM accounts", answer: "100", caption: L("B cannot see a version that is not committed yet, so it reads 100", "B에게는 아직 커밋되지 않은 새 버전이 보이지 않아서 100을 읽습니다"), ms: 3400,
    versions: [{ value: 100, state: "ending", seen: onlyB }, { value: 150, state: "uncommitted", seen: onlyA }], wireA: [], wireB: [0] },
  { who: "A", sql: "COMMIT", caption: L("A commits: the new version is the current one, the old one is now old", "A가 커밋합니다. 새 버전이 현재 값이 되고 옛 버전은 옛 버전이 됩니다"), ms: 3000,
    versions: [{ value: 100, state: "dead", seen: nobody }, { value: 150, state: "live", seen: both }], wireA: [1], wireB: [] },
  { who: "B", sql: "SELECT balance FROM accounts", answer: "150", caption: L("Now B reads the new version: 150", "이제 B는 새 버전을 읽습니다: 150"), ms: 2800,
    versions: [{ value: 100, state: "dead", seen: nobody }, { value: 150, state: "live", seen: both }], wireA: [], wireB: [1] },
  { who: "A", sql: "VACUUM accounts", caption: L("Nobody can need the old version any more: VACUUM removes it", "옛 버전은 이제 아무도 필요로 하지 않으므로 VACUUM이 치웁니다"), ms: 3200,
    versions: [{ value: 100, state: "off", seen: nobody }, { value: 150, state: "live", seen: both }], wireA: [0], wireB: [] },
];

const STATE_LABEL: Record<Exclude<State, "off">, Text> = {
  live: L("current", "현재 값"),
  uncommitted: L("not committed", "커밋 전"),
  ending: L("being replaced", "교체되는 중"),
  dead: L("old", "옛 버전"),
};

// each session has a color of its own (A the clients' blue, B the storage layer's pink), the row between them the transactions' amber
const WHO_COLOR: Record<Who, string> = { A: C.client, B: C.storage };

function Body({ lang, onLang }: { lang: Lang; onLang: (l: Lang) => void }) {
  const t = useT();
  const durations = useMemo(() => STEPS.map(s => s.ms), []);
  const player = usePlayer(durations);
  const step = STEPS[player.cur];

  // what each session last said (it stays on its box until it says something else) and whether its transaction is open
  const sessions = (["A", "B"] as Who[]).map(who => {
    let sql = "", answer = "", open = false;
    for (let i = 0; i <= player.cur; i++) {
      const s = STEPS[i];
      if (s.who !== who) continue;
      sql = s.sql ?? ""; answer = s.answer ?? "";
      if (s.sql === "BEGIN") open = true;
      if (s.sql === "COMMIT") open = false;
    }
    return { who, sql, answer, open, active: step.who === who };
  });

  const session = (s: typeof sessions[number]) => (
    <div className={`mv-session pk-card pk-rise ${s.active ? "mv-active" : ""}`} style={{ ...tint(WHO_COLOR[s.who]), ...rise(s.who === "A" ? 1 : 3), gridColumn: s.who === "A" ? 1 : 5, gridRow: "1 / 3" }}>
      <div className="mv-session-head">
        <span className="mv-avatar">{s.who}</span>
        <b>{t(L(`Session ${s.who}`, `세션 ${s.who}`))}</b>
        <span className={`mv-chip ${s.open ? "mv-on" : ""}`}>{s.open ? t(L("transaction open", "트랜잭션 열림")) : t(L("no transaction", "트랜잭션 없음"))}</span>
      </div>
      <div className="mv-said">
        {s.sql
          ? <div className="mv-sql" key={s.sql + player.cur}><code>{s.sql}</code>{s.answer && <span className="mv-answer">→ {s.answer}</span>}</div>
          : <div className="mv-dim">{t(L("nothing run yet", "아직 아무것도 실행하지 않았습니다"))}</div>}
      </div>
    </div>
  );

  const wire = (who: Who, row: number) => (
    <div className={`mv-wire ${(who === "A" ? step.wireA : step.wireB).includes(row) ? "mv-hot" : ""}`}
      style={{ gridColumn: who === "A" ? 2 : 4, gridRow: row + 1 }}>
      <FlowArrow c={WHO_COLOR[who]} dir="right" phase={row} />
    </div>
  );

  const version = (v: Version, row: number) => (
    <div className="mv-slot" style={{ gridColumn: 3, gridRow: row + 1 }}>
      <div className={`mv-ver mv-st-${v.state}`}>
        <span className="mv-value">balance = <b>{v.value}</b></span>
        {v.state !== "off" && <span className={`mv-state mv-${v.state}`}>{t(STATE_LABEL[v.state])}</span>}
        <span className="mv-seen">
          {(["A", "B"] as Who[]).map(who => <i key={who} className={`mv-eye ${v.seen[who] ? "on" : ""}`} style={tint(WHO_COLOR[who])} title={t(L(`Session ${who} sees this version`, `세션 ${who}에게 보이는 버전`))}>{who}</i>)}
        </span>
      </div>
    </div>
  );

  return (
    <div className="mv-page pk-page">
      <ArrowDefs />
      <PageHero accent={C.txn} lang={lang} onLang={onLang}
        icon={<svg viewBox="0 0 24 24"><path d="M12 3l9 4.5-9 4.5-9-4.5z" /><path d="M3 12l9 4.5 9-4.5" /><path d="M3 16.5L12 21l9-4.5" /></svg>}
        title={L("MVCC: versions of a row", "MVCC 시뮬레이션")}
        lead={L("A change does not overwrite a row: it adds a new version. Each session sees the version that is right for it, so a reader never waits for a writer.",
                "값을 바꾸면 덮어쓰지 않고 새 버전을 만듭니다. 세션마다 자기에게 맞는 버전을 보기 때문에, 읽는 쪽이 쓰는 쪽을 기다리지 않습니다.")} />

      <div className="mv-stage pk-rise" style={{ ...tint(C.txn), ...rise(1) }}>
        <div className="mv-grid">
          {session(sessions[0])}
          {version(step.versions[0], 0)}
          {version(step.versions[1], 1)}
          {session(sessions[1])}
          {[0, 1].map(row => <Fragment key={row}>{wire("A", row)}{wire("B", row)}</Fragment>)}
        </div>
        <div className="mv-msg" style={step.who ? tint(WHO_COLOR[step.who]) : undefined}>
          <span className="mv-badge">{step.who ? t(L(`Session ${step.who}`, `세션 ${step.who}`)) : t(L("Start", "시작"))}</span>
          <span className="mv-text" key={player.cur}>{t(step.caption)}</span>
        </div>
      </div>

      <div className="pk-rise" style={{ ...tint(C.txn), ...rise(2) }}>
        <PlayBar count={STEPS.length} cur={player.cur} playing={player.playing}
          onToggle={() => player.setPlaying(p => !p)} onReplay={player.replay} onJump={player.jump} />
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
      <div className="pk-scroll mv-scroll" style={tint(C.txn)}>
        <Body lang={lang} onLang={pick} />
      </div>
    </LangContext.Provider>
  );
}
