// CompatView.tsx
// "Compat" activity-bar page: how closely RuSQL answers like MySQL 8.4. Every corpus (a file of SQL statements) is run on RuSQL and on a real MySQL, and the
// answers are compared; the bars show how many statements came out the same. The numbers are a snapshot written by code/test/diff/make_scoreboard.py into
// src/lib/compatSnapshot.json. Every visible string is an English/Korean pair with the same EN/KO toggle as the Diagram page (kept under the same key).

import { useState } from "react";
import { L, LangContext, useT } from "./diagramShared";
import type { Lang, Text } from "./diagramShared";
import data from "../lib/compatSnapshot.json";
import "./CompatView.css";

const LANG_KEY = "rusql_diagram_lang";

function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

interface Corpus { name: string; en: string; ko: string; statements: number; different: number; bothFail: number }

const KNOWN: { en: string; ko: string }[] = [
  { en: "REPLACE of a row that is already identical counts 2 changed rows (MySQL: 1)", ko: "이미 같은 행을 REPLACE하면 바뀐 행이 2 (MySQL: 1)" },
  { en: "SUM / AVG over a DATE column reads the leading number of the text", ko: "DATE 열 그대로의 SUM / AVG는 글자 앞부분의 수를 읽음" },
  { en: "An UPDATE that shifts a UNIQUE key is let through when the WHERE's index order is reversed", ko: "WHERE가 고른 인덱스 순서가 거꾸로이면 UNIQUE 키를 밀어 옮기는 UPDATE를 통과시킴" },
  { en: "FLOAT arithmetic reads the stored digits, not the exact single-precision value; f = 0.1 is true here", ko: "FLOAT 산술은 단정밀도 정확한 값이 아니라 저장된 자릿수를 읽음; f = 0.1이 여기서는 참" },
  { en: "A subquery on the table being updated is allowed (MySQL: error 1093)", ko: "UPDATE 대상 표를 하위 질의에서 읽는 것을 허용 (MySQL: 오류 1093)" },
  { en: "SHOW TABLE STATUS gives only the name, engine and rows; SHOW ENGINES lists one engine", ko: "SHOW TABLE STATUS는 이름·엔진·행 수만, SHOW ENGINES는 엔진 하나" },
];

function pct(part: number, whole: number): string {
  return whole === 0 ? "0" : ((part / whole) * 100).toFixed(1);
}

function Body() {
  const t = useT();
  const corpora = data.corpora as Corpus[];
  const total = corpora.reduce((n, c) => n + c.statements, 0);
  const different = corpora.reduce((n, c) => n + c.different, 0);
  const same = total - different;
  const bothFail = corpora.reduce((n, c) => n + c.bothFail, 0);
  const tests = data.tests as { cases: number; assertions: number } | null;

  const stat = (value: string, label: Text) => (
    <div className="cp-stat"><div className="cp-num">{value}</div><div className="cp-label">{t(label)}</div></div>
  );

  return (
    <div className="cp-page">
      <h1>{t(L("MySQL compatibility", "MySQL 호환성"))}</h1>
      <p className="cp-sub">
        {t(L(`Every corpus is run on RuSQL and on a real MySQL ${data.mysql}; a statement counts as the same when both give the same rows, the same error or the same OK. Snapshot of ${data.generated}.`,
             `코퍼스마다 RuSQL과 진짜 MySQL ${data.mysql}에서 같은 문장을 돌려, 같은 행·같은 오류·같은 OK면 "같다"고 셉니다. ${data.generated} 시점의 스냅샷.`))}
      </p>

      <div className="cp-stats">
        {stat(total.toLocaleString(), L("statements compared", "비교한 문장"))}
        {stat(pct(same, total) + "%", L("answer exactly like MySQL", "MySQL과 똑같이 답함"))}
        {stat(bothFail.toLocaleString(), L("fail on both with an error", "둘 다 같은 오류로 거절"))}
        {stat(data.conformanceCases.toLocaleString(), L("conformance cases in the C++ tests", "C++ 시험의 MySQL 대조 사례"))}
        {tests && stat(`${tests.cases} / ${(tests.assertions / 1e6).toFixed(2)}M`, L("test cases / assertions", "시험 케이스 / assertions"))}
        {stat(String(data.bugsFound), L("bugs found by comparing", "비교로 찾아 고친 버그"))}
      </div>

      <h2>{t(L("Per corpus", "코퍼스별"))}</h2>
      <div className="cp-rows">
        {corpora.map(c => {
          const ok = c.statements - c.different;
          return (
            <div className="cp-row" key={c.name}>
              <div className="cp-name">{c.name}</div>
              <div className="cp-what">{t(L(c.en, c.ko))}</div>
              <div className="cp-bar" title={`${ok} / ${c.statements}`}>
                <div className="cp-same" style={{ width: pct(ok, c.statements) + "%" }} />
                {c.different > 0 && <div className="cp-diff" style={{ width: pct(c.different, c.statements) + "%" }} />}
              </div>
              <div className="cp-count">{ok}/{c.statements}</div>
            </div>
          );
        })}
      </div>

      <h2>{t(L("Known differences", "알려진 차이"))}</h2>
      <ul className="cp-known">
        {KNOWN.map((k, i) => <li key={i}>{t(L(k.en, k.ko))}</li>)}
      </ul>
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
      <div className="cp-scroll">
        <div className="cp-lang">
          <button className={lang === "ko" ? "on" : ""} onClick={() => pick("ko")}>KO</button>
          <button className={lang === "en" ? "on" : ""} onClick={() => pick("en")}>EN</button>
        </div>
        <Body />
      </div>
    </LangContext.Provider>
  );
}
