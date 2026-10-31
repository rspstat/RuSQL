// CompatView.tsx
// "Compat" activity-bar page: how closely RuSQL answers like MySQL 8.4. Every corpus (a file of SQL statements) is run on RuSQL and on a real MySQL, and the
// answers are compared; the bars show how many statements came out the same. The numbers are a snapshot written by code/test/diff/make_scoreboard.py into
// src/lib/compatSnapshot.json. Every visible string is an English/Korean pair with the same EN/KO toggle as the Diagram page (kept under the same key).

import { useState } from "react";
import { C, L, LangContext, tint, useT } from "./diagramShared";
import type { Lang, Text } from "./diagramShared";
import { CountUp, PageHero, rise } from "./PageKit";
import data from "../lib/compatSnapshot.json";
import "./CompatView.css";

const LANG_KEY = "rusql_diagram_lang";

function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

interface Corpus { name: string; en: string; ko: string; statements: number; different: number; bothFail: number }

const KNOWN: { en: string; ko: string }[] = [
  { en: "GROUPING(), the companion of GROUP BY ... WITH ROLLUP, is not there yet", ko: "GROUP BY ... WITH ROLLUP의 짝인 GROUPING()은 아직 없음" },
  { en: "PI() alone shows six decimals as in MySQL, but an expression made from it (PI() + 0) shows all its digits (MySQL: six)", ko: "PI() 단독은 MySQL처럼 소수 여섯 자리지만 PI()에서 만든 식(PI() + 0)은 전체 자릿수 (MySQL: 여섯 자리)" },
  { en: "A FLOAT given to a text function shows the stored digits (CONCAT(f, '') is 3.1415927, MySQL: 3.14159)", ko: "FLOAT를 글자 함수에 넣으면 저장된 자릿수 (CONCAT(f, '')가 3.1415927, MySQL: 3.14159)" },
  { en: "A column that two tables of a chain of USING joins both have is not refused as ambiguous (MySQL: error 1052)", ko: "이어지는 USING 조인에서 두 표가 다 가진 열을 모호하다고 거절하지 않음 (MySQL: 오류 1052)" },
  { en: "SHOW TABLE STATUS: Create_time and Update_time are NULL, and the sizes of a large table are an estimate in 16 KB pages", ko: "SHOW TABLE STATUS: Create_time·Update_time은 NULL이고 큰 표의 크기는 16 KB 쪽으로 추정한 값" },
  { en: "SHOW ENGINES lists MyISAM, MEMORY ... as MySQL does, but RuSQL has one InnoDB-like engine (ENGINE= is accepted and ignored) and no XA statements", ko: "SHOW ENGINES는 MySQL처럼 MyISAM·MEMORY 등을 보이지만 RuSQL의 엔진은 InnoDB 방식 하나뿐(ENGINE=은 받아서 무시)이고 XA 문장은 없음" },
  { en: "long_query_time is one value for the whole server (no per-session value), and SET slow_query_log without GLOBAL is accepted", ko: "long_query_time은 서버 전체에 하나(세션 값 없음)이고 GLOBAL 없는 SET slow_query_log도 받음" },
  { en: "A view that mixes an aggregate with a plain column is refused when it is created (MySQL refuses when it is selected)", ko: "집계와 일반 열이 섞인 뷰는 만들 때 거절 (MySQL은 조회할 때 거절)" },
];

function pct(part: number, whole: number): string {
  return whole === 0 ? "0" : ((part / whole) * 100).toFixed(1);
}

// the ring: a circle of RADIUS whose stroke is drawn for `share` percent of its length (it fills when the page opens)
const RADIUS = 52;
const FULL = 2 * Math.PI * RADIUS;

function Ring({ share }: { share: number }) {
  return (
    <svg className="cp-ring" viewBox="0 0 128 128" aria-hidden="true">
      <defs>
        <linearGradient id="cp-ring-grad" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stopColor="#4ec9b0" />
          <stop offset="100%" stopColor="#9cdcfe" />
        </linearGradient>
      </defs>
      <circle className="cp-ring-track" cx="64" cy="64" r={RADIUS} />
      <circle className="cp-ring-fill" cx="64" cy="64" r={RADIUS} transform="rotate(-90 64 64)"
        style={{ ["--full" as string]: FULL, strokeDasharray: FULL, strokeDashoffset: FULL * (1 - share / 100) }} />
    </svg>
  );
}

function Body({ lang, onLang }: { lang: Lang; onLang: (l: Lang) => void }) {
  const t = useT();
  const corpora = data.corpora as Corpus[];
  const total = corpora.reduce((n, c) => n + c.statements, 0);
  const different = corpora.reduce((n, c) => n + c.different, 0);
  const same = total - different;
  const bothFail = corpora.reduce((n, c) => n + c.bothFail, 0);
  const tests = data.tests as { cases: number; assertions: number } | null;
  const share = total === 0 ? 0 : (same / total) * 100;

  // each stat has a color of its own (the layer colors of the Diagram page)
  const stats: { n: number; format?: (n: number) => string; label: Text; c: string }[] = [
    { n: total, label: L("statements compared", "비교한 문장"), c: C.server },
    { n: bothFail, label: L("fail on both with an error", "둘 다 같은 오류로 거절"), c: C.txn },
    { n: data.conformanceCases, label: L("conformance cases in the C++ tests", "C++ 시험의 MySQL 대조 사례"), c: C.client },
    ...(tests ? [{ n: tests.assertions, format: (n: number) => `${(n / 1e6).toFixed(2)}M`, label: L(`test assertions (${tests.cases} cases)`, `시험 assertions (${tests.cases} 케이스)`), c: C.core }] : []),
    { n: data.bugsFound, label: L("bugs found by comparing", "비교로 찾아 고친 버그"), c: C.disk },
    { n: KNOWN.length, label: L("known differences left (below)", "남은 알려진 차이 (아래)"), c: C.storage },
  ];

  return (
    <div className="cp-page pk-page">
      <PageHero accent={C.server} lang={lang} onLang={onLang}
        icon={<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9.5" /><path d="M7.6 12.6l3 3 5.8-6.6" /></svg>}
        title={L("MySQL compatibility", "MySQL 호환성")}
        lead={L(`Every corpus is run on RuSQL and on a real MySQL ${data.mysql}; a statement counts as the same when both give the same rows, the same error or the same OK. Snapshot of ${data.generated}.`,
                `코퍼스마다 RuSQL과 진짜 MySQL ${data.mysql}에서 같은 문장을 돌려, 같은 행·같은 오류·같은 OK면 "같다"고 셉니다. ${data.generated} 시점의 스냅샷.`)} />

      <div className="cp-summary">
        <div className="cp-gauge pk-card pk-rise" style={{ ...tint(C.server), ...rise(1) }}>
          <div className="cp-gauge-ring">
            <Ring share={share} />
            <div className="cp-gauge-num"><CountUp value={share} format={n => n.toFixed(1)} /><small>%</small></div>
          </div>
          <div className="cp-gauge-label">{t(L("answer exactly like MySQL", "MySQL과 똑같이 답함"))}</div>
          <div className="cp-gauge-sub">{same.toLocaleString()} / {total.toLocaleString()}</div>
        </div>
        <div className="cp-stats">
          {stats.map((s, i) => (
            <div className="cp-stat pk-card pk-rise" key={i} style={{ ...tint(s.c), ...rise(2 + i) }}>
              <div className="cp-num"><CountUp value={s.n} format={s.format} /></div>
              <div className="cp-label">{t(s.label)}</div>
            </div>
          ))}
        </div>
      </div>

      <h2 className="pk-caption cp-h2 pk-rise" style={{ ...tint(C.server), ...rise(6) }}>
        <span className="pk-num" style={{ ["--k" as string]: 0 }}>1</span>
        {t(L("Per corpus", "코퍼스별"))}
        <em>{corpora.length}</em>
      </h2>
      <div className="cp-rows">
        {corpora.map((c, i) => {
          const ok = c.statements - c.different;
          const perfect = c.different === 0;
          return (
            <div className={`cp-row ${perfect ? "cp-perfect" : "cp-flawed"}`} key={c.name} style={{ ...tint(perfect ? C.server : "#e06c75"), ["--i" as string]: i }}>
              <div className="cp-name">{c.name}</div>
              <div className="cp-what">{t(L(c.en, c.ko))}</div>
              <div className="cp-bar" title={`${ok} / ${c.statements}`}>
                <div className="cp-same" style={{ width: pct(ok, c.statements) + "%" }}><i /></div>
                {c.different > 0 && <div className="cp-diff" style={{ width: pct(c.different, c.statements) + "%" }} />}
              </div>
              <div className="cp-count">{ok}/{c.statements}</div>
              <div className="cp-mark" aria-hidden="true">{perfect ? "✓" : "!"}</div>
            </div>
          );
        })}
      </div>

      <h2 className="pk-caption cp-h2 pk-rise" style={{ ...tint(C.txn), ...rise(7) }}>
        <span className="pk-num" style={{ ["--k" as string]: 1 }}>2</span>
        {t(L("Known differences", "알려진 차이"))}
        <em>{KNOWN.length}</em>
      </h2>
      <div className="cp-known">
        {KNOWN.map((k, i) => (
          <div className="cp-known-item pk-card pk-rise" key={i} style={{ ...tint(C.txn), ...rise(8 + i * 0.5) }}>
            <span className="cp-known-n">{i + 1}</span>
            <span>{t(L(k.en, k.ko))}</span>
          </div>
        ))}
      </div>
      <p className="cp-foot">{t(L("Regenerate: python code/test/diff/make_scoreboard.py <engine_server.exe>", "다시 만들기: python code/test/diff/make_scoreboard.py <engine_server.exe>"))}</p>
    </div>
  );
}

export default function CompatView() {
  const [lang, setLang] = useState<Lang>(loadLang);
  const pick = (next: Lang) => {
    setLang(next);
    try { localStorage.setItem(LANG_KEY, next); } catch { /* not saved */ }
  };
  return (
    <LangContext.Provider value={lang}>
      <div className="pk-scroll cp-scroll" style={tint(C.server)}>
        <Body lang={lang} onLang={pick} />
      </div>
    </LangContext.Provider>
  );
}
