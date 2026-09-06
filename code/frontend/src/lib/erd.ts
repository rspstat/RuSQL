// lib/erd.ts
// Pure ERD layout helpers, moved out of App.tsx unchanged during the App.tsx
// UI-section extraction refactor -- no behavior change. Only used by ErdView.tsx.

import type { ColumnDetail } from "../types";

export interface ErdPos { x: number; y: number; }
export const ERD_CARD_W = 360;
export const ERD_HEADER_H = 32;
export const ERD_COL_H = 24;

// fk_ref 형식: "db1.dept(id)" 또는 "dept(id)" — 괄호가 우선
export function parseRef(ref: string): { table: string; col: string } | null {
  const paren = ref.indexOf("(");
  if (paren > 0) return { table: ref.slice(0, paren), col: ref.slice(paren + 1).replace(")", "") };
  const dot = ref.lastIndexOf(".");
  if (dot > 0) return { table: ref.slice(0, dot), col: ref.slice(dot + 1) };
  return null;
}
// "db1.dept" → "dept" (DB 한정자 제거)
export function unqualify(name: string): string {
  const dot = name.indexOf(".");
  return dot >= 0 ? name.slice(dot + 1) : name;
}

// 직각 꺾임 경로: x1,y1 → midX 수평 → y2 수직 → x2 수평, 모서리 r=8 라운드
export function erdOrthPath(x1: number, y1: number, x2: number, y2: number): string {
  const r = 8;
  if (Math.abs(y1 - y2) < 1) return `M${x1} ${y1} H${x2}`;
  const midX = (x1 + x2) / 2;
  const sdx = Math.sign(midX - x1); // 수평 진행 방향 (+1 오른쪽, -1 왼쪽)
  const sdy = Math.sign(y2 - y1);   // 수직 진행 방향 (+1 아래, -1 위)
  const r1 = Math.min(r, Math.abs(midX - x1), Math.abs(y2 - y1) / 2);
  const r2 = Math.min(r, Math.abs(x2 - midX), Math.abs(y2 - y1) / 2);
  if (r1 < 1 || r2 < 1) return `M${x1} ${y1} H${midX} V${y2} H${x2}`;
  return [
    `M${x1} ${y1}`,
    `H${midX - sdx * r1}`,
    `Q${midX} ${y1} ${midX} ${y1 + sdy * r1}`,
    `V${y2 - sdy * r2}`,
    `Q${midX} ${y2} ${midX + sdx * r2} ${y2}`,
    `H${x2}`,
  ].join(" ");
}

// ─── ERD 레이아웃 계산 (pure) ─────────────────────────────────
export function computeErdLayout(columns: Record<string, ColumnDetail[]>): Record<string, ErdPos> {
  const allTables = Object.keys(columns);
  if (allTables.length === 0) return {};
  const deps: Record<string, Set<string>> = {};
  for (const t of allTables) deps[t] = new Set();
  for (const [tbl, cols] of Object.entries(columns)) {
    for (const col of cols) {
      if (col.fk_ref) {
        const parsed = parseRef(col.fk_ref);
        if (parsed) { const ref = unqualify(parsed.table); if (allTables.includes(ref)) deps[tbl].add(ref); }
      }
    }
  }
  const depth: Record<string, number> = {};
  const computing = new Set<string>();
  const getDepth = (t: string): number => {
    if (depth[t] !== undefined) return depth[t];
    if (computing.has(t)) { depth[t] = 0; return 0; }
    computing.add(t);
    depth[t] = deps[t].size === 0 ? 0 : Math.max(...Array.from(deps[t]).map(d => getDepth(d) + 1));
    computing.delete(t);
    return depth[t];
  };
  for (const t of allTables) getDepth(t);
  const byDepth: Record<number, string[]> = {};
  for (const t of allTables) { const d = depth[t]; (byDepth[d] = byDepth[d] ?? []).push(t); }
  const maxDepth = Math.max(...Object.keys(byDepth).map(Number));
  const cardH = (t: string) => ERD_HEADER_H + (columns[t]?.length ?? 0) * ERD_COL_H + 8;
  const COL_W = 480, ROW_GAP = 56;
  // 바리센터 정렬: 부모 노드 평균 위치 기준으로 자식 노드를 정렬해 교차선 최소화
  const sortedByDepth: Record<number, string[]> = { 0: [...(byDepth[0] ?? [])] };
  for (let d = 1; d <= maxDepth; d++) {
    const tables = [...(byDepth[d] ?? [])];
    const prevSorted = sortedByDepth[d - 1] ?? [];
    const bc: Record<string, number> = {};
    for (const t of tables) {
      const parents = [...deps[t]].filter(p => prevSorted.includes(p));
      bc[t] = parents.length === 0 ? prevSorted.length / 2 : parents.reduce((s, p) => s + prevSorted.indexOf(p), 0) / parents.length;
    }
    sortedByDepth[d] = tables.sort((a, b) => bc[a] - bc[b]);
  }
  // 열 높이 계산 후 수직 중앙 정렬
  const colH = (d: number) => (sortedByDepth[d] ?? []).reduce((s, t) => s + cardH(t) + ROW_GAP, -ROW_GAP);
  const maxColH = Math.max(0, ...Array.from({ length: maxDepth + 1 }, (_, d) => colH(d)));
  const positions: Record<string, ErdPos> = {};
  for (let d = 0; d <= maxDepth; d++) {
    let y = 60 + Math.max(0, (maxColH - colH(d)) / 2);
    for (const t of sortedByDepth[d] ?? []) {
      positions[t] = { x: 60 + d * COL_W, y };
      y += cardH(t) + ROW_GAP;
    }
  }
  return positions;
}
