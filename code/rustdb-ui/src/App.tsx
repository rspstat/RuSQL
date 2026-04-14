// App.tsx
import { useState, useEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import Editor from "@monaco-editor/react";
import type * as Monaco from "monaco-editor";
import "./App.css";

// ─── 타입 ─────────────────────────────────────────────────────
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
interface ServerStatus {
  running: boolean;
  port: number;
  client_count: number;
  log: string[];
}
interface IndexInfo {
  name: string;
  table: string;
  columns: string[];
  kind: "single" | "composite";
}
type ActiveView = "editor" | "gui" | "server";

// ─── 메인 컴포넌트 ────────────────────────────────────────────
function App() {
  // 에디터 상태
  const [query, setQuery] = useState(() => localStorage.getItem("rustdb_query") ?? "SHOW TABLES;");
  const [results, setResults] = useState<QueryResult[]>([]);
  const [tables, setTables] = useState<string[]>([]);
  const [views, setViews] = useState<string[]>([]);
  const [indexes, setIndexes] = useState<IndexInfo[]>([]);
  const [expandedTables, setExpandedTables] = useState<Set<string>>(new Set());
  const [tableColumns, setTableColumns] = useState<Record<string, string[]>>({});
  const [expandedViews, setExpandedViews] = useState<Set<string>>(new Set());
  const [viewColumns, setViewColumns] = useState<Record<string, string[]>>({});
  const [expandedIndexes, setExpandedIndexes] = useState<Set<string>>(new Set());
  const [tablesOpen, setTablesOpen] = useState(true);
  const [viewsOpen, setViewsOpen] = useState(true);
  const [indexesOpen, setIndexesOpen] = useState(true);
  const [ctxMenu, setCtxMenu] = useState<{ x: number; y: number } | null>(null);
  const [tableCtxMenu, setTableCtxMenu] = useState<{ x: number; y: number; table: string } | null>(null);
  const [isRunning, setIsRunning] = useState(false);
  const editorRef = useRef<Monaco.editor.IStandaloneCodeEditor | null>(null);
  const [resultHeight, setResultHeight] = useState(260);
  const [sidebarWidth, setSidebarWidth] = useState(240);
  const isDragging = useRef(false);
  const isSidebarDragging = useRef(false);

  // 뷰 전환
  const [activeView, setActiveView] = useState<ActiveView>("editor");

  // GUI 브라우저 상태
  const [guiTable, setGuiTable] = useState<string>("");
  const [guiResult, setGuiResult] = useState<QueryResult | null>(null);
  const [guiLoading, setGuiLoading] = useState(false);
  const [guiFilter, setGuiFilter] = useState("");

  // 서버 상태
  const [serverStatus, setServerStatus] = useState<ServerStatus>({
    running: false, port: 7878, client_count: 0, log: [],
  });
  const [portInput, setPortInput] = useState("7878");
  const [serverMsg, setServerMsg] = useState("");
  const logEndRef = useRef<HTMLDivElement>(null);

  // ─── 사이드바 데이터 갱신 ────────────────────────────────────
  const refreshSidebar = async () => {
    const [tbls, vws, idxs] = await Promise.all([
      invoke<string[]>("get_tables"),
      invoke<string[]>("get_views"),
      invoke<IndexInfo[]>("get_indexes"),
    ]);
    setTables(tbls);
    setViews(vws);
    setIndexes(idxs);
  };

  // ─── 초기 로드 ──────────────────────────────────────────────
  useEffect(() => { refreshSidebar(); }, []);

  // ─── 컨텍스트 메뉴 닫기 ──────────────────────────────────────
  useEffect(() => {
    const h = () => { setCtxMenu(null); setTableCtxMenu(null); };
    window.addEventListener("click", h);
    return () => window.removeEventListener("click", h);
  }, []);

  // ─── 결과창 드래그 ──────────────────────────────────────────
  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      if (isDragging.current) {
        const el = document.querySelector(".main") as HTMLElement;
        if (!el) return;
        const rect = el.getBoundingClientRect();
        setResultHeight(Math.max(100, Math.min(rect.bottom - e.clientY, rect.height - 100)));
      }
      if (isSidebarDragging.current) {
        const app = document.querySelector(".app") as HTMLElement;
        if (!app) return;
        const rect = app.getBoundingClientRect();
        const newW = e.clientX - rect.left - 48; // 48 = activity bar width
        setSidebarWidth(Math.max(160, Math.min(newW, 480)));
      }
    };
    const onUp = () => {
      isDragging.current = false;
      isSidebarDragging.current = false;
      document.body.style.cursor = "";
      document.body.style.userSelect = "";
    };
    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onUp);
    return () => { window.removeEventListener("mousemove", onMove); window.removeEventListener("mouseup", onUp); };
  }, []);

  // ─── 서버 상태 폴링 (서버 뷰 활성 시) ──────────────────────
  useEffect(() => {
    if (activeView !== "server") return;
    const poll = async () => {
      const s = await invoke<ServerStatus>("get_server_status");
      setServerStatus(s);
    };
    poll();
    const timer = setInterval(poll, 1500);
    return () => clearInterval(timer);
  }, [activeView]);

  // ─── 로그 자동 스크롤 ─────────────────────────────────────
  useEffect(() => {
    logEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [serverStatus.log]);

  // ─── 쿼리 실행 ──────────────────────────────────────────────
  const runQuery = async () => {
    const sel = editorRef.current?.getSelection()
      ? editorRef.current?.getModel()?.getValueInRange(editorRef.current.getSelection()!)
      : null;
    const q = (sel?.trim() ? sel : query).trim();
    if (!q) return;
    setResults([]);
    setIsRunning(true);
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: q, ts: Date.now() });
      setResults(res.results);
      await refreshSidebar();
    } catch (e) {
      setResults([{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }]);
    } finally {
      setIsRunning(false);
    }
  };

  const toggleTable = async (t: string) => {
    if (expandedTables.has(t)) {
      setExpandedTables(prev => { const s = new Set(prev); s.delete(t); return s; });
      return;
    }
    setExpandedTables(prev => new Set(prev).add(t));
    if (!tableColumns[t]) {
      const cols = await invoke<string[]>("get_columns", { table: t });
      setTableColumns(p => ({ ...p, [t]: cols }));
    }
    setQuery(`SELECT * FROM ${t};`);
  };

  const toggleView = async (v: string) => {
    if (expandedViews.has(v)) {
      setExpandedViews(prev => { const s = new Set(prev); s.delete(v); return s; });
      return;
    }
    setExpandedViews(prev => new Set(prev).add(v));
    if (!viewColumns[v]) {
      const cols = await invoke<string[]>("get_columns", { table: v });
      setViewColumns(p => ({ ...p, [v]: cols }));
    }
    setQuery(`SELECT * FROM ${v};`);
  };

  const toggleIndex = (name: string) => {
    setExpandedIndexes(prev => {
      const s = new Set(prev);
      s.has(name) ? s.delete(name) : s.add(name);
      return s;
    });
  };

  // ─── 테이블 우클릭 메뉴 핸들러 ────────────────────────────────
  const runCtxQuery = async (q: string, dropTable?: string) => {
    setTableCtxMenu(null);
    setQuery(q);
    setResults([]);
    setIsRunning(true);
    setActiveView("editor");
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: q, ts: Date.now() });
      setResults(res.results);
      await refreshSidebar();
      if (dropTable) setExpandedTables(prev => { const s = new Set(prev); s.delete(dropTable); return s; });
    } catch (e) {
      setResults([{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }]);
    } finally {
      setIsRunning(false);
    }
  };

  const handleCopyTableName = (t: string) => {
    navigator.clipboard.writeText(t);
    setTableCtxMenu(null);
  };

  // ─── GUI 브라우저 ───────────────────────────────────────────
  const loadGuiTable = async (t: string) => {
    if (!t) { setGuiResult(null); return; }
    setGuiLoading(true);
    setGuiFilter("");
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: `SELECT * FROM ${t};`, ts: Date.now() });
      setGuiResult(res.results[0] ?? null);
    } catch {
      setGuiResult({ columns: [], rows: [], message: "Error loading table", elapsed: 0, success: false });
    } finally {
      setGuiLoading(false);
    }
  };

  const handleGuiTableChange = (t: string) => {
    setGuiTable(t);
    loadGuiTable(t);
  };

  // ─── 서버 제어 ──────────────────────────────────────────────
  const handleStartServer = async () => {
    const port = parseInt(portInput) || 7878;
    try {
      const msg = await invoke<string>("start_server", { port });
      setServerMsg(msg);
    } catch (e) { setServerMsg(String(e)); }
  };

  const handleStopServer = async () => {
    try {
      const msg = await invoke<string>("stop_server");
      setServerMsg(msg);
    } catch (e) { setServerMsg(String(e)); }
  };

  const handleClearLog = async () => {
    await invoke("clear_server_log");
    setServerStatus(s => ({ ...s, log: [] }));
  };

  // ─── 렌더 ───────────────────────────────────────────────────
  return (
    <div className="app">

      {/* 액티비티 바 */}
      <div className="activity-bar">
        {/* Explorer */}
        <div
          className={`activity-icon ${activeView === "editor" ? "active" : ""}`}
          title="SQL Editor"
          onClick={() => setActiveView("editor")}
        >
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M3 3h8v2H5v14h14v-6h2v8H3V3zm11 0h7v7h-2V6.414l-9.293 9.293-1.414-1.414L17.586 5H14V3z"/>
          </svg>
        </div>

        {/* GUI Browser */}
        <div
          className={`activity-icon ${activeView === "gui" ? "active" : ""}`}
          title="Table Browser"
          onClick={() => setActiveView("gui")}
        >
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M3 3h18v2H3V3zm0 4h18v2H3V7zm0 4h18v2H3v-2zm0 4h18v2H3v-2zm0 4h18v2H3v-2z"/>
          </svg>
        </div>

        {/* Server Manager */}
        <div
          className={`activity-icon ${activeView === "server" ? "active" : ""}`}
          title="Server Manager"
          onClick={() => setActiveView("server")}
          style={{ position: "relative" }}
        >
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M4 1h16a1 1 0 0 1 1 1v4a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1zm0 8h16a1 1 0 0 1 1 1v4a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-4a1 1 0 0 1 1-1zm0 8h16a1 1 0 0 1 1 1v4a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-4a1 1 0 0 1 1-1zM6 4a1 1 0 1 0 0 2 1 1 0 0 0 0-2zm0 8a1 1 0 1 0 0 2 1 1 0 0 0 0-2zm0 8a1 1 0 1 0 0 2 1 1 0 0 0 0-2z"/>
          </svg>
          {/* 서버 실행 중 표시 dot */}
          {serverStatus.running && (
            <span style={{
              position: "absolute", top: 8, right: 8,
              width: 8, height: 8, borderRadius: "50%",
              background: "#4ec9b0", boxShadow: "0 0 4px #4ec9b0",
            }} />
          )}
        </div>

        <div className="activity-bar-bottom">
          <div className="activity-icon" title="Settings">
            <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
              <path d="M19.43 12.98c.04-.32.07-.64.07-.98s-.03-.66-.07-.98l2.11-1.65c.19-.15.24-.42.12-.64l-2-3.46c-.12-.22-.39-.3-.61-.22l-2.49 1c-.52-.4-1.08-.73-1.69-.98l-.38-2.65C14.46 2.18 14.25 2 14 2h-4c-.25 0-.46.18-.49.42l-.38 2.65c-.61.25-1.17.59-1.69.98l-2.49-1c-.23-.09-.49 0-.61.22l-2 3.46c-.13.22-.07.49.12.64l2.11 1.65c-.04.32-.07.65-.07.98s.03.66.07.98l-2.11 1.65c-.19.15-.24.42-.12.64l2 3.46c.12.22.39.3.61.22l2.49-1c.52.4 1.08.73 1.69.98l.38 2.65c.03.24.24.42.49.42h4c.25 0 .46-.18.49-.42l.38-2.65c.61-.25 1.17-.59 1.69-.98l2.49 1c.23.09.49 0 .61-.22l2-3.46c.12-.22.07-.49-.12-.64l-2.11-1.65zM12 15.5c-1.93 0-3.5-1.57-3.5-3.5s1.57-3.5 3.5-3.5 3.5 1.57 3.5 3.5-1.57 3.5-3.5 3.5z"/>
            </svg>
          </div>
        </div>
      </div>

      {/* ── 에디터 뷰 ──────────────────────────────────────────── */}
      {activeView === "editor" && (
        <>
          <div className="sidebar" style={{ width: `${sidebarWidth}px` }}>
            <div className="sidebar-title">EXPLORER</div>

            {/* ── TABLES ── */}
            <div className="sidebar-group">
              <div className="sidebar-group-header" onClick={() => setTablesOpen(o => !o)}>
                <span className="sidebar-group-arrow">{tablesOpen ? "▼" : "▶"}</span>
                TABLES
                <span className="sidebar-badge">{tables.length}</span>
              </div>
              {tablesOpen && (tables.length === 0 ? (
                <div className="sidebar-empty">No tables yet</div>
              ) : tables.map(t => (
                <div key={t}>
                  <div
                    className={`sidebar-item ${expandedTables.has(t) ? "active" : ""}`}
                    onClick={() => toggleTable(t)}
                    onContextMenu={e => {
                      e.preventDefault();
                      e.stopPropagation();
                      setTableCtxMenu({ x: e.clientX, y: e.clientY, table: t });
                    }}
                  >
                    <span className="sidebar-arrow">{expandedTables.has(t) ? "▼" : "▶"}</span>
                    <span className="sidebar-table-icon">⊞</span>
                    <span className="sidebar-name">{t}</span>
                  </div>
                  {expandedTables.has(t) && tableColumns[t] && (
                    <div className="sidebar-columns">
                      {tableColumns[t].map(col => (
                        <div key={col} className="sidebar-column">
                          <span className="col-icon">≡</span>
                          <span>{col}</span>
                        </div>
                      ))}
                    </div>
                  )}
                </div>
              )))}
            </div>

            {/* ── VIEWS ── */}
            <div className="sidebar-group">
              <div className="sidebar-group-header" onClick={() => setViewsOpen(o => !o)}>
                <span className="sidebar-group-arrow">{viewsOpen ? "▼" : "▶"}</span>
                VIEWS
                <span className="sidebar-badge">{views.length}</span>
              </div>
              {viewsOpen && (views.length === 0 ? (
                <div className="sidebar-empty">No views yet</div>
              ) : views.map(v => (
                <div key={v}>
                  <div
                    className={`sidebar-item ${expandedViews.has(v) ? "active" : ""}`}
                    onClick={() => toggleView(v)}
                  >
                    <span className="sidebar-arrow">{expandedViews.has(v) ? "▼" : "▶"}</span>
                    <span className="sidebar-view-icon">◈</span>
                    <span className="sidebar-name">{v}</span>
                  </div>
                  {expandedViews.has(v) && (
                    <div className="sidebar-columns">
                      {viewColumns[v] && viewColumns[v].length > 0
                        ? viewColumns[v].map(col => (
                            <div key={col} className="sidebar-column">
                              <span className="col-icon">◉</span>
                              <span>{col}</span>
                            </div>
                          ))
                        : <div className="sidebar-column" style={{ color: "var(--text-muted)" }}>no column info</div>
                      }
                    </div>
                  )}
                </div>
              )))}
            </div>

            {/* ── INDEXES ── */}
            <div className="sidebar-group">
              <div className="sidebar-group-header" onClick={() => setIndexesOpen(o => !o)}>
                <span className="sidebar-group-arrow">{indexesOpen ? "▼" : "▶"}</span>
                INDEXES
                <span className="sidebar-badge">{indexes.length}</span>
              </div>
              {indexesOpen && (indexes.length === 0 ? (
                <div className="sidebar-empty">No indexes yet</div>
              ) : indexes.map(idx => (
                <div key={idx.name}>
                  <div
                    className={`sidebar-item sidebar-index-item ${expandedIndexes.has(idx.name) ? "active" : ""}`}
                    onClick={() => toggleIndex(idx.name)}
                  >
                    <span className="sidebar-arrow">{expandedIndexes.has(idx.name) ? "▼" : "▶"}</span>
                    <span className="sidebar-index-icon">{idx.kind === "composite" ? "⋈" : "⌗"}</span>
                    <span className="sidebar-name">{idx.name}</span>
                    <span className="sidebar-index-table">{idx.table}</span>
                  </div>
                  {expandedIndexes.has(idx.name) && (
                    <div className="sidebar-columns">
                      <div className="sidebar-column" style={{ color: "var(--text-muted)", fontSize: "0.75rem" }}>
                        {idx.kind === "composite" ? "composite" : "single"}
                      </div>
                      {idx.columns.map(col => (
                        <div key={col} className="sidebar-column">
                          <span className="col-icon">◉</span>
                          <span>{col}</span>
                        </div>
                      ))}
                    </div>
                  )}
                </div>
              )))}
            </div>

            <div className="sidebar-bottom">
              <div className="sidebar-group-header">
                <span className="sidebar-group-arrow">▼</span>
                INFO
              </div>
              <div className="sidebar-info-item"><span className="col-icon">◉</span> v2.1.3</div>
              <div className="sidebar-info-item"><span className="col-icon">◉</span> B+Tree · WAL · MVCC</div>
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

          {/* 테이블 우클릭 컨텍스트 메뉴 */}
          {tableCtxMenu && (
            <div
              className="ctx-menu table-ctx-menu"
              style={{ top: tableCtxMenu.y, left: tableCtxMenu.x }}
              onClick={e => e.stopPropagation()}
            >
              <div className="ctx-menu-header">{tableCtxMenu.table}</div>
              <div className="ctx-divider" />
              <div onClick={() => runCtxQuery(`SELECT * FROM ${tableCtxMenu.table};`)}>Select Rows</div>
              <div onClick={() => runCtxQuery(`SELECT * FROM ${tableCtxMenu.table} LIMIT 100;`)}>Select Rows (LIMIT 100)</div>
              <div onClick={() => runCtxQuery(`DESCRIBE ${tableCtxMenu.table};`)}>Describe Table</div>
              <div className="ctx-divider" />
              <div onClick={() => handleCopyTableName(tableCtxMenu.table)}>Copy Table Name</div>
              <div onClick={() => {
                navigator.clipboard.writeText(`INSERT INTO ${tableCtxMenu.table} VALUES ();`);
                setTableCtxMenu(null);
              }}>Copy as INSERT</div>
              <div className="ctx-divider" />
              <div className="ctx-item-warn" onClick={() => runCtxQuery(`TRUNCATE TABLE ${tableCtxMenu.table};`)}>Truncate Table</div>
              <div className="ctx-item-danger" onClick={() => runCtxQuery(`DROP TABLE ${tableCtxMenu.table};`, tableCtxMenu.table)}>
                DROP Table
              </div>
            </div>
          )}

          <div className="main">
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

            <div className="breadcrumb">
              <span>rustdb</span>
              <span className="breadcrumb-sep">›</span>
              <span>query</span>
              <span className="breadcrumb-sep">›</span>
              <span className="breadcrumb-active">query.sql</span>
            </div>

            <div className="editor-container">
              <Editor
                height="100%"
                defaultLanguage="sql"
                value={query}
                onChange={val => {
                  const v = val ?? "";
                  setQuery(v);
                  localStorage.setItem("rustdb_query", v);
                }}
                theme="rustdb-dark"
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
                beforeMount={monaco => {
                  // 커스텀 테마: vs-dark 기반, 주석만 회색으로
                  monaco.editor.defineTheme("rustdb-dark", {
                    base: "vs-dark",
                    inherit: true,
                    rules: [
                      { token: "comment",             foreground: "6a6a6a" },
                      { token: "comment.line",        foreground: "6a6a6a" },
                      { token: "comment.block",       foreground: "6a6a6a" },
                      { token: "comment.line.sql",    foreground: "6a6a6a" },
                      { token: "comment.block.sql",   foreground: "6a6a6a" },
                    ],
                    colors: {
                      "editor.background":            "#1e1e1e",
                      "editor.foreground":            "#d4d4d4",
                      "editorLineNumber.foreground":  "#858585",
                      "editor.lineHighlightBackground": "#2a2d2e",
                    },
                  });

                  // # 주석 지원: 기존 SQL 토크나이저 위에 # 규칙 추가
                  monaco.languages.setMonarchTokensProvider("sql", {
                    defaultToken: "invalid",
                    tokenPostfix: ".sql",
                    ignoreCase: true,
                    brackets: [
                      { open: "(", close: ")", token: "delimiter.parenthesis" },
                      { open: "[", close: "]", token: "delimiter.square" },
                    ],
                    keywords: [
                      "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","SET",
                      "DELETE","CREATE","TABLE","DROP","ALTER","ADD","COLUMN","RENAME",
                      "TO","JOIN","LEFT","RIGHT","INNER","ON","AND","OR","NOT",
                      "ORDER","GROUP","BY","ASC","DESC","LIMIT","HAVING","IN",
                      "BETWEEN","LIKE","AS","DISTINCT","UNION","ALL",
                      "COUNT","SUM","AVG","MIN","MAX",
                      "INDEX","UNIQUE","VIEW","PRIMARY","KEY","FOREIGN","REFERENCES",
                      "CONSTRAINT","CASCADE","RESTRICT","NULL","NOT","AUTO","INCREMENT",
                      "SHOW","TABLES","DESCRIBE","TRUNCATE","IS",
                      "BEGIN","COMMIT","ROLLBACK","TRANSACTION",
                      "CHECKPOINT","ISOLATION","LEVEL",
                      "UNCOMMITTED","COMMITTED","REPEATABLE","SERIALIZABLE",
                      "VACUUM","FOR","LOCKS","SET",
                      "INT","TEXT","FLOAT","BOOLEAN","VARCHAR","DATETIME","DATE",
                    ],
                    tokenizer: {
                      root: [
                        { include: "@comments" },
                        [/[a-zA-Z_]\w*/, { cases: { "@keywords": "keyword", "@default": "identifier" } }],
                        [/'([^'\\]|\\.)*'/, "string"],
                        [/'([^'\\]|\\.)*$/,  "string.invalid"],
                        [/\d+(\.\d+)?/,      "number"],
                        [/[=!<>]+/,          "operator"],
                        [/[(),;.]/,          "delimiter"],
                        [/\s+/,              "white"],
                      ],
                      comments: [
                        [/--.*$/,  "comment"],
                        [/#.*$/,   "comment"],
                        [/\/\*/,   { token: "comment", next: "@blockComment" }],
                      ],
                      blockComment: [
                        [/[^/*]+/, "comment"],
                        [/\*\//,   { token: "comment", next: "@pop" }],
                        [/[/*]/,   "comment"],
                      ],
                    },
                  } as Parameters<typeof monaco.languages.setMonarchTokensProvider>[1]);
                }}
                onMount={(editor, monaco) => {
                  editorRef.current = editor;
                  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter, runQuery);
                }}
              />
            </div>

            <div
              className="divider"
              onMouseDown={() => {
                isDragging.current = true;
                document.body.style.cursor = "row-resize";
                document.body.style.userSelect = "none";
              }}
            />

            <div className="result-panel" style={{ height: `${resultHeight}px` }}>
              <div className="result-tab-bar">
                <div className="result-tab active">RESULTS</div>
                <div className="result-tab">MESSAGES</div>
                {results.length > 0 && (
                  <div className={`result-status ${results.every(r => r.success) ? "ok" : "err"}`}>
                    {results.every(r => r.success) ? `✓ ${results.length} query(s)` : "✗ Error"}
                  </div>
                )}
              </div>

              <div
                className="result-content"
                onContextMenu={e => { e.preventDefault(); setCtxMenu({ x: e.clientX, y: e.clientY }); }}
                onClick={e => { if (ctxMenu) { e.stopPropagation(); setCtxMenu(null); } }}
              >
                {results.length === 0 ? (
                  <div className="result-empty">Ctrl+Enter 또는 ▶ Run 으로 쿼리를 실행하세요</div>
                ) : results.map((r, i) => (
                  <div key={i} className="result-block">
                    {!r.success ? (
                      <div className="result-error">❌ {r.message}</div>
                    ) : r.columns.length === 0 ? (
                      <div className="result-msg">✅ {r.message} · {r.elapsed.toFixed(3)}s</div>
                    ) : (
                      <>
                        <div className="result-info">{r.rows.length} row(s) · {r.elapsed.toFixed(3)}s</div>
                        <table className="result-table">
                          <thead><tr>{r.columns.map(c => <th key={c}>{c}</th>)}</tr></thead>
                          <tbody>{r.rows.map((row, ri) => (
                            <tr key={ri}>{row.map((cell, ci) => <td key={ci}>{cell}</td>)}</tr>
                          ))}</tbody>
                        </table>
                      </>
                    )}
                  </div>
                ))}
                {ctxMenu && (
                  <div className="ctx-menu" style={{ top: ctxMenu.y, left: ctxMenu.x }}>
                    <div onClick={() => {
                      navigator.clipboard.writeText(
                        results.map(r =>
                          r.columns.length > 0
                            ? [r.columns.join("\t"), ...r.rows.map(row => row.join("\t"))].join("\n")
                            : r.message
                        ).join("\n\n")
                      );
                      setCtxMenu(null);
                    }}>Copy All Results</div>
                    <div className="ctx-divider" />
                    <div onClick={() => { setResults([]); setCtxMenu(null); }}>Clear Results</div>
                  </div>
                )}
              </div>
            </div>

            <div className="status-bar">
              <div className="status-left">
                <span className="status-item">⎇ main</span>
                <span
                  className="status-item"
                  style={{
                    color: serverStatus.running ? "#4ec9b0" : "#858585",
                    cursor: "pointer",
                  }}
                  onClick={() => setActiveView("server")}
                  title="서버 관리자 열기"
                >
                  {serverStatus.running
                    ? `● TCP :${serverStatus.port} (${serverStatus.client_count} clients)`
                    : "○ TCP Stopped"}
                </span>
              </div>
              <div className="status-right">
                <span className="status-item">RustDB v2.1.3</span>
                <span className="status-item">UTF-8</span>
                <span className="status-item">SQL</span>
                <span className="status-item">B+Tree · WAL · MVCC</span>
              </div>
            </div>
          </div>
        </>
      )}

      {/* ── GUI 테이블 브라우저 뷰 ─────────────────────────────── */}
      {activeView === "gui" && (
        <div className="gui-view">
          <div className="gui-header">
            <div className="gui-header-left">
              <span className="gui-header-icon">⊞</span>
              <span className="gui-header-title">Table Browser</span>
            </div>
            <div className="gui-header-controls">
              <select
                className="gui-select"
                value={guiTable}
                onChange={e => handleGuiTableChange(e.target.value)}
              >
                <option value="">— Select a table —</option>
                {tables.map(t => <option key={t} value={t}>{t}</option>)}
              </select>
              {guiTable && (
                <button className="gui-refresh-btn" onClick={() => loadGuiTable(guiTable)} title="Refresh">
                  ↻
                </button>
              )}
              {guiResult && guiResult.columns.length > 0 && (
                <input
                  className="gui-filter-input"
                  placeholder="Filter rows..."
                  value={guiFilter}
                  onChange={e => setGuiFilter(e.target.value)}
                />
              )}
            </div>
          </div>

          <div className="gui-body">
            {!guiTable ? (
              <div className="gui-empty">
                <div className="gui-empty-icon">⊞</div>
                <div className="gui-empty-text">테이블을 선택하면 데이터를 조회합니다</div>
              </div>
            ) : guiLoading ? (
              <div className="gui-empty">
                <div className="gui-empty-text">Loading...</div>
              </div>
            ) : !guiResult || !guiResult.success ? (
              <div className="gui-error">{guiResult?.message ?? "Unknown error"}</div>
            ) : guiResult.columns.length === 0 ? (
              <div className="gui-empty">
                <div className="gui-empty-text">{guiResult.message || "No rows"}</div>
              </div>
            ) : (() => {
              const filterLower = guiFilter.toLowerCase();
              const filtered = guiFilter
                ? guiResult.rows.filter(row => row.some(cell => cell.toLowerCase().includes(filterLower)))
                : guiResult.rows;
              return (
                <div className="gui-table-wrap">
                  <div className="gui-table-meta">
                    {filtered.length} / {guiResult.rows.length} row(s) · {guiResult.columns.length} col(s) · {guiResult.elapsed.toFixed(3)}s
                  </div>
                  <table className="gui-table">
                    <thead>
                      <tr>
                        <th className="gui-row-num">#</th>
                        {guiResult.columns.map(c => <th key={c}>{c}</th>)}
                      </tr>
                    </thead>
                    <tbody>
                      {filtered.map((row, ri) => (
                        <tr key={ri}>
                          <td className="gui-row-num">{ri + 1}</td>
                          {row.map((cell, ci) => (
                            <td key={ci} className={cell === "NULL" || cell === "" ? "gui-null" : ""}>{cell || <span className="gui-null-label">NULL</span>}</td>
                          ))}
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              );
            })()}
          </div>

          <div className="status-bar">
            <div className="status-left">
              <span className="status-item">⎇ main</span>
              {guiTable && <span className="status-item" style={{ color: "#4ec9b0" }}>⊞ {guiTable}</span>}
            </div>
            <div className="status-right">
              <span className="status-item">RustDB v2.1.3</span>
              <span className="status-item">Table Browser</span>
            </div>
          </div>
        </div>
      )}

      {/* ── 서버 관리 뷰 ───────────────────────────────────────── */}
      {activeView === "server" && (
        <div className="server-view">
          {/* 헤더 */}
          <div className="srv-header">
            <div className="srv-header-left">
              <span className="srv-header-icon">⚡</span>
              <span className="srv-header-title">Server Manager</span>
            </div>
            <span className="srv-header-sub">RustDB TCP Server · 127.0.0.1</span>
          </div>

          {/* 카드 영역 */}
          <div className="srv-cards">
            {/* 상태 카드 */}
            <div className="srv-card">
              <div className="srv-card-title">STATUS</div>
              <div className="srv-status-row">
                <span className={`srv-dot ${serverStatus.running ? "running" : "stopped"}`} />
                <span className={`srv-status-text ${serverStatus.running ? "running" : "stopped"}`}>
                  {serverStatus.running ? "RUNNING" : "STOPPED"}
                </span>
              </div>
              <div className="srv-meta-list">
                <div className="srv-meta-row">
                  <span className="srv-meta-label">Host</span>
                  <span className="srv-meta-value">127.0.0.1</span>
                </div>
                <div className="srv-meta-row">
                  <span className="srv-meta-label">Port</span>
                  <span className="srv-meta-value">{serverStatus.port}</span>
                </div>
                <div className="srv-meta-row">
                  <span className="srv-meta-label">Connections</span>
                  <span className="srv-meta-value srv-clients">{serverStatus.client_count}</span>
                </div>
                <div className="srv-meta-row">
                  <span className="srv-meta-label">Protocol</span>
                  <span className="srv-meta-value">TCP / Line</span>
                </div>
              </div>
            </div>

            {/* 설정 카드 */}
            <div className="srv-card">
              <div className="srv-card-title">CONFIGURATION</div>
              <div className="srv-field">
                <label className="srv-label">Host</label>
                <input className="srv-input" value="127.0.0.1" disabled />
              </div>
              <div className="srv-field">
                <label className="srv-label">Port</label>
                <input
                  className="srv-input"
                  value={portInput}
                  onChange={e => setPortInput(e.target.value)}
                  disabled={serverStatus.running}
                  placeholder="7878"
                  type="number"
                  min={1024}
                  max={65535}
                />
              </div>
              <div className="srv-btn-row">
                <button
                  className="srv-btn start"
                  onClick={handleStartServer}
                  disabled={serverStatus.running}
                >
                  ▶ Start Server
                </button>
                <button
                  className="srv-btn stop"
                  onClick={handleStopServer}
                  disabled={!serverStatus.running}
                >
                  ■ Stop Server
                </button>
              </div>
              {serverMsg && (
                <div className="srv-feedback">{serverMsg}</div>
              )}
            </div>

            {/* 연결 방법 카드 */}
            <div className="srv-card">
              <div className="srv-card-title">CONNECTION GUIDE</div>
              <div className="srv-guide-section">
                <div className="srv-guide-label">Netcat (Linux/Mac)</div>
                <code className="srv-code">nc 127.0.0.1 {serverStatus.port}</code>
              </div>
              <div className="srv-guide-section">
                <div className="srv-guide-label">PowerShell</div>
                <code className="srv-code">
                  {"$c = New-Object Net.Sockets.TcpClient('127.0.0.1',"}
                  {serverStatus.port}{")"}
                </code>
              </div>
              <div className="srv-guide-section">
                <div className="srv-guide-label">Protocol</div>
                <div className="srv-guide-text">
                  쿼리를 한 줄씩 전송 → 결과 수신 후 <code className="srv-code-inline">---END---</code> 확인
                </div>
              </div>
            </div>
          </div>

          {/* 로그 패널 */}
          <div className="srv-log-panel">
            <div className="srv-log-header">
              <span className="srv-card-title">ACTIVITY LOG</span>
              <div className="srv-log-actions">
                <span className="srv-log-count">{serverStatus.log.length} entries</span>
                <button className="srv-log-clear" onClick={handleClearLog}>Clear</button>
              </div>
            </div>
            <div className="srv-log-body">
              {serverStatus.log.length === 0 ? (
                <div className="srv-log-empty">서버 활동 로그가 여기에 표시됩니다.</div>
              ) : serverStatus.log.map((entry, i) => (
                <div key={i} className="srv-log-entry">
                  <span className="srv-log-time">{entry.slice(0, 10)}</span>
                  <span className="srv-log-msg">{entry.slice(11)}</span>
                </div>
              ))}
              <div ref={logEndRef} />
            </div>
          </div>

          {/* 하단 상태바 */}
          <div className="status-bar">
            <div className="status-left">
              <span className="status-item">⎇ main</span>
              <span
                className="status-item"
                style={{ color: serverStatus.running ? "#4ec9b0" : "#858585" }}
              >
                {serverStatus.running
                  ? `● TCP :${serverStatus.port} (${serverStatus.client_count} clients)`
                  : "○ TCP Stopped"}
              </span>
            </div>
            <div className="status-right">
              <span className="status-item">RustDB v2.1.3</span>
              <span className="status-item">Server Manager</span>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default App;
