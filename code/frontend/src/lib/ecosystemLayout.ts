// ecosystemLayout.ts
// The picture of the "Ecosystem" window as data: where every cylinder of RuSQL -> connections -> databases -> tables stands and which curves join them. The window
// (components/EcosystemView.tsx) only draws this and writes the words; code/test/frontend/test_libs.mjs tests it. No DOM, no React.
//
//   RuSQL (root) ─▶ connection ─▶ database ─▶ [ table  table  table  table  table ]   <- the tables of a database stand in a box, five to a row
//
// Heights are not fixed: a connection takes the room its databases take, a database the room of its rows of tables, so a big database does not push its neighbours over.
// The root stands in the middle of the picture, but never lower than ROOT.maxCy, so it is on the first screen however many connections there are.

export interface EcoTable { name: string; columns: number; primaryKey: string[]; foreignKeys: number; indexes: number; rows: number; bytes: number }
export interface EcoDatabase { name: string; tables: EcoTable[]; views: number; bytes: number }
// What the scan of a connection's data folder found (src-tauri main.rs: scan_ecosystem). `exists: false` is a connection that was never opened: it has no folder yet.
export interface EcoScan { id: string; exists: boolean; users: number; routines: number; triggers: number; bytes: number; databases: EcoDatabase[] }
export interface EcoConn { id: string; name: string; host: string; port: number; user: string; autoLogin: boolean; dataDir: string; favorite?: boolean; lastUsed?: number }

export type EcoKind = "root" | "connection" | "database" | "table" | "more";

export interface EcoNode {
  id: string;
  kind: EcoKind;
  parent: string | null;
  depth: number;       // 0 root, 1 connection, 2 database, 3 table / "more": the glow walks along the depths
  order: number;       // creation order: the cylinders pop in one after the other
  cx: number; cy: number; w: number; h: number;
  conn?: EcoConn;
  scan?: EcoScan;
  db?: EcoDatabase;
  table?: EcoTable;
  hidden?: number;     // kind "more": how many tables are not drawn
}

export interface EcoArrow { d: string; from: string; to: string; depth: number }
export interface EcoGroup { id: string; x: number; y: number; w: number; h: number }   // the box round the tables of one database
export interface EcoLayout { width: number; height: number; nodes: EcoNode[]; arrows: EcoArrow[]; groups: EcoGroup[] }

export const WIDTH = 1420;
export const RY = 13; // vertical radius of a cylinder's top ellipse (as in the Diagram page)

const ROOT = { cx: 108, w: 150, h: 108, maxCy: 330 }; // maxCy: with many connections the picture is taller than the window; the root stays in the first screen
const CONN = { cx: 400, w: 176, h: 78 };
const DB = { cx: 690, w: 136, h: 66 };
const GRID = { left: 858, boxW: 546, cols: 5, cell: 104, pitch: 60, tableW: 88, tableH: 44 };
const TOP = 30;
const CONN_GAP = 16;
const DB_GAP = 8;
const MIN_CONN = 104;
const MIN_DB = 88;

export const MAX_TABLES = 40; // a database with more tables draws the first 39 and one "+N" cylinder

const curve = (x1: number, y1: number, x2: number, y2: number): string => {
  const mid = (x1 + x2) / 2;
  return `M${x1} ${y1} C${mid} ${y1} ${mid} ${y2} ${x2} ${y2}`;
};

export function layoutEcosystem(conns: EcoConn[], scans: Record<string, EcoScan | undefined>, maxTables: number = MAX_TABLES): EcoLayout {
  const nodes: EcoNode[] = [];
  const arrows: EcoArrow[] = [];
  const groups: EcoGroup[] = [];
  const rootId = "root";
  let order = 1; // 0 is the root, added last (it needs the height)

  type Placed = { conn: EcoConn; scan: EcoScan | undefined; top: number; height: number; dbs: { db: EcoDatabase; top: number; height: number; shown: EcoTable[]; hidden: number }[] };
  const placed: Placed[] = [];
  let y = TOP;
  for (const conn of conns) {
    const scan = scans[conn.id];
    const dbs = (scan && scan.exists ? scan.databases : []).map(db => {
      const over = db.tables.length > maxTables;
      const shown = over ? db.tables.slice(0, maxTables - 1) : db.tables;
      const hidden = over ? db.tables.length - shown.length : 0;
      const cells = shown.length + (hidden ? 1 : 0);
      const rows = Math.max(1, Math.ceil(cells / GRID.cols));
      return { db, top: 0, height: Math.max(MIN_DB, rows * GRID.pitch + 28), shown, hidden };
    });
    const height = dbs.length ? dbs.reduce((sum, d) => sum + d.height, 0) + DB_GAP * (dbs.length - 1) : MIN_CONN;
    let dbTop = y;
    for (const d of dbs) { d.top = dbTop; dbTop += d.height + DB_GAP; }
    placed.push({ conn, scan, top: y, height, dbs });
    y += height + CONN_GAP;
  }
  const height = Math.max(360, y - CONN_GAP + TOP);
  const rootCy = Math.min(height / 2, ROOT.maxCy);

  for (const p of placed) {
    const connCy = p.top + p.height / 2;
    const connId = `c:${p.conn.id}`;
    nodes.push({ id: connId, kind: "connection", parent: rootId, depth: 1, order: order++, cx: CONN.cx, cy: connCy, w: CONN.w, h: CONN.h, conn: p.conn, scan: p.scan });
    arrows.push({ d: curve(ROOT.cx + ROOT.w / 2, rootCy, CONN.cx - CONN.w / 2 - 6, connCy), from: rootId, to: connId, depth: 1 });
    for (const d of p.dbs) {
      const dbCy = d.top + d.height / 2;
      const dbId = `${connId}/d:${d.db.name}`;
      nodes.push({ id: dbId, kind: "database", parent: connId, depth: 2, order: order++, cx: DB.cx, cy: dbCy, w: DB.w, h: DB.h, conn: p.conn, scan: p.scan, db: d.db });
      arrows.push({ d: curve(CONN.cx + CONN.w / 2, connCy, DB.cx - DB.w / 2 - 6, dbCy), from: connId, to: dbId, depth: 2 });
      const cells = d.shown.length + (d.hidden ? 1 : 0);
      const rows = Math.max(1, Math.ceil(cells / GRID.cols));
      groups.push({ id: dbId, x: GRID.left, y: d.top + 6, w: GRID.boxW, h: d.height - 12 });
      arrows.push({ d: `M${DB.cx + DB.w / 2} ${dbCy} H${GRID.left - 6}`, from: dbId, to: dbId, depth: 3 });
      const firstY = dbCy - ((rows - 1) * GRID.pitch) / 2;
      const cellAt = (i: number) => ({ cx: GRID.left + 13 + GRID.cell / 2 + (i % GRID.cols) * GRID.cell, cy: firstY + Math.floor(i / GRID.cols) * GRID.pitch });
      d.shown.forEach((table, i) => {
        nodes.push({ id: `${dbId}/t:${table.name}`, kind: "table", parent: dbId, depth: 3, order: order++, ...cellAt(i), w: GRID.tableW, h: GRID.tableH, conn: p.conn, scan: p.scan, db: d.db, table });
      });
      if (d.hidden) {
        nodes.push({ id: `${dbId}/more`, kind: "more", parent: dbId, depth: 3, order: order++, ...cellAt(d.shown.length), w: GRID.tableW, h: GRID.tableH, conn: p.conn, scan: p.scan, db: d.db, hidden: d.hidden });
      }
    }
  }
  nodes.unshift({ id: rootId, kind: "root", parent: null, depth: 0, order: 0, cx: ROOT.cx, cy: rootCy, w: ROOT.w, h: ROOT.h });
  return { width: WIDTH, height, nodes, arrows, groups };
}

export interface EcoTotals { connections: number; opened: number; databases: number; tables: number; rows: number; bytes: number }

export function ecosystemTotals(conns: EcoConn[], scans: Record<string, EcoScan | undefined>): EcoTotals {
  const totals: EcoTotals = { connections: conns.length, opened: 0, databases: 0, tables: 0, rows: 0, bytes: 0 };
  for (const conn of conns) {
    const scan = scans[conn.id];
    if (!scan || !scan.exists) continue;
    totals.opened++;
    totals.bytes += scan.bytes;
    for (const db of scan.databases) {
      totals.databases++;
      totals.tables += db.tables.length;
      for (const table of db.tables) totals.rows += table.rows;
    }
  }
  return totals;
}

// The cylinders and their relatives: what stays lit while the mouse is on `id` -- its parents up to the root and everything below it.
export function relatedTo(nodes: EcoNode[], id: string): Set<string> {
  const byId = new Map(nodes.map(n => [n.id, n] as const));
  const below = new Set<string>([id]); // the node and what hangs under it (grown before the parents are added, or the siblings would hang under them)
  let grew = true;
  while (grew) {
    grew = false;
    for (const n of nodes) {
      if (n.parent !== null && below.has(n.parent) && !below.has(n.id)) { below.add(n.id); grew = true; }
    }
  }
  const related = new Set<string>(below);
  for (let at = byId.get(id)?.parent ?? null; at !== null; at = byId.get(at)?.parent ?? null) related.add(at);
  return related;
}

export function humanBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

// A name that has to fit a cylinder: cut with an ellipsis (the whole name is in the tooltip).
export function fitName(name: string, max: number): string {
  const chars = Array.from(name);
  return chars.length <= max ? name : chars.slice(0, Math.max(1, max - 1)).join("") + "…";
}
