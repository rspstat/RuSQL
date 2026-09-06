// ContextMenus.tsx
// Extracted from App.tsx: the sidebar's 4 right-click menus (database / table / view /
// index). Grouped in one file since they're small, share the same ctx-menu shell, and
// were already adjacent in the source. Each menu still delegates the actual query
// execution to the run*CtxQuery helpers that remain in App.tsx (they already manage
// their own menu-close + result/refresh flow) -- these components only own rendering
// and the few actions (like "Copy Schema Name") that don't go through those helpers.

import { invoke } from "@tauri-apps/api/core";
import type { MultiQueryResult } from "../types";

export interface DbCtxMenuState { x: number; y: number; db: string; }
export interface TableCtxMenuState { x: number; y: number; table: string; }
export interface ViewCtxMenuState { x: number; y: number; view: string; }
export interface IndexCtxMenuState { x: number; y: number; index: string; table: string; kind: "single" | "composite" | "hash"; }

export function DbContextMenu({ menu, onClose, refreshSidebar, setEditorQuery, confirmThenRun, runDbCtxQuery }: {
  menu: DbCtxMenuState;
  onClose: () => void;
  refreshSidebar: () => Promise<void>;
  setEditorQuery: (q: string) => void;
  confirmThenRun: (title: string, message: string, action: () => void) => void;
  runDbCtxQuery: (q: string) => void;
}) {
  return (
    <>
      <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={onClose} />
      <div className="ctx-menu table-ctx-menu" style={{ top: menu.y, left: menu.x, zIndex: 1000 }}>
        <div className="ctx-menu-header">{menu.db}</div>
        <div className="ctx-divider" />
        <div onClick={async () => {
          onClose();
          await invoke<MultiQueryResult>("execute_query", { query: `USE ${menu.db};`, ts: Date.now() });
          await refreshSidebar();
        }}>Set as Default Schema</div>
        <div className="ctx-divider" />
        <div onClick={() => {
          onClose();
          setEditorQuery(
            `CREATE TABLE ${menu.db}.table_name (\n  id INT PRIMARY KEY AUTO INCREMENT,\n  name VARCHAR(50) NOT NULL\n);`
          );
        }}>Create Table...</div>
        <div className="ctx-divider" />
        <div onClick={() => {
          navigator.clipboard.writeText(menu.db);
          onClose();
        }}>Copy Schema Name</div>
        <div className="ctx-divider" />
        <div className="ctx-item-danger" onClick={() => {
          const db = menu.db;
          onClose();
          confirmThenRun("Drop Schema", `Permanently drop database "${db}" and all its tables? This cannot be undone.`,
            () => runDbCtxQuery(`DROP DATABASE ${db};`));
        }}>
          Drop Schema...
        </div>
      </div>
    </>
  );
}

export function TableContextMenu({ menu, onClose, openEditTableModal, runCtxQuery, handleCopyTableName, triggerCsvImport, confirmThenRun }: {
  menu: TableCtxMenuState;
  onClose: () => void;
  openEditTableModal: (table: string) => void;
  runCtxQuery: (q: string, dropTable?: string) => void;
  handleCopyTableName: (t: string) => void;
  triggerCsvImport: (table: string) => void;
  confirmThenRun: (title: string, message: string, action: () => void) => void;
}) {
  return (
    <>
      <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={onClose} />
      <div className="ctx-menu table-ctx-menu" style={{ top: menu.y, left: menu.x, zIndex: 1000 }}>
        <div className="ctx-menu-header">{menu.table}</div>
        <div className="ctx-divider" />
        <div onClick={() => openEditTableModal(menu.table)}>Edit Table...</div>
        <div className="ctx-divider" />
        <div onClick={() => runCtxQuery(`SELECT * FROM ${menu.table};`)}>Select Rows</div>
        <div onClick={() => runCtxQuery(`SELECT * FROM ${menu.table} LIMIT 100;`)}>Select Rows (LIMIT 100)</div>
        <div onClick={() => runCtxQuery(`DESCRIBE ${menu.table};`)}>Describe Table</div>
        <div onClick={() => runCtxQuery(`SHOW CREATE TABLE ${menu.table};`)}>Show Create Table</div>
        <div className="ctx-divider" />
        <div onClick={() => handleCopyTableName(menu.table)}>Copy Table Name</div>
        <div onClick={() => {
          navigator.clipboard.writeText(`INSERT INTO ${menu.table} VALUES ();`);
          onClose();
        }}>Copy as INSERT</div>
        <div onClick={() => { const t = menu.table; onClose(); triggerCsvImport(t); }}>Import CSV...</div>
        <div className="ctx-divider" />
        <div className="ctx-item-warn" onClick={() => {
          const t = menu.table;
          onClose();
          confirmThenRun("Truncate Table", `Permanently delete all rows in "${t}"? This cannot be undone.`,
            () => runCtxQuery(`TRUNCATE TABLE ${t};`));
        }}>Truncate Table</div>
        <div className="ctx-item-danger" onClick={() => {
          const t = menu.table;
          onClose();
          confirmThenRun("Drop Table", `Permanently drop table "${t}" and all its data? This cannot be undone.`,
            () => runCtxQuery(`DROP TABLE ${t};`, t));
        }}>
          DROP Table
        </div>
      </div>
    </>
  );
}

export function ViewContextMenu({ menu, onClose, runViewCtxQuery, confirmThenRun }: {
  menu: ViewCtxMenuState;
  onClose: () => void;
  runViewCtxQuery: (q: string, dropView?: string) => void;
  confirmThenRun: (title: string, message: string, action: () => void) => void;
}) {
  return (
    <>
      <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={onClose} />
      <div className="ctx-menu table-ctx-menu" style={{ top: menu.y, left: menu.x, zIndex: 1000 }}>
        <div className="ctx-menu-header">{menu.view}</div>
        <div className="ctx-divider" />
        <div onClick={() => runViewCtxQuery(`SELECT * FROM ${menu.view};`)}>Select Rows</div>
        <div onClick={() => runViewCtxQuery(`SELECT * FROM ${menu.view} LIMIT 100;`)}>Select Rows (LIMIT 100)</div>
        <div onClick={() => runViewCtxQuery(`SHOW CREATE VIEW ${menu.view};`)}>Show Create View</div>
        <div className="ctx-divider" />
        <div onClick={() => { navigator.clipboard.writeText(menu.view); onClose(); }}>Copy View Name</div>
        <div className="ctx-divider" />
        <div className="ctx-item-danger" onClick={() => {
          const v = menu.view;
          onClose();
          confirmThenRun("Drop View", `Permanently drop view "${v}"? This cannot be undone.`,
            () => runViewCtxQuery(`DROP VIEW ${v};`, v));
        }}>
          Drop View
        </div>
      </div>
    </>
  );
}

export function IndexContextMenu({ menu, onClose, runIndexCtxQuery, confirmThenRun }: {
  menu: IndexCtxMenuState;
  onClose: () => void;
  runIndexCtxQuery: (q: string, dropIndex?: string) => void;
  confirmThenRun: (title: string, message: string, action: () => void) => void;
}) {
  return (
    <>
      <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={onClose} />
      <div className="ctx-menu table-ctx-menu" style={{ top: menu.y, left: menu.x, zIndex: 1000 }}>
        <div className="ctx-menu-header">{menu.index}</div>
        <div className="ctx-divider" />
        <div onClick={() => runIndexCtxQuery(`SHOW INDEX FROM ${menu.table};`)}>Show Index Info</div>
        <div className="ctx-divider" />
        <div onClick={() => { navigator.clipboard.writeText(menu.index); onClose(); }}>Copy Index Name</div>
        <div className="ctx-divider" />
        <div className="ctx-item-danger" onClick={() => {
          const idx = menu.index, t = menu.table;
          onClose();
          confirmThenRun("Drop Index", `Permanently drop index "${idx}" on "${t}"? This cannot be undone.`,
            () => runIndexCtxQuery(`DROP INDEX ${idx} ON ${t};`, idx));
        }}>
          Drop Index
        </div>
      </div>
    </>
  );
}
