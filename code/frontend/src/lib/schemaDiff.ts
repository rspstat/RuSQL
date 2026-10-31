// schemaDiff.ts
// The difference between two databases' schemas, and the statements that turn the first (A, "what is there") into the second (B, "what it should be"). The two schemas
// are read from information_schema (columns, statistics, key_column_usage + referential_constraints) by the modal; this file holds no DOM and no Tauri call, so that the
// comparison and the SQL it writes can be tested on their own.
//
// What is compared: the tables, and for a table in both its columns (type, NOT NULL, default, AUTO_INCREMENT), its indexes (name, columns, unique) and its foreign keys
// (column, parent, rules). A table that is only in B is created with the DDL SHOW CREATE TABLE gives (the parents first); a table that is only in A is dropped, which is the
// last statement and is written as a comment, so that nothing is deleted by accident. A column that moved is not noticed (MySQL's ordinal position is not compared).

export interface ColumnInfo { name: string; type: string; notNull: boolean; defaultValue: string | null; autoIncrement: boolean }
export interface IndexInfo { name: string; columns: string[]; unique: boolean }
export interface ForeignKeyInfo { name: string; column: string; refTable: string; refColumn: string; onDelete: string; onUpdate: string }
export interface TableInfo { name: string; columns: ColumnInfo[]; indexes: IndexInfo[]; foreignKeys: ForeignKeyInfo[]; createSql?: string }
export type SchemaSnapshot = Record<string, TableInfo>;

export interface TableChange {
  table: string;
  addColumns: ColumnInfo[];
  dropColumns: ColumnInfo[];
  modifyColumns: { from: ColumnInfo; to: ColumnInfo; what: string[] }[];
  addIndexes: IndexInfo[];
  dropIndexes: IndexInfo[];
  addForeignKeys: ForeignKeyInfo[];
  dropForeignKeys: ForeignKeyInfo[];
}

export interface SchemaDiff {
  addedTables: string[];
  droppedTables: string[];
  changedTables: TableChange[];
}

const same = (a: string, b: string) => a.toLowerCase() === b.toLowerCase();
const sameList = (a: string[], b: string[]) => a.length === b.length && a.every((x, i) => same(x, b[i]));
const norm = (t: string) => t.replace(/\s+/g, "").toLowerCase();

export const ident = (n: string) => (/^[A-Za-z_][A-Za-z0-9_]*$/.test(n) ? n : "`" + n.replace(/`/g, "``") + "`");

function columnChanges(a: ColumnInfo, b: ColumnInfo): string[] {
  const what: string[] = [];
  if (norm(a.type) !== norm(b.type)) what.push(`type ${a.type} → ${b.type}`);
  if (a.notNull !== b.notNull) what.push(b.notNull ? "NOT NULL" : "NULL");
  if ((a.defaultValue ?? null) !== (b.defaultValue ?? null)) what.push(`default ${a.defaultValue ?? "none"} → ${b.defaultValue ?? "none"}`);
  if (a.autoIncrement !== b.autoIncrement) what.push(b.autoIncrement ? "AUTO_INCREMENT" : "no AUTO_INCREMENT");
  return what;
}

const sameIndex = (a: IndexInfo, b: IndexInfo) => a.unique === b.unique && sameList(a.columns, b.columns);
const sameForeignKey = (a: ForeignKeyInfo, b: ForeignKeyInfo) =>
  same(a.column, b.column) && same(a.refTable, b.refTable) && same(a.refColumn, b.refColumn) && a.onDelete.toUpperCase() === b.onDelete.toUpperCase() && a.onUpdate.toUpperCase() === b.onUpdate.toUpperCase();

export function diffSchemas(a: SchemaSnapshot, b: SchemaSnapshot): SchemaDiff {
  const lower = (s: SchemaSnapshot) => new Map(Object.entries(s).map(([k, v]) => [k.toLowerCase(), v]));
  const A = lower(a), B = lower(b);
  const diff: SchemaDiff = { addedTables: [], droppedTables: [], changedTables: [] };
  for (const [key, tb] of B) if (!A.has(key)) diff.addedTables.push(tb.name);
  for (const [key, ta] of A) if (!B.has(key)) diff.droppedTables.push(ta.name);
  for (const [key, ta] of A) {
    const tb = B.get(key);
    if (!tb) continue;
    const change: TableChange = { table: ta.name, addColumns: [], dropColumns: [], modifyColumns: [], addIndexes: [], dropIndexes: [], addForeignKeys: [], dropForeignKeys: [] };
    for (const cb of tb.columns) {
      const ca = ta.columns.find(c => same(c.name, cb.name));
      if (!ca) change.addColumns.push(cb);
      else if (columnChanges(ca, cb).length) change.modifyColumns.push({ from: ca, to: cb, what: columnChanges(ca, cb) });
    }
    for (const ca of ta.columns) if (!tb.columns.some(c => same(c.name, ca.name))) change.dropColumns.push(ca);
    for (const ib of tb.indexes) if (!ta.indexes.some(i => sameIndex(i, ib))) change.addIndexes.push(ib);
    for (const ia of ta.indexes) if (!tb.indexes.some(i => sameIndex(i, ia))) change.dropIndexes.push(ia);
    for (const fb of tb.foreignKeys) if (!ta.foreignKeys.some(f => sameForeignKey(f, fb))) change.addForeignKeys.push(fb);
    for (const fa of ta.foreignKeys) if (!tb.foreignKeys.some(f => sameForeignKey(f, fa))) change.dropForeignKeys.push(fa);
    const empty = Object.entries(change).every(([k, v]) => k === "table" || (v as unknown[]).length === 0);
    if (!empty) diff.changedTables.push(change);
  }
  diff.addedTables.sort();
  diff.droppedTables.sort();
  diff.changedTables.sort((x, y) => x.table.localeCompare(y.table));
  return diff;
}

export function isEmptyDiff(d: SchemaDiff): boolean {
  return d.addedTables.length + d.droppedTables.length + d.changedTables.length === 0;
}

export function columnDefinition(c: ColumnInfo): string {
  let sql = `${ident(c.name)} ${c.type}`;
  if (c.notNull) sql += " NOT NULL";
  if (c.autoIncrement) sql += " AUTO_INCREMENT";
  if (c.defaultValue !== null && c.defaultValue !== undefined && !c.autoIncrement) {
    const d = c.defaultValue;
    sql += /^-?\d+(\.\d+)?$/.test(d) || /^CURRENT_TIMESTAMP(\(\d*\))?$/i.test(d) ? ` DEFAULT ${d}` : ` DEFAULT '${d.replace(/'/g, "''")}'`;
  }
  return sql;
}

const foreignKeySql = (f: ForeignKeyInfo) =>
  `ADD CONSTRAINT ${ident(f.name)} FOREIGN KEY (${ident(f.column)}) REFERENCES ${ident(f.refTable)} (${ident(f.refColumn)}) ON DELETE ${f.onDelete} ON UPDATE ${f.onUpdate}`;

// tables of `names` ordered so that a table comes after the tables its foreign keys name (a cycle keeps the name order for what is left)
export function orderByParents(names: string[], snapshot: SchemaSnapshot): string[] {
  const rest = [...names].sort();
  const done: string[] = [];
  while (rest.length) {
    const ready = rest.findIndex(n => (snapshot[n]?.foreignKeys ?? []).every(f => same(f.refTable, n) || !rest.some(r => same(r, f.refTable))));
    done.push(...rest.splice(ready < 0 ? 0 : ready, 1));
  }
  return done;
}

// the statements that make A into B, in an order that works: new tables (parents first), then each changed table (foreign keys and indexes that go, columns that go,
// columns that come, columns that change, indexes and foreign keys that come), the dropped tables last, as comments
export function migrationSql(diff: SchemaDiff, b: SchemaSnapshot, targetDb?: string): string[] {
  const out: string[] = [];
  if (targetDb) out.push(`USE ${ident(targetDb)};`);
  for (const name of orderByParents(diff.addedTables, b)) {
    const sql = b[name]?.createSql?.trim();
    out.push(sql ? (sql.endsWith(";") ? sql : sql + ";") : `-- ${name}: its CREATE TABLE could not be read`);
  }
  for (const c of diff.changedTables) {
    const t = ident(c.table);
    for (const f of c.dropForeignKeys) out.push(`ALTER TABLE ${t} DROP FOREIGN KEY ${ident(f.name)};`);
    for (const i of c.dropIndexes) out.push(`DROP INDEX ${ident(i.name)} ON ${t};`);
    for (const col of c.dropColumns) out.push(`ALTER TABLE ${t} DROP COLUMN ${ident(col.name)};`);
    for (const col of c.addColumns) out.push(`ALTER TABLE ${t} ADD COLUMN ${columnDefinition(col)};`);
    for (const m of c.modifyColumns) out.push(`ALTER TABLE ${t} MODIFY COLUMN ${columnDefinition(m.to)};`);
    for (const i of c.addIndexes) out.push(`CREATE ${i.unique ? "UNIQUE " : ""}INDEX ${ident(i.name)} ON ${t} (${i.columns.map(ident).join(", ")});`);
    for (const f of c.addForeignKeys) out.push(`ALTER TABLE ${t} ${foreignKeySql(f)};`);
  }
  for (const name of diff.droppedTables) out.push(`-- DROP TABLE ${ident(name)};   (it is only in the first schema: not dropped unless you remove the comment)`);
  return out;
}
