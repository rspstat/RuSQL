// ErdView.tsx
// Extracted from App.tsx: the ERD (activity-bar "ERD Editor") view -- FK-relationship
// diagram canvas with draggable/zoomable table cards, auto-layout, and the bottom
// per-table data panel. Pure presentational + the local `filtered`/`widths` derivation
// that was already computed inline in the original JSX; all persisted ERD state (pan/
// zoom/positions/selected table/etc.), data loading, and drag-tracking refs stay owned
// by App.tsx (the drag mousemove/mouseup effect that mutates erdPositions/erdPan/
// erdDataHeight during a drag remains in App.tsx since it isn't part of this render).

import type { Dispatch, RefObject, SetStateAction } from "react";
import type { ColumnDetail, QueryResult } from "../types";
import { ERD_CARD_W, ERD_COL_H, ERD_HEADER_H, erdOrthPath, parseRef, unqualify, type ErdPos } from "../lib/erd";
import { measureTextPx } from "../lib/measureText";

interface CardDrag { table: string; startMX: number; startMY: number; startCX: number; startCY: number; zoom: number; }
interface CanvasDrag { startMX: number; startMY: number; startPX: number; startPY: number; }

interface Props {
  currentDb: string;
  erdColumns: Record<string, ColumnDetail[]>;
  erdPositions: Record<string, ErdPos>;
  erdZoom: number;
  setErdZoom: Dispatch<SetStateAction<number>>;
  erdPan: ErdPos;
  setErdPan: Dispatch<SetStateAction<ErdPos>>;
  isAutoLayout: boolean;
  autoLayoutErd: () => void;
  erdLoading: boolean;
  loadErd: () => void;
  erdSelectedTable: string;
  setErdSelectedTable: Dispatch<SetStateAction<string>>;
  handleErdCardClick: (tbl: string) => void;
  erdHoveredTable: string | null;
  setErdHoveredTable: Dispatch<SetStateAction<string | null>>;
  erdAnimating: boolean;
  erdCanvasRef: RefObject<HTMLDivElement | null>;
  erdCardDragRef: RefObject<CardDrag | null>;
  erdCanvasDragRef: RefObject<CanvasDrag | null>;
  erdCardWasDragged: RefObject<boolean>;
  erdCanvasWasDragged: RefObject<boolean>;
  erdDataDragging: RefObject<boolean>;
  erdDataHeight: number;
  setErdDataHeight: Dispatch<SetStateAction<number>>;
  erdTableData: QueryResult | null;
  setErdTableData: Dispatch<SetStateAction<QueryResult | null>>;
  erdTableLoading: boolean;
  loadErdTableData: (tbl: string) => void;
  erdFilter: string;
  setErdFilter: Dispatch<SetStateAction<string>>;
}

export default function ErdView(props: Props) {
  const {
    currentDb, erdColumns, erdPositions, erdZoom, setErdZoom, erdPan, setErdPan,
    isAutoLayout, autoLayoutErd, erdLoading, loadErd, erdSelectedTable, setErdSelectedTable,
    handleErdCardClick, erdHoveredTable, setErdHoveredTable, erdAnimating, erdCanvasRef,
    erdCardDragRef, erdCanvasDragRef, erdCardWasDragged, erdCanvasWasDragged, erdDataDragging,
    erdDataHeight, setErdDataHeight, erdTableData, setErdTableData, erdTableLoading,
    loadErdTableData, erdFilter, setErdFilter,
  } = props;

  return (
    <div className="erd-view">
      <div className="erd-header">
        <div className="erd-header-left">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" style={{ opacity: 0.7 }}>
            <rect x="1" y="2" width="9" height="6" rx="1.5"/>
            <rect x="1" y="16" width="9" height="6" rx="1.5"/>
            <rect x="14" y="9" width="9" height="6" rx="1.5"/>
            <path d="M10 5H12V12H14"/>
            <path d="M10 19H12V12"/>
          </svg>
          <span className="erd-header-title">ERD — {currentDb}</span>
          <span className="erd-table-count">{Object.keys(erdColumns).length} tables</span>
        </div>
        <div className="erd-header-right">
          <div className="erd-zoom-slider">
            <input
              type="range"
              min={0}
              max={200}
              value={Math.round(erdZoom * 100)}
              onChange={e => setErdZoom(Number(e.target.value) / 100)}
              title="Zoom (0~200%)"
            />
            <span className="erd-zoom-label">{Math.round(erdZoom * 100)}%</span>
          </div>
          <button className="erd-tool-btn" onClick={autoLayoutErd} title={isAutoLayout ? "Restore original positions" : "Auto arrange by FK"}>
            {isAutoLayout ? "↩ Reset Layout" : "⊞ Auto Layout"}
          </button>
          <button className="erd-tool-btn" onClick={() => { setErdPan({ x: 40, y: 40 }); setErdZoom(1); }} title="Reset view">⊡ Reset</button>
          <button className="erd-tool-btn" onClick={loadErd} title="Refresh">↻ Refresh</button>
        </div>
      </div>

      <div
        className="erd-canvas"
        ref={erdCanvasRef}
        onMouseDown={e => {
          if ((e.target as HTMLElement).closest(".erd-card")) return;
          erdCanvasWasDragged.current = false;
          erdCanvasDragRef.current = { startMX: e.clientX, startMY: e.clientY, startPX: erdPan.x, startPY: erdPan.y };
          document.body.style.cursor = "grabbing";
          document.body.style.userSelect = "none";
        }}
        onClick={e => {
          // 빈 캔버스를 클릭(드래그 아님)하면 선택 해제 + 데이터 패널 닫기
          if (erdCanvasWasDragged.current) return;
          if ((e.target as HTMLElement).closest(".erd-card")) return;
          setErdSelectedTable("");
          setErdTableData(null);
          setErdDataHeight(0);
        }}
        onWheel={e => {
          e.preventDefault();
          const factor = e.deltaY < 0 ? 1.1 : 0.9;
          setErdZoom(z => Math.max(0.2, Math.min(2.5, z * factor)));
        }}
      >
        {erdLoading ? (
          <div className="erd-loading">Loading ERD...</div>
        ) : Object.keys(erdColumns).length === 0 ? (
          <div className="erd-empty">
            <div className="erd-empty-icon">⬡</div>
            <div className="erd-empty-text">No tables in <b>{currentDb}</b></div>
            <div className="erd-empty-sub">Create a table and press ↻ Refresh</div>
          </div>
        ) : (
          <div
            className="erd-transform"
            style={{ transform: `translate(${erdPan.x}px, ${erdPan.y}px) scale(${erdZoom})`, transformOrigin: "0 0" }}
          >
            {/* FK 관계선 SVG */}
            <svg style={{ position: "absolute", top: 0, left: 0, width: 1, height: 1, overflow: "visible", pointerEvents: "none", zIndex: 0 }}>
              <defs>
                <marker id="erd-one" markerWidth="14" markerHeight="18" refX="11" refY="9" orient="auto">
                  <line x1="5" y1="3" x2="5" y2="15" stroke="#c0395a" strokeWidth="1.4"/>
                  <line x1="9" y1="3" x2="9" y2="15" stroke="#c0395a" strokeWidth="1.4"/>
                </marker>
                <marker id="erd-many" markerWidth="24" markerHeight="18" refX="21" refY="9" orient="auto-start-reverse">
                  <circle cx="4" cy="9" r="3" fill="none" stroke="#c0395a" strokeWidth="1.2"/>
                  <path d="M8 9 L21 3 M8 9 L21 9 M8 9 L21 15" stroke="#c0395a" strokeWidth="1.2" fill="none"/>
                </marker>
                <marker id="erd-one-hi" markerWidth="14" markerHeight="18" refX="11" refY="9" orient="auto">
                  <line x1="5" y1="3" x2="5" y2="15" stroke="#4ec9b0" strokeWidth="1.8"/>
                  <line x1="9" y1="3" x2="9" y2="15" stroke="#4ec9b0" strokeWidth="1.8"/>
                </marker>
                <marker id="erd-many-hi" markerWidth="24" markerHeight="18" refX="21" refY="9" orient="auto-start-reverse">
                  <circle cx="4" cy="9" r="3" fill="none" stroke="#4ec9b0" strokeWidth="1.5"/>
                  <path d="M8 9 L21 3 M8 9 L21 9 M8 9 L21 15" stroke="#4ec9b0" strokeWidth="1.5" fill="none"/>
                </marker>
              </defs>
              {Object.entries(erdColumns).flatMap(([tableName, cols]) =>
                cols.map((col, colIdx) => {
                  if (!col.fk_ref) return null;
                  const parsed = parseRef(col.fk_ref);
                  if (!parsed) return null;
                  const refTable = unqualify(parsed.table);
                  const srcPos = erdPositions[tableName];
                  const tgtPos = erdPositions[refTable];
                  const tgtCols = erdColumns[refTable];
                  if (!srcPos || !tgtPos || !tgtCols) return null;
                  const tgtColIdx = tgtCols.findIndex(c => c.name === parsed.col);
                  const srcY = srcPos.y + ERD_HEADER_H + colIdx * ERD_COL_H + ERD_COL_H / 2;
                  const tgtY = tgtPos.y + ERD_HEADER_H + (tgtColIdx >= 0 ? tgtColIdx * ERD_COL_H : 0) + ERD_COL_H / 2;
                  const srcRight = srcPos.x + ERD_CARD_W;
                  const tgtRight = tgtPos.x + ERD_CARD_W;
                  const isHovered = erdHoveredTable !== null && (erdHoveredTable === tableName || erdHoveredTable === refTable);
                  const isDimmed = erdHoveredTable !== null && !isHovered;
                  let px1: number, py1: number, px2: number, py2: number, pathD: string;
                  if (srcRight + 10 <= tgtPos.x) {
                    px1 = srcRight; py1 = srcY; px2 = tgtPos.x; py2 = tgtY;
                    pathD = erdOrthPath(px1, py1, px2, py2);
                  } else if (tgtRight + 10 <= srcPos.x) {
                    px1 = srcPos.x; py1 = srcY; px2 = tgtRight; py2 = tgtY;
                    pathD = erdOrthPath(px1, py1, px2, py2);
                  } else {
                    // 수평 겹침: 짧은 쪽으로 우회
                    const rightX = Math.max(srcRight, tgtRight) + 44 + colIdx * 14;
                    const leftX  = Math.min(srcPos.x, tgtPos.x) - 44 - colIdx * 14;
                    const useLeft = leftX > 0 && (rightX - leftX > leftX);
                    const detourX = useLeft ? leftX : rightX;
                    px1 = useLeft ? srcPos.x : srcRight; py1 = srcY;
                    px2 = useLeft ? tgtPos.x : tgtRight; py2 = tgtY;
                    const sdy = Math.sign(tgtY - srcY);
                    const r = Math.min(8, Math.abs(tgtY - srcY) / 2);
                    if (r < 1) {
                      pathD = `M${px1} ${srcY} H${detourX} V${tgtY} H${px2}`;
                    } else {
                      const sdx = useLeft ? 1 : -1;
                      pathD = [
                        `M${px1} ${srcY}`,
                        `H${detourX + sdx * r}`,
                        `Q${detourX} ${srcY} ${detourX} ${srcY + sdy * r}`,
                        `V${tgtY - sdy * r}`,
                        `Q${detourX} ${tgtY} ${detourX + sdx * r} ${tgtY}`,
                        `H${px2}`,
                      ].join(" ");
                    }
                  }
                  const stroke = isHovered ? "#4ec9b0" : "#c0395a";
                  const sw = isHovered ? 2.5 : 1.5;
                  const opacity = isDimmed ? 0.15 : isHovered ? 1 : 0.75;
                  return (
                    <g key={`${tableName}.${col.name}`}>
                      <path
                        d={pathD}
                        fill="none"
                        stroke={stroke}
                        strokeWidth={sw}
                        strokeDasharray="6 4"
                        opacity={opacity}
                        markerStart={isHovered ? "url(#erd-many-hi)" : "url(#erd-many)"}
                        markerEnd={isHovered ? "url(#erd-one-hi)" : "url(#erd-one)"}
                        className={isHovered ? "erd-edge-active" : isDimmed ? "erd-edge-dim" : "erd-edge"}
                      />
                      {isHovered && (
                        <>
                          <circle cx={px1} cy={py1} r="4" fill="#4ec9b0" opacity="0.9"/>
                          <circle cx={px2} cy={py2} r="4" fill="#4ec9b0" opacity="0.9"/>
                        </>
                      )}
                    </g>
                  );
                })
              )}
            </svg>

            {/* 테이블 카드 */}
            {Object.entries(erdColumns).map(([tableName, cols]) => {
              const pos = erdPositions[tableName];
              if (!pos) return null;
              const maxNameW = Math.max(40, ...cols.map(c => measureTextPx(c.name))) + 10;
              const isLinked = erdHoveredTable !== null && erdHoveredTable !== tableName && (
                cols.some(c => c.fk_ref && unqualify(parseRef(c.fk_ref)?.table ?? "") === erdHoveredTable) ||
                Object.entries(erdColumns).some(([t, cs]) => t === erdHoveredTable && cs.some(c => c.fk_ref && unqualify(parseRef(c.fk_ref)?.table ?? "") === tableName))
              );
              return (
                <div
                  key={tableName}
                  className={`erd-card${erdSelectedTable === tableName ? " erd-card-selected" : ""}${erdHoveredTable === tableName ? " erd-card-focused" : ""}${isLinked ? " erd-card-linked" : ""}${erdAnimating ? " erd-card-anim" : ""}`}
                  style={{ position: "absolute", left: pos.x, top: pos.y, width: ERD_CARD_W, zIndex: erdHoveredTable === tableName ? 10 : 1 }}
                  onClick={() => { if (!erdCardWasDragged.current) handleErdCardClick(tableName); }}
                  onMouseEnter={() => setErdHoveredTable(tableName)}
                  onMouseLeave={() => setErdHoveredTable(null)}
                >
                  <div
                    className="erd-card-header"
                    onMouseDown={e => {
                      e.stopPropagation();
                      e.preventDefault();
                      erdCardWasDragged.current = false;
                      erdCardDragRef.current = {
                        table: tableName,
                        startMX: e.clientX,
                        startMY: e.clientY,
                        startCX: pos.x,
                        startCY: pos.y,
                        zoom: erdZoom,
                      };
                      document.body.style.userSelect = "none";
                    }}
                  >
                    <span className="erd-card-name">{tableName}</span>
                    <span className="erd-card-comment">comment</span>
                  </div>
                  {cols.map(col => (
                    <div
                      key={col.name}
                      className={`erd-col-row${col.is_pk ? " erd-pk" : col.fk_ref ? " erd-fk" : ""}`}
                    >
                      <span className="erd-col-key">
                        {(col.is_pk || col.fk_ref) && (
                          <svg width="12" height="12" viewBox="0 0 24 24" fill={col.is_pk ? "#f0c040" : "#e0556b"}>
                            <path d="M12.65 10A5.99 5.99 0 0 0 7 6c-3.31 0-6 2.69-6 6s2.69 6 6 6a5.99 5.99 0 0 0 5.65-4H17v4h4v-4h2v-4H12.65zM7 14c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2z"/>
                          </svg>
                        )}
                      </span>
                      <span className="erd-col-name" style={{ width: maxNameW }}>{col.name}</span>
                      <span className="erd-col-type" title={col.data_type}>{col.data_type}</span>
                      <span className={`erd-badge${col.is_not_null ? " on-nn" : ""}`}>{col.is_not_null ? "N-N" : "NULL"}</span>
                      <span className={`erd-badge${col.is_unique ? " on-uq" : ""}`}>UQ</span>
                      <span className={`erd-badge${col.is_auto_inc ? " on-ai" : ""}`}>AI</span>
                    </div>
                  ))}
                </div>
              );
            })}
          </div>
        )}
      </div>

      {/* 데이터 패널 */}
      {erdSelectedTable && (
        <>
          <div
            className="divider"
            onMouseDown={() => {
              erdDataDragging.current = true;
              document.body.style.cursor = "row-resize";
              document.body.style.userSelect = "none";
            }}
          />
          <div className="erd-data-panel" style={{ height: erdDataHeight }}>
            <div className="erd-data-header">
              <span className="erd-data-table-name">⊞ {erdSelectedTable}</span>
              <input
                className="erd-data-filter"
                placeholder="Filter rows..."
                value={erdFilter}
                onChange={e => setErdFilter(e.target.value)}
              />
              <button className="erd-tool-btn" onClick={() => loadErdTableData(erdSelectedTable)} title="Refresh">↻</button>
              <button className="erd-tool-btn" onClick={() => { setErdSelectedTable(""); setErdTableData(null); setErdDataHeight(0); }} title="Close">✕</button>
            </div>
            <div className="erd-data-body">
              {erdTableLoading ? (
                <div className="erd-data-empty">Loading...</div>
              ) : !erdTableData || !erdTableData.success ? (
                <div className="erd-data-error">{erdTableData?.message ?? "Unknown error"}</div>
              ) : erdTableData.columns.length === 0 ? (
                <div className="erd-data-empty">{erdTableData.message || "No rows"}</div>
              ) : (() => {
                const low = erdFilter.toLowerCase();
                const filtered = erdFilter
                  ? erdTableData.rows.filter(r => r.some(c => c.toLowerCase().includes(low)))
                  : erdTableData.rows;
                // 컬럼 자동 너비 — 쿼리 결과 표와 동일 로직 (canvas measureText, 한글/CJK 지원)
                const CELL_PAD = 36;
                const sample = filtered.slice(0, 200);
                const widths = erdTableData.columns.map((col, ci) => {
                  const maxData = sample.reduce((m, row) => Math.max(m, measureTextPx(row[ci] ?? "")), 0);
                  const w = Math.max(measureTextPx(col), maxData);
                  return Math.min(500, Math.max(60, Math.round(w + CELL_PAD)));
                });
                return (
                  <>
                    <div className="erd-data-meta">
                      {filtered.length}{erdFilter ? ` / ${erdTableData.rows.length}` : ""} row(s) · {erdTableData.columns.length} col(s) · {erdTableData.elapsed.toFixed(3)}s
                    </div>
                    <table className="erd-data-table" style={{ tableLayout: "fixed", width: "auto" }}>
                      <thead><tr>
                        <th className="erd-data-rownum" style={{ width: 40 }}>#</th>
                        {erdTableData.columns.map((c, ci) => <th key={c} style={{ width: widths[ci] }}>{c}</th>)}
                      </tr></thead>
                      <tbody>
                        {filtered.map((row, ri) => (
                          <tr key={ri}>
                            <td className="erd-data-rownum">{ri + 1}</td>
                            {row.map((cell, ci) => (
                              <td key={ci}>{cell || <span className="erd-data-null">NULL</span>}</td>
                            ))}
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </>
                );
              })()}
            </div>
          </div>
        </>
      )}

      <div className="status-bar">
        <div className="status-left">
          <span className="status-item">⎇ main</span>
          <span className="status-item" style={{ color: "#9cdcfe" }}>⬡ {currentDb}</span>
          {erdSelectedTable && <span className="status-item" style={{ color: "#4ec9b0" }}>⊞ {erdSelectedTable}</span>}
          <span className="status-item" style={{ color: "#555" }}>
            {Object.keys(erdColumns).length} tables · {Object.values(erdColumns).flat().filter(c => c.fk_ref).length} relations
          </span>
        </div>
        <div className="status-right">
          <span className="status-item">RuSQL v2.3.0</span>
          <span className="status-item">ERD Editor</span>
        </div>
      </div>
    </div>
  );
}
