import { useState, useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";
import Editor from "@monaco-editor/react";
import "./App.css";

interface QueryResult {
  columns: string[];
  rows: string[][];
  message: string;
  elapsed: number;
  success: boolean;
}

function App() {
  const [query, setQuery] = useState("SELECT * FROM users;");
  const [result, setResult] = useState<QueryResult | null>(null);
  const [tables, setTables] = useState<string[]>([]);

  useEffect(() => {
    invoke<string[]>("get_tables").then(setTables);
  }, []);

  const runQuery = async () => {
    if (!query.trim()) return;
    try {
      const res = await invoke<QueryResult>("execute_query", { query });
      setResult(res);
      // 테이블 목록 갱신
      const tableList = await invoke<string[]>("get_tables");
      setTables(tableList);
    } catch (e) {
      setResult({
        columns: [],
        rows: [],
        message: String(e),
        elapsed: 0,
        success: false,
      });
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
            <div key={t} className="sidebar-item"
              onClick={() => setQuery(`SELECT * FROM ${t};`)}>
              ▸ {t}
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
        {/* 탭 바 */}
        <div className="tab-bar">
          <div className="tab active">query.sql</div>
          <div className="tab-actions">
            <button className="run-btn" onClick={runQuery}>▶ Run</button>
          </div>
        </div>

        {/* Monaco 에디터 */}
        <div className="editor-container">
          <Editor
            height="100%"
            defaultLanguage="sql"
            value={query}
            onChange={(val) => setQuery(val ?? "")}
            theme="vs-dark"
            options={{
              fontSize: 14,
              fontFamily: "Consolas, 'Courier New', monospace",
              minimap: { enabled: true },
              scrollBeyondLastLine: false,
              lineNumbers: "on",
              renderLineHighlight: "all",
              cursorStyle: "line",
              automaticLayout: true,
              padding: { top: 12 },
              suggest: { showKeywords: true },
            }}
            onMount={(editor, monaco) => {
              // Ctrl+Enter로 실행
              editor.addCommand(
                monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter,
                () => runQuery()
              );
            }}
          />
        </div>

        {/* 구분선 */}
        <div className="divider" />

        {/* 결과 패널 */}
        <div className="result-panel">
          <div className="result-tab-bar">
            <div className="result-tab active">Results</div>
            {result && (
              <div className="result-status">
                {result.success
                  ? result.columns.length > 0
                    ? `✓ ${result.rows.length} row(s) · ${result.elapsed.toFixed(3)}s`
                    : `✓ ${result.message} · ${result.elapsed.toFixed(3)}s`
                  : `✗ Error`}
              </div>
            )}
          </div>

          <div className="result-content">
            {result === null ? (
              <div className="result-empty">
                Ctrl+Enter 또는 ▶ Run 버튼으로 쿼리를 실행하세요
              </div>
            ) : !result.success ? (
              <div className="result-error">❌ {result.message}</div>
            ) : result.columns.length === 0 ? (
              <div className="result-msg">✅ {result.message}</div>
            ) : (
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
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

export default App;