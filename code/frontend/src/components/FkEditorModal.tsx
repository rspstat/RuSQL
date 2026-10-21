// FkEditorModal.tsx
// Adds, changes or drops one foreign key from the ERD. The form and the SQL are one thing: the DDL under the form is rebuilt on every change, it is what "Apply"
// sends, and nothing is sent before that. Changing a key is one ALTER TABLE that drops the old constraint and adds the new one, so the engine applies both or neither.

import { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { ColumnDetail, MultiQueryResult } from "../types";
import { parseRef, unqualify } from "../lib/erd";
import { clearSqlCompletionCache } from "../lib/sqlCompletion";
import "./FkEditorModal.css";

const ACTIONS = ["RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT"] as const;
type Action = (typeof ACTIONS)[number];

export type FkTarget = { kind: "add"; table?: string } | { kind: "edit"; table: string; column: string };

const ident = (n: string) => (/^[A-Za-z_][A-Za-z0-9_]*$/.test(n) ? n : "`" + n.replace(/`/g, "``") + "`");
const lit = (s: string) => "'" + s.replace(/\\/g, "\\\\").replace(/'/g, "''") + "'";

async function run(query: string): Promise<MultiQueryResult> {
  return invoke<MultiQueryResult>("execute_query", { query, ts: Date.now() });
}

const baseType = (t: string) => t.replace(/\(.*\)/, "").trim().toUpperCase();

interface Props {
  target: FkTarget;
  db: string;
  columns: Record<string, ColumnDetail[]>;
  onClose: () => void;
  onApplied: () => void;
}

export default function FkEditorModal({ target, db, columns, onClose, onApplied }: Props) {
  const tableNames = Object.keys(columns).sort();
  const edit = target.kind === "edit";
  const firstTable = target.table ?? tableNames[0] ?? "";

  const [childTable, setChildTable] = useState(firstTable);
  const [childCol, setChildCol] = useState(edit ? target.column : "");
  const initial = edit ? parseRef(columns[target.table]?.find(c => c.name === target.column)?.fk_ref ?? "") : null;
  const [parentTable, setParentTable] = useState(initial ? unqualify(initial.table) : tableNames.find(t => t !== firstTable) ?? tableNames[0] ?? "");
  const [parentCol, setParentCol] = useState(initial?.col ?? "");
  const [oldName, setOldName] = useState("");
  const [name, setName] = useState("");
  const [onDelete, setOnDelete] = useState<Action>("RESTRICT");
  const [onUpdate, setOnUpdate] = useState<Action>("RESTRICT");
  const [dropping, setDropping] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  // the constraint as the engine has it: its name and its rules
  useEffect(() => {
    if (!edit) return;
    let alive = true;
    (async () => {
      try {
        const rules = await run(`SELECT CONSTRAINT_NAME, UPDATE_RULE, DELETE_RULE FROM information_schema.referential_constraints WHERE CONSTRAINT_SCHEMA = ${lit(db)} AND TABLE_NAME = ${lit(target.table)};`);
        const usage = await run(`SELECT CONSTRAINT_NAME FROM information_schema.key_column_usage WHERE TABLE_SCHEMA = ${lit(db)} AND TABLE_NAME = ${lit(target.table)} AND COLUMN_NAME = ${lit(target.column)} AND REFERENCED_TABLE_NAME IS NOT NULL;`);
        const wanted = usage.results[0]?.rows[0]?.[0];
        const r = rules.results[0]?.rows.find(row => row[0] === wanted);
        if (!alive || !wanted) return;
        setOldName(wanted);
        setName(wanted);
        if (r) {
          if ((ACTIONS as readonly string[]).includes(r[1])) setOnUpdate(r[1] as Action);
          if ((ACTIONS as readonly string[]).includes(r[2])) setOnDelete(r[2] as Action);
        }
      } catch { /* the form still works without them: the name stays empty and the old key is dropped by its column */ }
    })();
    return () => { alive = false; };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const childCols = columns[childTable] ?? [];
  const parentCols = columns[parentTable] ?? [];
  // a default name for a new key
  const shownName = name || (childCol ? `fk_${childTable}_${childCol}` : "");
  const dropName = oldName || `fk_${target.kind === "edit" ? target.column : childCol}`;

  const ddl = useMemo(() => {
    const part = (v: string, placeholder: string) => (v ? ident(v) : `<${placeholder}>`); // (an empty choice shows what is still to be chosen)
    const add = `ADD CONSTRAINT ${part(shownName, "name")} FOREIGN KEY (${part(childCol, "child column")}) REFERENCES ${part(parentTable, "parent table")} (${part(parentCol, "parent column")})\n    ON DELETE ${onDelete} ON UPDATE ${onUpdate}`;
    if (dropping) return `ALTER TABLE ${ident(childTable)}\n  DROP FOREIGN KEY ${ident(dropName)};`;
    if (edit) return `ALTER TABLE ${ident(childTable)}\n  DROP FOREIGN KEY ${ident(dropName)},\n  ${add};`;
    return `ALTER TABLE ${ident(childTable)}\n  ${add};`;
  }, [shownName, childTable, childCol, parentTable, parentCol, onDelete, onUpdate, dropping, edit, dropName]);

  // what the form can already tell the person
  const child = childCols.find(c => c.name === childCol);
  const parent = parentCols.find(c => c.name === parentCol);
  const warnings: string[] = [];
  if (child && parent && baseType(child.data_type) !== baseType(parent.data_type)) warnings.push(`${childTable}.${childCol} is ${child.data_type} but ${parentTable}.${parentCol} is ${parent.data_type}: the types should be the same.`);
  if (parent && !parent.is_pk && !parent.is_unique) warnings.push(`${parentTable}.${parentCol} is neither a primary key nor unique; a foreign key should point at one.`);
  if (child && (onDelete === "SET NULL" || onUpdate === "SET NULL") && child.is_not_null) warnings.push(`${childTable}.${childCol} is NOT NULL, so SET NULL can never apply.`);
  if (!edit && child?.fk_ref) warnings.push(`${childTable}.${childCol} already references ${child.fk_ref}; edit that relationship instead.`);

  const complete = dropping || (childTable && childCol && parentTable && parentCol && shownName);
  const blocked = !edit && !!child?.fk_ref;

  const apply = async () => {
    setBusy(true);
    setError("");
    try {
      const res = await run(ddl);
      const bad = res.results.find(r => !r.success);
      if (bad) { setError(bad.message); return; }
      clearSqlCompletionCache();
      onApplied();
      onClose();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="fk-modal" onClick={e => e.stopPropagation()}>
        <div className="fk-header">
          <span>{edit ? "Edit relationship" : "New relationship"}</span>
          <button className="fk-close" onClick={onClose}>✕</button>
        </div>
        <div className="fk-body">
          <div className="fk-grid">
            <label>Child table</label>
            <select value={childTable} disabled={edit} onChange={e => { setChildTable(e.target.value); setChildCol(""); }}>
              {tableNames.map(t => <option key={t} value={t}>{t}</option>)}
            </select>
            <label>Child column</label>
            <select value={childCol} disabled={edit} onChange={e => setChildCol(e.target.value)}>
              <option value="">— choose —</option>
              {childCols.map(c => <option key={c.name} value={c.name}>{c.name} ({c.data_type}){c.fk_ref ? " → " + c.fk_ref : ""}</option>)}
            </select>
            <label>Parent table</label>
            <select value={parentTable} disabled={dropping} onChange={e => { setParentTable(e.target.value); setParentCol(""); }}>
              {tableNames.map(t => <option key={t} value={t}>{t}</option>)}
            </select>
            <label>Parent column</label>
            <select value={parentCol} disabled={dropping} onChange={e => setParentCol(e.target.value)}>
              <option value="">— choose —</option>
              {parentCols.map(c => <option key={c.name} value={c.name}>{c.name} ({c.data_type}){c.is_pk ? " PK" : c.is_unique ? " UQ" : ""}</option>)}
            </select>
            <label>Constraint name</label>
            <input value={name} placeholder={shownName} disabled={dropping} onChange={e => setName(e.target.value)} />
            <label>ON DELETE</label>
            <select value={onDelete} disabled={dropping} onChange={e => setOnDelete(e.target.value as Action)}>{ACTIONS.map(a => <option key={a}>{a}</option>)}</select>
            <label>ON UPDATE</label>
            <select value={onUpdate} disabled={dropping} onChange={e => setOnUpdate(e.target.value as Action)}>{ACTIONS.map(a => <option key={a}>{a}</option>)}</select>
          </div>

          {warnings.map((w, i) => <div key={i} className="fk-warn">⚠ {w}</div>)}

          <div className="fk-ddl-title">DDL preview <span className="fk-dim">(nothing is run until Apply)</span></div>
          <pre className="fk-ddl">{ddl}</pre>
          {error && <div className="fk-error">{error}</div>}
        </div>
        <div className="fk-footer">
          {edit && (
            <button className="fk-btn fk-danger" onClick={() => setDropping(d => !d)}>{dropping ? "Keep editing" : "Delete relationship"}</button>
          )}
          <span style={{ flex: 1 }} />
          <button className="fk-btn" onClick={onClose}>Cancel</button>
          <button className="fk-btn fk-primary" disabled={busy || !complete || blocked} onClick={apply}>
            {busy ? "Applying..." : dropping ? "Drop foreign key" : edit ? "Apply change" : "Create relationship"}
          </button>
        </div>
      </div>
    </div>
  );
}
