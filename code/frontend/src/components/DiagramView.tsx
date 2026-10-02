// DiagramView.tsx
// "Diagram" activity-bar page: one picture of the whole system -- who talks to RuSQL, the
// path a SQL statement takes through engine_server -> engine_core -> transactions ->
// storage -> durability, and where AI fits (Claude via MCP; no private NL-to-SQL model, see
// docs/mds/AI.md). Content mirrors README.md "Architecture" and docs/mds (FUNCTIONS.md,
// architecture-diagram.md). Every visible string is an English/Korean pair; the EN/KO
// toggle at the top switches them all at once and is remembered across visits. A second
// toggle next to it flips between this layered picture and the cylinder-and-arrow picture
// (DiagramCylinders.tsx).

import { useState } from "react";
import type { CSSProperties, ReactNode } from "react";
import DiagramCylinders from "./DiagramCylinders";
import { ArrowDefs, C, FlowArrow, L, LangContext, tint, useT } from "./diagramShared";
import type { Lang, Text } from "./diagramShared";
import "./DiagramView.css";

type View = "layers" | "cylinders";

const LANG_KEY = "rusql_diagram_lang";
const VIEW_KEY = "rusql_diagram_view";

// The left button of each toggle is the default: Korean for the language, cylinders for the view.
function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

function loadView(): View {
  try { return localStorage.getItem(VIEW_KEY) === "layers" ? "layers" : "cylinders"; } catch { return "cylinders"; }
}

type Item = { t: Text; s: Text };

// --d staggers the page-load animation by layer: the grid row (first number of "2 / 7") sets the delay.
const delay = (d: number): CSSProperties => ({ ["--d" as string]: d });
const at = (col: string, row: string): CSSProperties => ({ gridColumn: col, gridRow: row, ...delay(parseInt(row, 10)) });

// --k makes every badge of step n pulse at the same moment (ribbon, panel header, stage), step after step.
function Num({ n, c }: { n: number; c: string }) {
  return <span className="dg-num" style={{ ...tint(c), ["--k" as string]: n - 1 }}>{n}</span>;
}

// Vertical arrow between two layers (the shared arrow style); `label` sits on the line.
// Its packet phase comes from the grid cell, so neighbouring arrows don't pulse in unison.
function Wire({ label, c, dashed, style }: { label?: Text; c: string; dashed?: boolean; style?: CSSProperties }) {
  const t = useT();
  const phase = (parseInt(String(style?.gridRow), 10) || 0) + (parseInt(String(style?.gridColumn), 10) || 0) * 0.37;
  return (
    <div className="dg-wire" style={{ ...tint(c), ...style }}>
      <FlowArrow c={c} dashed={dashed} phase={phase} />
      {label && <span>{t(label)}</span>}
    </div>
  );
}

// Horizontal arrow between two pipeline stages.
function HArrow({ c, phase }: { c: string; phase: number }) {
  return <div className="dg-harrow"><FlowArrow c={c} dir="right" phase={phase} /></div>;
}

function Panel({ n, title, sub, c, style, children }: {
  n?: number; title: Text; sub: Text; c: string; style?: CSSProperties; children: ReactNode;
}) {
  const t = useT();
  return (
    <section className="dg-panel" style={{ ...tint(c), ...style }}>
      <header className="dg-panel-head">
        {n !== undefined && <Num n={n} c={c} />}
        <h2>{t(title)}</h2>
        <span>{t(sub)}</span>
      </header>
      {children}
    </section>
  );
}

function Chips({ items }: { items: Item[] }) {
  const t = useT();
  return (
    <div className="dg-chips">
      {items.map((i, k) => (
        <div key={k} className="dg-chip">
          <b>{t(i.t)}</b>
          <span>{t(i.s)}</span>
        </div>
      ))}
    </div>
  );
}

function Card({ c, title, sub, note, style }: { c: string; title: Text; sub: Text; note?: Text; style?: CSSProperties }) {
  const t = useT();
  return (
    <div className="dg-card" style={{ ...tint(c), ...style }}>
      <b>{t(title)}</b>
      <span>{t(sub)}</span>
      {note && <em>{t(note)}</em>}
    </div>
  );
}

const RIBBON: { n: number; label: Text; c: string }[] = [
  { n: 1, label: L("Client", "클라이언트"), c: C.client },
  { n: 2, label: L("Server", "서버"), c: C.server },
  { n: 3, label: L("Parse", "파싱"), c: C.core },
  { n: 4, label: L("Plan", "계획"), c: C.core },
  { n: 5, label: L("Execute", "실행"), c: C.core },
  { n: 6, label: L("Transactions", "트랜잭션"), c: C.txn },
  { n: 7, label: L("Storage", "스토리지"), c: C.storage },
  { n: 8, label: L("Durability", "내구성"), c: C.disk },
];

const SERVER: Item[] = [
  { t: L("Native protocol :7878", "네이티브 프로토콜 :7878"), s: L("UI, MCP and engine_client", "UI, MCP, engine_client") },
  {
    t: L("MySQL wire protocol :3306", "MySQL 와이어 프로토콜 :3306"),
    s: L("mysql CLI, DBeaver, JDBC; COM_CHANGE_USER for pool drivers", "mysql CLI, DBeaver, JDBC, 풀 드라이버용 COM_CHANGE_USER"),
  },
  {
    t: L("Challenge-response auth", "챌린지-응답 인증"),
    s: L("mysql_native_password style, password never sent in plaintext", "mysql_native_password 방식, 비밀번호를 평문으로 보내지 않음"),
  },
  {
    t: L("One session per connection", "연결당 세션 1개"),
    s: L("own Executor over the shared database (RwLock)", "공유 데이터베이스(RwLock) 위에서 세션마다 독립 Executor"),
  },
];

const SIDE_MODULES: Item[] = [
  { t: L("Query result cache", "쿼리 결과 캐시"), s: L("LRU-512, invalidated on DML and COMMIT", "LRU 512개, DML·COMMIT 시 무효화") },
  {
    t: L("Thread pool", "스레드 풀"),
    s: L("parallel scan, GROUP BY, ORDER BY, hash-join probe", "병렬 스캔, GROUP BY, ORDER BY, 해시 조인 probe"),
  },
  {
    t: L("Statistics", "통계"),
    s: L("ANALYZE: equi-depth histogram + most-common values", "ANALYZE: 등깊이 히스토그램 + 최빈값(MCV)"),
  },
  {
    t: L("Catalog", "카탈로그"),
    s: L("schemas, views, users / roles / grants, procedures, triggers", "스키마, 뷰, 사용자·역할·권한, 프로시저, 트리거"),
  },
];

const TXN: Item[] = [
  {
    t: L("Row-level MVCC", "행 단위 MVCC"),
    s: L("_xmin / _xmax version chains, snapshot visibility, GC-horizon VACUUM", "_xmin / _xmax 버전 체인, 스냅샷 가시성, GC 호라이즌 VACUUM"),
  },
  {
    t: L("4 isolation levels", "격리 수준 4단계"),
    s: L("READ UNCOMMITTED to SERIALIZABLE, genuinely distinct", "READ UNCOMMITTED ~ SERIALIZABLE, 실제로 서로 다르게 동작"),
  },
  {
    t: L("Lock manager", "락 매니저"),
    s: L("row locks, FOR UPDATE / SHARE, blocking wait, deadlock detection", "행 락, FOR UPDATE / SHARE, 블로킹 대기, 데드락 감지"),
  },
  { t: L("Gap lock + SSI", "Gap 락 + SSI"), s: L("phantom prevention; non-blocking predicate locks", "팬텀 방지, 비블로킹 predicate 락") },
  {
    t: L("Transaction manager", "트랜잭션 매니저"),
    s: L("BEGIN / COMMIT / ROLLBACK, SAVEPOINT, group commit", "BEGIN / COMMIT / ROLLBACK, SAVEPOINT, 그룹 커밋"),
  },
];

const STORAGE: Item[] = [
  {
    t: L("B+Tree indexes", "B+Tree 인덱스"),
    s: L("single, composite, clustered; keys compare as numbers (7 = 7.00); maintained incrementally, one bucket write per statement",
         "단일·복합·클러스터드, 키를 숫자로 비교(7 = 7.00), 증분 갱신(문장당 버킷 1회 기록)"),
  },
  { t: L("Hash index", "해시 인덱스"), s: L("USING HASH, O(1) equality lookups, bucketed by numeric value", "USING HASH, 등호 조회 O(1), 숫자 값 기준 버킷") },
  {
    t: L("Table data", "테이블 데이터"),
    s: L("in-memory MVCC row versions + the pk-to-position cache that turns an index hit into its row", "메모리 상의 MVCC 행 버전 + 인덱스 결과를 실제 행으로 연결하는 PK→위치 캐시"),
  },
  { t: L("Buffer pool", "버퍼 풀"), s: L("LRU, 64 pages x 16 KB", "LRU, 16KB 페이지 64개") },
  { t: L("Table files", "테이블 파일"), s: L("binary .rdb with LZ4 compression", "LZ4 압축 바이너리 .rdb") },
];

const DURABILITY: Item[] = [
  {
    t: L("Redo log", "Redo 로그"),
    s: L("a commit appends one checksummed batch, group-commit fsync", "커밋 = 체크섬 붙은 배치 1개 추가 + 그룹 커밋 fsync"),
  },
  {
    t: "WAL",
    s: L("binary write-ahead log, FNV-1a record checksums; COMMIT never re-reads it (which transactions are in the file is tracked in memory)",
         "바이너리 선행 기록 로그, FNV-1a 레코드 체크섬, 어떤 트랜잭션이 들어 있는지 메모리에서 추적해 COMMIT이 다시 읽지 않음"),
  },
  { t: L("Undo log", "Undo 로그"), s: L("per-transaction undo records for rollback", "롤백용 트랜잭션별 undo 레코드") },
  {
    t: L("Checkpoint", "체크포인트"),
    s: L("redo past a limit that grows with the table (min 4 MB), before DDL, after recovery: rewrite table files", "redo가 테이블 크기에 비례하는 한도(최소 4MB)를 넘을 때·DDL 직전·복구 직후에 테이블 파일 재기록"),
  },
  {
    t: L("Crash recovery", "크래시 복구"),
    s: L("on boot: undo unfinished transactions, replay the redo log, rebuild every index from the rows",
         "부팅 시 미완료 트랜잭션 undo, redo 로그 재생, 모든 인덱스를 행에서 재구성"),
  },
];

const AI: Item[] = [
  {
    t: L("Natural language to SQL", "자연어 → SQL"),
    s: L("done by Claude on the client side; RuSQL only ever receives plain SQL", "클라이언트 쪽 Claude가 수행, RuSQL은 항상 평범한 SQL만 받음"),
  },
  {
    t: L("MCP tools", "MCP 도구"),
    s: L(
      "execute_sql with a dangerous-SQL guard, schema and index introspection, editor and server control",
      "위험 SQL 안전장치가 있는 execute_sql, 스키마·인덱스 조회, 에디터·서버 제어",
    ),
  },
  {
    t: L("No private model", "사설 모델 없음"),
    s: L(
      "a small fine-tuned NL-to-SQL model would duplicate MCP and be less capable than Claude, so it is not part of RuSQL",
      "소형 파인튜닝 NL→SQL 모델은 MCP와 중복되고 Claude보다 성능이 낮아 RuSQL에 포함하지 않음",
    ),
  },
];

export default function DiagramView() {
  const [lang, setLang] = useState<Lang>(loadLang);
  const [view, setView] = useState<View>(loadView);

  const choose = (l: Lang) => {
    setLang(l);
    try { localStorage.setItem(LANG_KEY, l); } catch { /* storage unavailable: the choice just won't persist */ }
  };

  const chooseView = (v: View) => {
    setView(v);
    try { localStorage.setItem(VIEW_KEY, v); } catch { /* storage unavailable: the choice just won't persist */ }
  };

  return (
    <LangContext.Provider value={lang}>
      <DiagramBody lang={lang} choose={choose} view={view} chooseView={chooseView} />
    </LangContext.Provider>
  );
}

// The original picture: numbered layers top to bottom, connected by arrows.
function LayersView() {
  const t = useT();
  return (
    <>
      <nav className="dg-ribbon" aria-label={t(L("Flow overview", "흐름 개요"))}>
        {RIBBON.map((r, i) => (
          <span key={r.n} className="dg-ribbon-step">
            {i > 0 && <i>›</i>}
            <Num n={r.n} c={r.c} />
            {t(r.label)}
          </span>
        ))}
        <span className="dg-ribbon-back">{t(L("↩ result returns the same way", "↩ 결과는 같은 경로로 되돌아옴"))}</span>
      </nav>

      <div className="dg-caption">
        <Num n={1} c={C.client} /> {t(L("Clients: five ways to reach the engine", "클라이언트: 엔진에 접속하는 다섯 가지 방법"))}
      </div>

      <div className="dg-grid">
        {/* 1. clients */}
        <Card style={at("1", "1")} c={C.client} title="engine_cli" sub={L("Terminal REPL", "터미널 REPL")}
          note={L("runs the engine in-process", "엔진을 프로세스 안에서 직접 실행")} />
        <Card style={at("2", "1")} c={C.client} title={L("Desktop UI", "데스크톱 UI")} sub="Tauri + React + Monaco"
          note={L("SQL editor, ERD, Server Manager", "SQL 에디터, ERD, 서버 매니저")} />
        <Card style={at("3", "1")} c={C.client} title="Claude Desktop + MCP" sub="Python FastMCP, stdio JSON-RPC"
          note={L("25 tools; drives the UI via ui_commands.json", "도구 25개, ui_commands.json으로 UI 제어")} />
        <Card style={at("4", "1")} c={C.client} title="engine_client" sub={L("TCP client CLI", "TCP 클라이언트 CLI")} note="-u -p -h -P" />
        <Card style={at("5", "1")} c={C.client} title={L("MySQL clients", "MySQL 클라이언트")} sub="mysql CLI, DBeaver, JDBC"
          note={L("HikariCP and other pools", "HikariCP 등 커넥션 풀")} />

        {/* wires from clients */}
        <Wire style={at("1", "2 / 7")} c={C.client} dashed label={L("in-process, no network", "프로세스 내부 호출, 네트워크 없음")} />
        <Wire style={at("2", "2")} c={C.client} label="IPC invoke()" />
        <Wire style={at("3", "2 / 5")} c={C.server} label={L(":7878 native", ":7878 네이티브")} />
        <Wire style={at("4", "2 / 5")} c={C.server} label={L(":7878 native", ":7878 네이티브")} />
        <Wire style={at("5", "2 / 5")} c={C.server} label={L(":3306 MySQL wire", ":3306 MySQL 프로토콜")} />

        <Card style={at("2", "3")} c={C.client} title={L("Tauri backend", "Tauri 백엔드")} sub={L("Rust shell, TCP client", "Rust 셸, TCP 클라이언트")}
          note={L("spawns engine_server, owns the connection", "engine_server를 띄우고 연결을 보유")} />
        <Wire style={at("2", "4")} c={C.server} label=":7878" />

        {/* 2. server */}
        <Panel n={2} style={at("2 / 6", "5")} c={C.server} title="engine_server"
          sub={L("TCP front-end: authenticates, then hands each statement to its session", "TCP 프런트엔드: 인증 후 각 문장을 해당 세션에 전달")}>
          <Chips items={SERVER} />
        </Panel>
        <Wire style={at("2 / 6", "6")} c={C.server} label={L("SQL text", "SQL 텍스트")} />

        {/* 3-5. engine core */}
        <Panel style={at("1 / 6", "7")} c={C.core} title="engine_core" sub={L("C++20 library: the SQL engine itself", "C++20 라이브러리: SQL 엔진 본체")}>
          <div className="dg-pipeline">
            <div className="dg-stage" style={tint(C.core)}>
              <div className="dg-stage-head"><Num n={3} c={C.core} /><b>{t(L("Lexer + Parser", "렉서 + 파서"))}</b></div>
              <span>{t(L("Tokens, then a recursive-descent parser builds the AST: DDL, DML, DCL, TCL, procedures",
                         "토큰화 후 재귀 하향 파서가 AST 생성: DDL, DML, DCL, TCL, 프로시저"))}</span>
            </div>
            <HArrow c={C.core} phase={1} />
            <div className="dg-stage" style={tint(C.core)}>
              <div className="dg-stage-head"><Num n={4} c={C.core} /><b>{t(L("Query planner", "쿼리 플래너"))}</b></div>
              <span>{t(L("Cost-based: access path per table (also used to find the rows of UPDATE / DELETE; an AND with an indexed equality starts from that index), join algorithm, System-R DP join order",
                         "비용 기반: 테이블별 접근 경로(UPDATE·DELETE의 대상 행 탐색에도 사용, 인덱스 등호가 든 AND는 그 인덱스로 시작), 조인 알고리즘, System-R DP 조인 순서"))}</span>
            </div>
            <HArrow c={C.core} phase={2} />
            <div className="dg-stage" style={tint(C.core)}>
              <div className="dg-stage-head"><Num n={5} c={C.core} /><b>{t(L("Executor", "실행기"))}</b></div>
              <span>{t(L("Runs the plan: joins (NL, hash, sort-merge, index NL), subqueries, CTE, window functions. Scans, aggregates, GROUP BY, DISTINCT and ORDER BY work on pointers to the table's rows (no copies); joins apply single-table WHERE parts before joining and hash LEFT JOINs and multi-condition ONs on their equality; uncorrelated subqueries (scalar, EXISTS, IN) run once per statement; a multi-row INSERT checks duplicates and updates indexes once per statement. UPDATE checks PRIMARY KEY / UNIQUE and applies fully or not at all",
                         "계획 실행: 조인(NL, 해시, 소트-머지, 인덱스 NL), 서브쿼리, CTE, 윈도우 함수. 스캔·집계·GROUP BY·DISTINCT·ORDER BY는 테이블 행을 복사하지 않고 포인터로 처리하고, 조인은 한 테이블만 읽는 WHERE 조건을 조인 전에 적용하며 LEFT JOIN과 여러 조건 ON을 등호로 해시 처리하고, 상관 없는 서브쿼리(스칼라·EXISTS·IN)는 문장당 한 번만 실행하고, 여러 행 INSERT는 중복 검사와 인덱스 갱신을 문장당 한 번만 함. UPDATE는 PRIMARY KEY·UNIQUE를 검사하고 전부 적용되거나 전혀 적용되지 않음"))}</span>
            </div>
          </div>
          <Chips items={SIDE_MODULES} />
        </Panel>
        <Wire style={at("1 / 6", "8")} c={C.txn} label={L("reads and writes under MVCC + locks", "MVCC·락 아래에서 읽기/쓰기")} />

        {/* 6. transactions */}
        <Panel n={6} style={at("1 / 6", "9")} c={C.txn} title={L("Transactions & concurrency", "트랜잭션 & 동시성")}
          sub={L("decides what each statement may see and touch", "각 문장이 무엇을 보고 건드릴 수 있는지 결정")}>
          <Chips items={TXN} />
        </Panel>
        <Wire style={at("1 / 6", "10")} c={C.storage} label={L("row versions, index access", "행 버전, 인덱스 접근")} />

        {/* 7. storage */}
        <Panel n={7} style={at("1 / 6", "11")} c={C.storage} title={L("Storage engine", "스토리지 엔진")}
          sub={L("where rows and indexes live", "행과 인덱스가 사는 곳")}>
          <Chips items={STORAGE} />
        </Panel>
        <Wire style={at("1 / 6", "12")} c={C.disk} label={L("log, then flush", "로그 기록 후 flush")} />

        {/* 8. durability */}
        <Panel n={8} style={at("1 / 6", "13")} c={C.disk} title={L("Durability & recovery", "내구성 & 복구")}
          sub={L("what survives a crash", "크래시 후에도 살아남는 것")}>
          <Chips items={DURABILITY} />
          <div className="dg-disk">
            <b>{t(L("On disk", "디스크 파일"))}</b>
            <span>{t(L("*.rdb  indexes.json  views.json  rusql.redo  rusql.wal  _undo.log  _system/ (users, grants, roles, synonyms)",
                       "*.rdb  indexes.json  views.json  rusql.redo  rusql.wal  _undo.log  _system/ (사용자, 권한, 역할, 동의어)"))}</span>
          </div>
        </Panel>
      </div>

      <Panel style={{ marginTop: 28, ...delay(14) }} c={C.client} title={L("AI integration", "AI 연동")}
        sub={L("Claude through MCP is the only natural-language path", "자연어 경로는 MCP를 통한 Claude 하나뿐")}>
        <Chips items={AI} />
      </Panel>
    </>
  );
}

// Split out so that useT() (inside the provider) sees the chosen language.
function DiagramBody({ lang, choose, view, chooseView }: {
  lang: Lang; choose: (l: Lang) => void; view: View; chooseView: (v: View) => void;
}) {
  const t = useT();
  const layersLabel = t(L("Layered view", "계층형 보기"));
  const cylindersLabel = t(L("Cylinder view: hover a cylinder for details", "원통형 보기: 원통에 마우스를 올리면 설명 표시"));
  return (
    <div className="diagram-view">
      <div className="dg-scroll">
        <div className="dg-page" lang={lang}>
          <ArrowDefs />
          <div className="dg-top">
            <h1 className="dg-title">{t(L("System Architecture & Flow", "시스템 아키텍처 및 흐름"))}</h1>
            <div className="dg-controls">
              <div className="dg-seg" role="group" aria-label="Language">
                <button className={lang === "ko" ? "active" : ""} aria-pressed={lang === "ko"} onClick={() => choose("ko")}>한국어</button>
                <button className={lang === "en" ? "active" : ""} aria-pressed={lang === "en"} onClick={() => choose("en")}>EN</button>
              </div>
              <div className="dg-seg dg-seg-icons" role="group" aria-label={t(L("View", "보기"))}>
                <button className={view === "cylinders" ? "active" : ""} aria-pressed={view === "cylinders"} title={cylindersLabel} aria-label={cylindersLabel}
                  onClick={() => chooseView("cylinders")}>
                  <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
                    <ellipse cx="6.5" cy="4" rx="4" ry="1.8" />
                    <path d="M2.5 4v8c0 1 1.8 1.8 4 1.8s4-.8 4-1.8V4" />
                    <path d="M11 9.5h5.2M14 7.3l2.2 2.2-2.2 2.2" />
                  </svg>
                </button>
                <button className={view === "layers" ? "active" : ""} aria-pressed={view === "layers"} title={layersLabel} aria-label={layersLabel}
                  onClick={() => chooseView("layers")}>
                  <svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" aria-hidden="true">
                    <rect x="2" y="2" width="14" height="3.6" rx="1" />
                    <rect x="2" y="7.2" width="14" height="3.6" rx="1" />
                    <rect x="2" y="12.4" width="14" height="3.6" rx="1" />
                  </svg>
                </button>
              </div>
            </div>
          </div>
          <p className="dg-lead">
            {t(L("How a SQL statement travels from a client down to disk, and how the result comes back.",
                 "SQL 문장이 클라이언트에서 디스크까지 내려가는 경로와, 결과가 되돌아오는 흐름."))}
          </p>

          {view === "layers" ? <LayersView /> : <DiagramCylinders />}
        </div>
      </div>
    </div>
  );
}
