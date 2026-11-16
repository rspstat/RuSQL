// EcosystemView.tsx
// The "Ecosystem" window of the home screen: one picture of everything RuSQL holds -- every saved connection, the databases in it and the tables in those -- drawn in the
// cylinder style of the Diagram page (cylinders, arrows with packets running along them, a tooltip while the mouse is on one). It replaces the whole home screen (no menu
// bar, no connection list): the only way out is the Back button or Escape.
//
// The picture is read from disk (src-tauri main.rs: scan_ecosystem), so a connection that is not signed in shows its databases too; it is read again every few seconds,
// so what another RuSQL window or Claude does shows up. A connection that was never opened has no data folder yet and is drawn dashed. The geometry is
// lib/ecosystemLayout.ts (tested); this file draws it and writes the words. Every visible string is an English/Korean pair, the language is the one of the Diagram page.

import { useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { ArrowDefs, C, L, LangContext, headId, tint, useT } from "./diagramShared";
import type { Lang, Text } from "./diagramShared";
import { RY, ecosystemTotals, fitName, humanBytes, layoutEcosystem, relatedTo } from "../lib/ecosystemLayout";
import type { EcoConn, EcoNode, EcoScan } from "../lib/ecosystemLayout";
import "./DiagramView.css";
import "./EcosystemView.css";

const LANG_KEY = "rusql_diagram_lang"; // shared with the Diagram page: one choice for both
const REFRESH_MS = 4000;

function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

const KIND_COLOR: Record<EcoNode["kind"], string> = {
  root: C.server, connection: C.core, database: C.txn, table: C.client, more: C.client,
};
const ARROW_COLOR = [C.server, C.server, C.core, C.txn]; // by the depth of the cylinder the arrow ends at

const num = (n: number) => n.toLocaleString("en-US");

interface Tip { title: string; color: string; lines: Text[] }

function whenUsed(ms: number | undefined, lang: Lang): string {
  if (!ms) return "";
  try { return new Date(ms).toLocaleString(lang === "ko" ? "ko-KR" : "en-US"); } catch { return ""; }
}

// What the tooltip says about a cylinder.
function tipOf(n: EcoNode, nodes: EcoNode[], lang: Lang): Tip {
  const color = KIND_COLOR[n.kind];
  if (n.kind === "root") {
    const conns = nodes.filter(x => x.kind === "connection");
    const opened = conns.filter(x => x.scan?.exists).length;
    const dbs = nodes.filter(x => x.kind === "database").length;
    const tables = nodes.filter(x => x.kind === "table").length;
    return {
      title: "RuSQL", color,
      lines: [
        L(`${conns.length} saved connection(s), ${opened} with a data folder`, `저장된 연결 ${conns.length}개 중 ${opened}개가 데이터 폴더를 가짐`),
        L(`${dbs} database(s) and ${tables} table(s) shown`, `데이터베이스 ${dbs}개 · 그림에 그린 테이블 ${tables}개`),
        L("Every connection runs its own engine_server process on its own data folder.", "연결마다 자기 데이터 폴더를 쓰는 engine_server 프로세스가 따로 뜹니다."),
      ],
    };
  }
  if (n.kind === "connection" && n.conn) {
    const c = n.conn, s = n.scan;
    const lines: Text[] = [`${c.user}@${c.host}:${c.port}`];
    if (!s || !s.exists) {
      lines.push(L("No data folder yet: this connection has never been opened.", "아직 데이터 폴더가 없습니다: 이 연결로 한 번도 접속하지 않았습니다."));
    } else {
      const tables = s.databases.reduce((a, d) => a + d.tables.length, 0);
      lines.push(L(`${s.databases.length} database(s), ${tables} table(s), ${s.users} user(s)`, `데이터베이스 ${s.databases.length}개 · 테이블 ${tables}개 · 사용자 ${s.users}명`));
      if (s.routines || s.triggers) lines.push(L(`${s.routines} stored routine(s), ${s.triggers} trigger(s)`, `저장 루틴 ${s.routines}개 · 트리거 ${s.triggers}개`));
      lines.push(L(`${humanBytes(s.bytes)} on disk`, `디스크 ${humanBytes(s.bytes)}`));
    }
    lines.push(L(`Data folder: ${c.dataDir}`, `데이터 폴더: ${c.dataDir}`));
    const en = [c.favorite ? "favorite" : "", c.autoLogin ? "auto sign-in" : ""].filter(x => x !== "");
    const ko = [c.favorite ? "즐겨찾기" : "", c.autoLogin ? "자동 로그인" : ""].filter(x => x !== "");
    if (en.length) lines.push(L(en.join(", "), ko.join(", ")));
    const used = whenUsed(c.lastUsed, lang);
    if (used) lines.push(L(`Last opened ${used}`, `마지막 접속 ${used}`));
    return { title: c.name, color, lines };
  }
  if (n.kind === "database" && n.db) {
    const d = n.db;
    const rows = d.tables.reduce((a, t) => a + t.rows, 0);
    const indexes = d.tables.reduce((a, t) => a + t.indexes, 0);
    const lines: Text[] = [
      L(`${d.tables.length} table(s), ${d.views} view(s), ${indexes} index(es)`, `테이블 ${d.tables.length}개 · 뷰 ${d.views}개 · 인덱스 ${indexes}개`),
      L(`${num(rows)} row(s) stored, ${humanBytes(d.bytes)} on disk`, `저장된 행 ${num(rows)}개 · 디스크 ${humanBytes(d.bytes)}`),
    ];
    return { title: d.name, color, lines };
  }
  if (n.kind === "table" && n.table) {
    const t = n.table;
    const lines: Text[] = [
      L(`${t.columns} column(s)${t.primaryKey.length ? `, primary key ${t.primaryKey.join(", ")}` : ""}`,
        `열 ${t.columns}개${t.primaryKey.length ? ` · 기본키 ${t.primaryKey.join(", ")}` : ""}`),
      L(`${t.foreignKeys} foreign key(s), ${t.indexes} index(es)`, `외래키 ${t.foreignKeys}개 · 인덱스 ${t.indexes}개`),
      L(`${num(t.rows)} row(s) stored in the table file, ${humanBytes(t.bytes)}`, `테이블 파일에 저장된 행 ${num(t.rows)}개 · ${humanBytes(t.bytes)}`),
    ];
    return { title: `${n.db?.name ?? ""}.${t.name}`, color, lines };
  }
  const hidden = n.hidden ?? 0;
  return { title: `+${hidden}`, color, lines: [L(`${hidden} more table(s) of this database are not drawn.`, `이 데이터베이스의 테이블 ${hidden}개는 그리지 않았습니다.`)] };
}

// What stands under a cylinder.
function captionOf(n: EcoNode): Text | null {
  if (n.kind === "connection") {
    if (!n.scan || !n.scan.exists) return L("no data folder yet", "아직 데이터 폴더 없음");
    const tables = n.scan.databases.reduce((a, d) => a + d.tables.length, 0);
    if (n.scan.databases.length === 0) return L("no database yet", "데이터베이스 없음");
    return L(`${n.scan.databases.length} DB · ${tables} table${tables === 1 ? "" : "s"}`, `DB ${n.scan.databases.length}개 · 테이블 ${tables}개`);
  }
  if (n.kind === "database" && n.db) {
    const rows = n.db.tables.reduce((a, t) => a + t.rows, 0);
    return L(`${n.db.tables.length} table${n.db.tables.length === 1 ? "" : "s"} · ${num(rows)} row${rows === 1 ? "" : "s"}`, `테이블 ${n.db.tables.length}개 · 행 ${num(rows)}개`);
  }
  return null;
}

function Cylinder({ n, glow, onEnter, onLeave, dim }: { n: EcoNode; glow: number; onEnter: () => void; onLeave: () => void; dim: boolean }) {
  const t = useT();
  const rx = n.w / 2;
  const l = n.cx - rx, r = n.cx + rx, top = n.cy - n.h / 2, bottom = n.cy + n.h / 2;
  const small = n.kind === "table" || n.kind === "more";
  const empty = n.kind === "connection" && (!n.scan || !n.scan.exists);
  const ry = small ? 8 : RY;
  const label = n.kind === "root" ? "RuSQL"
    : n.kind === "connection" ? fitName(n.conn?.name ?? "", 17)
    : n.kind === "database" ? fitName(n.db?.name ?? "", 13)
    : n.kind === "table" ? fitName(n.table?.name ?? "", 12)
    : `+${n.hidden ?? 0}`;
  return (
    <g className={`eco-node eco-${n.kind}${empty ? " eco-empty" : ""}${dim ? " dim" : ""}`} data-id={n.id}
      style={{ ...tint(KIND_COLOR[n.kind]), ["--p" as string]: Math.min(n.order, 45), ["--g" as string]: glow }}
      tabIndex={small ? -1 : 0} role="img" aria-label={t(tipLabel(n))}
      onMouseEnter={onEnter} onMouseLeave={onLeave} onFocus={onEnter} onBlur={onLeave}>
      <path className="eco-body" d={`M${l} ${top + ry} V${bottom - ry} A${rx} ${ry} 0 0 0 ${r} ${bottom - ry} V${top + ry} Z`} />
      <ellipse className="eco-top" cx={n.cx} cy={top + ry} rx={rx} ry={ry} />
      <text className="eco-name" x={n.cx} y={n.cy + ry / 2 + (small ? 4 : 5)} textAnchor="middle">{label}</text>
    </g>
  );
}

function tipLabel(n: EcoNode): Text {
  if (n.kind === "root") return "RuSQL";
  if (n.kind === "connection") return n.conn?.name ?? "";
  if (n.kind === "database") return n.db?.name ?? "";
  if (n.kind === "table") return n.table?.name ?? "";
  return `+${n.hidden ?? 0}`;
}

// The logo (the Ecosystem button of the home screen and the title of this window): one plain cylinder, the shape the whole picture is made of. Colors are currentColor,
// so the button's hover colour and the window's teal both work.
export function EcosystemLogo({ size = 28 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinejoin="round" aria-hidden="true">
      <path d="M4 6V18a8 3.4 0 0 0 16 0V6" />
      <ellipse cx="12" cy="6" rx="8" ry="3.4" />
    </svg>
  );
}

function Picture({ connections, lang }: { connections: EcoConn[]; lang: Lang }) {
  const t = useT();
  const [scans, setScans] = useState<Record<string, EcoScan>>({});
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState("");
  const [again, setAgain] = useState(0);
  const [hover, setHover] = useState<string | null>(null);
  const seen = useRef("");
  const requestKey = useMemo(() => JSON.stringify(connections.map(c => [c.id, c.dataDir])), [connections]);

  useEffect(() => {
    let stopped = false;
    const read = () => {
      invoke<EcoScan[]>("scan_ecosystem", { requests: connections.map(c => ({ id: c.id, dataDir: c.dataDir })) })
        .then(list => {
          if (stopped) return;
          const json = JSON.stringify(list);
          if (json !== seen.current) { seen.current = json; setScans(Object.fromEntries(list.map(s => [s.id, s]))); }
          setLoaded(true);
          setError("");
        })
        .catch(e => { if (!stopped) { setError(String(e)); setLoaded(true); } });
    };
    read();
    const timer = setInterval(read, REFRESH_MS);
    return () => { stopped = true; clearInterval(timer); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [requestKey, again]);

  const layout = useMemo(() => layoutEcosystem(connections, scans), [connections, scans]);
  const totals = useMemo(() => ecosystemTotals(connections, scans), [connections, scans]);
  const related = useMemo(() => (hover ? relatedTo(layout.nodes, hover) : null), [hover, layout]);
  const tipNode = hover ? layout.nodes.find(n => n.id === hover) : undefined;
  const tip = tipNode ? tipOf(tipNode, layout.nodes, lang) : undefined;
  const tipUp = tipNode !== undefined && tipNode.cy > layout.height - 190;

  return (
    <>
      <div className="eco-stats" aria-live="polite">
        <span><b>{totals.connections}</b>{t(L(" connections", "개 연결"))}</span>
        <span><b>{totals.databases}</b>{t(L(" databases", "개 데이터베이스"))}</span>
        <span><b>{num(totals.tables)}</b>{t(L(" tables", "개 테이블"))}</span>
        <span><b>{num(totals.rows)}</b>{t(L(" rows", "개 행"))}</span>
        <span><b>{humanBytes(totals.bytes)}</b>{t(L(" on disk", " 디스크"))}</span>
        <button className="eco-refresh" onClick={() => setAgain(n => n + 1)} title={t(L("Read the data folders again", "데이터 폴더를 다시 읽기"))}>
          <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M17.65 6.35A7.96 7.96 0 0012 4a8 8 0 108 8h-2a6 6 0 11-1.76-4.24L13 11h7V4l-2.35 2.35z" /></svg>
          {t(L("Refresh", "새로 고침"))}
        </button>
      </div>

      <div className="eco-scroll">
        {!loaded ? (
          <div className="eco-note">{t(L("Reading the data folders…", "데이터 폴더를 읽는 중…"))}</div>
        ) : connections.length === 0 ? (
          <div className="eco-note">{t(L("There is no saved connection yet. Add one on the home screen and it appears here.", "저장된 연결이 아직 없습니다. 홈 화면에서 연결을 추가하면 여기에 나타납니다."))}</div>
        ) : (
          <div className="eco-wrap">
            <svg className="eco-svg" viewBox={`0 0 ${layout.width} ${layout.height}`} role="group"
              aria-label={t(L("Cylinder picture of every connection, database and table", "모든 연결 · 데이터베이스 · 테이블의 원통 그림"))}>
              {layout.groups.map(g => (
                <rect key={g.id} className={`eco-group${related && !related.has(g.id) ? " dim" : ""}`} x={g.x} y={g.y} width={g.w} height={g.h} rx="12" />
              ))}
              <g>
                {layout.arrows.map((a, i) => {
                  const c = ARROW_COLOR[a.depth] ?? C.core;
                  const lit = !related || (related.has(a.from) && related.has(a.to));
                  return (
                    <g key={`${a.from}>${a.to}>${i}`} className={`eco-arrow${lit ? "" : " dim"}`} style={tint(c)}>
                      <path className="dg-aline" d={a.d} markerEnd={`url(#${headId(c)})`} />
                      <path className="dg-aflow" d={a.d} />
                      <circle className="dg-apacket" r="3.2">
                        <animateMotion dur="2.8s" begin={`${-((i * 0.53) % 2.8).toFixed(2)}s`} repeatCount="indefinite" path={a.d} />
                      </circle>
                    </g>
                  );
                })}
              </g>
              {layout.nodes.map(n => {
                const caption = captionOf(n);
                const below = n.cy + n.h / 2 + 16;
                return caption ? (
                  <text key={`cap-${n.id}`} className={`eco-caption${related && !related.has(n.id) ? " dim" : ""}`} x={n.cx} y={below} textAnchor="middle">{t(caption)}</text>
                ) : null;
              })}
              {layout.nodes.map(n => (
                <Cylinder key={n.id} n={n} glow={n.depth + (n.order % 4) * 0.12} dim={related !== null && !related.has(n.id)}
                  onEnter={() => setHover(n.id)} onLeave={() => setHover(h => (h === n.id ? null : h))} />
              ))}
            </svg>

            {tip && tipNode && (
              <div className={`eco-tip${tipUp ? " up" : ""}`}
                style={{
                  ...tint(tip.color),
                  left: `${Math.min(84, Math.max(16, (tipNode.cx / layout.width) * 100))}%`,
                  top: `${((tipUp ? tipNode.cy - tipNode.h / 2 : tipNode.cy + tipNode.h / 2 + (captionOf(tipNode) ? 14 : 0)) / layout.height) * 100}%`,
                }}>
                <b>{tip.title}</b>
                {tip.lines.map((line, i) => <span key={i}>{t(line)}</span>)}
              </div>
            )}
          </div>
        )}
        {error && <div className="eco-note eco-error">{error}</div>}
      </div>

      <div className="eco-legend">
        <span><i style={tint(C.server)} className="eco-key" />{t(L("RuSQL", "RuSQL"))}</span>
        <span><i style={tint(C.core)} className="eco-key" />{t(L("connection", "연결"))}</span>
        <span><i style={tint(C.txn)} className="eco-key" />{t(L("database", "데이터베이스"))}</span>
        <span><i style={tint(C.client)} className="eco-key" />{t(L("table", "테이블"))}</span>
        <span><i className="eco-key eco-key-dashed" />{t(L("dashed: no data folder yet", "점선: 아직 데이터 폴더 없음"))}</span>
        <span className="eco-hint">{t(L("Hover a cylinder: details, and its path through the picture stays lit", "원통에 마우스를 올리면 설명이 보이고, 그 원통의 경로만 밝게 남습니다"))}</span>
      </div>
    </>
  );
}

export default function EcosystemView({ connections, onBack }: { connections: EcoConn[]; onBack: () => void }) {
  const [lang, setLang] = useState<Lang>(loadLang);
  const choose = (l: Lang) => {
    setLang(l);
    try { localStorage.setItem(LANG_KEY, l); } catch { /* the choice is only not remembered */ }
  };
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if (e.key === "Escape") onBack(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onBack]);

  return (
    <LangContext.Provider value={lang}>
      <div className="eco-window" lang={lang}>
        <ArrowDefs />
        <div className="eco-header">
          <button className="eco-back" onClick={onBack} title="Esc">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z" /></svg>
            {lang === "ko" ? "홈으로" : "Back to Home"}
          </button>
          <div className="eco-title">
            <EcosystemLogo />
            <h1>{lang === "ko" ? "RuSQL 생태계" : "RuSQL Ecosystem"}</h1>
            <span>{lang === "ko" ? "모든 연결 · 데이터베이스 · 테이블을 한눈에" : "Every connection, database and table at a glance"}</span>
          </div>
          <div className="dg-seg" role="group" aria-label="Language">
            <button className={lang === "ko" ? "active" : ""} aria-pressed={lang === "ko"} onClick={() => choose("ko")}>한국어</button>
            <button className={lang === "en" ? "active" : ""} aria-pressed={lang === "en"} onClick={() => choose("en")}>EN</button>
          </div>
        </div>
        <Picture connections={connections} lang={lang} />
      </div>
    </LangContext.Provider>
  );
}
