// SchemaDiffModal.tsx
// "Compare Schema..." in a database's right-click menu: two databases, what differs between their schemas (tables, columns, indexes, foreign keys) and the statements that
// turn the first into the second. The schemas are read with information_schema (and SHOW CREATE TABLE for a table that only the second has); the comparison and the SQL are in
// lib/schemaDiff.ts. Nothing is run here: the statements are shown, copied or put into the editor, to be run by the person.

import { useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { MultiQueryResult } from "../types";
import { diffSchemas, isEmptyDiff, migrationSql, type ColumnInfo, type ForeignKeyInfo, type IndexInfo, type SchemaDiff, type SchemaSnapshot, type TableInfo } from "../lib/schemaDiff";
import "./SchemaDiffModal.css";

const lit = (s: string) => "'" + s.replace(/\\/g, "\\\\").replace(/'/g, "''") + "'";
const quote = (n: string) => (/^[A-Za-z_][A-Za-z0-9_]*$/.test(n) ? n : "`" + n.replace(/`/g, "``") + "`");

async function ask(query: string): Promise<{ columns: string[]; rows: string[][]; message: string }> {
  const res = await invoke<MultiQueryResult>("execute_query", { query, ts: Date.now() });
  const r = res.results[0];
  if (!r || !r.success) throw new Error(r?.message ?? "no answer");
  return { columns: r.columns, rows: r.rows, message: r.message ?? "" };
}

const at = (columns: string[], name: string) => columns.findIndex(c => c.toLowerCase() === name.toLowerCase());

// one database as a SchemaSnapshot: three queries on information_schema, put together here
export async function readSchema(db: string, withCreate: (table: string) => boolean): Promise<SchemaSnapshot> {
  const cols = await ask(`SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, EXTRA FROM information_schema.columns WHERE TABLE_SCHEMA = ${lit(db)} ORDER BY TABLE_NAME, ORDINAL_POSITION;`);
  const stats = await ask(`SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, SEQ_IN_INDEX, COLUMN_NAME FROM information_schema.statistics WHERE TABLE_SCHEMA = ${lit(db)} ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX;`);
  const keys = await ask(`SELECT TABLE_NAME, CONSTRAINT_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME FROM information_schema.key_column_usage WHERE TABLE_SCHEMA = ${lit(db)} AND REFERENCED_TABLE_NAME IS NOT NULL;`);
  const rules = await ask(`SELECT TABLE_NAME, CONSTRAINT_NAME, UPDATE_RULE, DELETE_RULE FROM information_schema.referential_constraints WHERE CONSTRAINT_SCHEMA = ${lit(db)};`);
  const snapshot: SchemaSnapshot = {};
  const table = (name: string): TableInfo => (snapshot[name] ??= { name, columns: [], indexes: [], foreignKeys: [] });
  for (const r of cols.rows) {
    const col: ColumnInfo = {
      name: r[at(cols.columns, "COLUMN_NAME")], type: r[at(cols.columns, "COLUMN_TYPE")],
      notNull: r[at(cols.columns, "IS_NULLABLE")] === "NO",
      defaultValue: r[at(cols.columns, "COLUMN_DEFAULT")] === "NULL" ? null : r[at(cols.columns, "COLUMN_DEFAULT")],
      autoIncrement: /auto_increment/i.test(r[at(cols.columns, "EXTRA")] ?? ""),
    };
    table(r[at(cols.columns, "TABLE_NAME")]).columns.push(col);
  }
  const indexes = new Map<string, IndexInfo & { table: string }>();
  for (const r of stats.rows) {
    const name = r[at(stats.columns, "INDEX_NAME")];
    if (name === "PRIMARY") continue; // (a primary key cannot be added or dropped afterwards: it is part of CREATE TABLE)
    const tableName = r[at(stats.columns, "TABLE_NAME")];
    const key = `${tableName}\u0000${name}`;
    const index = indexes.get(key) ?? { table: tableName, name, columns: [], unique: r[at(stats.columns, "NON_UNIQUE")] === "0" };
    index.columns.push(r[at(stats.columns, "COLUMN_NAME")]);
    indexes.set(key, index);
  }
  for (const { table: t, ...index } of indexes.values()) if (snapshot[t]) snapshot[t].indexes.push(index);
  for (const r of keys.rows) {
    const t = r[at(keys.columns, "TABLE_NAME")], name = r[at(keys.columns, "CONSTRAINT_NAME")];
    const rule = rules.rows.find(x => x[at(rules.columns, "TABLE_NAME")] === t && x[at(rules.columns, "CONSTRAINT_NAME")] === name);
    const fk: ForeignKeyInfo = {
      name, column: r[at(keys.columns, "COLUMN_NAME")], refTable: r[at(keys.columns, "REFERENCED_TABLE_NAME")], refColumn: r[at(keys.columns, "REFERENCED_COLUMN_NAME")],
      onDelete: rule?.[at(rules.columns, "DELETE_RULE")] ?? "NO ACTION", onUpdate: rule?.[at(rules.columns, "UPDATE_RULE")] ?? "NO ACTION",
    };
    if (snapshot[t]) snapshot[t].foreignKeys.push(fk);
  }
  for (const name of Object.keys(snapshot)) {
    if (!withCreate(name)) continue;
    try {
      const shown = await ask(`SHOW CREATE TABLE ${quote(db)}.${quote(name)};`);
      // (the app's own connection answers SHOW CREATE TABLE as text: a line "Table: name", then the statement; a MySQL client gets it as a row)
      snapshot[name].createSql = shown.rows[0]?.[1] ?? shown.rows[0]?.[0] ?? shown.message.replace(/^Table: [^\r\n]*\r?\n/, "");
    } catch { /* the statement is then a comment that says so */ }
  }
  return snapshot;
}

interface Props {
  databases: string[];
  initialDb: string;
  onClose: () => void;
  onUseSql: (sql: string) => void;
}

export default function SchemaDiffModal({ databases, initialDb, onClose, onUseSql }: Props) {
  const [first, setFirst] = useState(initialDb);
  const [second, setSecond] = useState(databases.find(d => d !== initialDb) ?? initialDb);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [diff, setDiff] = useState<SchemaDiff | null>(null);
  const [statements, setStatements] = useState<string[]>([]);
  const [copied, setCopied] = useState(false);

  const compare = async () => {
    setBusy(true);
    setError("");
    setDiff(null);
    setCopied(false);
    try {
      const a = await readSchema(first, () => false);
      const bBare = await readSchema(second, () => false);
      const wanted = diffSchemas(a, bBare).addedTables; // (the CREATE TABLE of a table is only asked for the tables that are new)
      const b = wanted.length ? await readSchema(second, name => wanted.includes(name)) : bBare;
      const d = diffSchemas(a, b);
      setDiff(d);
      setStatements(migrationSql(d, b, first));
    } catch (e) {
      setError(String(e instanceof Error ? e.message : e));
    } finally {
      setBusy(false);
    }
  };

  const sql = statements.join("\n");
  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="sd-modal" onClick={e => e.stopPropagation()}>
        <div className="sd-header">
          <span>Compare schemas</span>
          <button className="sd-close" onClick={onClose}>✕</button>
        </div>
        <div className="sd-body">
          <div className="sd-pick">
            <label>Database to change
              <select value={first} onChange={e => setFirst(e.target.value)}>{databases.map(d => <option key={d}>{d}</option>)}</select>
            </label>
            <span className="sd-arrow">→ should look like →</span>
            <label>Database to copy from
              <select value={second} onChange={e => setSecond(e.target.value)}>{databases.map(d => <option key={d}>{d}</option>)}</select>
            </label>
            <button className="sd-btn sd-primary" disabled={busy || first === second} onClick={compare}>{busy ? "Reading..." : "Compare"}</button>
          </div>
          {first === second && <div className="sd-hint">Choose two different databases.</div>}
          {error && <div className="sd-error">{error}</div>}
          {diff && (isEmptyDiff(diff) ? (
            <div className="sd-same">✓ The two schemas are the same (tables, columns, indexes, foreign keys).</div>
          ) : (
            <>
              <div className="sd-summary">
                {diff.addedTables.length > 0 && <span className="sd-add">+ {diff.addedTables.length} table(s) only in {second}: {diff.addedTables.join(", ")}</span>}
                {diff.droppedTables.length > 0 && <span className="sd-del">− {diff.droppedTables.length} table(s) only in {first}: {diff.droppedTables.join(", ")}</span>}
              </div>
              <div className="sd-changes">
                {diff.changedTables.map(c => (
                  <div key={c.table} className="sd-table">
                    <div className="sd-table-name">{c.table}</div>
                    {c.addColumns.map(x => <div key={"ac" + x.name} className="sd-add">+ column {x.name} {x.type}{x.notNull ? " NOT NULL" : ""}</div>)}
                    {c.dropColumns.map(x => <div key={"dc" + x.name} className="sd-del">− column {x.name}</div>)}
                    {c.modifyColumns.map(x => <div key={"mc" + x.to.name} className="sd-mod">~ column {x.to.name}: {x.what.join("; ")}</div>)}
                    {c.addIndexes.map(x => <div key={"ai" + x.name} className="sd-add">+ {x.unique ? "unique " : ""}index {x.name} ({x.columns.join(", ")})</div>)}
                    {c.dropIndexes.map(x => <div key={"di" + x.name} className="sd-del">− index {x.name} ({x.columns.join(", ")})</div>)}
                    {c.addForeignKeys.map(x => <div key={"af" + x.name} className="sd-add">+ foreign key {x.column} → {x.refTable}({x.refColumn}) ON DELETE {x.onDelete}</div>)}
                    {c.dropForeignKeys.map(x => <div key={"df" + x.name} className="sd-del">− foreign key {x.name} ({x.column} → {x.refTable})</div>)}
                  </div>
                ))}
              </div>
              <div className="sd-sql-title">Migration: {first} → {second} <span className="sd-dim">(nothing is run; the table drops are comments)</span></div>
              <pre className="sd-sql">{sql}</pre>
            </>
          ))}
        </div>
        <div className="sd-footer">
          <span style={{ flex: 1 }} />
          {diff && !isEmptyDiff(diff) && (
            <>
              <button className="sd-btn" onClick={() => { navigator.clipboard.writeText(sql); setCopied(true); }}>{copied ? "Copied" : "Copy SQL"}</button>
              <button className="sd-btn sd-primary" onClick={() => { onUseSql(sql); onClose(); }}>Open in editor</button>
            </>
          )}
          <button className="sd-btn" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}
