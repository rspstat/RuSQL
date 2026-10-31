// PageKit.tsx
// The look the three teaching pages share (B+Tree, MVCC, MySQL compatibility), taken from the Diagram page: a header with a glowing icon badge, the title, a lead line and the
// EN/KO toggle; boxes that rise into place one after another and light up in the color of their page; numbers that count up. The rules are in PageKit.css (.pk-*).
// A page sets its color with `accent` (a layer color of the Diagram page), reads it back as var(--c), and staggers its boxes with rise(i).

import { useEffect, useRef, useState } from "react";
import type { CSSProperties, ReactNode } from "react";
import type { Lang, Text } from "./diagramShared";
import { tint, useT } from "./diagramShared";
import "./PageKit.css";

// --d staggers the entrance of the nth box (the same custom property the Diagram page uses)
export const rise = (i: number): CSSProperties => ({ ["--d" as string]: i });

export function PageHero({ accent, icon, title, lead, lang, onLang, children }: {
  accent: string; icon: ReactNode; title: Text; lead?: Text; lang: Lang; onLang: (l: Lang) => void; children?: ReactNode;
}) {
  const t = useT();
  return (
    <header className="pk-hero" style={tint(accent)}>
      <span className="pk-glow" aria-hidden="true" />
      <div className="pk-hero-main">
        <span className="pk-badge" aria-hidden="true">{icon}</span>
        <div className="pk-hero-text">
          <h1 className="pk-title">{t(title)}</h1>
          {lead && <p className="pk-lead">{t(lead)}</p>}
        </div>
        <div className="pk-seg" role="group" aria-label="Language">
          <button className={lang === "ko" ? "active" : ""} aria-pressed={lang === "ko"} onClick={() => onLang("ko")}>한국어</button>
          <button className={lang === "en" ? "active" : ""} aria-pressed={lang === "en"} onClick={() => onLang("en")}>EN</button>
        </div>
      </div>
      {children}
    </header>
  );
}

// The playback of a scripted animation: step `cur` is shown for durations[cur] ms, then the next one; after the last one it waits `endPause` and starts again. It plays
// as soon as the page opens; jump() stops at a step, replay() starts over.
export function usePlayer(durations: number[], endPause = 3200) {
  const [cur, setCur] = useState(0);
  const [playing, setPlaying] = useState(true);
  useEffect(() => {
    if (!playing) return;
    const last = cur >= durations.length - 1;
    const timer = setTimeout(() => setCur(last ? 0 : cur + 1), last ? endPause : durations[cur]);
    return () => clearTimeout(timer);
  }, [playing, cur, durations, endPause]);
  return {
    cur, playing, setPlaying,
    jump: (i: number) => { setPlaying(false); setCur(i); },
    replay: () => { setCur(0); setPlaying(true); },
  };
}

// The only controls of a scripted page: pause / play, start over, and one segment for each step (the ones behind are filled; click one to stop there)
export function PlayBar({ count, cur, playing, onToggle, onReplay, onJump }: {
  count: number; cur: number; playing: boolean; onToggle: () => void; onReplay: () => void; onJump: (i: number) => void;
}) {
  const t = useT();
  return (
    <div className="pk-play">
      <button className="pk-btn primary pk-play-btn" onClick={onToggle} aria-label={playing ? t({ en: "Pause", ko: "멈춤" }) : t({ en: "Play", ko: "재생" })}>{playing ? "❚❚" : "▶"}</button>
      <button className="pk-btn pk-play-btn" onClick={onReplay} aria-label={t({ en: "Start over", ko: "처음부터" })}>↻</button>
      <div className="pk-track" role="group">
        {Array.from({ length: count }, (_, i) => (
          <i key={i} className={i < cur ? "done" : i === cur ? "now" : ""} onClick={() => onJump(i)} />
        ))}
      </div>
    </div>
  );
}

// A number that counts up from 0 to `value` when it appears (and again when the value changes); `format` writes it (a percent, a thousands separator)
export function CountUp({ value, format = (n: number) => Math.round(n).toLocaleString(), ms = 1100 }: { value: number; format?: (n: number) => string; ms?: number }) {
  const [shown, setShown] = useState(0);
  const from = useRef(0);
  useEffect(() => {
    const reduced = typeof window !== "undefined" && window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
    if (reduced) { setShown(value); from.current = value; return; }
    const start = performance.now(), begin = from.current;
    let frame = 0;
    const tick = (now: number) => {
      const k = Math.min(1, (now - start) / ms);
      const eased = 1 - Math.pow(1 - k, 3);
      setShown(begin + (value - begin) * eased);
      if (k < 1) frame = requestAnimationFrame(tick);
      else from.current = value;
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [value, ms]);
  return <>{format(shown)}</>;
}
