// Sidebar.tsx
// Extracted from App.tsx: the left "SCHEMAS" tree (MySQL Workbench style) -- database
// nodes, each with Tables/Views sub-trees, table sub-sections (Columns/Indexes/Foreign
// Keys/Triggers), bookmarks, and the bottom info strip. Pure presentational + the
// handful of purely-local toggle closures (toggleDb/switchDb/secOpen/toggleSec) that
// were already defined inline in the original JSX -- all persisted state and data
// fetching stays owned by App.tsx and is passed down as props.

import { invoke } from "@tauri-apps/api/core";
import type { RefObject } from "react";
import type { ColumnDetail, DbData, MultiQueryResult, ServerStatus } from "../types";

interface Bookmark { id: string; name: string; sql: string; }

interface Props {
  databases: string[];
  currentDb: string;
  expandedDbs: Set<string>;
  setExpandedDbs: (updater: (prev: Set<string>) => Set<string>) => void;
  dbData: Record<string, DbData>;
  loadDbData: (db: string) => Promise<void>;
  refreshSidebar: () => Promise<void>;
  tablesOpen: Record<string, boolean>;
  setTablesOpen: (updater: (prev: Record<string, boolean>) => Record<string, boolean>) => void;
  viewsOpen: Record<string, boolean>;
  setViewsOpen: (updater: (prev: Record<string, boolean>) => Record<string, boolean>) => void;
  dbAllExpanded: Record<string, boolean>;
  toggleDbExpandAll: (dbName: string, e: React.MouseEvent) => void;
  sidebarSearch: string;
  setSidebarSearch: (v: string) => void;
  expandedTables: Set<string>;
  toggleTable: (t: string) => void;
  tableColumns: Record<string, ColumnDetail[]>;
  expandedTableSections: Record<string, boolean>;
  setExpandedTableSections: (updater: (prev: Record<string, boolean>) => Record<string, boolean>) => void;
  expandedViews: Set<string>;
  toggleView: (v: string) => void;
  viewColumns: Record<string, string[]>;
  bookmarks: Bookmark[];
  setEditorQuery: (q: string) => void;
  removeBookmark: (id: string) => void;
  serverStatus: ServerStatus;
  sidebarWidth: number;
  isSidebarDragging: RefObject<boolean>;
  setDbCtxMenu: (m: { x: number; y: number; db: string } | null) => void;
  setTableCtxMenu: (m: { x: number; y: number; table: string } | null) => void;
  setViewCtxMenu: (m: { x: number; y: number; view: string } | null) => void;
  setIndexCtxMenu: (m: { x: number; y: number; index: string; table: string; kind: "single" | "composite" | "hash" } | null) => void;
}

export default function Sidebar(props: Props) {
  const {
    databases, currentDb, expandedDbs, setExpandedDbs, dbData, loadDbData, refreshSidebar,
    tablesOpen, setTablesOpen, viewsOpen, setViewsOpen, dbAllExpanded, toggleDbExpandAll,
    sidebarSearch, setSidebarSearch, expandedTables, toggleTable, tableColumns,
    expandedTableSections, setExpandedTableSections, expandedViews, toggleView, viewColumns,
    bookmarks, setEditorQuery, removeBookmark, serverStatus, sidebarWidth, isSidebarDragging,
    setDbCtxMenu, setTableCtxMenu, setViewCtxMenu, setIndexCtxMenu,
  } = props;

  return (
    <>
      <div className="sidebar" style={{ width: `${sidebarWidth}px` }}>
        <div className="sidebar-title-row">
          <span className="sidebar-title">SCHEMAS</span>
          <button
            className="sidebar-refresh-btn"
            onClick={refreshSidebar}
            title="Refresh"
          >⟳</button>
        </div>
        <input
          className="sidebar-search"
          placeholder="Search tables..."
          value={sidebarSearch}
          onChange={e => setSidebarSearch(e.target.value)}
        />

        {/* ── DATABASE NODES (MySQL Workbench style) ── */}
        <div className="sidebar-db-node">
          {databases.length === 0 ? (
            <div className="sidebar-empty" style={{ padding: "8px 12px" }}>No databases</div>
          ) : databases.map(dbName => {
            const isActive = dbName === currentDb;
            const isOpen = expandedDbs.has(dbName);
            const data = dbData[dbName] ?? { tables: [], views: [], indexes: [], triggers: [] };
            const tOpen = tablesOpen[dbName] ?? true;
            const vOpen = viewsOpen[dbName] ?? true;

            const toggleDb = async () => {
              const willOpen = !isOpen;
              setExpandedDbs(prev => {
                const s = new Set(prev);
                willOpen ? s.add(dbName) : s.delete(dbName);
                return s;
              });
              if (willOpen) await loadDbData(dbName);
            };

            const switchDb = async (e: React.MouseEvent) => {
              e.stopPropagation();
              if (isActive) return;
              await invoke<MultiQueryResult>("execute_query", { query: `USE ${dbName};`, ts: Date.now() });
              await refreshSidebar();
            };

            return (
              <div key={dbName}>
                <div
                  className={`sidebar-db-header${isActive ? " sidebar-db-active" : ""}`}
                  onClick={toggleDb}
                  onDoubleClick={switchDb}
                  onContextMenu={e => {
                    e.preventDefault();
                    e.stopPropagation();
                    setDbCtxMenu({ x: e.clientX, y: e.clientY, db: dbName });
                  }}
                  title={isActive ? "Current database" : "Double-click to switch"}
                >
                  <span className="sidebar-group-arrow">{isOpen ? "▼" : "▶"}</span>
                  <svg className="sidebar-db-icon" viewBox="0 0 24 24" width="13" height="16" preserveAspectRatio="none" fill="none">
                    <ellipse cx="12" cy="5" rx="9" ry="3.5" stroke="currentColor" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                    <path d="M3 5v6c0 1.93 4.03 3.5 9 3.5s9-1.57 9-3.5V5" stroke="currentColor" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                    <path d="M3 11v6c0 1.93 4.03 3.5 9 3.5s9-1.57 9-3.5v-6" stroke="currentColor" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                  </svg>
                  <span className="sidebar-db-name">{dbName}{isActive ? " ◀" : ""}</span>
                  <button
                    className={`sidebar-db-expand-btn${dbAllExpanded[dbName] ? " active" : ""}`}
                    onClick={e => toggleDbExpandAll(dbName, e)}
                    title={dbAllExpanded[dbName] ? "Collapse all" : "Expand all"}
                  >
                    {dbAllExpanded[dbName] ? (
                      <svg viewBox="0 0 14 14" width="11" height="11" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
                        <polyline points="2,9 7,5 12,9"/>
                        <polyline points="2,5 7,1 12,5"/>
                      </svg>
                    ) : (
                      <svg viewBox="0 0 14 14" width="11" height="11" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
                        <polyline points="2,5 7,9 12,5"/>
                        <polyline points="2,9 7,13 12,9"/>
                      </svg>
                    )}
                  </button>
                </div>

                {isOpen && (
                  <div className="sidebar-db-children">

                    {/* ── TABLES ── */}
                    <div className="sidebar-group sidebar-group-nested">
                      <div className="sidebar-group-header sidebar-section-header" onClick={() => setTablesOpen(p => ({ ...p, [dbName]: !tOpen }))}>
                        <span className="sidebar-group-arrow">{tOpen ? "▼" : "▶"}</span>
                        <span className="sidebar-section-icon">⊞</span>
                        Tables
                        <span className="sidebar-badge">{data.tables.length}</span>
                      </div>
                      {tOpen && (data.tables.length === 0 ? (
                        <div className="sidebar-empty sidebar-empty-nested">No tables yet</div>
                      ) : data.tables.filter(t => !sidebarSearch || t.toLowerCase().includes(sidebarSearch.toLowerCase())).map(t => {
                        const tExpanded = expandedTables.has(t);
                        const tCols     = tableColumns[t] ?? [];
                        const tIdxs     = data.indexes.filter(i => i.table === t);
                        const tFkeys    = tCols.filter(c => c.fk_ref);
                        const tTrgs     = data.triggers.filter(tr => tr.table === t);
                        const secOpen   = (sec: string) => expandedTableSections[`${t}::${sec}`] ?? false;
                        const toggleSec = (sec: string) =>
                          setExpandedTableSections(p => ({ ...p, [`${t}::${sec}`]: !p[`${t}::${sec}`] }));
                        return (
                        <div key={t}>
                          {/* 테이블 행 */}
                          <div
                            className={`sidebar-item sidebar-item-nested ${tExpanded ? "sidebar-item-expanded" : ""}`}
                            onClick={() => toggleTable(t)}
                            onContextMenu={e => {
                              e.preventDefault();
                              e.stopPropagation();
                              setTableCtxMenu({ x: e.clientX, y: e.clientY, table: t });
                            }}
                          >
                            <span className="sidebar-arrow">{tExpanded ? "▼" : "▶"}</span>
                            <span className="sidebar-table-icon">⊞</span>
                            <span className="sidebar-name">{t}</span>
                          </div>

                          {/* 하위 섹션: Columns / Indexes / Foreign Keys / Triggers */}
                          {tExpanded && (
                            <div className="sidebar-table-children">

                              {/* Columns */}
                              <div className="sidebar-subsec-header" onClick={() => toggleSec("columns")}>
                                <span className="sidebar-group-arrow">{secOpen("columns") ? "▼" : "▶"}</span>
                                <span className="sidebar-subsec-icon" style={{ color: "#9cdcfe" }}>≡</span>
                                Columns
                                <span className="sidebar-badge">{tCols.length}</span>
                              </div>
                              {secOpen("columns") && (
                                <div className="sidebar-subsec-list">
                                  {tCols.length === 0
                                    ? <div className="sidebar-subsec-empty">loading…</div>
                                    : tCols.map(col => (
                                      <div key={col.name} className="sidebar-subsec-item" title={[
                                        col.data_type,
                                        col.is_pk ? "PRIMARY KEY" : "",
                                        col.is_not_null ? "NOT NULL" : "",
                                        col.is_unique && !col.is_pk ? "UNIQUE" : "",
                                        col.is_auto_inc ? "AUTO_INCREMENT" : "",
                                        col.default_val ? `DEFAULT ${col.default_val}` : "",
                                        col.fk_ref ? `FK → ${col.fk_ref}` : "",
                                      ].filter(Boolean).join(" | ")}>
                                        <span className="col-icon" style={{ color: col.is_pk ? "#f0c040" : col.fk_ref ? "#9cdcfe" : "#666" }}>
                                          {col.is_pk ? "🔑" : col.fk_ref ? "🔗" : "≡"}
                                        </span>
                                        <span className="col-name">{col.name}</span>
                                        <span className="col-type">{col.data_type}</span>
                                        {col.is_not_null && <span className="col-badge col-badge-nn">NN</span>}
                                        {col.is_unique && !col.is_pk && <span className="col-badge col-badge-uq">UQ</span>}
                                      </div>
                                    ))
                                  }
                                </div>
                              )}

                              {/* Indexes */}
                              <div className="sidebar-subsec-header" onClick={() => toggleSec("indexes")}>
                                <span className="sidebar-group-arrow">{secOpen("indexes") ? "▼" : "▶"}</span>
                                <span className="sidebar-subsec-icon" style={{ color: "#c586c0" }}>⌗</span>
                                Indexes
                                <span className="sidebar-badge">{tIdxs.length}</span>
                              </div>
                              {secOpen("indexes") && (
                                <div className="sidebar-subsec-list">
                                  {tIdxs.length === 0
                                    ? <div className="sidebar-subsec-empty">no indexes</div>
                                    : tIdxs.map(idx => (
                                      <div key={idx.name} className="sidebar-subsec-item"
                                        title={`${idx.kind} · ${idx.columns.join(", ")}`}
                                        onContextMenu={e => {
                                          e.preventDefault();
                                          e.stopPropagation();
                                          setIndexCtxMenu({ x: e.clientX, y: e.clientY, index: idx.name, table: idx.table, kind: idx.kind });
                                        }}
                                      >
                                        <span className="sidebar-subsec-icon" style={{ color: idx.kind === "hash" ? "#4ec9b0" : idx.kind === "composite" ? "#ddb05d" : "#c586c0" }}>
                                          {idx.kind === "hash" ? "#" : idx.kind === "composite" ? "⋈" : "⌗"}
                                        </span>
                                        <span className="col-name">{idx.name}</span>
                                        <span className="col-type" style={{ fontSize: "10px" }}>{idx.kind}</span>
                                      </div>
                                    ))
                                  }
                                </div>
                              )}

                              {/* Foreign Keys */}
                              <div className="sidebar-subsec-header" onClick={() => toggleSec("fkeys")}>
                                <span className="sidebar-group-arrow">{secOpen("fkeys") ? "▼" : "▶"}</span>
                                <span className="sidebar-subsec-icon" style={{ color: "#9cdcfe" }}>🔗</span>
                                Foreign Keys
                                <span className="sidebar-badge">{tFkeys.length}</span>
                              </div>
                              {secOpen("fkeys") && (
                                <div className="sidebar-subsec-list">
                                  {tFkeys.length === 0
                                    ? <div className="sidebar-subsec-empty">no foreign keys</div>
                                    : tFkeys.map(col => (
                                      <div key={col.name} className="sidebar-subsec-item" title={`${col.name} → ${col.fk_ref}`}>
                                        <span className="col-icon" style={{ color: "#9cdcfe" }}>🔗</span>
                                        <span className="col-name">{col.name}</span>
                                        <span className="col-type" style={{ fontSize: "10px", color: "#888" }}>→ {col.fk_ref}</span>
                                      </div>
                                    ))
                                  }
                                </div>
                              )}

                              {/* Triggers */}
                              <div className="sidebar-subsec-header" onClick={() => toggleSec("triggers")}>
                                <span className="sidebar-group-arrow">{secOpen("triggers") ? "▼" : "▶"}</span>
                                <span className="sidebar-subsec-icon" style={{ color: "#f08080" }}>⚡</span>
                                Triggers
                                <span className="sidebar-badge">{tTrgs.length}</span>
                              </div>
                              {secOpen("triggers") && (
                                <div className="sidebar-subsec-list">
                                  {tTrgs.length === 0
                                    ? <div className="sidebar-subsec-empty">no triggers</div>
                                    : tTrgs.map(trg => (
                                      <div key={trg.name} className="sidebar-subsec-item" title={`${trg.timing} ${trg.event}`}>
                                        <span className="sidebar-subsec-icon" style={{ color: "#f08080" }}>⚡</span>
                                        <span className="col-name">{trg.name}</span>
                                        <span className="col-type" style={{ fontSize: "10px" }}>{trg.timing} {trg.event}</span>
                                      </div>
                                    ))
                                  }
                                </div>
                              )}

                            </div>
                          )}
                        </div>
                        );
                      }))}
                    </div>

                    {/* ── VIEWS ── */}
                    <div className="sidebar-group sidebar-group-nested">
                      <div className="sidebar-group-header sidebar-section-header" onClick={() => setViewsOpen(p => ({ ...p, [dbName]: !vOpen }))}>
                        <span className="sidebar-group-arrow">{vOpen ? "▼" : "▶"}</span>
                        <span className="sidebar-section-icon">◈</span>
                        Views
                        <span className="sidebar-badge">{data.views.length}</span>
                      </div>
                      {vOpen && (data.views.length === 0 ? (
                        <div className="sidebar-empty sidebar-empty-nested">No views yet</div>
                      ) : data.views.map(v => (
                        <div key={v}>
                          <div
                            className={`sidebar-item sidebar-item-nested ${expandedViews.has(v) ? "active" : ""}`}
                            onClick={() => toggleView(v)}
                            onContextMenu={e => {
                              e.preventDefault();
                              e.stopPropagation();
                              setViewCtxMenu({ x: e.clientX, y: e.clientY, view: v });
                            }}
                          >
                            <span className="sidebar-arrow">{expandedViews.has(v) ? "▼" : "▶"}</span>
                            <span className="sidebar-view-icon">◈</span>
                            <span className="sidebar-name">{v}</span>
                          </div>
                          {expandedViews.has(v) && (
                            <div className="sidebar-columns sidebar-columns-nested">
                              {viewColumns[v] && viewColumns[v].length > 0
                                ? viewColumns[v].map(col => (
                                    <div key={col} className="sidebar-column sidebar-column-nested">
                                      <span className="col-icon">◉</span>
                                      <span>{col}</span>
                                    </div>
                                  ))
                                : <div className="sidebar-column sidebar-column-nested" style={{ color: "var(--text-muted)" }}>no column info</div>
                              }
                            </div>
                          )}
                        </div>
                      )))}
                    </div>

                  </div>
                )}
              </div>
            );
          })}
        </div>

        {bookmarks.length > 0 && (
          <div className="sidebar-bookmarks">
            <div className="sidebar-group-header">
              <span className="sidebar-group-arrow">▼</span>
              BOOKMARKS
            </div>
            {bookmarks.map(bk => (
              <div key={bk.id} className="sidebar-bookmark-item">
                <span className="sidebar-bookmark-star">★</span>
                <span className="sidebar-bookmark-name" onClick={() => setEditorQuery(bk.sql)} title={bk.sql}>{bk.name}</span>
                <span className="sidebar-bookmark-del" onClick={() => removeBookmark(bk.id)}>×</span>
              </div>
            ))}
          </div>
        )}

        <div className="sidebar-bottom">
          <div className="sidebar-group-header">
            <span className="sidebar-group-arrow">▼</span>
            INFO
          </div>
          <div className="sidebar-info-item"><span className="col-icon">◉</span> v2.3.0</div>
          <div className="sidebar-info-item"><span className="col-icon">◉</span> Rust · Python</div>
          <div className="sidebar-info-item">
            <span className="col-icon" style={{ color: serverStatus.running ? "#4ec9b0" : "#858585" }}>◉</span>
            {serverStatus.running ? `TCP :${serverStatus.port} (${serverStatus.client_count})` : "TCP Stopped"}
          </div>
        </div>
      </div>

      {/* 사이드바 ↔ 에디터 구분선 (가로 드래그) */}
      <div
        className="sidebar-divider"
        onMouseDown={() => {
          isSidebarDragging.current = true;
          document.body.style.cursor = "col-resize";
          document.body.style.userSelect = "none";
        }}
      />
    </>
  );
}
