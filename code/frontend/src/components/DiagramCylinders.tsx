// DiagramCylinders.tsx
// The second Diagram picture: every component is a database-style cylinder, connected by
// animated arrows (request path left to right, then down and back along the bottom row).
// A cylinder only carries its name -- the description appears in a tooltip while the mouse
// (or keyboard focus) is on it. Same facts as the layered view in DiagramView.tsx.

import { useState } from "react";
import { C, headId, L, tint, useT } from "./diagramShared";
import type { Text } from "./diagramShared";
import "./DiagramCylinders.css";

const W = 1300;
const H = 700;
const RY = 13; // vertical radius of a cylinder's top ellipse

type Node = { id: string; x: number; y: number; w: number; h: number; c: string; name: Text; desc: Text };

// Order = flow order: it drives the entrance stagger and the glow that walks along the path.
const NODES: Node[] = [
  {
    id: "cli", x: 95, y: 100, w: 140, h: 84, c: C.client, name: "engine_cli",
    desc: L("Terminal REPL running the engine in-process: no network, no server.",
            "엔진을 프로세스 안에서 직접 실행하는 터미널 REPL. 네트워크·서버 없음."),
  },
  {
    id: "ui", x: 95, y: 225, w: 140, h: 84, c: C.client, name: L("Desktop UI", "데스크톱 UI"),
    desc: L("Tauri + React + Monaco: SQL editor (autocomplete, statement Journey), ERD with a relationship editor, B+Tree and MVCC pages, Server Manager (sessions / locks), Compat page. Talks to engine_server over TCP :7878.",
            "Tauri + React + Monaco: SQL 에디터(자동완성, 한 문장의 여행), 관계 편집기가 있는 ERD, B+Tree·MVCC 페이지, 서버 매니저(세션·락), 호환성 페이지. TCP :7878로 engine_server와 통신."),
  },
  {
    id: "mcp", x: 95, y: 350, w: 140, h: 84, c: C.client, name: "Claude + MCP",
    desc: L("Claude Desktop drives RuSQL through a Python MCP server (26 tools, TCP :7878). Claude writes the SQL; RuSQL has no private NL-to-SQL model.",
            "Claude Desktop이 Python MCP 서버(도구 26개, TCP :7878)로 RuSQL을 조작. SQL은 Claude가 작성하며 사설 NL→SQL 모델은 없음."),
  },
  {
    id: "client", x: 95, y: 475, w: 140, h: 84, c: C.client, name: "engine_client",
    desc: L("TCP client CLI (-u -p -h -P) for the native protocol on :7878.",
            "네이티브 프로토콜(:7878)용 TCP 클라이언트 CLI (-u -p -h -P)."),
  },
  {
    id: "mysql", x: 95, y: 600, w: 140, h: 84, c: C.client, name: L("MySQL clients", "MySQL 클라이언트"),
    desc: L("mysql CLI, DBeaver, JDBC and pool drivers over the MySQL wire protocol (:3306).",
            "mysql CLI, DBeaver, JDBC, 풀 드라이버가 MySQL 와이어 프로토콜(:3306)로 접속."),
  },
  {
    id: "server", x: 335, y: 350, w: 150, h: 112, c: C.server, name: "engine_server",
    desc: L("TCP front-end: native :7878 + MySQL wire :3306, challenge-response auth, one session per connection. Answers the metadata statements MySQL clients send; result columns carry types.",
            "TCP 프런트엔드: 네이티브 :7878 + MySQL 와이어 :3306, 챌린지-응답 인증, 연결당 세션 1개. MySQL 클라이언트의 메타데이터 문장에 답하고 결과 열에 타입을 실어 보냄."),
  },
  {
    id: "parser", x: 525, y: 350, w: 120, h: 98, c: C.core, name: L("Parser", "파서"),
    desc: L("SQL text → AST (DDL, DML, DCL, TCL, procedures). Reads mysqldump files and real-client statements, window frames / WINDOW w, named foreign keys, TRACE. Names ignore case.",
            "SQL 텍스트 → AST (DDL, DML, DCL, TCL, 프로시저). mysqldump 파일과 실제 클라이언트 문장, 윈도 프레임·WINDOW w, 이름 있는 외래키, TRACE를 읽음. 이름은 대소문자 무시."),
  },
  {
    id: "planner", x: 705, y: 350, w: 120, h: 98, c: C.core, name: L("Planner", "플래너"),
    desc: L("Cost-based: access path per table, join algorithm and order (System-R DP) from histogram + most-common-value statistics. Indexes also start UPDATE / DELETE, GROUP BY, ORDER BY … LIMIT.",
            "비용 기반: 히스토그램 + 최빈값 통계로 테이블별 접근 경로, 조인 알고리즘·순서(System-R DP)를 선택. UPDATE·DELETE, GROUP BY, ORDER BY … LIMIT도 인덱스로 시작."),
  },
  {
    id: "executor", x: 885, y: 350, w: 120, h: 98, c: C.core, name: L("Executor", "실행기"),
    desc: L("Runs the plan: NL / hash / sort-merge / index joins, subqueries, CTE, window functions (over groups too). Result cache (LRU-512) + thread pool. MySQL-exact results (DECIMAL places, strict errors); MAX_EXECUTION_TIME (3024); TRACE.",
            "계획 실행: NL·해시·소트-머지·인덱스 조인, 서브쿼리, CTE, 윈도 함수(그룹 위에서도). 결과 캐시(LRU 512) + 스레드 풀. MySQL과 같은 결과(DECIMAL 자릿수, 엄격한 오류), MAX_EXECUTION_TIME(3024), TRACE."),
  },
  {
    id: "txn", x: 1085, y: 350, w: 130, h: 104, c: C.txn, name: L("Transactions", "트랜잭션"),
    desc: L("Row-level MVCC, 4 isolation levels, row locks with deadlock detection, gap locks, SSI, group commit.",
            "행 단위 MVCC, 격리 수준 4단계, 데드락 감지가 있는 행 락, Gap 락, SSI, 그룹 커밋."),
  },
  {
    id: "storage", x: 1085, y: 600, w: 130, h: 104, c: C.storage, name: L("Storage", "스토리지"),
    desc: L("B+Tree and hash indexes (compared like MySQL: 7 = 7.00, 'A' = 'a'), in-memory MVCC row versions, buffer pool (LRU, 64 x 16 KB), LZ4-compressed .rdb files that hold committed rows only.",
            "B+Tree·해시 인덱스(MySQL처럼 비교: 7 = 7.00, 'A' = 'a'), 메모리 상의 MVCC 행 버전, 버퍼 풀(LRU, 16KB x 64), 커밋된 행만 담는 LZ4 압축 .rdb 파일."),
  },
  {
    id: "durability", x: 825, y: 600, w: 130, h: 104, c: C.disk, name: L("Durability", "내구성"),
    desc: L("Redo log (checksummed batch per commit, group-commit fsync), WAL, undo log. Checkpoints rewrite the table files; on boot: undo unfinished transactions, replay redo, rebuild indexes.",
            "Redo 로그(커밋마다 체크섬 배치, 그룹 커밋 fsync), WAL, undo 로그. 체크포인트가 테이블 파일을 재기록하고, 부팅 시 미완료 트랜잭션 undo → redo 재생 → 인덱스 재구성."),
  },
  {
    id: "disk", x: 565, y: 600, w: 130, h: 104, c: C.disk, name: L("Disk files", "디스크 파일"),
    desc: L("*.rdb, indexes.json, views.json, rusql.redo, rusql.wal, _undo.log and _system/ (users, grants, roles, synonyms).",
            "*.rdb, indexes.json, views.json, rusql.redo, rusql.wal, _undo.log, _system/ (사용자, 권한, 역할, 동의어)."),
  },
];

type Arrow = { d: string; c: string; dashed?: boolean };

// d is drawn in the direction of the arrow; each ends 6px short of the target so the head sits just outside it.
const ARROWS: Arrow[] = [
  { d: "M171 225 C220 225 205 328 254 328", c: C.server },
  { d: "M171 350 H254", c: C.server },
  { d: "M171 475 C220 475 205 366 254 366", c: C.server },
  { d: "M171 600 C230 600 195 385 254 385", c: C.server },
  { d: "M171 100 H495 Q525 100 525 130 V295", c: C.client, dashed: true },
  { d: "M416 350 H459", c: C.core },
  { d: "M591 350 H639", c: C.core },
  { d: "M771 350 H819", c: C.core },
  { d: "M951 350 H1014", c: C.txn },
  { d: "M1085 408 V542", c: C.storage },
  { d: "M1014 600 H896", c: C.disk },
  { d: "M754 600 H636", c: C.disk },
];

// The two lower clients' arrows curve upwards, so their labels sit below the arrow's start instead of above it.
const PORT_LABELS: { x: number; y: number; text: Text }[] = [
  { x: 180, y: 215, text: ":7878" },
  { x: 180, y: 340, text: ":7878" },
  { x: 180, y: 497, text: ":7878" },
  { x: 180, y: 622, text: ":3306" },
  { x: 330, y: 90, text: L("in-process, no network", "프로세스 내부 호출, 네트워크 없음") },
];

function Cylinder({ n, order, onEnter, onLeave }: { n: Node; order: number; onEnter: () => void; onLeave: () => void }) {
  const t = useT();
  const rx = n.w / 2;
  const l = n.x - rx, r = n.x + rx, top = n.y - n.h / 2, bottom = n.y + n.h / 2;
  return (
    <g className="dgc-node" style={{ ...tint(n.c), ["--o" as string]: order }} tabIndex={0}
      onMouseEnter={onEnter} onMouseLeave={onLeave} onFocus={onEnter} onBlur={onLeave}>
      <path className="dgc-body" d={`M${l} ${top + RY} V${bottom - RY} A${rx} ${RY} 0 0 0 ${r} ${bottom - RY} V${top + RY} Z`} />
      <ellipse className="dgc-top" cx={n.x} cy={top + RY} rx={rx} ry={RY} />
      <text className="dgc-name" x={n.x} y={n.y + RY / 2 + 5} textAnchor="middle">{t(n.name)}</text>
    </g>
  );
}

export default function DiagramCylinders() {
  const t = useT();
  const [hover, setHover] = useState<string | null>(null);
  const tip = NODES.find(n => n.id === hover);
  // The bottom row and the pipeline row (x >= 500) open their tooltip upwards: below them sits the next row, above them empty space.
  const tipUp = tip !== undefined && (tip.y >= 475 || tip.x >= 500);

  return (
    <div className="dgc">
      <div className="dgc-wrap">
        <svg className="dgc-svg" viewBox={`0 0 ${W} ${H}`} role="img"
          aria-label={t(L("Cylinder diagram of the RuSQL system flow", "RuSQL 시스템 흐름 원통 다이어그램"))}>
          <g className="dgc-arrows">
            {ARROWS.map((a, i) => (
              <g key={i} style={tint(a.c)}>
                {/* arrow style shared with the layered view (heads come from ArrowDefs, mounted in DiagramView) */}
                <path className="dg-aline" d={a.d} markerEnd={`url(#${headId(a.c)})`} strokeDasharray={a.dashed ? "7 6" : undefined} />
                <path className="dg-aflow" d={a.d} />
                {/* a packet travelling along the arrow; the negative begin staggers the arrows without a dot parked at the origin */}
                <circle className="dg-apacket" r="3.4">
                  <animateMotion dur="2.6s" begin={`${-(i * 0.65).toFixed(2)}s`} repeatCount="indefinite" path={a.d} />
                </circle>
              </g>
            ))}
          </g>

          {PORT_LABELS.map((p, i) => (
            <text key={i} className="dgc-port" x={p.x} y={p.y}>{t(p.text)}</text>
          ))}

          {NODES.map((n, i) => (
            <Cylinder key={n.id} n={n} order={i} onEnter={() => setHover(n.id)} onLeave={() => setHover(null)} />
          ))}
        </svg>

        {tip && (
          <div className={`dgc-tip${tipUp ? " up" : ""}`}
            style={{
              ...tint(tip.c),
              left: `${Math.min(86, Math.max(14, (tip.x / W) * 100))}%`,
              top: `${((tipUp ? tip.y - tip.h / 2 : tip.y + tip.h / 2) / H) * 100}%`,
            }}>
            <b>{t(tip.name)}</b>
            <span>{t(tip.desc)}</span>
          </div>
        )}
      </div>

      <div className="dgc-legend">
        <span><i className="dgc-key solid" />{t(L("request path", "요청 경로"))}</span>
        <span><i className="dgc-key dashed" />{t(L("in-process call", "프로세스 내부 호출"))}</span>
        <span className="dgc-hint">{t(L("Hover a cylinder for details", "원통에 마우스를 올리면 설명이 표시됩니다"))}</span>
      </div>
    </div>
  );
}
