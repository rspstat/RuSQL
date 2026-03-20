// App.tsx
import { useState, useEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import Editor from "@monaco-editor/react";
import type * as Monaco from "monaco-editor";
import "./App.css";

interface QueryResult {
  columns: string[];
  rows: string[][];
  message: string;
  elapsed: number;
  success: boolean;
}

interface MultiQueryResult {
  results: QueryResult[];
  total_elapsed: number;
}

function App() {
  const [query, setQuery] = useState(() => {
    return localStorage.getItem("rustdb_query") ?? "SELECT * FROM users;";
  });
  const [results, setResults] = useState<QueryResult[]>([]);
  const [tables, setTables] = useState<string[]>([]);
  const [expandedTable, setExpandedTable] = useState<string | null>(null);
  const [tableColumns, setTableColumns] = useState<Record<string, string[]>>({});
  const [ctxMenu, setCtxMenu] = useState<{ x: number; y: number } | null>(null);
  const [isRunning, setIsRunning] = useState(false);
  const editorRef = useRef<Monaco.editor.IStandaloneCodeEditor | null>(null);

  useEffect(() => {
    invoke<string[]>("get_tables").then(setTables);
  }, []);

  useEffect(() => {
    const handleClick = () => setCtxMenu(null);
    window.addEventListener("click", handleClick);
    return () => window.removeEventListener("click", handleClick);
  }, []);

  const runQuery = async () => {
    const selectedText = editorRef.current?.getSelection()
      ? editorRef.current?.getModel()?.getValueInRange(editorRef.current.getSelection()!)
      : null;
    const queryToRun = (selectedText?.trim() ? selectedText : query).trim();
    if (!queryToRun) return;
    setResults([]);
    setIsRunning(true);
    try {
      const res = await invoke<MultiQueryResult>("execute_query", {
        query: queryToRun,
        ts: Date.now(),
      });
      setResults(res.results);
      const tableList = await invoke<string[]>("get_tables");
      setTables(tableList);
    } catch (e) {
      setResults([{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }]);
    } finally {
      setIsRunning(false);
    }
  };

  const toggleTable = async (t: string) => {
    if (expandedTable === t) {
      setExpandedTable(null);
    } else {
      setExpandedTable(t);
      if (!tableColumns[t]) {
        const cols = await invoke<string[]>("get_columns", { table: t });
        setTableColumns((prev) => ({ ...prev, [t]: cols }));
      }
      setQuery(`SELECT * FROM ${t};`);
    }
  };

  return (
    <div className="app">

      {/* 좌측 액티비티 바 */}
      <div className="activity-bar">
        <div className="activity-icon active" title="Explorer">
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M3 3h8v2H5v14h14v-6h2v8H3V3zm11 0h7v7h-2V6.414l-9.293 9.293-1.414-1.414L17.586 5H14V3z"/>
          </svg>
        </div>
        <div className="activity-icon" title="Search">
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M15.5 14h-.79l-.28-.27A6.471 6.471 0 0 0 16 9.5 6.5 6.5 0 1 0 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z"/>
          </svg>
        </div>
        <div className="activity-icon" title="Source Control">
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M7 3a2 2 0 1 0 0 4 2 2 0 0 0 0-4zm0 10a2 2 0 1 0 0 4 2 2 0 0 0 0-4zm10-10a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM7 5a1 1 0 1 1 0 2A1 1 0 0 1 7 5zm0 10a1 1 0 1 1 0 2 1 1 0 0 1 0-2zm10-10a1 1 0 1 1 0 2 1 1 0 0 1 0-2zM8 7.5v9a1 1 0 0 0 2 0v-9a1 1 0 0 0-2 0zm7-1.5a1 1 0 0 0-1 1v.268l-4 2.25V7a1 1 0 0 0-2 0v10a1 1 0 0 0 2 0v-2.018l4 2.25V17a1 1 0 0 0 2 0V6a1 1 0 0 0-1-1z"/>
          </svg>
        </div>
        <div className="activity-icon" title="Database">
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M12 2C8.13 2 5 3.34 5 5v14c0 1.66 3.13 3 7 3s7-1.34 7-3V5c0-1.66-3.13-3-7-3zm0 2c3.31 0 5 1.12 5 1.5S15.31 7 12 7 7 5.88 7 5.5 8.69 4 12 4zm5 14.5c0 .38-1.69 1.5-5 1.5s-5-1.12-5-1.5V16.73c1.34.84 3.04 1.27 5 1.27s3.66-.43 5-1.27v1.77zm0-4c0 .38-1.69 1.5-5 1.5s-5-1.12-5-1.5V12.73c1.34.84 3.04 1.27 5 1.27s3.66-.43 5-1.27v1.77zm0-4c0 .38-1.69 1.5-5 1.5s-5-1.12-5-1.5V8.73c1.34.84 3.04 1.27 5 1.27s3.66-.43 5-1.27V10.5z"/>
          </svg>
        </div>
        <div className="activity-bar-bottom">
          <div className="activity-icon" title="Settings">
            <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
              <path d="M19.43 12.98c.04-.32.07-.64.07-.98s-.03-.66-.07-.98l2.11-1.65c.19-.15.24-.42.12-.64l-2-3.46c-.12-.22-.39-.3-.61-.22l-2.49 1c-.52-.4-1.08-.73-1.69-.98l-.38-2.65C14.46 2.18 14.25 2 14 2h-4c-.25 0-.46.18-.49.42l-.38 2.65c-.61.25-1.17.59-1.69.98l-2.49-1c-.23-.09-.49 0-.61.22l-2 3.46c-.13.22-.07.49.12.64l2.11 1.65c-.04.32-.07.65-.07.98s.03.66.07.98l-2.11 1.65c-.19.15-.24.42-.12.64l2 3.46c.12.22.39.3.61.22l2.49-1c.52.4 1.08.73 1.69.98l.38 2.65c.03.24.24.42.49.42h4c.25 0 .46-.18.49-.42l.38-2.65c.61-.25 1.17-.59 1.69-.98l2.49 1c.23.09.49 0 .61-.22l2-3.46c.12-.22.07-.49-.12-.64l-2.11-1.65zM12 15.5c-1.93 0-3.5-1.57-3.5-3.5s1.57-3.5 3.5-3.5 3.5 1.57 3.5 3.5-1.57 3.5-3.5 3.5z"/>
            </svg>
          </div>
        </div>
      </div>

      {/* 사이드바 */}
      <div className="sidebar">
        <div className="sidebar-title">EXPLORER</div>
        <div className="sidebar-group">
          <div className="sidebar-group-header">
            <span className="sidebar-group-arrow">▼</span>
            TABLES
          </div>
          {tables.length === 0 ? (
            <div className="sidebar-empty">No tables yet</div>
          ) : (
            tables.map((t) => (
              <div key={t}>
                <div
                  className={`sidebar-item ${expandedTable === t ? "active" : ""}`}
                  onClick={() => toggleTable(t)}
                >
                  <span className="sidebar-arrow">
                    {expandedTable === t ? "▼" : "▶"}
                  </span>
                  <span className="sidebar-table-icon">⊞</span>
                  <span className="sidebar-name">{t}</span>
                </div>
                {expandedTable === t && tableColumns[t] && (
                  <div className="sidebar-columns">
                    {tableColumns[t].map((col) => (
                      <div key={col} className="sidebar-column">
                        <span className="col-icon">≡</span>
                        <span>{col}</span>
                      </div>
                    ))}
                  </div>
                )}
              </div>
            ))
          )}
        </div>
        <div className="sidebar-bottom">
          <div className="sidebar-group-header">
            <span className="sidebar-group-arrow">▼</span>
            INFO
          </div>
          <div className="sidebar-info-item">
            <span className="col-icon">◉</span> v0.1.0
          </div>
          <div className="sidebar-info-item">
            <span className="col-icon">◉</span> B+Tree · WAL
          </div>
        </div>
      </div>

      {/* 메인 */}
      <div className="main">

        {/* 탭 바 */}
        <div className="tab-bar">
          <div className="tab active">
            <span className="tab-icon">⊞</span>
            query.sql
            <span className="tab-close">×</span>
          </div>
          <div className="tab-bar-right">
            <button className="run-btn" onClick={runQuery} disabled={isRunning}>
              {isRunning ? "⏳" : "▶ Run"}
            </button>
          </div>
        </div>

        {/* 빵부스러기 */}
        <div className="breadcrumb">
          <span>rustdb</span>
          <span className="breadcrumb-sep">›</span>
          <span>query</span>
          <span className="breadcrumb-sep">›</span>
          <span className="breadcrumb-active">query.sql</span>
        </div>

        {/* 에디터 */}
        <div className="editor-container">
          <Editor
            height="100%"
            defaultLanguage="sql"
            value={query}
            onChange={(val) => {
              const v = val ?? "";
              setQuery(v);
              localStorage.setItem("rustdb_query", v);
            }}
            theme="vs-dark"
            options={{
              fontSize: 14,
              fontFamily: "Consolas, 'Courier New', monospace",
              minimap: { enabled: true },
              scrollBeyondLastLine: false,
              lineNumbers: "on",
              renderLineHighlight: "all",
              automaticLayout: true,
              padding: { top: 12 },
            }}
            onMount={(editor, monaco) => {
              editorRef.current = editor;
              editor.addCommand(
                monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter,
                () => runQuery()
              );
            }}
          />
        </div>

        <div className="divider" />

        {/* 결과 패널 */}
        <div className="result-panel">
          <div className="result-tab-bar">
            <div className="result-tab active">RESULTS</div>
            <div className="result-tab">MESSAGES</div>
            {results.length > 0 && (
              <div className={`result-status ${results.every(r => r.success) ? "ok" : "err"}`}>
                {results.every(r => r.success) ? `✓ ${results.length} query(s)` : `✗ Error`}
              </div>
            )}
          </div>

          <div
            className="result-content"
            onContextMenu={(e) => {
              e.preventDefault();
              setCtxMenu({ x: e.clientX, y: e.clientY });
            }}
            onClick={(e) => {
              if (ctxMenu) { e.stopPropagation(); setCtxMenu(null); }
            }}
          >
            {results.length === 0 ? (
              <div className="result-empty">
                Ctrl+Enter 또는 ▶ Run 으로 쿼리를 실행하세요
              </div>
            ) : (
              results.map((result, idx) => (
                <div key={idx} className="result-block">
                  {!result.success ? (
                    <div className="result-error">❌ {result.message}</div>
                  ) : result.columns.length === 0 ? (
                    <div className="result-msg">✅ {result.message} · {result.elapsed.toFixed(3)}s</div>
                  ) : (
                    <>
                      <div className="result-info">
                        {result.rows.length} row(s) · {result.elapsed.toFixed(3)}s
                      </div>
                      <table className="result-table">
                        <thead>
                          <tr>{result.columns.map((col) => <th key={col}>{col}</th>)}</tr>
                        </thead>
                        <tbody>
                          {result.rows.map((row, i) => (
                            <tr key={i}>{row.map((cell, j) => <td key={j}>{cell}</td>)}</tr>
                          ))}
                        </tbody>
                      </table>
                    </>
                  )}
                </div>
              ))
            )}

            {ctxMenu && (
              <div className="ctx-menu" style={{ top: ctxMenu.y, left: ctxMenu.x }}>
                <div onClick={() => {
                  const text = results.map(r =>
                    r.columns.length > 0
                      ? [r.columns.join('\t'), ...r.rows.map(row => row.join('\t'))].join('\n')
                      : r.message
                  ).join('\n\n');
                  navigator.clipboard.writeText(text);
                  setCtxMenu(null);
                }}>📋 Copy All Results</div>
                <div className="ctx-divider" />
                <div onClick={() => { setResults([]); setCtxMenu(null); }}>🗑 Clear Results</div>
              </div>
            )}
          </div>
        </div>

        {/* 하단 상태바 */}
        <div className="status-bar">
          <div className="status-left">
            <span className="status-item">⎇ main</span>
            <span className="status-item">✓ 0 △ 0</span>
          </div>
          <div className="status-right">
            <span className="status-item">RustDB v0.1.0</span>
            <span className="status-item">UTF-8</span>
            <span className="status-item">SQL</span>
            <span className="status-item">B+Tree · WAL</span>
          </div>
        </div>
      </div>
    </div>
  );
}

export default App;