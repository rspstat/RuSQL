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
    desc: L("Terminal REPL that runs the engine in-process: no network and no server in between.",
            "엔진을 프로세스 안에서 직접 실행하는 터미널 REPL. 네트워크도 서버도 거치지 않음."),
  },
  {
    id: "ui", x: 95, y: 225, w: 140, h: 84, c: C.client, name: L("Desktop UI", "데스크톱 UI"),
    desc: L("Tauri + React + Monaco (SQL editor, ERD, Server Manager). Its Rust backend spawns engine_server and talks to it over TCP :7878.",
            "Tauri + React + Monaco(SQL 에디터, ERD, 서버 매니저). Rust 백엔드가 engine_server를 띄우고 TCP :7878로 통신."),
  },
  {
    id: "mcp", x: 95, y: 350, w: 140, h: 84, c: C.client, name: "Claude + MCP",
    desc: L("Claude Desktop drives RuSQL through the Python MCP server (25 tools, TCP :7878). Claude writes the SQL; RuSQL has no private NL-to-SQL model.",
            "Claude Desktop이 Python MCP 서버(도구 25개, TCP :7878)로 RuSQL을 조작. SQL은 Claude가 작성하며 RuSQL에는 사설 NL→SQL 모델이 없음."),
  },
  {
    id: "client", x: 95, y: 475, w: 140, h: 84, c: C.client, name: "engine_client",
    desc: L("TCP client CLI (-u -p -h -P) for the native protocol on :7878.",
            "네이티브 프로토콜(:7878)용 TCP 클라이언트 CLI (-u -p -h -P)."),
  },
  {
    id: "mysql", x: 95, y: 600, w: 140, h: 84, c: C.client, name: L("MySQL clients", "MySQL 클라이언트"),
    desc: L("mysql CLI, DBeaver, JDBC and pool drivers (HikariCP) over the MySQL wire protocol on :3306.",
            "mysql CLI, DBeaver, JDBC, 풀 드라이버(HikariCP)가 MySQL 와이어 프로토콜(:3306)로 접속."),
  },
  {
    id: "server", x: 335, y: 350, w: 150, h: 112, c: C.server, name: "engine_server",
    desc: L("TCP front-end: native :7878 and MySQL wire :3306, challenge-response auth (no plaintext password), one session per connection over the shared database.",
            "TCP 프런트엔드: 네이티브 :7878과 MySQL 와이어 :3306, 챌린지-응답 인증(평문 비밀번호 없음), 공유 데이터베이스 위에서 연결당 세션 1개."),
  },
  {
    id: "parser", x: 525, y: 350, w: 120, h: 98, c: C.core, name: L("Parser", "파서"),
    desc: L("Lexer + recursive-descent parser: SQL text becomes an AST (DDL, DML, DCL, TCL, procedures).",
            "렉서 + 재귀 하향 파서: SQL 텍스트를 AST로 변환(DDL, DML, DCL, TCL, 프로시저)."),
  },
  {
    id: "planner", x: 705, y: 350, w: 120, h: 98, c: C.core, name: L("Planner", "플래너"),
    desc: L("Cost-based: picks the access path per table, the join algorithm and the join order (System-R DP) from histogram + most-common-value statistics. The same access paths find the rows of UPDATE and DELETE, and an AND with an indexed equality starts from that index.",
            "비용 기반: 히스토그램 + 최빈값 통계로 테이블별 접근 경로, 조인 알고리즘, 조인 순서(System-R DP)를 선택. 같은 접근 경로가 UPDATE·DELETE의 대상 행도 찾고, 인덱스 등호가 든 AND는 그 인덱스로 시작함."),
  },
  {
    id: "executor", x: 885, y: 350, w: 120, h: 98, c: C.core, name: L("Executor", "실행기"),
    desc: L("Runs the plan: joins (NL, hash, sort-merge, index NL), subqueries, CTE, window functions. Result cache (LRU-512) and a thread pool for parallel scans. Scans, aggregates, GROUP BY, DISTINCT and ORDER BY work on pointers to the table's rows (no copies); joins apply single-table WHERE parts before joining and hash LEFT JOINs and multi-condition ONs on their equality; uncorrelated subqueries (scalar, EXISTS, IN) run once per statement; a multi-row INSERT checks duplicates and updates indexes once per statement. UPDATE / DELETE take index hits only as candidates, verify each against the real row and scan when in doubt; UPDATE checks PRIMARY KEY / UNIQUE and applies fully or not at all.",
            "계획 실행: 조인(NL, 해시, 소트-머지, 인덱스 NL), 서브쿼리, CTE, 윈도우 함수. 결과 캐시(LRU 512)와 병렬 스캔용 스레드 풀 포함. 스캔·집계·GROUP BY·DISTINCT·ORDER BY는 테이블 행을 복사하지 않고 포인터로 처리하고, 조인은 한 테이블만 읽는 WHERE 조건을 조인 전에 적용하며 LEFT JOIN과 여러 조건 ON을 등호로 해시 처리하고, 상관 없는 서브쿼리(스칼라·EXISTS·IN)는 문장당 한 번만 실행하고, 여러 행 INSERT는 중복 검사와 인덱스 갱신을 문장당 한 번만 함. UPDATE·DELETE는 인덱스 결과를 후보로만 쓰고 실제 행으로 검증하며 의심스러우면 스캔. UPDATE는 PRIMARY KEY·UNIQUE를 검사하고 전부 적용되거나 전혀 적용되지 않음."),
  },
  {
    id: "txn", x: 1085, y: 350, w: 130, h: 104, c: C.txn, name: L("Transactions", "트랜잭션"),
    desc: L("Row-level MVCC (_xmin / _xmax version chains), 4 isolation levels, row locks with blocking wait and deadlock detection, gap locks, SSI, group commit.",
            "행 단위 MVCC(_xmin / _xmax 버전 체인), 격리 수준 4단계, 블로킹 대기·데드락 감지가 있는 행 락, Gap 락, SSI, 그룹 커밋."),
  },
  {
    id: "storage", x: 1085, y: 600, w: 130, h: 104, c: C.storage, name: L("Storage", "스토리지"),
    desc: L("B+Tree and hash indexes (keys compare as numbers, so 7 = 7.00; a statement rewrites each secondary-index bucket once), in-memory MVCC row versions with a pk-to-position cache, buffer pool (LRU, 64 pages x 16 KB), binary .rdb files with LZ4 compression.",
            "B+Tree·해시 인덱스(키를 숫자로 비교해 7 = 7.00, 문장당 보조 인덱스 버킷을 한 번만 다시 기록), PK→위치 캐시가 있는 메모리 상의 MVCC 행 버전, 버퍼 풀(LRU, 16KB 페이지 64개), LZ4 압축 바이너리 .rdb 파일."),
  },
  {
    id: "durability", x: 825, y: 600, w: 130, h: 104, c: C.disk, name: L("Durability", "내구성"),
    desc: L("Redo log (one checksummed batch per commit, group-commit fsync), WAL and undo log (which transactions a log file holds is tracked in memory, so COMMIT and ROLLBACK never re-read it). Checkpoints rewrite the table files once the redo log outgrows a limit that scales with the table size; on boot, unfinished transactions are undone, the redo log is replayed (identical rows of one table all survive) and every index is rebuilt from the rows.",
            "Redo 로그(커밋마다 체크섬 배치 1개, 그룹 커밋 fsync), WAL, undo 로그(로그 파일에 어떤 트랜잭션이 들어 있는지 메모리에서 추적해 COMMIT·ROLLBACK이 파일을 다시 읽지 않음). redo 로그가 테이블 크기에 비례하는 한도를 넘으면 체크포인트가 테이블 파일을 재기록하고, 부팅 시 미완료 트랜잭션을 undo하고 redo를 재생(한 테이블의 동일한 행도 모두 보존)한 뒤 모든 인덱스를 행에서 재구성."),
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
