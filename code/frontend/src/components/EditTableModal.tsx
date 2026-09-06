// EditTableModal.tsx
// Extracted from App.tsx's "Edit Table" modal (table right-click menu → "Edit Table...").
// Pure presentational; App.tsx still owns editTableModal/editTableNewCol state and the
// dropColumn/addColumn handlers.

import type { ColumnDetail } from "../types";

export interface EditTableModalState { table: string; cols: ColumnDetail[]; }
export interface NewColState { name: string; type: string; notNull: boolean; defaultVal: string; }

const NEW_COL_TYPES = [
  "INT", "BIGINT", "VARCHAR(50)", "VARCHAR(100)", "VARCHAR(255)", "TEXT",
  "FLOAT", "DOUBLE", "DECIMAL(10,2)", "BOOLEAN", "DATE", "DATETIME", "TIMESTAMP",
];

interface Props {
  modal: EditTableModalState;
  newCol: NewColState;
  onClose: () => void;
  onNewColChange: (next: NewColState) => void;
  onDropColumn: (table: string, colName: string) => void;
  onAddColumn: () => void;
}

export default function EditTableModal({ modal, newCol, onClose, onNewColChange, onDropColumn, onAddColumn }: Props) {
  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="edit-table-modal" onClick={e => e.stopPropagation()}>
        <div className="edit-table-header">
          <span>Edit Table: <strong>{modal.table}</strong></span>
          <button className="edit-table-close" onClick={onClose}>✕</button>
        </div>
        <div className="edit-table-body">
          <div className="edit-table-section">Columns</div>
          <table className="edit-table-cols">
            <thead><tr><th>Name</th><th>Type</th><th>Constraints</th><th></th></tr></thead>
            <tbody>
              {modal.cols.map(col => (
                <tr key={col.name}>
                  <td>{col.is_pk ? "🔑 " : ""}{col.name}</td>
                  <td>{col.data_type}</td>
                  <td className="edit-table-constraints">
                    {[col.is_pk && "PK", col.is_not_null && "NOT NULL", col.is_unique && !col.is_pk && "UNIQUE", col.is_auto_inc && "AUTO_INC"].filter(Boolean).join(", ")}
                  </td>
                  <td>
                    {!col.is_pk && (
                      <button className="drop-col-btn" onClick={() => onDropColumn(modal.table, col.name)}>Drop</button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <div className="edit-table-section" style={{ marginTop: 20 }}>Add Column</div>
          <div className="add-col-form">
            <input
              className="add-col-input"
              placeholder="Column name"
              value={newCol.name}
              onChange={e => onNewColChange({ ...newCol, name: e.target.value })}
              onKeyDown={e => { if (e.key === "Enter") onAddColumn(); }}
            />
            <select
              className="add-col-select"
              value={newCol.type}
              onChange={e => onNewColChange({ ...newCol, type: e.target.value })}
            >
              {NEW_COL_TYPES.map(t => (
                <option key={t} value={t}>{t}</option>
              ))}
            </select>
            <label className="add-col-check">
              <input type="checkbox" checked={newCol.notNull} onChange={e => onNewColChange({ ...newCol, notNull: e.target.checked })} />
              NOT NULL
            </label>
            <input
              className="add-col-input"
              placeholder="DEFAULT value (optional)"
              value={newCol.defaultVal}
              onChange={e => onNewColChange({ ...newCol, defaultVal: e.target.value })}
              onKeyDown={e => { if (e.key === "Enter") onAddColumn(); }}
            />
            <button className="add-col-btn" onClick={onAddColumn}>Add Column</button>
          </div>
        </div>
      </div>
    </div>
  );
}
