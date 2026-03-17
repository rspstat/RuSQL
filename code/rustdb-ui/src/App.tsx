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
      setResults([{
        columns: [],
        rows: [],
        message: String(e),
        elapsed: 0,
        success: false,
      }]);
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
      {/* 사이드바 */}
      <div className="sidebar">
        <div className="sidebar-logo">🗄 RustDB</div>
        <div className="sidebar-section">TABLES</div>
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
                {t}
              </div>
              {expandedTable === t && tableColumns[t] && (
                <div className="sidebar-columns">
                  {tableColumns[t].map((col) => (
                    <div key={col} className="sidebar-column">
                      <span className="col-icon">⬡</span>
                      <span>{col}</span>
                    </div>
                  ))}
                </div>
              )}
            </div>
          ))
        )}
        <div className="sidebar-bottom">
          <div className="sidebar-section">INFO</div>
          <div className="sidebar-info">v0.1.0</div>
          <div className="sidebar-info">B+Tree · WAL</div>
        </div>
      </div>

      {/* 메인 */}
      <div className="main">
        <div className="tab-bar">
          <div className="tab active">query.sql</div>
          <div className="tab-actions">
            <button className="run-btn" onClick={runQuery} disabled={isRunning}>
              {isRunning ? "⏳" : "▶ Run"}
            </button>
          </div>
        </div>

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

        <div className="result-panel">
          <div className="result-tab-bar">
            <div className="result-tab active">Results</div>
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
              if (ctxMenu) {
                e.stopPropagation();
                setCtxMenu(null);
              }
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
                          <tr>
                            {result.columns.map((col) => (
                              <th key={col}>{col}</th>
                            ))}
                          </tr>
                        </thead>
                        <tbody>
                          {result.rows.map((row, i) => (
                            <tr key={i}>
                              {row.map((cell, j) => (
                                <td key={j}>{cell}</td>
                              ))}
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </>
                  )}
                </div>
              ))
            )}

            {/* 컨텍스트 메뉴 */}
            {ctxMenu && (
              <div
                className="ctx-menu"
                style={{ top: ctxMenu.y, left: ctxMenu.x }}
              >
                <div onClick={() => {
                  const text = results.map(r =>
                    r.columns.length > 0
                      ? [r.columns.join('\t'), ...r.rows.map(row => row.join('\t'))].join('\n')
                      : r.message
                  ).join('\n\n');
                  navigator.clipboard.writeText(text);
                  setCtxMenu(null);
                }}>
                  📋 Copy All Results
                </div>
                <div className="ctx-divider" />
                <div onClick={() => { setResults([]); setCtxMenu(null); }}>
                  🗑 Clear Results
                </div>
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

export default App;