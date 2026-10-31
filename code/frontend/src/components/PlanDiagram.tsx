// PlanDiagram.tsx
// The plan as boxes joined by arrows, above the rows of EXPLAIN / EXPLAIN ANALYZE: scan -> join -> filter -> result. A join box has the table it reads under it. After
// EXPLAIN ANALYZE each box says how many rows it left and how long it took (a bar for its share of the time; the slowest box is marked); after a plain EXPLAIN the boxes
// are the access and the joins the planner chose. The reading is in lib/planDiagram.ts.

import { buildPlan } from "../lib/planDiagram";
import "./PlanDiagram.css";

const ICONS: Record<string, string> = { scan: "▤", join: "⋈", filter: "▽", result: "▣", step: "◇" };

export default function PlanDiagram({ lines }: { lines: string[] }) {
  const plan = buildPlan(lines);
  if (plan.nodes.length === 0) return null;
  return (
    <div className="plan-diagram">
      <div className="plan-diagram-title">PLAN DIAGRAM</div>
      <div className="plan-flow">
        {plan.nodes.map((n, i) => (
          <div key={i} className="plan-flow-item">
            {i > 0 && <span className="plan-arrow" aria-hidden="true">→</span>}
            <div className={`plan-node plan-${n.kind}${i === plan.slowest ? " plan-slowest" : ""}`} title={[n.title, ...n.detail].join("\n")}>
              <div className="plan-node-head"><span className="plan-icon">{ICONS[n.kind]}</span><span className="plan-node-title">{n.title}</span></div>
              {n.detail.map((d, k) => <div key={k} className="plan-node-detail">{d}</div>)}
              {n.rows !== undefined && (
                <div className="plan-node-stat">{n.rows.toLocaleString()} rows · {(n.ms ?? 0).toFixed(3)} ms · {n.share}%</div>
              )}
              {n.share !== undefined && <div className="plan-heat"><span style={{ width: `${Math.max(Math.min(n.share, 100), 2)}%` }} /></div>}
              {n.input && (
                <div className="plan-input"><span aria-hidden="true">↑</span> reads <b>{n.input.title}</b></div>
              )}
            </div>
          </div>
        ))}
      </div>
      {plan.facts.length > 0 && <div className="plan-facts">{plan.facts.join("  ·  ")}</div>}
    </div>
  );
}
