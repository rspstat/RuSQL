// planDiagram.ts
// The plan of EXPLAIN / EXPLAIN ANALYZE as nodes (scan -> join -> filter -> result) for the diagram above the plan's rows. The lines are the ones the engine writes, one
// "label : value" per line:
//   Table : orders / Rows (total) : 10000 / Access : Index Scan  PK = 5 / Join : Hash Join       probe=a build=b → customers  cost≈26        (EXPLAIN)
//   Step 1 Scan orders : 10000 rows, 3.600 ms, 90%  /  Step 2 Join customers (inner, hash join) : 100 rows, 0.300 ms, 7%  /  Step 3 Filter (WHERE) and expressions ...
// (EXPLAIN ANALYZE: the steps are what ran, in order, with the rows each left and its share of the time). No DOM here, so that the reading can be tested on its own.

export interface PlanNode {
  kind: "scan" | "join" | "filter" | "result" | "step";
  title: string;
  detail: string[];
  rows?: number;
  ms?: number;
  share?: number;                          // percent of the time (EXPLAIN ANALYZE only)
  input?: { title: string; detail: string }; // the table a join reads: a chip under the join
}

export interface PlanModel {
  nodes: PlanNode[];
  measured: boolean;   // the plan has steps with times
  slowest: number;     // index of the node that took the longest (-1: none, or only one step)
  facts: string[];     // Estimated / actual rows, execution time
}

const STEP = /^Step (\d+) (.+?) : (\d+) rows, ([\d.]+) ms, (\d+)%$/;

function split(line: string): [string, string] {
  const at = line.indexOf(":");
  return at < 0 ? [line.trim(), ""] : [line.slice(0, at).trim(), line.slice(at + 1).trim()];
}

// "Hash Join       probe=a build=b → customers  cost≈26": the table the join reads, and what is said of how
function readJoin(value: string): { table: string; how: string } {
  const [how, rest = ""] = value.split("→");
  const table = rest.trim().split(/\s+/)[0] ?? "";
  const cost = rest.match(/cost≈(\d+)/);
  return { table, how: how.replace(/\s+/g, " ").trim() + (cost ? ` · cost ≈ ${cost[1]}` : "") };
}

export function buildPlan(lines: string[]): PlanModel {
  const info: Record<string, string[]> = {};
  const facts: string[] = [];
  const steps: { label: string; rows: number; ms: number; share: number }[] = [];
  for (const raw of lines) {
    const line = raw.trim();
    const step = line.match(STEP);
    if (step) {
      steps.push({ label: step[2], rows: Number(step[3]), ms: Number(step[4]), share: Number(step[5]) });
      continue;
    }
    const [label, value] = split(line);
    if (/^Estimated rows/i.test(label)) {
      const m = line.match(/Estimated rows\s*:\s*(\d+)\s+Actual rows\s*:\s*(\d+)(?:\s+Accuracy\s*:\s*(\S+))?/);
      if (m) facts.push(`rows: ${m[1]} estimated, ${m[2]} read${m[3] ? ` (${m[3]})` : ""}`);
    } else if (/^Execution time/i.test(label)) {
      facts.push(`time: ${value}`);
    } else if (label) {
      (info[label] ??= []).push(value);
    }
  }
  const table = info["Table"]?.[0] ?? "";
  const access = info["Access"]?.[0] ?? "";
  const rowsOf = info["Rows (total)"]?.[0];
  const joins = (info["Join"] ?? []).map(readJoin);
  const scanDetail = [access, rowsOf ? `${rowsOf} rows in the table` : ""].filter(Boolean);

  const nodes: PlanNode[] = [];
  if (steps.length > 0) {
    for (const s of steps) {
      const timing = { rows: s.rows, ms: s.ms, share: s.share };
      if (/^Scan /.test(s.label)) {
        nodes.push({ kind: "scan", title: s.label, detail: scanDetail, ...timing });
      } else if (/^Join /.test(s.label)) {
        const named = s.label.match(/^Join (\S+)(?: \((.*)\))?/);
        const target = named?.[1] ?? "";
        const planned = joins.find(j => j.table === target || j.table.endsWith("." + target) || target.endsWith("." + j.table));
        nodes.push({
          kind: "join", title: `Join ${target}`.trim(), detail: [named?.[2] ?? "", planned?.how ?? ""].filter(Boolean), ...timing,
          input: { title: target, detail: "" },
        });
      } else if (/^Filter/.test(s.label)) {
        nodes.push({ kind: "filter", title: "Filter", detail: [s.label.replace(/^Filter\s*/, "")], ...timing });
      } else if (/^Limit/.test(s.label)) {
        nodes.push({ kind: "result", title: "Result", detail: [s.label], ...timing });
      } else {
        nodes.push({ kind: "step", title: s.label, detail: [], ...timing });
      }
    }
  } else {
    if (table || access) nodes.push({ kind: "scan", title: table ? `Scan ${table}` : "Scan", detail: scanDetail });
    for (const j of joins) nodes.push({ kind: "join", title: `Join ${j.table}`, detail: [j.how], input: { title: j.table, detail: "" } });
  }
  let slowest = -1;
  if (steps.length > 1) {
    let best = -1;
    nodes.forEach((n, i) => { if ((n.ms ?? -1) > best) { best = n.ms ?? -1; slowest = i; } });
  }
  return { nodes, measured: steps.length > 0, slowest, facts };
}
