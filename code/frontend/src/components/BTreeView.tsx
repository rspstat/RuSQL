// BTreeView.tsx
// "B+Tree" activity-bar page: a teaching B+Tree that is drawn step by step. Insert, delete or look up a key and the page replays what happens -- the walk down to the
// leaf, a node that overflows and splits (the key that goes up), a node that underflows and borrows from a sibling or merges with it, the root that grows or
// shrinks -- one frame at a time (play / pause / step / speed). The tree and its frames come from lib/bplusTree.ts; every visible string is an English/Korean pair
// with the same EN/KO toggle as the Diagram page.

import { useEffect, useMemo, useRef, useState } from "react";
import { BPlusTree } from "../lib/bplusTree";
import type { Frame, FrameKind, Snap } from "../lib/bplusTree";
import { L, LangContext, useT } from "./diagramShared";
import type { Lang } from "./diagramShared";
import "./BTreeView.css";

const LANG_KEY = "rusql_diagram_lang";
function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

const KEY_W = 34, KEY_H = 30, PAD = 6, GAP = 24, LEVEL_H = 90, MARGIN = 20;
const STEP_MS = 1300;

interface Pos { x: number; y: number; w: number }
interface Laid { pos: Record<number, Pos>; width: number; height: number }

const widthOf = (keys: number) => Math.max(1, keys) * KEY_W + 2 * PAD;

// leaves left to right, every internal node centred over its children
function layout(snap: Snap): Laid {
  const pos: Record<number, Pos> = {};
  let cursor = MARGIN, levels = 0;
  const place = (id: number, depth: number) => {
    levels = Math.max(levels, depth + 1);
    const n = snap.nodes[id];
    const w = widthOf(n.keys.length);
    if (n.leaf) {
      pos[id] = { x: cursor, y: MARGIN + depth * LEVEL_H, w };
      cursor += w + GAP;
      return;
    }
    n.children.forEach(c => place(c, depth + 1));
    const first = pos[n.children[0]], last = pos[n.children[n.children.length - 1]];
    pos[id] = { x: (first.x + last.x + last.w) / 2 - w / 2, y: MARGIN + depth * LEVEL_H, w };
  };
  place(snap.root, 0);
  return { pos, width: cursor - GAP + MARGIN, height: MARGIN * 2 + (levels - 1) * LEVEL_H + KEY_H };
}

const KIND_LABEL: Record<FrameKind, { en: string; ko: string }> = {
  ready: { en: "ready", ko: "준비" }, visit: { en: "search path", ko: "탐색 경로" }, insert: { en: "insert", ko: "삽입" }, overflow: { en: "overflow", ko: "넘침" },
  split: { en: "split", ko: "분할" }, delete: { en: "delete", ko: "삭제" }, underflow: { en: "underflow", ko: "모자람" }, borrow: { en: "borrow", ko: "빌려옴" },
  merge: { en: "merge", ko: "병합" }, root: { en: "new root", ko: "루트 변화" }, found: { en: "found", ko: "찾음" }, miss: { en: "not found", ko: "없음" },
};

const LEGEND: FrameKind[] = ["visit", "insert", "overflow", "split", "delete", "underflow", "borrow", "merge", "found"];

function TreeSvg({ frame, previous }: { frame: Frame; previous: Frame | null }) {
  const { snap } = frame;
  const laid = useMemo(() => layout(snap), [snap]);
  const fresh = previous ? new Set(Object.keys(snap.nodes).filter(id => !(id in previous.snap.nodes)).map(Number)) : new Set<number>();
  const hi = new Set(frame.hi);
  const ids = Object.keys(snap.nodes).map(Number);
  return (
    <svg className="bt-svg" viewBox={`0 0 ${Math.max(laid.width, 320)} ${laid.height}`} preserveAspectRatio="xMidYMin meet" style={{ height: Math.max(laid.height, 150) }}>
      <defs>
        <marker id="bt-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0 L10 5 L0 10 z" fill="#4ec9b0" /></marker>
      </defs>
      {ids.map(id => {
        const n = snap.nodes[id], p = laid.pos[id];
        if (!p) return null;
        return n.children.map((c, i) => {
          const q = laid.pos[c];
          const x1 = p.x + PAD + i * KEY_W, y1 = p.y + KEY_H, x2 = q.x + q.w / 2, y2 = q.y, ym = (y1 + y2) / 2;
          return <path key={`${id}-${c}`} className="bt-edge" style={{ d: `path("M${x1} ${y1} C${x1} ${ym} ${x2} ${ym} ${x2} ${y2}")` } as React.CSSProperties} />;
        });
      })}
      {ids.map(id => {
        const n = snap.nodes[id], p = laid.pos[id];
        if (!p || !n.leaf || n.next === null || !laid.pos[n.next]) return null;
        const q = laid.pos[n.next];
        return <line key={`next-${id}`} className="bt-link" x1={p.x + p.w} y1={p.y + KEY_H / 2} x2={q.x - 2} y2={q.y + KEY_H / 2} markerEnd="url(#bt-arrow)" />;
      })}
      {ids.map(id => {
        const n = snap.nodes[id], p = laid.pos[id];
        if (!p) return null;
        const cls = ["bt-node", n.leaf ? "bt-leaf" : "bt-inner", hi.has(id) ? `bt-hi bt-k-${frame.kind}` : "", fresh.has(id) ? "bt-new" : ""].join(" ");
        return (
          <g key={id} className={cls} style={{ transform: `translate(${p.x}px, ${p.y}px)` }}>
            <rect className="bt-box" width={p.w} height={KEY_H} rx={5} />
            {n.keys.length === 0 && <text className="bt-empty" x={p.w / 2} y={KEY_H / 2 + 4} textAnchor="middle">∅</text>}
            {n.keys.map((k, i) => (
              <g key={i}>
                {i > 0 && <line className="bt-div" x1={PAD + i * KEY_W} y1={4} x2={PAD + i * KEY_W} y2={KEY_H - 4} />}
                <text className="bt-key" x={PAD + i * KEY_W + KEY_W / 2} y={KEY_H / 2 + 5} textAnchor="middle">{k}</text>
              </g>
            ))}
          </g>
        );
      })}
    </svg>
  );
}

function Body() {
  const t = useT();
  const [maxKeys, setMaxKeys] = useState(3);
  const tree = useRef(new BPlusTree(3));
  const [frames, setFrames] = useState<Frame[]>(() => [tree.current.frame("ready", [], { en: "An empty tree: one empty leaf that is also the root", ko: "빈 트리: 루트이자 잎인 빈 노드 하나" })]);
  const [cur, setCur] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(1);
  const [text, setText] = useState("");

  const frame = frames[Math.min(cur, frames.length - 1)];
  const done = cur >= frames.length - 1;

  useEffect(() => {
    if (!playing) return;
    if (done) { setPlaying(false); return; }
    const timer = setTimeout(() => setCur(c => c + 1), STEP_MS / speed);
    return () => clearTimeout(timer);
  }, [playing, cur, done, speed]);

  const run = (make: (tr: BPlusTree) => Frame[]) => {
    const tr = tree.current;
    const start = tr.frame("ready", [], { en: "The tree as it is now", ko: "지금의 트리" });
    const made = make(tr);
    setFrames([start, ...made]);
    setCur(0);
    setPlaying(true);
  };
  const keyOf = () => {
    const k = parseInt(text, 10);
    return Number.isFinite(k) ? k : null;
  };
  const reset = (m: number) => {
    tree.current = new BPlusTree(m);
    setFrames([tree.current.frame("ready", [], { en: "An empty tree: one empty leaf that is also the root", ko: "빈 트리: 루트이자 잎인 빈 노드 하나" })]);
    setCur(0);
    setPlaying(false);
  };
  const randomKeys = (tr: BPlusTree, count: number) => {
    const out: number[] = [];
    while (out.length < count && out.length < 90) {
      const k = 1 + Math.floor(Math.random() * 99);
      if (!tr.has(k) && !out.includes(k)) out.push(k);
    }
    return out;
  };

  const height = (() => { let d = 1, n = frame.snap.nodes[frame.snap.root]; while (n && !n.leaf) { n = frame.snap.nodes[n.children[0]]; d++; } return d; })();
  const keyCount = Object.values(frame.snap.nodes).filter(n => n.leaf).reduce((s, n) => s + n.keys.length, 0);

  return (
    <div className="bt-page">
      <h1>{t(L("B+Tree: how an index splits and rebalances", "B+Tree: 인덱스가 쪼개지고 균형을 맞추는 모습"))}</h1>
      <p className="bt-sub">
        {t(L("Every key lives in a leaf and the leaves are linked, so a range scan walks along them. A node that grows past its limit splits and sends a key up; a node that shrinks below its minimum borrows from a sibling or merges with it. Try inserting 1 to 12 in order, then deleting keys.",
             "모든 키는 잎에 있고 잎끼리 이어져 있어서 범위 조회는 잎을 따라 걷습니다. 한도를 넘긴 노드는 쪼개져 키 하나를 위로 올리고, 최소에 못 미치는 노드는 형제에게서 빌리거나 합쳐집니다. 1부터 12까지 차례로 넣은 뒤 키를 지워 보세요."))}
      </p>

      <div className="bt-controls">
        <input className="bt-input" type="number" min={1} max={999} placeholder={t(L("key", "키"))} value={text}
          onChange={e => setText(e.target.value)} onKeyDown={e => { const k = keyOf(); if (e.key === "Enter" && k !== null) run(tr => tr.insert(k)); }} />
        <button className="bt-btn bt-primary" disabled={keyOf() === null} onClick={() => { const k = keyOf(); if (k !== null) run(tr => tr.insert(k)); }}>{t(L("Insert", "삽입"))}</button>
        <button className="bt-btn" disabled={keyOf() === null} onClick={() => { const k = keyOf(); if (k !== null) run(tr => tr.remove(k)); }}>{t(L("Delete", "삭제"))}</button>
        <button className="bt-btn" disabled={keyOf() === null} onClick={() => { const k = keyOf(); if (k !== null) run(tr => tr.search(k)); }}>{t(L("Search", "찾기"))}</button>
        <span className="bt-sep" />
        <button className="bt-btn" onClick={() => run(tr => { const out: Frame[] = []; for (let k = 1; k <= 12; k++) if (!tr.has(k)) out.push(...tr.insert(k)); return out; })}>{t(L("Insert 1–12", "1–12 넣기"))}</button>
        <button className="bt-btn" onClick={() => run(tr => randomKeys(tr, 8).flatMap(k => tr.insert(k)))}>{t(L("Random ×8", "무작위 ×8"))}</button>
        <button className="bt-btn" onClick={() => reset(maxKeys)}>{t(L("Reset", "비우기"))}</button>
        <span className="bt-sep" />
        <label className="bt-label">{t(L("max keys per node", "노드당 최대 키"))}
          <select className="bt-select" value={maxKeys} onChange={e => { const m = Number(e.target.value); setMaxKeys(m); reset(m); }}>
            {[3, 4, 5].map(m => <option key={m} value={m}>{m}</option>)}
          </select>
        </label>
      </div>

      <div className="bt-stage">
        <div className="bt-canvas">
          <TreeSvg frame={frame} previous={cur > 0 ? frames[cur - 1] : null} />
        </div>
        <div className={`bt-msg bt-k-${frame.kind}`}>
          <span className="bt-badge">{t(KIND_LABEL[frame.kind])}</span>
          <span>{t(frame.msg)}</span>
          <span className="bt-stats">{t(L(`${keyCount} keys · ${height} levels`, `키 ${keyCount}개 · ${height}층`))}</span>
        </div>
      </div>

      <div className="bt-play">
        <button className="bt-btn" disabled={cur <= 0} onClick={() => { setPlaying(false); setCur(c => Math.max(0, c - 1)); }}>◀</button>
        <button className="bt-btn bt-primary" onClick={() => { if (done) setCur(0); setPlaying(p => (done ? true : !p)); }}>
          {done ? t(L("↻ Replay", "↻ 다시 보기")) : playing ? t(L("❚❚ Pause", "❚❚ 멈춤")) : t(L("▶ Play", "▶ 재생"))}
        </button>
        <button className="bt-btn" disabled={done} onClick={() => { setPlaying(false); setCur(c => Math.min(frames.length - 1, c + 1)); }}>▶</button>
        <input className="bt-slider" type="range" min={0} max={Math.max(0, frames.length - 1)} value={Math.min(cur, frames.length - 1)} onChange={e => { setPlaying(false); setCur(Number(e.target.value)); }} />
        <span className="bt-count">{Math.min(cur, frames.length - 1) + 1} / {frames.length}</span>
        <select className="bt-select" value={speed} onChange={e => setSpeed(Number(e.target.value))}>
          <option value={0.5}>0.5x</option><option value={1}>1x</option><option value={2}>2x</option><option value={4}>4x</option>
        </select>
      </div>

      <div className="bt-legend">
        {LEGEND.map(k => <span key={k} className="bt-leg"><i className={`bt-dot bt-k-${k}`} />{t(KIND_LABEL[k])}</span>)}
        <span className="bt-leg"><i className="bt-dot bt-link-dot" />{t(L("leaf link", "잎 연결"))}</span>
      </div>
    </div>
  );
}

export default function BTreeView() {
  const [lang, setLang] = useState<Lang>(loadLang);
  const pick = (next: Lang) => {
    setLang(next);
    try { localStorage.setItem(LANG_KEY, next); } catch { /* not saved */ }
  };
  return (
    <LangContext.Provider value={lang}>
      <div className="bt-scroll">
        <div className="bt-lang">
          <button className={lang === "ko" ? "on" : ""} onClick={() => pick("ko")}>KO</button>
          <button className={lang === "en" ? "on" : ""} onClick={() => pick("en")}>EN</button>
        </div>
        <Body />
      </div>
    </LangContext.Provider>
  );
}
