// BTreeView.tsx
// "B+Tree" activity-bar page: a teaching B+Tree that is drawn step by step. Insert, delete or look up a key and the page replays what happens -- the walk down to the
// leaf, a node that overflows and splits (the key that goes up), a node that underflows and borrows from a sibling or merges with it, the root that grows or
// shrinks -- one frame at a time (play / pause / step / speed). The tree and its frames come from lib/bplusTree.ts; every visible string is an English/Korean pair
// with the same EN/KO toggle as the Diagram page. The look (hero, glowing nodes, flowing edges, the progress line) is PageKit.css + BTreeView.css.

import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { BPlusTree } from "../lib/bplusTree";
import type { Frame, FrameKind, Snap } from "../lib/bplusTree";
import { C, L, LangContext, tint, useT } from "./diagramShared";
import type { Lang } from "./diagramShared";
import { CountUp, PageHero, rise } from "./PageKit";
import "./BTreeView.css";

const LANG_KEY = "rusql_diagram_lang";
function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

const KEY_W = 56, KEY_H = 48, PAD = 9, GAP = 38, LEVEL_H = 132, MARGIN = 32;
// a tree that is narrower than its board is drawn up to MAX_ZOOM times larger (a small tree fills the stage); one that is wider is drawn smaller to fit, down to MIN_ZOOM,
// and scrolls beyond that
const MAX_ZOOM = 1.6, MIN_ZOOM = 0.62;
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

// the four ideas the page shows, each in the color of the step it belongs to
const IDEAS: { c: string; title: { en: string; ko: string }; text: { en: string; ko: string } }[] = [
  { c: "#569cd6", title: { en: "Walk down", ko: "아래로 걷기" },
    text: { en: "At each level the key is compared with the separators and one child is followed, down to a leaf: about log N steps, however many keys there are.", ko: "층마다 키를 구분 키와 비교해 자식 하나를 따라 잎까지 내려갑니다. 키가 아무리 많아도 log N 걸음 안팎입니다." } },
  { c: "#c586c0", title: { en: "Split", ko: "쪼개기" },
    text: { en: "A node that gets one key too many splits in two and sends its middle key up. If the root splits, the tree grows one level.", ko: "키가 하나 넘친 노드는 둘로 쪼개져 가운데 키를 위로 올립니다. 루트가 쪼개지면 트리가 한 층 자랍니다." } },
  { c: "#9cdcfe", title: { en: "Borrow or merge", ko: "빌리기와 합치기" },
    text: { en: "A node left with too few keys borrows one from a sibling, or merges with it. When the root has one child left, the tree shrinks a level.", ko: "키가 모자란 노드는 형제에게서 하나 빌리거나 형제와 합쳐집니다. 루트에 자식이 하나만 남으면 한 층 줄어듭니다." } },
  { c: "#4ec9b0", title: { en: "Linked leaves", ko: "이어진 잎" },
    text: { en: "The leaves point to each other from left to right, so a range scan finds its first key once and then just walks along.", ko: "잎은 왼쪽에서 오른쪽으로 서로 이어져서, 범위 조회는 첫 키를 한 번 찾고 그다음은 따라 걷기만 합니다." } },
];

function TreeSvg({ frame, previous, room }: { frame: Frame; previous: Frame | null; room: number }) {
  const { snap } = frame;
  const laid = useMemo(() => layout(snap), [snap]);
  const fresh = previous ? new Set(Object.keys(snap.nodes).filter(id => !(id in previous.snap.nodes)).map(Number)) : new Set<number>();
  const hi = new Set(frame.hi);
  const ids = Object.keys(snap.nodes).map(Number);
  const boardW = Math.max(laid.width, 420), shift = (boardW - laid.width) / 2;
  const zoom = Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, room / boardW));
  const boardH = Math.max(laid.height, 200);
  return (
    <svg className={`bt-svg bt-k-${frame.kind}`} viewBox={`${-shift} 0 ${boardW} ${boardH}`} preserveAspectRatio="xMidYMin meet" style={{ width: boardW * zoom, height: boardH * zoom }}>
      <defs>
        <marker id="bt-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10 z" fill="#4ec9b0" /></marker>
        <linearGradient id="bt-grad-sheen" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stopColor="#ffffff" stopOpacity="0.2" /><stop offset="55%" stopColor="#ffffff" stopOpacity="0" /></linearGradient>
        <linearGradient id="bt-grad-leaf" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stopColor="#35564d" /><stop offset="100%" stopColor="#233b35" /></linearGradient>
        <linearGradient id="bt-grad-inner" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stopColor="#3b3a58" /><stop offset="100%" stopColor="#272740" /></linearGradient>
      </defs>
      {ids.map(id => {
        const n = snap.nodes[id], p = laid.pos[id];
        if (!p) return null;
        return n.children.map((c, i) => {
          const q = laid.pos[c];
          const x1 = p.x + PAD + i * KEY_W, y1 = p.y + KEY_H, x2 = q.x + q.w / 2, y2 = q.y, ym = (y1 + y2) / 2;
          const hot = hi.has(id) && hi.has(c); // (an edge between two nodes the step is about: the walk down)
          return <path key={`${id}-${c}`} className={`bt-edge ${hot ? "bt-edge-hot" : ""}`} style={{ d: `path("M${x1} ${y1} C${x1} ${ym} ${x2} ${ym} ${x2} ${y2}")` } as React.CSSProperties} />;
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
        const cls = ["bt-node", n.leaf ? "bt-leaf" : "bt-inner", hi.has(id) ? `bt-hi bt-k-${frame.kind}` : ""].join(" ");
        return (
          <g key={id} className={cls} style={{ transform: `translate(${p.x}px, ${p.y}px)` }}>
            <g className={fresh.has(id) ? "bt-pop" : ""}>
              <rect className="bt-box" width={p.w} height={KEY_H} rx={12} />
              <rect className="bt-sheen" width={p.w - 4} height={KEY_H / 2} x={2} y={2} rx={10} />
              {n.keys.length === 0 && <text className="bt-empty" x={p.w / 2} y={KEY_H / 2 + 7} textAnchor="middle">∅</text>}
              {n.keys.map((k, i) => (
                <g key={i}>
                  {i > 0 && <line className="bt-div" x1={PAD + i * KEY_W} y1={8} x2={PAD + i * KEY_W} y2={KEY_H - 8} />}
                  <text className="bt-key" x={PAD + i * KEY_W + KEY_W / 2} y={KEY_H / 2 + 7.5} textAnchor="middle">{k}</text>
                </g>
              ))}
            </g>
          </g>
        );
      })}
    </svg>
  );
}

function Body({ lang, onLang }: { lang: Lang; onLang: (l: Lang) => void }) {
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
  const canvas = useRef<HTMLDivElement>(null);
  const [room, setRoom] = useState(0);
  useLayoutEffect(() => {
    const el = canvas.current;
    if (!el) return;
    const measure = () => setRoom(Math.max(0, el.clientWidth - 40)); // (minus the padding of the board)
    measure();
    if (typeof ResizeObserver === "undefined") return;
    const watch = new ResizeObserver(measure);
    watch.observe(el);
    return () => watch.disconnect();
  }, []);

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
  const empty = keyCount === 0 && Object.keys(frame.snap.nodes).length === 1;
  const at = Math.min(cur, frames.length - 1);

  return (
    <div className="bt-page pk-page">
      <PageHero accent={C.storage} lang={lang} onLang={onLang}
        icon={<svg viewBox="0 0 24 24"><rect x="8" y="2.5" width="8" height="5" rx="1.6" /><rect x="1.8" y="16.5" width="8" height="5" rx="1.6" /><rect x="14.2" y="16.5" width="8" height="5" rx="1.6" /><path d="M12 7.5v3.2M12 10.7H5.8v5.8M12 10.7h6.2v5.8" /></svg>}
        title={L("B+Tree: how an index splits and rebalances", "B+Tree 애니메이션")}
        lead={L("Every key lives in a leaf and the leaves are linked, so a range scan walks along them. A node that grows past its limit splits and sends a key up; a node that shrinks below its minimum borrows from a sibling or merges with it. Try inserting 1 to 12 in order, then deleting keys.",
                "모든 키는 잎에 있고 잎끼리 이어져 있어서 범위 조회는 잎을 따라 걷습니다. 한도를 넘긴 노드는 쪼개져 키 하나를 위로 올리고, 최소에 못 미치는 노드는 형제에게서 빌리거나 합쳐집니다. 1부터 12까지 차례로 넣은 뒤 키를 지워 보세요.")} />

      <div className="bt-toolbar pk-card pk-rise" style={{ ...tint(C.storage), ...rise(1) }}>
        <input className="pk-input bt-input" type="number" min={1} max={999} placeholder={t(L("key", "키"))} value={text}
          onChange={e => setText(e.target.value)} onKeyDown={e => { const k = keyOf(); if (e.key === "Enter" && k !== null) run(tr => tr.insert(k)); }} />
        <button className="pk-btn primary" disabled={keyOf() === null} onClick={() => { const k = keyOf(); if (k !== null) run(tr => tr.insert(k)); }}>{t(L("Insert", "삽입"))}</button>
        <button className="pk-btn" disabled={keyOf() === null} onClick={() => { const k = keyOf(); if (k !== null) run(tr => tr.remove(k)); }}>{t(L("Delete", "삭제"))}</button>
        <button className="pk-btn" disabled={keyOf() === null} onClick={() => { const k = keyOf(); if (k !== null) run(tr => tr.search(k)); }}>{t(L("Search", "찾기"))}</button>
        <span className="bt-sep" />
        <button className="pk-btn" onClick={() => run(tr => { const out: Frame[] = []; for (let k = 1; k <= 12; k++) if (!tr.has(k)) out.push(...tr.insert(k)); return out; })}>{t(L("Insert 1–12", "1–12 넣기"))}</button>
        <button className="pk-btn" onClick={() => run(tr => randomKeys(tr, 8).flatMap(k => tr.insert(k)))}>{t(L("Random ×8", "무작위 ×8"))}</button>
        <button className="pk-btn" onClick={() => reset(maxKeys)}>{t(L("Reset", "비우기"))}</button>
        <span className="bt-sep" />
        <label className="bt-label">{t(L("max keys per node", "노드당 최대 키"))}
          <select className="pk-select" value={maxKeys} onChange={e => { const m = Number(e.target.value); setMaxKeys(m); reset(m); }}>
            {[3, 4, 5].map(m => <option key={m} value={m}>{m}</option>)}
          </select>
        </label>
      </div>

      <div className={`bt-stage pk-rise bt-k-${frame.kind}`} style={{ ...tint(C.storage), ...rise(2) }}>
        <div className="bt-canvas" ref={canvas}>
          <TreeSvg frame={frame} previous={cur > 0 ? frames[cur - 1] : null} room={room} />
          {empty && <div className="bt-hint">{t(L("Type a key and press Insert, or try “Insert 1–12”", "키를 넣고 삽입을 눌러 보세요. “1–12 넣기”도 좋습니다"))}</div>}
        </div>
        <div className="bt-msg">
          <span className="bt-dot-live" aria-hidden="true" />
          <span className="bt-badge">{t(KIND_LABEL[frame.kind])}</span>
          <span className="bt-text" key={`${at}-${frame.kind}`}>{t(frame.msg)}</span>
          <span className="bt-stats">
            <span className="bt-stat"><b><CountUp value={keyCount} ms={500} /></b> {t(L("keys", "키"))}</span>
            <span className="bt-stat"><b><CountUp value={height} ms={500} /></b> {t(L("levels", "층"))}</span>
          </span>
        </div>
        <div className="bt-progress" aria-hidden="true"><i style={{ width: `${((at + 1) / frames.length) * 100}%` }} /></div>
      </div>

      <div className="bt-play pk-rise" style={{ ...tint(C.storage), ...rise(3) }}>
        <button className="pk-btn bt-round" disabled={cur <= 0} onClick={() => { setPlaying(false); setCur(c => Math.max(0, c - 1)); }} aria-label={t(L("previous step", "이전 단계"))}>◀</button>
        <button className="pk-btn primary bt-playbtn" onClick={() => { if (done) setCur(0); setPlaying(p => (done ? true : !p)); }}>
          {done ? t(L("↻ Replay", "↻ 다시 보기")) : playing ? t(L("❚❚ Pause", "❚❚ 멈춤")) : t(L("▶ Play", "▶ 재생"))}
        </button>
        <button className="pk-btn bt-round" disabled={done} onClick={() => { setPlaying(false); setCur(c => Math.min(frames.length - 1, c + 1)); }} aria-label={t(L("next step", "다음 단계"))}>▶</button>
        <input className="bt-slider" type="range" min={0} max={Math.max(0, frames.length - 1)} value={at} onChange={e => { setPlaying(false); setCur(Number(e.target.value)); }} />
        <span className="bt-count">{at + 1} / {frames.length}</span>
        <select className="pk-select" value={speed} onChange={e => setSpeed(Number(e.target.value))}>
          <option value={0.5}>0.5x</option><option value={1}>1x</option><option value={2}>2x</option><option value={4}>4x</option>
        </select>
      </div>

      <div className="bt-legend pk-rise" style={rise(4)}>
        {LEGEND.map(k => <span key={k} className={`bt-leg bt-k-${k}`}><i className="bt-dot" />{t(KIND_LABEL[k])}</span>)}
        <span className="bt-leg"><i className="bt-dot bt-link-dot" />{t(L("leaf link", "잎 연결"))}</span>
      </div>

      <div className="bt-ideas">
        {IDEAS.map((idea, i) => (
          <div key={i} className="bt-idea pk-card pk-rise" style={{ ...tint(idea.c), ...rise(5 + i) }}>
            <b>{t(idea.title)}</b>
            <span>{t(idea.text)}</span>
          </div>
        ))}
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
      <div className="pk-scroll bt-scroll" style={tint(C.storage)}>
        <Body lang={lang} onLang={pick} />
      </div>
    </LangContext.Provider>
  );
}
