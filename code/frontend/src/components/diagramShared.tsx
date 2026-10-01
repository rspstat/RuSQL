// diagramShared.tsx
// Pieces shared by the two Diagram views (layered view in DiagramView.tsx, cylinder view in
// DiagramCylinders.tsx): the EN/KO text pair + language context, the per-layer colors and the
// arrow style both pictures use.

import { createContext, useContext } from "react";
import type { CSSProperties } from "react";

export type Lang = "en" | "ko";
// A plain string is language-neutral (names, ports, file names); a pair is translated.
export type Text = string | { en: string; ko: string };
export const L = (en: string, ko: string): Text => ({ en, ko });

export const LangContext = createContext<Lang>("en");

export function useT() {
  const lang = useContext(LangContext);
  return (x: Text) => (typeof x === "string" ? x : x[lang]);
}

export const C = {
  client: "#9cdcfe",
  server: "#4ec9b0",
  core: "#8b5cf6",
  txn: "#ddb05d",
  storage: "#c586c0",
  disk: "#f08080",
};

// Every colored element reads its layer color from the --c custom property.
export const tint = (c: string): CSSProperties => ({ ["--c" as string]: c });

// ---- arrows ---------------------------------------------------------------------------------
// The arrow style of both pictures: a dim base line, bright dashes sliding along it, a glowing
// packet running along it and a head in the layer color. The look itself lives in the
// .dg-aline / .dg-aflow / .dg-apacket rules of DiagramView.css.

export const headId = (c: string) => `dg-head-${c.slice(1)}`;

// Arrow heads, one per layer color, referenced as marker-end="url(#...)" by every arrow on the
// page. Render once; the zero-size svg only holds definitions.
export function ArrowDefs() {
  return (
    <svg width="0" height="0" style={{ position: "absolute" }} aria-hidden="true">
      <defs>
        {Object.values(C).map(c => (
          <marker key={c} id={headId(c)} viewBox="0 0 10 10" refX="10" refY="5" markerWidth="7" markerHeight="7" orient="auto">
            <path d="M0 0 L10 5 L0 10 z" fill={c} />
          </marker>
        ))}
      </defs>
    </svg>
  );
}

// A straight arrow that fills its parent (percent coordinates), so the layered view can stretch it
// to whatever height or width its grid cell has. `phase` offsets the packet so arrows don't pulse in unison.
export function FlowArrow({ c, dir = "down", dashed, phase = 0 }: { c: string; dir?: "down" | "right"; dashed?: boolean; phase?: number }) {
  const down = dir === "down";
  const line = down ? { x1: "50%", y1: "0", x2: "50%", y2: "100%" } : { x1: "0", y1: "50%", x2: "100%", y2: "50%" };
  return (
    <svg className="dg-farrow" style={tint(c)} aria-hidden="true">
      <line className="dg-aline" {...line} markerEnd={`url(#${headId(c)})`} strokeDasharray={dashed ? "7 6" : undefined} />
      <line className="dg-aflow" {...line} />
      <circle className="dg-apacket" r="3.4" {...(down ? { cx: "50%" } : { cy: "50%" })}>
        <animate attributeName={down ? "cy" : "cx"} values="0%;100%" dur="2.6s" begin={`${-(phase * 0.65).toFixed(2)}s`} repeatCount="indefinite" />
      </circle>
    </svg>
  );
}
