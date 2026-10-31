// BTreeView.tsx
// "B+Tree" activity-bar page: one small B+Tree that is drawn by itself, step by step, over and over -- the keys 1 to 12 go in one after the other (a node that gets a fourth
// key splits and sends a key up, and the tree grows a level), then the way down to the key 7. Nothing to type: only pause / play and start over. The tree and its frames
// come from lib/bplusTree.ts; every visible string is an English/Korean pair with the same EN/KO toggle as the Diagram page. The look is PageKit.css + BTreeView.css.

import { useLayoutEffect, useMemo, useRef, useState } from "react";
import { BPlusTree } from "../lib/bplusTree";
import type { Frame, FrameKind, Snap } from "../lib/bplusTree";
import { C, L, LangContext, tint, useT } from "./diagramShared";
import type { Lang, Text } from "./diagramShared";
import { CountUp, PageHero, PlayBar, rise, usePlayer } from "./PageKit";
import "./BTreeView.css";

const LANG_KEY = "rusql_diagram_lang";
function loadLang(): Lang {
  try { return localStorage.getItem(LANG_KEY) === "en" ? "en" : "ko"; } catch { return "ko"; }
}

const KEY_W = 52, KEY_H = 48, PAD = 8, GAP = 28, LEVEL_H = 190, MARGIN = 36, GUTTER = 96;
// a tree that is narrower than its board is drawn up to MAX_ZOOM times larger (a small tree fills the stage); one that is wider is drawn smaller to fit, down to MIN_ZOOM,
// and scrolls beyond that
const MAX_ZOOM = 1.1, MIN_ZOOM = 0.62;
const MAX_KEYS = 3; // (a node holds up to this many keys)

interface Pos { x: number; y: number; w: number }
interface Laid { pos: Record<number, Pos>; width: number; height: number; levels: number }

const widthOf = (keys: number) => Math.max(1, keys) * KEY_W + 2 * PAD;
const rowY = (depth: number) => MARGIN + depth * LEVEL_H;

// leaves left to right, every internal node centred over its children; the left GUTTER is where the names of the levels stand
function layout(snap: Snap): Laid {
  const pos: Record<number, Pos> = {};
  let cursor = GUTTER, levels = 0;
  const place = (id: number, depth: number) => {
    levels = Math.max(levels, depth + 1);
    const n = snap.nodes[id];
    const w = widthOf(n.keys.length);
    if (n.leaf) {
      pos[id] = { x: cursor, y: rowY(depth), w };
      cursor += w + GAP;
      return;
    }
    n.children.forEach(c => place(c, depth + 1));
    const first = pos[n.children[0]], last = pos[n.children[n.children.length - 1]];
    pos[id] = { x: (first.x + last.x + last.w) / 2 - w / 2, y: rowY(depth), w };
  };
  place(snap.root, 0);
  return { pos, width: cursor - GAP + MARGIN, height: MARGIN * 2 + (levels - 1) * LEVEL_H + KEY_H, levels };
}

// the size a laid-out tree is drawn at on a board `room` wide
const fit = (laid: Laid, room: number) => {
  const boardW = Math.max(laid.width, 520), boardH = Math.max(laid.height, 200);
  return { boardW, boardH, shift: (boardW - laid.width) / 2, zoom: Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, room / boardW)) };
};

const KIND_LABEL: Record<FrameKind, { en: string; ko: string }> = {
  ready: { en: "ready", ko: "준비" }, visit: { en: "search path", ko: "탐색 경로" }, insert: { en: "insert", ko: "삽입" }, overflow: { en: "overflow", ko: "넘침" },
  split: { en: "split", ko: "분할" }, delete: { en: "delete", ko: "삭제" }, underflow: { en: "underflow", ko: "모자람" }, borrow: { en: "borrow", ko: "빌려옴" },
  merge: { en: "merge", ko: "병합" }, root: { en: "new root", ko: "루트 변화" }, found: { en: "found", ko: "찾음" }, miss: { en: "not found", ko: "없음" },
};

// How many levels a snapshot has
const heightOf = (snap: Snap) => { let d = 1, n = snap.nodes[snap.root]; while (n && !n.leaf) { n = snap.nodes[n.children[0]]; d++; } return d; };

// What is shown, in order: an empty tree, the keys 1 to 12 going in (the walk down to the leaf is left out: what matters is the key that goes in, the node that overflows and the
// split), then the way down to the key 7. Each step has a short sentence and the time it stays.
interface Step { frame: Frame; caption: Text; ms: number }

function buildSteps(): Step[] {
  const tree = new BPlusTree(MAX_KEYS);
  const steps: Step[] = [{
    frame: tree.frame("ready", [], { en: "", ko: "" }),
    caption: L(`An empty tree: a node holds up to ${MAX_KEYS} keys`, `빈 트리에서 시작합니다. 노드 하나에 키는 ${MAX_KEYS}개까지`),
    ms: 1800,
  }];
  let levels = 1;
  for (let k = 1; k <= 12; k++) {
    for (const f of tree.insert(k)) {
      if (f.kind === "visit") continue;
      const grew = heightOf(f.snap) > levels;
      levels = heightOf(f.snap);
      if (f.kind === "insert") steps.push({ frame: f, caption: L(`Insert ${k}`, `${k} 넣기`), ms: 800 });
      else if (f.kind === "overflow") steps.push({ frame: f, caption: L("Too many keys: the node has to split", "키가 넘쳤습니다. 노드를 쪼개야 합니다"), ms: 1400 });
      else steps.push({
        frame: f,
        caption: grew ? L("Split in two: the middle key goes up and the tree grows a level", "둘로 쪼개고 가운데 키를 위로 올립니다. 트리가 한 층 자랍니다")
                      : L("Split in two: the middle key goes up", "둘로 쪼개고 가운데 키를 위로 올립니다"),
        ms: 2100,
      });
    }
  }
  const found = tree.search(7);
  found.forEach((f, i) => steps.push({
    frame: f,
    caption: f.kind === "found" ? L("Found 7 in this leaf", "찾았습니다. 7은 이 잎에 있습니다")
      : i === 0 ? L("Find 7: start at the root", "7 찾기: 루트에서 시작합니다")
      : L("Go down to the child that can hold 7", "7이 있을 만한 자식으로 내려갑니다"),
    ms: f.kind === "found" ? 2400 : 1200,
  }));
  return steps;
}

function TreeSvg({ frame, previous, room }: { frame: Frame; previous: Frame | null; room: number }) {
  const t = useT();
  const { snap } = frame;
  const laid = useMemo(() => layout(snap), [snap]);
  const fresh = previous ? new Set(Object.keys(snap.nodes).filter(id => !(id in previous.snap.nodes)).map(Number)) : new Set<number>();
  const hi = new Set(frame.hi);
  const ids = Object.keys(snap.nodes).map(Number);
  const { boardW, boardH, shift, zoom } = fit(laid, room);
  // the name of each level: the root, the nodes between, the leaves
  const tier = (d: number): { name: { en: string; ko: string }; cls: string } =>
    laid.levels === 1 ? { name: { en: "ROOT · LEAF", ko: "루트 · 잎" }, cls: "bt-leaf" }
      : d === 0 ? { name: { en: "ROOT", ko: "루트" }, cls: "bt-root" }
      : d === laid.levels - 1 ? { name: { en: "LEAF", ko: "잎" }, cls: "bt-leaf" }
      : { name: { en: "INTERNAL", ko: "안쪽 노드" }, cls: "bt-inner" };
  return (
    <svg className={`bt-svg bt-k-${frame.kind}`} viewBox={`${-shift} 0 ${boardW} ${boardH}`} preserveAspectRatio="xMidYMin meet" style={{ width: boardW * zoom, height: boardH * zoom }}>
      <defs>
        <marker id="bt-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10 z" fill="#4ec9b0" /></marker>
      </defs>
      {Array.from({ length: laid.levels }, (_, d) => {
        const y = rowY(d), info = tier(d);
        return (
          <g key={`tier-${d}`} className={`bt-tier-row ${info.cls}`}>
            <rect className="bt-band" x={-shift} y={y - 22} width={boardW} height={KEY_H + 44} />
            <circle className="bt-tier-dot" cx={-shift + 22} cy={y + KEY_H / 2} r={4} />
            <text className="bt-tier" x={-shift + 34} y={y + KEY_H / 2 + 4}>{t(info.name)}</text>
          </g>
        );
      })}
      {ids.map(id => {
        const n = snap.nodes[id], p = laid.pos[id];
        if (!p) return null;
        return n.children.map((c, i) => {
          const q = laid.pos[c];
          const x1 = p.x + PAD + i * KEY_W, y1 = p.y + KEY_H, x2 = q.x + q.w / 2, y2 = q.y;
          const hot = hi.has(id) && hi.has(c); // (an edge between two nodes the step is about: the walk down)
          return <path key={`${id}-${c}`} className={`bt-edge ${hot ? "bt-edge-hot" : ""}`} style={{ d: `path("M${x1} ${y1} L${x2} ${y2}")` } as React.CSSProperties} />;
        });
      })}
      {ids.map(id => {
        const n = snap.nodes[id], p = laid.pos[id];
        if (!p || !n.leaf || n.next === null || !laid.pos[n.next]) return null;
        const q = laid.pos[n.next];
        return <line key={`next-${id}`} className="bt-link" x1={p.x + p.w + 2} y1={p.y + KEY_H / 2} x2={q.x - 2} y2={q.y + KEY_H / 2} markerEnd="url(#bt-arrow)" />;
      })}
      {ids.map(id => {
        const n = snap.nodes[id], p = laid.pos[id];
        if (!p) return null;
        const kind = n.leaf ? "bt-leaf" : id === snap.root ? "bt-root" : "bt-inner";
        const cls = ["bt-node", kind, hi.has(id) ? `bt-hi bt-k-${frame.kind}` : ""].join(" ");
        return (
          <g key={id} className={cls} style={{ transform: `translate(${p.x}px, ${p.y}px)` }}>
            <g className={fresh.has(id) ? "bt-pop" : ""}>
              <rect className="bt-box" width={p.w} height={KEY_H} />
              {n.keys.length === 0 && <text className="bt-empty" x={p.w / 2} y={KEY_H / 2 + 8} textAnchor="middle">∅</text>}
              {n.keys.map((k, i) => (
                <g key={i}>
                  {i > 0 && <line className="bt-div" x1={PAD + i * KEY_W} y1={10} x2={PAD + i * KEY_W} y2={KEY_H - 10} />}
                  <text className="bt-key" x={PAD + i * KEY_W + KEY_W / 2} y={KEY_H / 2 + 7} textAnchor="middle">{k}</text>
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
  const steps = useMemo(buildSteps, []);
  const durations = useMemo(() => steps.map(x => x.ms), [steps]);
  const player = usePlayer(durations);
  const step = steps[player.cur];
  const frame = step.frame;

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

  // the board is as tall as the biggest tree it ever shows, from the first step on: it does not grow with the tree
  const treeH = useMemo(() => Math.max(...steps.map(s => { const f = fit(layout(s.frame.snap), room); return f.boardH * f.zoom; })), [steps, room]);

  const keyCount = Object.values(frame.snap.nodes).filter(n => n.leaf).reduce((n, node) => n + node.keys.length, 0);
  const previous = player.cur > 0 ? steps[player.cur - 1].frame : null;

  return (
    <div className="bt-page pk-page">
      <PageHero accent={C.storage} lang={lang} onLang={onLang}
        icon={<svg viewBox="0 0 24 24"><rect x="8" y="2.5" width="8" height="5" rx="1.6" /><rect x="1.8" y="16.5" width="8" height="5" rx="1.6" /><rect x="14.2" y="16.5" width="8" height="5" rx="1.6" /><path d="M12 7.5v3.2M12 10.7H5.8v5.8M12 10.7h6.2v5.8" /></svg>}
        title={L("B+Tree: how an index grows", "B+Tree 애니메이션")}
        lead={L("Keys go in one by one: a node that gets too full splits and the tree grows. A lookup walks one path from the root down to a leaf.",
                "키를 하나씩 넣으면 가득 찬 노드가 쪼개지면서 트리가 자랍니다. 찾을 때는 루트에서 잎까지 한 길로 내려갑니다.")} />

      <div className={`bt-stage pk-rise bt-k-${frame.kind}`} style={{ ...tint(C.storage), ...rise(1) }}>
        <div className="bt-canvas" ref={canvas} style={{ ["--tree-h" as string]: `${treeH}px` }}>
          <TreeSvg frame={frame} previous={previous} room={room} />
        </div>
        <div className="bt-msg">
          <span className="bt-dot-live" aria-hidden="true" />
          <span className="bt-badge">{t(KIND_LABEL[frame.kind])}</span>
          <span className="bt-text" key={player.cur}>{t(step.caption)}</span>
          <span className="bt-stats">
            <span className="bt-stat"><b><CountUp value={keyCount} ms={500} /></b> {t(L("keys", "키"))}</span>
            <span className="bt-stat"><b><CountUp value={heightOf(frame.snap)} ms={500} /></b> {t(L("levels", "층"))}</span>
          </span>
        </div>
      </div>

      <div className="pk-rise" style={{ ...tint(C.storage), ...rise(2) }}>
        <PlayBar count={steps.length} cur={player.cur} playing={player.playing}
          onToggle={() => player.setPlaying(p => !p)} onReplay={player.replay} onJump={player.jump} />
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
