// App.tsx
import { useState, useEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import Editor from "@monaco-editor/react";
import type * as Monaco from "monaco-editor";
import "./App.css";
import { format as sqlFormat } from "sql-formatter";

// ─── 타입 ─────────────────────────────────────────────────────
interface HistoryEntry {
  id: string;
  sql: string;
  ts: number;
  success: boolean;
  elapsed: number;
}
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
interface ColumnDetail {
  name: string;
  data_type: string;
  is_pk: boolean;
  is_not_null: boolean;
  is_unique: boolean;
  is_auto_inc: boolean;
  default_val: string | null;
  fk_ref: string | null;
}
type ActiveView = "editor" | "erd" | "server" | "ai";
const PAGE_SIZE = 100;

// ─── ERD 타입/상수 ────────────────────────────────────────────
interface ErdPos { x: number; y: number; }
const ERD_CARD_W = 220;
const ERD_HEADER_H = 32;
const ERD_COL_H = 24;

// fk_ref 형식: "db1.dept(id)" 또는 "dept(id)" — 괄호가 우선
function parseRef(ref: string): { table: string; col: string } | null {
  const paren = ref.indexOf("(");
  if (paren > 0) return { table: ref.slice(0, paren), col: ref.slice(paren + 1).replace(")", "") };
  const dot = ref.lastIndexOf(".");
  if (dot > 0) return { table: ref.slice(0, dot), col: ref.slice(dot + 1) };
  return null;
}
// "db1.dept" → "dept" (DB 한정자 제거)
function unqualify(name: string): string {
  const dot = name.indexOf(".");
  return dot >= 0 ? name.slice(dot + 1) : name;
}

// 직각 꺾임 경로: x1,y1 → midX 수평 → y2 수직 → x2 수평, 모서리 r=8 라운드
function erdOrthPath(x1: number, y1: number, x2: number, y2: number): string {
  const r = 8;
  if (Math.abs(y1 - y2) < 1) return `M${x1} ${y1} H${x2}`;
  const midX = (x1 + x2) / 2;
  const sdx = Math.sign(midX - x1); // 수평 진행 방향 (+1 오른쪽, -1 왼쪽)
  const sdy = Math.sign(y2 - y1);   // 수직 진행 방향 (+1 아래, -1 위)
  const r1 = Math.min(r, Math.abs(midX - x1), Math.abs(y2 - y1) / 2);
  const r2 = Math.min(r, Math.abs(x2 - midX), Math.abs(y2 - y1) / 2);
  if (r1 < 1 || r2 < 1) return `M${x1} ${y1} H${midX} V${y2} H${x2}`;
  return [
    `M${x1} ${y1}`,
    `H${midX - sdx * r1}`,
    `Q${midX} ${y1} ${midX} ${y1 + sdy * r1}`,
    `V${y2 - sdy * r2}`,
    `Q${midX} ${y2} ${midX + sdx * r2} ${y2}`,
    `H${x2}`,
  ].join(" ");
}

// ─── 탭 타입 ──────────────────────────────────────────────────
interface Tab { id: string; name: string; content: string; }

const MAX_HISTORY = 200;

function loadTabs(connId: string): Tab[] {
  try {
    const saved = localStorage.getItem(`rustdb_tabs_${connId}`);
    if (saved) return JSON.parse(saved);
  } catch {}
  return [{ id: "1", name: "query.sql", content: localStorage.getItem(`rustdb_query_${connId}`) ?? "SHOW TABLES;" }];
}
function loadActiveTabId(connId: string): string {
  return localStorage.getItem(`rustdb_active_tab_${connId}`) ?? "1";
}
function loadHistory(connId: string): HistoryEntry[] {
  try {
    const saved = localStorage.getItem(`rustdb_history_${connId}`);
    if (saved) return JSON.parse(saved);
  } catch {}
  return [];
}
function saveHistory(connId: string, h: HistoryEntry[]) {
  localStorage.setItem(`rustdb_history_${connId}`, JSON.stringify(h.slice(0, MAX_HISTORY)));
}

// ─── 메인 컴포넌트 ────────────────────────────────────────────
function App() {
  // 현재 연결 ID (localStorage 키 네임스페이스용, ref는 클로저에서 안전하게 참조)
  const connIdRef = useRef<string>("");
  const [, setSessionConnId] = useState<string>("");

  // 탭 상태 (로그인 전까지 기본값, doLogin 시 연결별 저장 데이터로 교체)
  const [tabs, setTabs] = useState<Tab[]>([{ id: "1", name: "query.sql", content: "SHOW TABLES;" }]);
  const [activeTabId, setActiveTabId] = useState<string>("1");
  const activeTab = tabs.find(t => t.id === activeTabId) ?? tabs[0];
  const queryRef = useRef<string>(activeTab?.content ?? "");
  // setValue() 호출 중 onChange가 잘못된 탭에 내용을 저장하지 못하도록 막는 플래그
  const isSwitchingTab = useRef(false);
  const [tabResults, setTabResults] = useState<Record<string, QueryResult[]>>({});
  const [tabResultPages, setTabResultPages] = useState<Record<string, Record<number, number>>>({});
  const [tabColWidths, setTabColWidths] = useState<Record<string, Record<number, number[]>>>({});
  const [tabSortState, setTabSortState] = useState<Record<string, Record<number, { col: number; dir: 'asc' | 'desc' } | null>>>({});
  const [tabResultSearch, setTabResultSearch] = useState<Record<string, string>>({});
  const [sidebarSearch, setSidebarSearch] = useState("");
  const [bookmarks, setBookmarks] = useState<{ id: string; name: string; sql: string }[]>(() => {
    try { return JSON.parse(localStorage.getItem("rustdb_bookmarks") ?? "[]"); } catch { return []; }
  });
  const [databases, setDatabases] = useState<string[]>(["rustdb"]);
  const [expandedTables, setExpandedTables] = useState<Set<string>>(new Set());
  const [tableColumns, setTableColumns] = useState<Record<string, ColumnDetail[]>>({});
  const [expandedViews, setExpandedViews] = useState<Set<string>>(new Set());
  const [viewColumns, setViewColumns] = useState<Record<string, string[]>>({});
  const [expandedIndexes, setExpandedIndexes] = useState<Set<string>>(new Set());
  const [currentDb, setCurrentDb] = useState<string>("rustdb");
  const [expandedDbs, setExpandedDbs] = useState<Set<string>>(new Set(["rustdb"]));
  // DB별 Tables/Views/Indexes 데이터
  interface DbData { tables: string[]; views: string[]; indexes: IndexInfo[]; }
  const [dbData, setDbData] = useState<Record<string, DbData>>({});
  const [tablesOpen, setTablesOpen] = useState<Record<string, boolean>>({});
  const [viewsOpen, setViewsOpen] = useState<Record<string, boolean>>({});
  const [indexesOpen, setIndexesOpen] = useState<Record<string, boolean>>({});
  const [ctxMenu, setCtxMenu] = useState<{ x: number; y: number } | null>(null);
  const [tableCtxMenu, setTableCtxMenu] = useState<{ x: number; y: number; table: string } | null>(null);
  const [dbCtxMenu, setDbCtxMenu] = useState<{ x: number; y: number; db: string } | null>(null);
  const [editingTabId, setEditingTabId] = useState<string | null>(null);
  const [editingTabName, setEditingTabName] = useState("");
  const [isRunning, setIsRunning] = useState(false);
  const [queryHistory, setQueryHistory] = useState<HistoryEntry[]>([]);
  const [resultTab, setResultTab] = useState<"results" | "history">("results");
  const editorRef = useRef<Monaco.editor.IStandaloneCodeEditor | null>(null);
  const schemaRef = useRef<{ tables: string[]; columns: Record<string, string[]> }>({ tables: [], columns: {} });
  const [resultHeight, setResultHeight] = useState(260);
  const [sidebarWidth, setSidebarWidth] = useState(240);
  const isDragging = useRef(false);
  const isSidebarDragging = useRef(false);

  // ERD 상태
  const [erdColumns, setErdColumns] = useState<Record<string, ColumnDetail[]>>({});
  const [erdPositions, setErdPositions] = useState<Record<string, ErdPos>>({});
  const [erdLoading, setErdLoading] = useState(false);
  const [erdPan, setErdPan] = useState<ErdPos>({ x: 40, y: 40 });
  const [erdZoom, setErdZoom] = useState(1);
  const erdCanvasRef = useRef<HTMLDivElement>(null);
  const erdCardDragRef = useRef<{ table: string; startMX: number; startMY: number; startCX: number; startCY: number; zoom: number } | null>(null);
  const erdCanvasDragRef = useRef<{ startMX: number; startMY: number; startPX: number; startPY: number } | null>(null);
  const erdCardWasDragged = useRef(false);
  const [erdSelectedTable, setErdSelectedTable] = useState<string>("");
  const [erdTableData, setErdTableData] = useState<QueryResult | null>(null);
  const [erdTableLoading, setErdTableLoading] = useState(false);
  const [erdFilter, setErdFilter] = useState("");
  const [erdDataHeight, setErdDataHeight] = useState(0);
  const erdDataDragging = useRef(false);

  // ─── 연결 관리 ───────────────────────────────────────────────
  interface Connection {
    id: string; name: string; host: string; port: number;
    user: string; password: string; autoLogin: boolean; dataDir: string;
  }
  const loadConnections = (): Connection[] => {
    try {
      const s = localStorage.getItem("rustdb_connections");
      if (s) {
        const conns = JSON.parse(s) as Connection[];
        return conns.map(c => ({
          ...c,
          password: c.password ?? "",
          autoLogin: c.autoLogin ?? false,
          dataDir: c.dataDir ?? (c.id === "1" ? "data" : `data_${c.id}`),
        }));
      }
    } catch {}
    return [{ id: "1", name: "RustDB Local", host: "localhost", port: 7878, user: "root", password: "root", autoLogin: false, dataDir: "data" }];
  };
  const [connections, setConnections] = useState<Connection[]>(loadConnections);
  const saveConnections = (c: Connection[]) => { localStorage.setItem("rustdb_connections", JSON.stringify(c)); setConnections(c); };

  // 홈 화면 상태
  const [loggedIn, setLoggedIn] = useState(false);
  const [sessionUser, setSessionUser] = useState("");

  // 연결 다이얼로그 상태
  const [connectingTo, setConnectingTo] = useState<Connection | null>(null);
  const [dlgPass, setDlgPass] = useState("");
  const [dlgPassVisible, setDlgPassVisible] = useState(false);
  const [dlgError, setDlgError] = useState("");
  const [dlgLoading, setDlgLoading] = useState(false);

  // 새 연결 추가 폼 상태
  const [showNewConn, setShowNewConn] = useState(false);
  const [newName, setNewName] = useState("New Connection");
  const [newHost, setNewHost] = useState("localhost");
  const [newPort, setNewPort] = useState("7878");
  const [newUser, setNewUser] = useState("root");
  const [newPass, setNewPass] = useState("root");
  const [newPassVisible, setNewPassVisible] = useState(false);
  const [newAutoLogin, setNewAutoLogin] = useState(false);

  // 연결 성공 시 해당 연결의 저장 데이터를 로드하고 메인 앱으로 전환
  const doLogin = (conn: Connection) => {
    const id = conn.id;
    const newTabs = loadTabs(id);
    const newActiveId = loadActiveTabId(id);
    const newHistory = loadHistory(id);
    const activeContent = (newTabs.find(t => t.id === newActiveId) ?? newTabs[0])?.content ?? "SHOW TABLES;";

    connIdRef.current = id;
    queryRef.current = activeContent;

    setSessionConnId(id);
    setSessionUser(conn.user);
    setTabs(newTabs);
    setActiveTabId(newActiveId);
    setQueryHistory(newHistory);
    setTabResults({});
    setTabResultPages({});
    setTabColWidths({});
    // ERD UI 상태 초기화 (연결마다 독립)
    setErdColumns({});
    setErdPositions({});
    setErdPan({ x: 40, y: 40 });
    setErdZoom(1);
    setErdFilter("");
    setErdSelectedTable("");
    setErdTableData(null);
    // 서버 상태 초기화 (연결마다 독립)
    setServerStatus({ running: false, port: conn.port, client_count: 0, log: [] });
    setPortInput(conn.port.toString());
    setSrvConnName(conn.name);
    setServerMsg("");
    setActiveView("editor");
    setLoggedIn(true);
    setConnectingTo(null);
    setDlgPass("");
  };

  const saveBookmarkList = (bks: { id: string; name: string; sql: string }[]) => {
    localStorage.setItem("rustdb_bookmarks", JSON.stringify(bks));
    setBookmarks(bks);
  };
  const addBookmark = () => {
    const sql = queryRef.current.trim();
    if (!sql) return;
    const name = sql.split("\n")[0].trim().slice(0, 40) || "Bookmark";
    saveBookmarkList([...bookmarks, { id: Date.now().toString(), name, sql }]);
  };
  const removeBookmark = (id: string) => saveBookmarkList(bookmarks.filter(b => b.id !== id));

  const handleAutoLogin = async (conn: Connection) => {
    const ok = await invoke<boolean>("authenticate", { user: conn.user, password: conn.password, dataDir: conn.dataDir });
    if (ok) doLogin(conn);
  };

  const handleConnect = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!connectingTo) return;
    setDlgLoading(true);
    setDlgError("");
    const ok = await invoke<boolean>("authenticate", { user: connectingTo.user, password: dlgPass, dataDir: connectingTo.dataDir });
    setDlgLoading(false);
    if (ok) {
      doLogin(connectingTo);
    } else {
      setDlgError(`Access denied for user '${connectingTo.user}' (using password: ${dlgPass ? "YES" : "NO"})`);
    }
  };

  const handleAddConnection = () => {
    const port = parseInt(newPort) || 7878;
    const id = Date.now().toString();
    const conn: Connection = {
      id, name: newName, host: newHost, port,
      user: newUser, password: newPass, autoLogin: newAutoLogin,
      dataDir: `data_${id}`,
    };
    saveConnections([...connections, conn]);
    setShowNewConn(false);
    setNewName("New Connection"); setNewHost("localhost"); setNewPort("7878");
    setNewUser("root"); setNewPass("root"); setNewAutoLogin(false); setNewPassVisible(false);
  };

  // 뷰 전환
  const [activeView, setActiveView] = useState<ActiveView>("editor");

  // 서버 상태
  const [serverStatus, setServerStatus] = useState<ServerStatus>({
    running: false, port: 7878, client_count: 0, log: [],
  });
  const [portInput, setPortInput] = useState("7878");
  const [serverMsg, setServerMsg] = useState("");
  const [srvConnName, setSrvConnName] = useState("RustDB Local");
  const [srvUser, setSrvUser] = useState("root");
  const [srvPass, setSrvPass] = useState("root");
  const [srvTab, setSrvTab] = useState<"main" | "guide">("main");
  const [srvPassVisible, setSrvPassVisible] = useState(false);
  const logEndRef = useRef<HTMLDivElement>(null);

  // ─── DB 하나의 데이터 로드 ────────────────────────────────────
  const loadDbData = async (db: string) => {
    const [tbls, vws, idxs] = await Promise.all([
      invoke<string[]>("get_tables_for_db", { db }),
      invoke<string[]>("get_views_for_db", { db }),
      invoke<IndexInfo[]>("get_indexes_for_db", { db }),
    ]);
    setDbData(prev => ({ ...prev, [db]: { tables: tbls, views: vws, indexes: idxs } }));
  };

  // ─── 사이드바 데이터 갱신 ────────────────────────────────────
  const refreshSidebar = async () => {
    const [dbs, cdb] = await Promise.all([
      invoke<string[]>("get_databases"),
      invoke<string>("get_current_db"),
    ]);
    setDatabases(dbs);
    setCurrentDb(cdb);
    setExpandedDbs(prev => {
      const next = new Set(prev);
      next.add(cdb);
      // 삭제된 DB는 제거
      for (const d of next) { if (!dbs.includes(d)) next.delete(d); }
      return next;
    });
    // 현재 펼쳐진 모든 DB 데이터 갱신
    const expanded = new Set([cdb, ...Array.from(expandedDbs)]);
    await Promise.all(Array.from(expanded).filter(d => dbs.includes(d)).map(loadDbData));
  };

  // ─── 초기 로드 (연결할 때마다 재실행) ──────────────────────
  useEffect(() => { if (loggedIn) refreshSidebar(); }, [loggedIn]);

  // 탭별 결과 파생값
  const results = tabResults[activeTabId] ?? [];
  const resultPages = tabResultPages[activeTabId] ?? {};
  const colWidths = tabColWidths[activeTabId] ?? {};
  const sortState = tabSortState[activeTabId] ?? {};
  const resultSearch = tabResultSearch[activeTabId] ?? "";

  // schemaRef 업데이트 (Monaco 자동완성용)
  useEffect(() => {
    const tables: string[] = [];
    const columns: Record<string, string[]> = {};
    for (const data of Object.values(dbData)) tables.push(...data.tables);
    for (const [t, cols] of Object.entries(tableColumns)) columns[t] = cols.map(c => c.name);
    schemaRef.current = { tables, columns };
  }, [dbData, tableColumns]);

  // ─── 메뉴바 상태 ────────────────────────────────────────────
  const [openMenu, setOpenMenu] = useState<string | null>(null);

  // ─── 컨텍스트 메뉴 + 메뉴바 닫기 ────────────────────────────
  useEffect(() => {
    const h = () => { setCtxMenu(null); setTableCtxMenu(null); setDbCtxMenu(null); setOpenMenu(null); };
    window.addEventListener("click", h);
    return () => window.removeEventListener("click", h);
  }, []);

  // ─── 키보드 단축키 ──────────────────────────────────────────
  useEffect(() => {
    if (!loggedIn) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.ctrlKey && !e.shiftKey && e.key === "t") { e.preventDefault(); addTab(); }
      if (e.ctrlKey && !e.shiftKey && e.key === "w") { e.preventDefault(); closeTab(activeTabId); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [loggedIn, activeTabId, tabs]);

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

  // ─── ERD 테이블 데이터 로드 ─────────────────────────────────
  const loadErdTableData = async (tbl: string) => {
    setErdTableLoading(true);
    setErdFilter("");
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: `SELECT * FROM ${tbl};`, ts: Date.now() });
      setErdTableData(res.results[0] ?? null);
    } catch {
      setErdTableData({ columns: [], rows: [], message: "Error loading table", elapsed: 0, success: false });
    } finally {
      setErdTableLoading(false);
    }
  };

  const handleErdCardClick = (tbl: string) => {
    if (erdSelectedTable === tbl) {
      setErdSelectedTable("");
      setErdTableData(null);
      setErdDataHeight(0);
    } else {
      setErdSelectedTable(tbl);
      loadErdTableData(tbl);
      if (erdDataHeight === 0) setErdDataHeight(220);
    }
  };

  // ─── ERD 데이터 로드 (뷰 전환 / DB 변경 시) ────────────────
  useEffect(() => {
    if (activeView === "erd") loadErd();
  }, [activeView, currentDb]);

  // ─── ERD 드래그 핸들러 ───────────────────────────────────────
  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      const cd = erdCardDragRef.current;
      if (cd) {
        if (!erdCardWasDragged.current) {
          const dist = Math.hypot(e.clientX - cd.startMX, e.clientY - cd.startMY);
          if (dist > 4) erdCardWasDragged.current = true;
        }
        if (erdCardWasDragged.current) {
          const dx = (e.clientX - cd.startMX) / cd.zoom;
          const dy = (e.clientY - cd.startMY) / cd.zoom;
          setErdPositions(p => ({ ...p, [cd.table]: { x: cd.startCX + dx, y: cd.startCY + dy } }));
        }
      }
      const pd = erdCanvasDragRef.current;
      if (pd) {
        setErdPan({ x: pd.startPX + e.clientX - pd.startMX, y: pd.startPY + e.clientY - pd.startMY });
      }
      if (erdDataDragging.current) {
        const view = document.querySelector(".erd-view") as HTMLElement;
        if (view) {
          const rect = view.getBoundingClientRect();
          const newH = Math.max(80, Math.min(rect.bottom - e.clientY - 22, rect.height - 150));
          setErdDataHeight(newH);
        }
      }
    };
    const onUp = () => {
      erdCardDragRef.current = null;
      erdCanvasDragRef.current = null;
      erdDataDragging.current = false;
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
      const s = await invoke<ServerStatus>("get_server_status", { connId: connIdRef.current });
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
    const q = (sel?.trim() ? sel : (editorRef.current?.getValue() ?? queryRef.current)).trim();
    if (!q) return;
    setTabResults(p => ({ ...p, [activeTabId]: [] }));
    setTabResultPages(p => ({ ...p, [activeTabId]: {} }));
    setTabColWidths(p => ({ ...p, [activeTabId]: {} }));
    setTabSortState(p => ({ ...p, [activeTabId]: {} }));
    setTabResultSearch(p => ({ ...p, [activeTabId]: "" }));
    setResultTab("results");
    setIsRunning(true);
    const startTs = Date.now();
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: q, ts: startTs });
      setTabResults(p => ({ ...p, [activeTabId]: res.results }));
      const entry: HistoryEntry = {
        id: startTs.toString(),
        sql: q,
        ts: startTs,
        success: res.results.every(r => r.success),
        elapsed: res.total_elapsed,
      };
      setQueryHistory(prev => {
        const next = [entry, ...prev].slice(0, MAX_HISTORY);
        saveHistory(connIdRef.current, next);
        return next;
      });
      await refreshSidebar();
    } catch (e) {
      setTabResults(p => ({ ...p, [activeTabId]: [{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }] }));
      const entry: HistoryEntry = {
        id: startTs.toString(),
        sql: q,
        ts: startTs,
        success: false,
        elapsed: (Date.now() - startTs) / 1000,
      };
      setQueryHistory(prev => {
        const next = [entry, ...prev].slice(0, MAX_HISTORY);
        saveHistory(connIdRef.current, next);
        return next;
      });
    } finally {
      setIsRunning(false);
    }
  };

  // 에디터 내용 프로그래밍 방식으로 변경 — executeEdits로 undo 히스토리 보존
  const setEditorQuery = (q: string) => {
    queryRef.current = q;
    saveTabs(tabs.map(t => t.id === activeTabId ? { ...t, content: q } : t));
    if (editorRef.current) {
      const model = editorRef.current.getModel();
      if (model) {
        editorRef.current.pushUndoStop();
        editorRef.current.executeEdits("sidebar", [{ range: model.getFullModelRange(), text: q }]);
        editorRef.current.pushUndoStop();
      } else {
        editorRef.current.setValue(q);
      }
    }
  };

  // 탭 저장 헬퍼 (연결별 키)
  const saveTabs = (next: Tab[]) => {
    setTabs(next);
    localStorage.setItem(`rustdb_tabs_${connIdRef.current}`, JSON.stringify(next));
  };

  // 현재 에디터 내용을 탭에 저장한 후 탭 전환
  const switchTab = (id: string) => {
    if (id === activeTabId) return;
    const currentContent = editorRef.current?.getValue() ?? queryRef.current;
    const updated = tabs.map(t => t.id === activeTabId ? { ...t, content: currentContent } : t);
    saveTabs(updated);
    setActiveTabId(id);
    localStorage.setItem(`rustdb_active_tab_${connIdRef.current}`, id);
    const target = updated.find(t => t.id === id);
    if (target) {
      queryRef.current = target.content;
      isSwitchingTab.current = true;
      editorRef.current?.setValue(target.content);
      isSwitchingTab.current = false;
    }
  };

  // 새 탭 추가
  const addTab = () => {
    const currentContent = editorRef.current?.getValue() ?? queryRef.current;
    const updated = tabs.map(t => t.id === activeTabId ? { ...t, content: currentContent } : t);
    const newId = Date.now().toString();
    // 기존 탭 이름에서 최대 번호를 구해 중복 방지
    const maxNum = tabs.reduce((max, t) => {
      const m = t.name.match(/^query(\d+)\.sql$/);
      return m ? Math.max(max, parseInt(m[1], 10)) : max;
    }, 0);
    const newTab: Tab = { id: newId, name: `query${maxNum + 1}.sql`, content: "" };
    const next = [...updated, newTab];
    saveTabs(next);
    setActiveTabId(newId);
    localStorage.setItem(`rustdb_active_tab_${connIdRef.current}`, newId);
    queryRef.current = "";
    isSwitchingTab.current = true;
    editorRef.current?.setValue("");
    isSwitchingTab.current = false;
  };

  // 탭 닫기
  const closeTab = (id: string, e?: React.MouseEvent) => {
    e?.stopPropagation();
    if (tabs.length === 1) return; // 마지막 탭은 닫지 않음
    const idx = tabs.findIndex(t => t.id === id);
    const next = tabs.filter(t => t.id !== id);
    saveTabs(next);
    if (activeTabId === id) {
      const newActive = next[Math.min(idx, next.length - 1)];
      setActiveTabId(newActive.id);
      localStorage.setItem("rustdb_active_tab", newActive.id);
      queryRef.current = newActive.content;
      editorRef.current?.setValue(newActive.content);
    }
  };

  const toggleTable = async (t: string) => {
    if (expandedTables.has(t)) {
      setExpandedTables(prev => { const s = new Set(prev); s.delete(t); return s; });
      return;
    }
    setExpandedTables(prev => new Set(prev).add(t));
    if (!tableColumns[t]) {
      const cols = await invoke<ColumnDetail[]>("get_columns_detail", { table: t });
      setTableColumns(p => ({ ...p, [t]: cols }));
    }
    setEditorQuery(`SELECT * FROM ${t};`);
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
    setEditorQuery(`SELECT * FROM ${v};`);
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
    setEditorQuery(q);
    setTabResults(p => ({ ...p, [activeTabId]: [] }));
    setTabResultPages(p => ({ ...p, [activeTabId]: {} }));
    setTabColWidths(p => ({ ...p, [activeTabId]: {} }));
    setIsRunning(true);
    setActiveView("editor");
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: q, ts: Date.now() });
      setTabResults(p => ({ ...p, [activeTabId]: res.results }));
      await refreshSidebar();
      if (dropTable) setExpandedTables(prev => { const s = new Set(prev); s.delete(dropTable); return s; });
    } catch (e) {
      setTabResults(p => ({ ...p, [activeTabId]: [{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }] }));
    } finally {
      setIsRunning(false);
    }
  };

  // ─── 탭 이름 편집 ─────────────────────────────────────────────
  const commitTabRename = () => {
    if (!editingTabId) return;
    const trimmed = editingTabName.trim();
    const name = trimmed || (tabs.find(t => t.id === editingTabId)?.name ?? "query.sql");
    saveTabs(tabs.map(t => t.id === editingTabId ? { ...t, name } : t));
    setEditingTabId(null);
  };

  // ─── DB 우클릭 메뉴 핸들러 ────────────────────────────────────
  const runDbCtxQuery = async (q: string) => {
    setDbCtxMenu(null);
    setEditorQuery(q);
    setTabResults(p => ({ ...p, [activeTabId]: [] }));
    setTabResultPages(p => ({ ...p, [activeTabId]: {} }));
    setTabColWidths(p => ({ ...p, [activeTabId]: {} }));
    setIsRunning(true);
    setActiveView("editor");
    try {
      const res = await invoke<MultiQueryResult>("execute_query", { query: q, ts: Date.now() });
      setTabResults(p => ({ ...p, [activeTabId]: res.results }));
      await refreshSidebar();
    } catch (e) {
      setTabResults(p => ({ ...p, [activeTabId]: [{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }] }));
    } finally {
      setIsRunning(false);
    }
  };

  const handleCopyTableName = (t: string) => {
    navigator.clipboard.writeText(t);
    setTableCtxMenu(null);
  };

  // ─── ERD 로드 ────────────────────────────────────────────────
  const loadErd = async () => {
    setErdLoading(true);
    try {
      const tblList = await invoke<string[]>("get_tables");
      if (tblList.length === 0) { setErdColumns({}); setErdPositions({}); return; }
      const entries = await Promise.all(
        tblList.map(async t => {
          const cols = await invoke<ColumnDetail[]>("get_columns_detail", { table: t });
          return [t, cols] as [string, ColumnDetail[]];
        })
      );
      setErdColumns(Object.fromEntries(entries));
      const gridCols = Math.max(1, Math.ceil(Math.sqrt(tblList.length)));
      setErdPositions(prev => {
        const next: Record<string, ErdPos> = {};
        tblList.forEach((t, i) => {
          next[t] = prev[t] ?? {
            x: (i % gridCols) * (ERD_CARD_W + 60) + 40,
            y: Math.floor(i / gridCols) * 260 + 40,
          };
        });
        return next;
      });
    } finally {
      setErdLoading(false);
    }
  };

  // ─── 서버 제어 ──────────────────────────────────────────────
  const handleStartServer = async () => {
    const port = parseInt(portInput) || 7878;
    try {
      const msg = await invoke<string>("start_server", { connId: connIdRef.current, port });
      setServerMsg(msg);
    } catch (e) { setServerMsg(String(e)); }
  };

  const handleStopServer = async () => {
    try {
      const msg = await invoke<string>("stop_server", { connId: connIdRef.current });
      setServerMsg(msg);
    } catch (e) { setServerMsg(String(e)); }
  };

  const handleClearLog = async () => {
    await invoke("clear_server_log", { connId: connIdRef.current });
    setServerStatus(s => ({ ...s, log: [] }));
  };

  // ─── 렌더 ───────────────────────────────────────────────────
  // ─── 메뉴 항목 정의 ─────────────────────────────────────────
  const menus: { label: string; items: { label: string; shortcut?: string; divider?: boolean; action?: () => void }[] }[] = [
    {
      label: "파일",
      items: [
        { label: "새 탭", shortcut: "Ctrl+T", action: () => { addTab(); setActiveView("editor"); } },
        { label: "탭 닫기", shortcut: "Ctrl+W", action: () => closeTab(activeTabId) },
        { label: "", divider: true },
        { label: "저장", shortcut: "Ctrl+S", action: () => {
          const content = queryRef.current;
          const blob = new Blob([content], { type: "text/plain" });
          const a = document.createElement("a");
          a.href = URL.createObjectURL(blob);
          a.download = (tabs.find(t => t.id === activeTabId)?.name ?? "query") + (tabs.find(t => t.id === activeTabId)?.name?.endsWith(".sql") ? "" : ".sql");
          a.click();
        }},
        { label: "", divider: true },
        { label: "종료", shortcut: "Alt+F4", action: () => window.close() },
      ],
    },
    {
      label: "편집",
      items: [
        { label: "실행 취소", shortcut: "Ctrl+Z", action: () => editorRef.current?.trigger("menu", "undo", null) },
        { label: "다시 실행", shortcut: "Ctrl+Y", action: () => editorRef.current?.trigger("menu", "redo", null) },
        { label: "", divider: true },
        { label: "잘라내기", shortcut: "Ctrl+X", action: () => editorRef.current?.trigger("menu", "editor.action.clipboardCutAction", null) },
        { label: "복사", shortcut: "Ctrl+C", action: () => editorRef.current?.trigger("menu", "editor.action.clipboardCopyAction", null) },
        { label: "붙여넣기", shortcut: "Ctrl+V", action: () => editorRef.current?.trigger("menu", "editor.action.clipboardPasteAction", null) },
        { label: "", divider: true },
        { label: "모두 선택", shortcut: "Ctrl+A", action: () => editorRef.current?.trigger("menu", "editor.action.selectAll", null) },
        { label: "찾기", shortcut: "Ctrl+F", action: () => editorRef.current?.trigger("menu", "actions.find", null) },
        { label: "", divider: true },
        { label: "SQL 포매터", shortcut: "Ctrl+Shift+F", action: () => {
          try {
            const fmt = sqlFormat(editorRef.current?.getValue() ?? "", { language: 'sql', tabWidth: 2, keywordCase: 'upper' });
            editorRef.current?.setValue(fmt);
          } catch {}
        }},
        { label: "북마크 추가", shortcut: "★", action: addBookmark },
      ],
    },
    {
      label: "보기",
      items: [
        { label: "SQL 에디터", shortcut: "Ctrl+1", action: () => setActiveView("editor") },
        { label: "ERD 편집기", shortcut: "Ctrl+2", action: () => setActiveView("erd") },
        { label: "서버 관리자", shortcut: "Ctrl+3", action: () => setActiveView("server") },
        { label: "AI 어시스턴트", shortcut: "Ctrl+4", action: () => setActiveView("ai") },
        { label: "", divider: true },
        { label: "사이드바 토글", shortcut: "Ctrl+B", action: () => setSidebarWidth(w => w > 0 ? 0 : 240) },
      ],
    },
    {
      label: "실행",
      items: [
        { label: "쿼리 실행", shortcut: "F5", action: () => { setActiveView("editor"); runQuery(); } },
        { label: "", divider: true },
        { label: "새 쿼리 탭", shortcut: "Ctrl+T", action: () => { addTab(); setActiveView("editor"); } },
        { label: "탭 닫기", shortcut: "Ctrl+W", action: () => closeTab(activeTabId) },
      ],
    },
    {
      label: "터미널",
      items: [
        { label: "새 터미널", shortcut: "Ctrl+`", action: () => setActiveView("server") },
        { label: "", divider: true },
        { label: "서버 시작", action: () => setActiveView("server") },
      ],
    },
  ];

  // ─── 로그인 화면 ────────────────────────────────────────────
  if (!loggedIn) {
    const homeMenuLabels = ["파일", "편집", "보기", "실행", "터미널"];
    return (
      <div className="home-bg">

        {/* ── 메뉴바 (쿼리 편집기와 동일) ── */}
        <div className="menu-bar" onClick={e => e.stopPropagation()}>
          {homeMenuLabels.map(label => (
            <div
              key={label}
              className={`menu-item ${openMenu === label ? "open" : ""}`}
              onClick={() => setOpenMenu(prev => prev === label ? null : label)}
              onMouseEnter={() => { if (openMenu !== null) setOpenMenu(label); }}
            >
              <span>{label}</span>
              {openMenu === label && (
                <div className="menu-dropdown">
                  <div className="menu-dropdown-item" style={{ color: "#555", cursor: "default", fontSize: 11 }}>
                    연결 후 사용 가능
                  </div>
                </div>
              )}
            </div>
          ))}
          <div className="menu-bar-right">
            <span style={{ display: "flex", alignItems: "center", gap: 6, color: "#999", fontSize: 12 }}>
              <svg width="13" height="16" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                <ellipse cx="12" cy="5" rx="7" ry="3" stroke="#4ec9b0" strokeWidth="1.8" vectorEffect="non-scaling-stroke"/>
                <path d="M5 5v6c0 1.66 3.13 3 7 3s7-1.34 7-3V5" stroke="#4ec9b0" strokeWidth="1.8" fill="none" vectorEffect="non-scaling-stroke"/>
                <path d="M5 11v6c0 1.66 3.13 3 7 3s7-1.34 7-3v-6" stroke="#4ec9b0" strokeWidth="1.4" fill="none" vectorEffect="non-scaling-stroke"/>
              </svg>
              <span style={{ fontWeight: 600, color: "#ccc" }}>RustDB</span>
              <span className="home-topbar-ver">v2.2.0</span>
            </span>
          </div>
        </div>

        {/* ── 레이아웃 (사이드바 + 메인) ── */}
        <div className="home-layout" onClick={() => setOpenMenu(null)}>

          {/* 사이드바 */}
          <div className="home-sidebar">
            <div className="home-sidebar-header">연결</div>
            {connections.map(conn => (
              <div
                key={conn.id}
                className="home-sidebar-item"
                title={conn.name}
                onClick={() => {
                  if (conn.autoLogin && conn.password) { handleAutoLogin(conn); }
                  else { setConnectingTo(conn); setDlgPass(conn.password ?? ""); setDlgError(""); }
                }}
              >
                <svg width="13" height="16" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none" style={{ flexShrink: 0 }}>
                  <ellipse cx="12" cy="5" rx="7" ry="3" stroke="#4ec9b0" strokeWidth="1.6" vectorEffect="non-scaling-stroke"/>
                  <path d="M5 5v6c0 1.66 3.13 3 7 3s7-1.34 7-3V5" stroke="#4ec9b0" strokeWidth="1.6" fill="none" vectorEffect="non-scaling-stroke"/>
                  <path d="M5 11v6c0 1.66 3.13 3 7 3s7-1.34 7-3v-6" stroke="#4ec9b0" strokeWidth="1.2" fill="none" vectorEffect="non-scaling-stroke"/>
                </svg>
                <span className="home-sidebar-name">{conn.name}</span>
                {conn.autoLogin && <span className="home-sidebar-badge">자동</span>}
              </div>
            ))}
            <div className="home-sidebar-add" onClick={() => setShowNewConn(true)}>
              <svg width="11" height="11" viewBox="0 0 24 24" fill="currentColor"><path d="M19 13H13v6h-2v-6H5v-2h6V5h2v6h6v2z"/></svg>
              새 연결
            </div>
          </div>

          {/* 메인 콘텐츠 */}
          <div className="home-main">

            {/* 헤더 */}
            <div className="home-header">
              <div className="home-header-icon">
                <svg width="48" height="58" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                  <ellipse cx="12" cy="5" rx="9" ry="3.5" stroke="#4ec9b0" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                  <path d="M3 5v6c0 1.93 4.03 3.5 9 3.5s9-1.57 9-3.5V5" stroke="#4ec9b0" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                  <path d="M3 11v6c0 1.93 4.03 3.5 9 3.5s9-1.57 9-3.5v-6" stroke="#4ec9b0" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                </svg>
              </div>
              <div>
                <h1 className="home-title">RustDB Connections</h1>
                <p className="home-desc">연결할 데이터베이스를 선택하거나 새 연결을 추가하세요.</p>
              </div>
            </div>

            {/* 연결 목록 헤더 */}
            <div className="home-section-bar">
              <span className="home-section-label">저장된 연결</span>
              <button className="home-add-btn" onClick={() => setShowNewConn(true)}>
                <svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor"><path d="M19 13H13v6h-2v-6H5v-2h6V5h2v6h6v2z"/></svg>
                새 연결
              </button>
            </div>

            {/* 연결 카드 그리드 */}
            <div className="home-conn-grid">
              {connections.map(conn => (
                <div
                  key={conn.id}
                  className="home-conn-card"
                  onClick={() => {
                    if (conn.autoLogin && conn.password) { handleAutoLogin(conn); }
                    else { setConnectingTo(conn); setDlgPass(conn.password ?? ""); setDlgError(""); }
                  }}
                >
                  <div className="home-conn-card-icon">
                    <svg width="28" height="34" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                      <ellipse cx="12" cy="5" rx="8" ry="3.5" stroke="#4ec9b0" strokeWidth="1.4" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 5v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5V5" stroke="#4ec9b0" strokeWidth="1.4" fill="none" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 11v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5v-6" stroke="#4ec9b0" strokeWidth="1.4" fill="none" vectorEffect="non-scaling-stroke"/>
                    </svg>
                  </div>
                  <div className="home-conn-info">
                    <div className="home-conn-name">{conn.name}</div>
                    <div className="home-conn-meta">
                      <span className="home-conn-chip">
                        <svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><path d="M12 12c2.7 0 4.8-2.1 4.8-4.8S14.7 2.4 12 2.4 7.2 4.5 7.2 7.2 9.3 12 12 12zm0 2.4c-3.2 0-9.6 1.6-9.6 4.8v2.4h19.2v-2.4c0-3.2-6.4-4.8-9.6-4.8z"/></svg>
                        {conn.user}
                      </span>
                      <span className="home-conn-chip" title={`데이터 디렉토리: ${conn.dataDir}`}>
                        <svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><path d="M20 6h-8l-2-2H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2z"/></svg>
                        {conn.dataDir}
                      </span>
                      {conn.autoLogin && <span className="home-conn-chip" style={{ color: "#4ec9b0" }}>자동 로그인</span>}
                    </div>
                  </div>
                  <button
                    className="home-conn-del"
                    title="삭제"
                    onClick={e => {
                      e.stopPropagation();
                      // localStorage 키 정리
                      localStorage.removeItem(`rustdb_tabs_${conn.id}`);
                      localStorage.removeItem(`rustdb_active_tab_${conn.id}`);
                      localStorage.removeItem(`rustdb_history_${conn.id}`);
                      localStorage.removeItem(`rustdb_query_${conn.id}`);
                      // 디스크 데이터 디렉토리 삭제
                      invoke("delete_conn_data", { dataDir: conn.dataDir });
                      saveConnections(connections.filter(c => c.id !== conn.id));
                    }}
                  >✕</button>
                </div>
              ))}
            </div>
          </div>
        </div>

        {/* ── 연결 다이얼로그 ── */}
        {connectingTo && (
          <div className="dlg-overlay" onClick={() => setConnectingTo(null)}>
            <div className="dlg-box" onClick={e => e.stopPropagation()}>
              <div className="dlg-header">
                <svg width="18" height="22" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                  <ellipse cx="12" cy="5" rx="8" ry="3.5" stroke="#4ec9b0" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 5v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5V5" stroke="#4ec9b0" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 11v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5v-6" stroke="#4ec9b0" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                </svg>
                <div>
                  <div className="dlg-title">{connectingTo.name}</div>
                  <div className="dlg-subtitle">{connectingTo.host}:{connectingTo.port} · {connectingTo.user}</div>
                </div>
              </div>
              <form onSubmit={handleConnect}>
                <div className="dlg-fields">
                  <div className="dlg-row">
                    <label>Password</label>
                    <div className="dlg-pass-wrap">
                      <input
                        type={dlgPassVisible ? "text" : "password"}
                        value={dlgPass}
                        onChange={e => setDlgPass(e.target.value)}
                        className="dlg-input"
                        placeholder="비밀번호 입력"
                        autoFocus
                      />
                      <button type="button" className="dlg-eye" tabIndex={-1}
                        onClick={() => setDlgPassVisible(v => !v)}>
                        {dlgPassVisible ? "🙈" : "👁"}
                      </button>
                    </div>
                  </div>
                </div>
                {dlgError && <div className="dlg-error">{dlgError}</div>}
                <div className="dlg-actions">
                  <button type="button" className="dlg-cancel" onClick={() => setConnectingTo(null)}>취소</button>
                  <button type="submit" className="dlg-connect" disabled={dlgLoading}>
                    {dlgLoading ? "연결 중..." : "연결"}
                  </button>
                </div>
              </form>
            </div>
          </div>
        )}

        {/* ── 새 연결 추가 다이얼로그 ── */}
        {showNewConn && (
          <div className="dlg-overlay" onClick={() => setShowNewConn(false)}>
            <div className="dlg-box dlg-box-wide" onClick={e => e.stopPropagation()}>
              <div className="dlg-header">
                <svg width="18" height="22" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                  <ellipse cx="12" cy="5" rx="8" ry="3.5" stroke="#4ec9b0" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 5v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5V5" stroke="#4ec9b0" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 11v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5v-6" stroke="#4ec9b0" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                </svg>
                <div>
                  <div className="dlg-title">새 연결 추가</div>
                  <div className="dlg-subtitle">접속 정보를 입력하세요</div>
                </div>
              </div>
              <div className="dlg-fields">
                <div className="dlg-section-label">사용자 정보</div>
                <div className="dlg-row">
                  <label>연결 이름</label>
                  <input type="text" value={newName} onChange={e => setNewName(e.target.value)} className="dlg-input" autoFocus/>
                </div>
                <div className="dlg-row">
                  <label>사용자 이름</label>
                  <input type="text" value={newUser} onChange={e => setNewUser(e.target.value)} className="dlg-input"/>
                </div>
                <div className="dlg-row">
                  <label>비밀번호</label>
                  <div className="dlg-pass-wrap">
                    <input
                      type={newPassVisible ? "text" : "password"}
                      value={newPass}
                      onChange={e => setNewPass(e.target.value)}
                      className="dlg-input"
                      placeholder="비밀번호"
                    />
                    <button type="button" className="dlg-eye" tabIndex={-1}
                      onClick={() => setNewPassVisible(v => !v)}>
                      {newPassVisible ? "🙈" : "👁"}
                    </button>
                  </div>
                </div>
                <div className="dlg-section-label" style={{ marginTop: 14 }}>세부 정보</div>
                <div className="dlg-field-row">
                  <div className="dlg-row" style={{ flex: 1 }}>
                    <label>호스트 이름 / 주소</label>
                    <input type="text" value={newHost} onChange={e => setNewHost(e.target.value)} className="dlg-input"/>
                  </div>
                  <div className="dlg-row" style={{ flex: "0 0 90px" }}>
                    <label>포트</label>
                    <input type="text" value={newPort} onChange={e => setNewPort(e.target.value)} className="dlg-input"/>
                  </div>
                </div>
                <div className="dlg-row dlg-row-toggle">
                  <label>자동 로그인</label>
                  <button
                    type="button"
                    className={`dlg-toggle ${newAutoLogin ? "dlg-toggle-on" : ""}`}
                    onClick={() => setNewAutoLogin(v => !v)}
                  >
                    <span className="dlg-toggle-knob"/>
                  </button>
                </div>
              </div>
              <div className="dlg-actions">
                <button type="button" className="dlg-cancel" onClick={() => setShowNewConn(false)}>취소</button>
                <button type="button" className="dlg-connect" onClick={handleAddConnection}>추가</button>
              </div>
            </div>
          </div>
        )}
      </div>
    );
  }

  return (
    <div className="app">

      {/* ── 메뉴바 ───────────────────────────────────────────────── */}
      <div className="menu-bar" onClick={e => e.stopPropagation()}>
        {menus.map(menu => (
          <div
            key={menu.label}
            className={`menu-item ${openMenu === menu.label ? "open" : ""}`}
            onClick={() => setOpenMenu(prev => prev === menu.label ? null : menu.label)}
            onMouseEnter={() => { if (openMenu !== null) setOpenMenu(menu.label); }}
          >
            <span>{menu.label}</span>
            {openMenu === menu.label && (
              <div className="menu-dropdown">
                {menu.items.map((item, i) =>
                  item.divider ? (
                    <div key={i} className="menu-divider" />
                  ) : (
                    <div
                      key={i}
                      className="menu-dropdown-item"
                      onClick={() => { setOpenMenu(null); item.action?.(); }}
                    >
                      <span>{item.label}</span>
                      {item.shortcut && <span className="menu-shortcut">{item.shortcut}</span>}
                    </div>
                  )
                )}
              </div>
            )}
          </div>
        ))}
        {/* 우측 세션 정보 */}
        <div className="menu-bar-right">
          <span className="menu-session-user">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor" style={{ marginRight: 4 }}>
              <path d="M12 12c2.7 0 4.8-2.1 4.8-4.8S14.7 2.4 12 2.4 7.2 4.5 7.2 7.2 9.3 12 12 12zm0 2.4c-3.2 0-9.6 1.6-9.6 4.8v2.4h19.2v-2.4c0-3.2-6.4-4.8-9.6-4.8z"/>
            </svg>
            {sessionUser}
          </span>
          <button
            className="menu-logout-btn"
            onClick={() => {
              connIdRef.current = "";
              setSessionConnId("");
              setSessionUser("");
              setLoggedIn(false);
              setTabs([{ id: "1", name: "query.sql", content: "SHOW TABLES;" }]);
              setActiveTabId("1");
              setQueryHistory([]);
              setTabResults({});
              setDlgPass(""); setDlgError("");
            }}
          >로그아웃</button>
        </div>
      </div>

      {/* ── 본문 (액티비티 바 + 콘텐츠) ─────────────────────────── */}
      <div className="app-body">

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

        {/* ERD Editor */}
        <div
          className={`activity-icon ${activeView === "erd" ? "active" : ""}`}
          title="ERD Editor"
          onClick={() => setActiveView("erd")}
        >
          <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
            <rect x="1" y="2" width="9" height="6" rx="1.5"/>
            <rect x="1" y="16" width="9" height="6" rx="1.5"/>
            <rect x="14" y="9" width="9" height="6" rx="1.5"/>
            <path d="M10 5H12V12H14"/>
            <path d="M10 19H12V12"/>
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

        {/* AI Assistant */}
        <div
          className={`activity-icon ${activeView === "ai" ? "active" : ""}`}
          title="AI Assistant"
          onClick={() => setActiveView("ai")}
        >
          <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
            <path d="M12 2l1.5 4.5L18 8l-4.5 1.5L12 14l-1.5-4.5L6 8l4.5-1.5L12 2z"/>
            <path d="M19 14l.75 2.25L22 17l-2.25.75L19 20l-.75-2.25L16 17l2.25-.75L19 14z"/>
            <path d="M5 17l.5 1.5L7 19l-1.5.5L5 21l-.5-1.5L3 19l1.5-.5L5 17z"/>
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

      {/* ── 에디터 뷰 ──────────────────────────────────────────── */}
      {activeView === "editor" && (
        <>
          <div className="sidebar" style={{ width: `${sidebarWidth}px` }}>
            <div className="sidebar-title">SCHEMAS</div>
            <input
              className="sidebar-search"
              placeholder="테이블 검색..."
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
                const data = dbData[dbName] ?? { tables: [], views: [], indexes: [] };
                const tOpen = tablesOpen[dbName] ?? true;
                const vOpen = viewsOpen[dbName] ?? true;
                const iOpen = indexesOpen[dbName] ?? true;

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
                      title={isActive ? "현재 데이터베이스" : "더블클릭으로 전환"}
                    >
                      <span className="sidebar-group-arrow">{isOpen ? "▼" : "▶"}</span>
                      <span className="sidebar-db-icon">🗄</span>
                      <span className="sidebar-db-name">{dbName}{isActive ? " ◀" : ""}</span>
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
                          ) : data.tables.filter(t => !sidebarSearch || t.toLowerCase().includes(sidebarSearch.toLowerCase())).map(t => (
                            <div key={t}>
                              <div
                                className={`sidebar-item sidebar-item-nested ${expandedTables.has(t) ? "active" : ""}`}
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
                                <div className="sidebar-columns sidebar-columns-nested">
                                  {tableColumns[t].map(col => (
                                    <div key={col.name} className="sidebar-column sidebar-column-nested" title={[
                                      col.data_type,
                                      col.is_pk ? "PRIMARY KEY" : "",
                                      col.is_not_null ? "NOT NULL" : "",
                                      col.is_unique && !col.is_pk ? "UNIQUE" : "",
                                      col.is_auto_inc ? "AUTO INCREMENT" : "",
                                      col.default_val ? `DEFAULT ${col.default_val}` : "",
                                      col.fk_ref ? `FK → ${col.fk_ref}` : "",
                                    ].filter(Boolean).join(" | ")}>
                                      <span className="col-icon" style={{ color: col.is_pk ? "#f0c040" : col.fk_ref ? "#9cdcfe" : "var(--text-muted)" }}>
                                        {col.is_pk ? "🔑" : col.fk_ref ? "🔗" : "≡"}
                                      </span>
                                      <span className="col-name">{col.name}</span>
                                      <span className="col-type">{col.data_type}</span>
                                      {col.is_not_null && <span className="col-badge col-badge-nn">NN</span>}
                                      {col.is_unique && !col.is_pk && <span className="col-badge col-badge-uq">UQ</span>}
                                    </div>
                                  ))}
                                </div>
                              )}
                            </div>
                          )))}
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

                        {/* ── INDEXES ── */}
                        <div className="sidebar-group sidebar-group-nested">
                          <div className="sidebar-group-header sidebar-section-header" onClick={() => setIndexesOpen(p => ({ ...p, [dbName]: !iOpen }))}>
                            <span className="sidebar-group-arrow">{iOpen ? "▼" : "▶"}</span>
                            <span className="sidebar-section-icon">⌗</span>
                            Indexes
                            <span className="sidebar-badge">{data.indexes.length}</span>
                          </div>
                          {iOpen && (data.indexes.length === 0 ? (
                            <div className="sidebar-empty sidebar-empty-nested">No indexes yet</div>
                          ) : data.indexes.map(idx => (
                            <div key={idx.name}>
                              <div
                                className={`sidebar-item sidebar-item-nested sidebar-index-item ${expandedIndexes.has(idx.name) ? "active" : ""}`}
                                onClick={() => toggleIndex(idx.name)}
                              >
                                <span className="sidebar-arrow">{expandedIndexes.has(idx.name) ? "▼" : "▶"}</span>
                                <span className="sidebar-index-icon">{idx.kind === "composite" ? "⋈" : "⌗"}</span>
                                <span className="sidebar-name">{idx.name}</span>
                                <span className="sidebar-index-table">{idx.table}</span>
                              </div>
                              {expandedIndexes.has(idx.name) && (
                                <div className="sidebar-columns sidebar-columns-nested">
                                  <div className="sidebar-column sidebar-column-nested" style={{ color: "var(--text-muted)", fontSize: "0.75rem" }}>
                                    {idx.kind === "composite" ? "composite" : "single"}
                                  </div>
                                  {idx.columns.map(col => (
                                    <div key={col} className="sidebar-column sidebar-column-nested">
                                      <span className="col-icon">◉</span>
                                      <span>{col}</span>
                                    </div>
                                  ))}
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
              <div className="sidebar-info-item"><span className="col-icon">◉</span> v2.2.0</div>
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

          {/* DB 우클릭 컨텍스트 메뉴 */}
          {dbCtxMenu && (
            <div
              className="ctx-menu table-ctx-menu"
              style={{ top: dbCtxMenu.y, left: dbCtxMenu.x }}
              onClick={e => e.stopPropagation()}
            >
              <div className="ctx-menu-header">{dbCtxMenu.db}</div>
              <div className="ctx-divider" />
              <div onClick={async () => {
                setDbCtxMenu(null);
                await invoke<MultiQueryResult>("execute_query", { query: `USE ${dbCtxMenu.db};`, ts: Date.now() });
                await refreshSidebar();
              }}>Set as Default Schema</div>
              <div className="ctx-divider" />
              <div onClick={() => {
                setDbCtxMenu(null);
                setEditorQuery(
                  `CREATE TABLE ${dbCtxMenu.db}.table_name (\n  id INT PRIMARY KEY AUTO INCREMENT,\n  name VARCHAR(50) NOT NULL\n);`
                );
              }}>Create Table...</div>
              <div className="ctx-divider" />
              <div onClick={() => {
                navigator.clipboard.writeText(dbCtxMenu.db);
                setDbCtxMenu(null);
              }}>Copy Schema Name</div>
              <div className="ctx-divider" />
              <div className="ctx-item-danger" onClick={() => runDbCtxQuery(`DROP DATABASE ${dbCtxMenu.db};`)}>
                Drop Schema...
              </div>
            </div>
          )}

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
              <div onClick={() => runCtxQuery(`SHOW CREATE TABLE ${tableCtxMenu.table};`)}>Show Create Table</div>
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
              <div className="tab-list">
                {tabs.map(tab => (
                  <div
                    key={tab.id}
                    className={`tab ${tab.id === activeTabId ? "active" : ""}`}
                    onClick={() => switchTab(tab.id)}
                    onDoubleClick={e => {
                      e.stopPropagation();
                      setEditingTabId(tab.id);
                      setEditingTabName(tab.name);
                    }}
                  >
                    <span className="tab-icon">⊞</span>
                    {editingTabId === tab.id ? (
                      <input
                        className="tab-name-input"
                        value={editingTabName}
                        autoFocus
                        onChange={e => setEditingTabName(e.target.value)}
                        onBlur={commitTabRename}
                        onKeyDown={e => {
                          if (e.key === "Enter") { e.preventDefault(); commitTabRename(); }
                          if (e.key === "Escape") { e.preventDefault(); setEditingTabId(null); }
                        }}
                        onClick={e => e.stopPropagation()}
                      />
                    ) : (
                      tab.name
                    )}
                    <span
                      className="tab-close"
                      onClick={e => closeTab(tab.id, e)}
                      title="Close tab"
                    >×</span>
                  </div>
                ))}
                <div className="tab-add-wrap">
                  <button className="tab-add-btn" onClick={addTab} title="New query tab">+</button>
                </div>
              </div>
              <div className="tab-bar-right">
                <button className="bookmark-btn" onClick={addBookmark} title="현재 쿼리 북마크 추가 (★)">★</button>
                <button className="run-btn" onClick={runQuery} disabled={isRunning}>
                  {isRunning ? "⏳" : "▶ Run"}
                </button>
              </div>
            </div>

            <div className="breadcrumb">
              <span>{currentDb}</span>
              <span className="breadcrumb-sep">›</span>
              <span>query</span>
              <span className="breadcrumb-sep">›</span>
              <span className="breadcrumb-active">{activeTab?.name ?? "query.sql"}</span>
            </div>

            <div className="editor-container">
              <Editor
                height="100%"
                defaultLanguage="sql"
                defaultValue={queryRef.current}
                onChange={val => {
                  if (isSwitchingTab.current) return;
                  queryRef.current = val ?? "";
                  setTabs(prev => {
                    const next = prev.map(t => t.id === activeTabId ? { ...t, content: queryRef.current } : t);
                    localStorage.setItem(`rustdb_tabs_${connIdRef.current}`, JSON.stringify(next));
                    return next;
                  });
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

                  // SQL 자동완성
                  monaco.languages.registerCompletionItemProvider("sql", {
                    triggerCharacters: [" ", ".", "\n"],
                    provideCompletionItems: (model: Monaco.editor.ITextModel, position: Monaco.Position) => {
                      const word = model.getWordUntilPosition(position);
                      const range = {
                        startLineNumber: position.lineNumber,
                        endLineNumber: position.lineNumber,
                        startColumn: word.startColumn,
                        endColumn: word.endColumn,
                      };
                      const { tables, columns } = schemaRef.current;
                      const suggestions: Monaco.languages.CompletionItem[] = [];
                      const kws = [
                        "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","SET",
                        "DELETE","CREATE","TABLE","DROP","ALTER","ADD","COLUMN","RENAME","TO",
                        "JOIN","LEFT","RIGHT","INNER","ON","AND","OR","NOT",
                        "ORDER","GROUP","BY","ASC","DESC","LIMIT","OFFSET","HAVING","IN",
                        "BETWEEN","LIKE","AS","DISTINCT","UNION","ALL",
                        "COUNT","SUM","AVG","MIN","MAX","GROUP_CONCAT",
                        "INDEX","UNIQUE","VIEW","PRIMARY","KEY","FOREIGN","REFERENCES",
                        "CASCADE","RESTRICT","NULL","AUTO","INCREMENT",
                        "SHOW","TABLES","DESCRIBE","TRUNCATE","IS","IS NULL","IS NOT NULL",
                        "BEGIN","COMMIT","ROLLBACK","SAVEPOINT",
                        "CHECKPOINT","ISOLATION","LEVEL","VACUUM","EXPLAIN","USE","DATABASE",
                        "IF","EXISTS","CASE","WHEN","THEN","ELSE","END","WITH","RECURSIVE",
                        "COALESCE","IFNULL","NULLIF","CAST",
                        "UPPER","LOWER","LENGTH","CONCAT","TRIM","SUBSTR","REPLACE","LPAD","RPAD",
                        "ROUND","ABS","CEIL","FLOOR","MOD",
                        "NOW","DATEDIFF","DATE_ADD","DATE_FORMAT","CURDATE",
                        "INT","TEXT","FLOAT","BOOLEAN","VARCHAR","DATETIME","DATE","ENUM",
                      ];
                      for (const kw of kws) {
                        suggestions.push({ label: kw, kind: monaco.languages.CompletionItemKind.Keyword, insertText: kw, range });
                      }
                      for (const t of tables) {
                        suggestions.push({ label: t, kind: monaco.languages.CompletionItemKind.Class, insertText: t, range, detail: "table" });
                      }
                      for (const [tbl, cols] of Object.entries(columns)) {
                        for (const col of cols) {
                          suggestions.push({ label: col, kind: monaco.languages.CompletionItemKind.Field, insertText: col, range, detail: tbl });
                        }
                      }
                      return { suggestions };
                    },
                  });
                }}
                onMount={(editor, monaco) => {
                  editorRef.current = editor;
                  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter, runQuery);
                  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyMod.Shift | monaco.KeyCode.KeyF, () => {
                    try {
                      const fmt = sqlFormat(editor.getValue(), { language: 'sql', tabWidth: 2, keywordCase: 'upper' });
                      editor.setValue(fmt);
                    } catch {}
                  });
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
                <div
                  className={`result-tab ${resultTab === "results" ? "active" : ""}`}
                  onClick={() => setResultTab("results")}
                >RESULTS</div>
                <div
                  className={`result-tab ${resultTab === "history" ? "active" : ""}`}
                  onClick={() => setResultTab("history")}
                >HISTORY {queryHistory.length > 0 && <span className="history-count">{queryHistory.length}</span>}</div>
                {resultTab === "results" && results.length > 0 && (
                  <div className={`result-status ${results.every(r => r.success) ? "ok" : "err"}`}>
                    {results.every(r => r.success) ? `✓ ${results.length} query(s)` : "✗ Error"}
                  </div>
                )}
              </div>

              {resultTab === "history" ? (
                <div className="result-content">
                  {queryHistory.length === 0 ? (
                    <div className="result-empty">실행 기록이 없습니다</div>
                  ) : (
                    <>
                      <div className="history-toolbar">
                        <span className="history-toolbar-info">{queryHistory.length}개 기록</span>
                        <button className="history-clear-btn" onClick={() => {
                          setQueryHistory([]);
                          saveHistory(connIdRef.current, []);
                        }}>전체 삭제</button>
                      </div>
                      {queryHistory.map(h => {
                        const d = new Date(h.ts);
                        const time = d.toLocaleTimeString("ko-KR", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
                        const date = d.toLocaleDateString("ko-KR", { month: "2-digit", day: "2-digit" });
                        const firstLine = h.sql.split("\n")[0].trim();
                        const preview = firstLine.length > 80 ? firstLine.slice(0, 80) + "…" : firstLine;
                        return (
                          <div
                            key={h.id}
                            className="history-item"
                            onClick={() => setEditorQuery(h.sql)}
                            title="클릭하면 에디터에 불러옵니다"
                          >
                            <div className="history-item-header">
                              <span className={`history-icon ${h.success ? "ok" : "err"}`}>
                                {h.success ? "✓" : "✗"}
                              </span>
                              <span className="history-time">{date} {time}</span>
                              <span className="history-elapsed">{h.elapsed.toFixed(3)}s</span>
                            </div>
                            <div className="history-sql">{preview}</div>
                          </div>
                        );
                      })}
                    </>
                  )}
                </div>
              ) : (
              <div
                className="result-content"
                onContextMenu={e => { e.preventDefault(); setCtxMenu({ x: e.clientX, y: e.clientY }); }}
                onClick={e => { if (ctxMenu) { e.stopPropagation(); setCtxMenu(null); } }}
              >
                {results.length > 0 && (
                  <div className="result-search-bar">
                    <input
                      className="result-search-input"
                      placeholder="결과 내 검색..."
                      value={resultSearch}
                      onChange={e => setTabResultSearch(p => ({ ...p, [activeTabId]: e.target.value }))}
                    />
                    {resultSearch && (
                      <span className="result-search-clear" onClick={() => setTabResultSearch(p => ({ ...p, [activeTabId]: "" }))}>×</span>
                    )}
                  </div>
                )}
                {results.length === 0 ? (
                  <div className="result-empty">Ctrl+Enter 또는 ▶ Run 으로 쿼리를 실행하세요</div>
                ) : results.map((r, i) => (
                  <div key={i} className="result-block">
                    {!r.success ? (
                      <div className="result-error">❌ {r.message}</div>
                    ) : r.columns.length === 0 ? (() => {
                      // EXPLAIN 트리 시각화
                      if (r.message.includes("QUERY PLAN")) {
                        const planLines = r.message.split("\n")
                          .filter(l => l.startsWith("|") && !l.includes("QUERY PLAN"))
                          .map(l => l.replace(/^\|\s*/, "").replace(/\s*\|$/, "").trim())
                          .filter(Boolean);
                        return (
                          <div className="explain-tree">
                            <div className="explain-tree-header">
                              <span className="explain-tree-icon">⟳</span> QUERY PLAN · {r.elapsed.toFixed(3)}s
                            </div>
                            {planLines.map((line, li) => {
                              const [label, ...rest] = line.split(":");
                              const value = rest.join(":").trim();
                              return (
                                <div key={li} className="explain-tree-row">
                                  <span className="explain-tree-label">{label.trim()}</span>
                                  {value && <span className="explain-tree-value">{value}</span>}
                                </div>
                              );
                            })}
                          </div>
                        );
                      }
                      return <div className="result-msg">✅ {r.message} · {r.elapsed.toFixed(3)}s</div>;
                    })() : (() => {
                        const sortInfo = sortState[i] ?? null;
                        const low = resultSearch.toLowerCase();
                        let rows = resultSearch
                          ? r.rows.filter(row => row.some(c => c.toLowerCase().includes(low)))
                          : r.rows;
                        if (sortInfo) {
                          rows = [...rows].sort((a, b) => {
                            const av = a[sortInfo.col] ?? "";
                            const bv = b[sortInfo.col] ?? "";
                            const an = parseFloat(av), bn = parseFloat(bv);
                            const cmp = !isNaN(an) && !isNaN(bn) ? an - bn : av.localeCompare(bv);
                            return sortInfo.dir === 'asc' ? cmp : -cmp;
                          });
                        }
                        const page = resultPages[i] ?? 0;
                        const total = rows.length;
                        const pageCount = Math.ceil(total / PAGE_SIZE);
                        const pageRows = rows.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE);
                        const toggleSort = (ci: number) => {
                          setTabSortState(prev => {
                            const tab = { ...(prev[activeTabId] ?? {}) };
                            const cur = tab[i];
                            if (!cur || cur.col !== ci) tab[i] = { col: ci, dir: 'asc' };
                            else if (cur.dir === 'asc') tab[i] = { col: ci, dir: 'desc' };
                            else tab[i] = null;
                            return { ...prev, [activeTabId]: tab };
                          });
                          setTabResultPages(p => ({ ...p, [activeTabId]: { ...(p[activeTabId] ?? {}), [i]: 0 } }));
                        };
                        return (
                          <>
                            <div className="result-info">
                              {resultSearch ? `${total} / ${r.rows.length}` : total} row(s) · {r.elapsed.toFixed(3)}s
                              <button
                                className="csv-btn"
                                title="CSV로 내보내기"
                                onClick={() => {
                                  const escape = (v: string) =>
                                    /[",\n\r]/.test(v) ? `"${v.replace(/"/g, '""')}"` : v;
                                  const lines = [
                                    r.columns.map(escape).join(","),
                                    ...r.rows.map(row => row.map(escape).join(",")),
                                  ].join("\r\n");
                                  const blob = new Blob(["﻿" + lines], { type: "text/csv;charset=utf-8" });
                                  const a = document.createElement("a");
                                  a.href = URL.createObjectURL(blob);
                                  a.download = `result_${i + 1}.csv`;
                                  a.click();
                                  URL.revokeObjectURL(a.href);
                                }}
                              >⬇ CSV</button>
                              {pageCount > 1 && (
                                <span className="result-page-info">
                                  &nbsp;· 표시: {page * PAGE_SIZE + 1}–{Math.min((page + 1) * PAGE_SIZE, total)} / {total}
                                  <button className="page-btn" disabled={page === 0}
                                    onClick={() => setTabResultPages(p => ({ ...p, [activeTabId]: { ...(p[activeTabId] ?? {}), [i]: page - 1 } }))}>‹</button>
                                  <span className="page-indicator">{page + 1} / {pageCount}</span>
                                  <button className="page-btn" disabled={page >= pageCount - 1}
                                    onClick={() => setTabResultPages(p => ({ ...p, [activeTabId]: { ...(p[activeTabId] ?? {}), [i]: page + 1 } }))}>›</button>
                                </span>
                              )}
                            </div>
                            <table className="result-table" style={{ tableLayout: colWidths[i] ? 'fixed' : undefined }}>
                              <thead><tr>
                                <th className="result-rownum">#</th>
                                {r.columns.map((c, ci) => (
                                <th key={c} style={{ width: colWidths[i]?.[ci], position: 'relative', userSelect: 'none', cursor: 'pointer' }}
                                    onClick={() => toggleSort(ci)}>
                                  <span className="result-th-label">{c}</span>
                                  <span className="result-sort-icon">
                                    {sortInfo?.col === ci ? (sortInfo.dir === 'asc' ? ' ▲' : ' ▼') : ' ⇅'}
                                  </span>
                                  <div
                                    style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 4, cursor: 'col-resize' }}
                                    onMouseDown={e => {
                                      e.stopPropagation();
                                      e.preventDefault();
                                      const thEl = e.currentTarget.parentElement as HTMLTableCellElement;
                                      const startX = e.clientX;
                                      const startW = thEl.getBoundingClientRect().width;
                                      const initWidths = Array.from(thEl.parentElement!.querySelectorAll<HTMLTableCellElement>('th'))
                                        .map(th => th.getBoundingClientRect().width);
                                      const onMove = (mv: MouseEvent) => {
                                        setTabColWidths(prev => {
                                          const tab = { ...(prev[activeTabId] ?? {}) };
                                          const arr = [...(tab[i] ?? initWidths)];
                                          arr[ci] = Math.max(40, startW + mv.clientX - startX);
                                          tab[i] = arr;
                                          return { ...prev, [activeTabId]: tab };
                                        });
                                      };
                                      const onUp = () => {
                                        window.removeEventListener('mousemove', onMove);
                                        window.removeEventListener('mouseup', onUp);
                                      };
                                      window.addEventListener('mousemove', onMove);
                                      window.addEventListener('mouseup', onUp);
                                    }}
                                  />
                                </th>
                              ))}</tr></thead>
                              <tbody>{pageRows.map((row, ri) => (
                                <tr key={ri}>
                                  <td className="result-rownum">{page * PAGE_SIZE + ri + 1}</td>
                                  {row.map((cell, ci) => (
                                    <td key={ci} style={colWidths[i] ? { overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' } : undefined}>{cell}</td>
                                  ))}
                                </tr>
                              ))}</tbody>
                            </table>
                          </>
                        );
                      })()}
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
                    <div onClick={() => { setTabResults(p => ({ ...p, [activeTabId]: [] })); setCtxMenu(null); }}>Clear Results</div>
                  </div>
                )}
              </div>
              )}
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
                <span className="status-item">RustDB v2.2.0</span>
                <span className="status-item">UTF-8</span>
                <span className="status-item">SQL</span>
                <span className="status-item">B+Tree · WAL · MVCC</span>
              </div>
            </div>
          </div>
        </>
      )}

      {/* ── ERD Editor 뷰 ────────────────────────────────────── */}
      {activeView === "erd" && (
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
              <button className="erd-tool-btn" onClick={() => { setErdPan({ x: 40, y: 40 }); setErdZoom(1); }} title="Reset view">⊡ Reset</button>
              <button className="erd-tool-btn" onClick={loadErd} title="Refresh">↻ Refresh</button>
              <span className="erd-zoom-label">{Math.round(erdZoom * 100)}%</span>
            </div>
          </div>

          <div
            className="erd-canvas"
            ref={erdCanvasRef}
            onMouseDown={e => {
              if ((e.target as HTMLElement).closest(".erd-card")) return;
              erdCanvasDragRef.current = { startMX: e.clientX, startMY: e.clientY, startPX: erdPan.x, startPY: erdPan.y };
              document.body.style.cursor = "grabbing";
              document.body.style.userSelect = "none";
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
                <div className="erd-empty-sub">테이블 생성 후 ↻ Refresh를 눌러주세요</div>
              </div>
            ) : (
              <div
                className="erd-transform"
                style={{ transform: `translate(${erdPan.x}px, ${erdPan.y}px) scale(${erdZoom})`, transformOrigin: "0 0" }}
              >
                {/* FK 관계선 SVG */}
                <svg style={{ position: "absolute", top: 0, left: 0, width: 1, height: 1, overflow: "visible", pointerEvents: "none", zIndex: 0 }}>
                  <defs>
                    <marker id="erd-arrow" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto">
                      <path d="M0,1 L0,7 L7,4 z" fill="#6a9fd8" opacity="0.85"/>
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
                      let pathD: string;
                      if (srcRight + 10 <= tgtPos.x) {
                        // 소스가 타깃 왼쪽: 오른쪽 → 왼쪽
                        pathD = erdOrthPath(srcRight, srcY, tgtPos.x, tgtY);
                      } else if (tgtRight + 10 <= srcPos.x) {
                        // 소스가 타깃 오른쪽: 왼쪽 → 오른쪽
                        pathD = erdOrthPath(srcPos.x, srcY, tgtRight, tgtY);
                      } else {
                        // 수평 겹침: 오른쪽 바깥으로 우회
                        const detourX = Math.max(srcRight, tgtRight) + 44;
                        const sdy = Math.sign(tgtY - srcY);
                        const r = Math.min(8, Math.abs(tgtY - srcY) / 2);
                        if (r < 1) {
                          pathD = `M${srcRight} ${srcY} H${detourX} V${tgtY} H${tgtRight}`;
                        } else {
                          pathD = [
                            `M${srcRight} ${srcY}`,
                            `H${detourX - r}`,
                            `Q${detourX} ${srcY} ${detourX} ${srcY + sdy * r}`,
                            `V${tgtY - sdy * r}`,
                            `Q${detourX} ${tgtY} ${detourX - r} ${tgtY}`,
                            `H${tgtRight}`,
                          ].join(" ");
                        }
                      }
                      return (
                        <path
                          key={`${tableName}.${col.name}`}
                          d={pathD}
                          fill="none"
                          stroke="#6a9fd8"
                          strokeWidth="1.5"
                          opacity="0.7"
                          markerEnd="url(#erd-arrow)"
                        />
                      );
                    })
                  )}
                </svg>

                {/* 테이블 카드 */}
                {Object.entries(erdColumns).map(([tableName, cols]) => {
                  const pos = erdPositions[tableName];
                  if (!pos) return null;
                  return (
                    <div
                      key={tableName}
                      className={`erd-card${erdSelectedTable === tableName ? " erd-card-selected" : ""}`}
                      style={{ position: "absolute", left: pos.x, top: pos.y, width: ERD_CARD_W, zIndex: 1 }}
                      onClick={() => { if (!erdCardWasDragged.current) handleErdCardClick(tableName); }}
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
                        <span className="erd-card-icon">⊞</span>
                        <span className="erd-card-name">{tableName}</span>
                      </div>
                      {cols.map(col => (
                        <div
                          key={col.name}
                          className={`erd-col-row${col.is_pk ? " erd-pk" : col.fk_ref ? " erd-fk" : ""}`}
                        >
                          <span className="erd-col-icon">{col.is_pk ? "🔑" : col.fk_ref ? "🔗" : "·"}</span>
                          <span className="erd-col-name">{col.name}</span>
                          <span className="erd-col-type">{col.data_type.split("(")[0]}</span>
                          {col.is_not_null && <span className="erd-badge-nn">NN</span>}
                          {col.is_unique && !col.is_pk && <span className="erd-badge-uq">UQ</span>}
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
                    return (
                      <>
                        <div className="erd-data-meta">
                          {filtered.length}{erdFilter ? ` / ${erdTableData.rows.length}` : ""} row(s) · {erdTableData.columns.length} col(s) · {erdTableData.elapsed.toFixed(3)}s
                        </div>
                        <table className="erd-data-table">
                          <thead><tr>
                            <th className="erd-data-rownum">#</th>
                            {erdTableData.columns.map(c => <th key={c}>{c}</th>)}
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
              <span className="status-item">RustDB v2.2.0</span>
              <span className="status-item">ERD Editor</span>
            </div>
          </div>
        </div>
      )}

      {/* ── AI Assistant 뷰 ───────────────────────────────────── */}
      {activeView === "ai" && (
        <div className="ai-view">
          <div className="ai-header">
            <div className="ai-header-left">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor" style={{ opacity: 0.8 }}>
                <path d="M12 2l1.5 4.5L18 8l-4.5 1.5L12 14l-1.5-4.5L6 8l4.5-1.5L12 2z"/>
                <path d="M19 14l.75 2.25L22 17l-2.25.75L19 20l-.75-2.25L16 17l2.25-.75L19 14z"/>
                <path d="M5 17l.5 1.5L7 19l-1.5.5L5 21l-.5-1.5L3 19l1.5-.5L5 17z"/>
              </svg>
              <span className="ai-header-title">AI Assistant</span>
            </div>
          </div>
          <div className="ai-body">
            <div className="ai-empty">
              <svg width="48" height="48" viewBox="0 0 24 24" fill="currentColor" style={{ opacity: 0.2 }}>
                <path d="M12 2l1.5 4.5L18 8l-4.5 1.5L12 14l-1.5-4.5L6 8l4.5-1.5L12 2z"/>
                <path d="M19 14l.75 2.25L22 17l-2.25.75L19 20l-.75-2.25L16 17l2.25-.75L19 14z"/>
                <path d="M5 17l.5 1.5L7 19l-1.5.5L5 21l-.5-1.5L3 19l1.5-.5L5 17z"/>
              </svg>
              <div className="ai-empty-title">AI Assistant</div>
              <div className="ai-empty-sub">Coming soon</div>
            </div>
          </div>
          <div className="status-bar">
            <div className="status-left">
              <span className="status-item">⎇ main</span>
            </div>
            <div className="status-right">
              <span className="status-item">RustDB v2.2.0</span>
              <span className="status-item">AI Assistant</span>
            </div>
          </div>
        </div>
      )}

      {/* ── 서버 관리 뷰 ───────────────────────────────────────── */}
      {activeView === "server" && (
        <div className="server-view">
          <div className="srv-scroll-area">
            {/* ── 연결 구성 패널 ── */}
            <div className="srv-conn-panel">

              {/* 아이콘 + 제목 */}
              <div className="srv-conn-header">
                <svg className="srv-db-icon" viewBox="0 0 48 48" fill="none" xmlns="http://www.w3.org/2000/svg">
                  {/* 섀시 외곽 */}
                  <rect x="2" y="3" width="44" height="42" rx="2.5" fill="#1e1e1e" stroke="#484848" strokeWidth="1.5"/>
                  {/* 서버 유닛 1 */}
                  <rect x="5" y="6" width="38" height="13" rx="1.5" fill="#2b2b2b" stroke="#3e3e3e" strokeWidth="0.8"/>
                  <rect x="8"  y="9.5"  width="18" height="1.5" rx="0.6" fill="#1a1a1a"/>
                  <rect x="8"  y="12.5" width="18" height="1.5" rx="0.6" fill="#1a1a1a"/>
                  <circle cx="30" cy="10.5" r="1.6" fill="#4ec9b0"/>
                  <circle cx="34" cy="10.5" r="1.6" fill="#4ec9b0" opacity="0.3"/>
                  <circle cx="38.5" cy="10.5" r="1.6" fill="#1a6ca8"/>
                  <rect x="29" y="14.5" width="12" height="2.5" rx="0.8" fill="#161616" stroke="#333" strokeWidth="0.5"/>
                  {/* 서버 유닛 2 */}
                  <rect x="5" y="21" width="38" height="13" rx="1.5" fill="#2b2b2b" stroke="#3e3e3e" strokeWidth="0.8"/>
                  <rect x="8"  y="24.5" width="18" height="1.5" rx="0.6" fill="#1a1a1a"/>
                  <rect x="8"  y="27.5" width="18" height="1.5" rx="0.6" fill="#1a1a1a"/>
                  <circle cx="30" cy="25.5" r="1.6" fill="#444"/>
                  <circle cx="34" cy="25.5" r="1.6" fill="#444"/>
                  <circle cx="38.5" cy="25.5" r="1.6" fill="#1a6ca8" opacity="0.55"/>
                  <rect x="29" y="29.5" width="12" height="2.5" rx="0.8" fill="#161616" stroke="#333" strokeWidth="0.5"/>
                  {/* 하단 패널 */}
                  <rect x="5" y="36" width="38" height="7" rx="1.5" fill="#232323" stroke="#3a3a3a" strokeWidth="0.6"/>
                  {/* 전원 버튼 */}
                  <circle cx="11" cy="39.5" r="2.6" fill="#2e2e2e" stroke="#505050" strokeWidth="1"/>
                  <circle cx="11" cy="39.5" r="1.1" fill="#4ec9b0"/>
                  {/* USB 포트들 */}
                  <rect x="17" y="38" width="4.5" height="3" rx="0.6" fill="#161616" stroke="#383838" strokeWidth="0.5"/>
                  <rect x="23" y="38" width="4.5" height="3" rx="0.6" fill="#161616" stroke="#383838" strokeWidth="0.5"/>
                  {/* 라벨 슬롯 */}
                  <rect x="29" y="38" width="11" height="3" rx="0.5" fill="#1a1a1a" stroke="#333" strokeWidth="0.4"/>
                </svg>
                <div>
                  <div className="srv-conn-title">서버에 연결</div>
                  <div className="srv-conn-sub">RustDB TCP Server</div>
                </div>
              </div>

              {/* 이름 + 그룹 행 */}
              <div className="srv-name-row">
                <div className="srv-name-field">
                  <label className="srv-name-label">이름</label>
                  <input
                    className="srv-name-input"
                    value={srvConnName}
                    onChange={e => setSrvConnName(e.target.value)}
                    placeholder="연결 이름"
                  />
                </div>
                <div className="srv-name-field" style={{ maxWidth: 180 }}>
                  <label className="srv-name-label">그룹</label>
                  <input className="srv-name-input" defaultValue="기본값" disabled />
                </div>
              </div>

              {/* 탭 바 */}
              <div className="srv-tab-bar">
                <button
                  className={`srv-tab ${srvTab === "main" ? "active" : ""}`}
                  onClick={() => setSrvTab("main")}
                >
                  <span className="srv-tab-icon">⚙</span> 메인
                </button>
                <button
                  className={`srv-tab ${srvTab === "guide" ? "active" : ""}`}
                  onClick={() => setSrvTab("guide")}
                >
                  <span className="srv-tab-icon">☰</span> CLI 가이드
                </button>
              </div>

              {/* ── 메인 탭 ── */}
              {srvTab === "main" && (
                <div className="srv-form-body">
                  {/* 호스트 / 포트 */}
                  <div className="srv-form-row">
                    <div className="srv-form-col">
                      <label className="srv-form-label"><span className="srv-req">*</span> 호스트</label>
                      <input className="srv-form-input" value="127.0.0.1" disabled />
                    </div>
                    <div className="srv-form-col" style={{ maxWidth: 160 }}>
                      <label className="srv-form-label"><span className="srv-req">*</span> 포트</label>
                      <div className="srv-port-wrap">
                        <button
                          className="srv-port-btn"
                          onClick={() => setPortInput(p => String(Math.max(1024, parseInt(p || "7878") - 1)))}
                          disabled={serverStatus.running}
                        >−</button>
                        <input
                          className="srv-form-input srv-port-num"
                          value={portInput}
                          onChange={e => setPortInput(e.target.value)}
                          disabled={serverStatus.running}
                        />
                        <button
                          className="srv-port-btn"
                          onClick={() => setPortInput(p => String(Math.min(65535, parseInt(p || "7878") + 1)))}
                          disabled={serverStatus.running}
                        >+</button>
                      </div>
                    </div>
                  </div>

                  {/* 사용자 / 비밀번호 */}
                  <div className="srv-form-row">
                    <div className="srv-form-col">
                      <label className="srv-form-label"><span className="srv-req">*</span> 사용자 이름</label>
                      <input
                        className="srv-form-input"
                        value={srvUser}
                        onChange={e => setSrvUser(e.target.value)}
                        placeholder="root"
                      />
                    </div>
                    <div className="srv-form-col">
                      <label className="srv-form-label"><span className="srv-req">*</span> 비밀번호</label>
                      <div className="srv-pass-wrap">
                        <input
                          className="srv-form-input"
                          type={srvPassVisible ? "text" : "password"}
                          value={srvPass}
                          onChange={e => setSrvPass(e.target.value)}
                          placeholder="비밀번호"
                        />
                        <button
                          className="srv-pass-toggle"
                          onClick={() => setSrvPassVisible(v => !v)}
                          title={srvPassVisible ? "숨기기" : "표시"}
                        >{srvPassVisible ? "🙈" : "👁"}</button>
                      </div>
                    </div>
                  </div>

                  {/* 인증 방식 */}
                  <div className="srv-form-row">
                    <div className="srv-form-col srv-full-col">
                      <label className="srv-form-label">인증 방식</label>
                      <div className="srv-auth-badge">RustDB AUTH Protocol v1 · 평문 (내부 네트워크 전용)</div>
                    </div>
                  </div>

                  <div className="srv-divider" />

                  {/* 상태 표시 */}
                  <div className="srv-status-strip">
                    <span className={`srv-dot ${serverStatus.running ? "running" : "stopped"}`} />
                    <span className={`srv-strip-text ${serverStatus.running ? "running" : "stopped"}`}>
                      {serverStatus.running
                        ? `RUNNING · 127.0.0.1:${serverStatus.port} · ${serverStatus.client_count} 클라이언트`
                        : "STOPPED"}
                    </span>
                  </div>

                  {serverMsg && <div className="srv-feedback">{serverMsg}</div>}

                  {/* 버튼 행 */}
                  <div className="srv-action-row">
                    <button
                      className="srv-action-btn primary"
                      onClick={handleStartServer}
                      disabled={serverStatus.running}
                    >▶ 서버 시작</button>
                    <button
                      className="srv-action-btn danger"
                      onClick={handleStopServer}
                      disabled={!serverStatus.running}
                    >■ 중지</button>
                    <div style={{ flex: 1 }} />
                    <button
                      className="srv-action-btn save"
                      onClick={() => setServerMsg("설정이 저장되었습니다.")}
                    >저장</button>
                  </div>
                </div>
              )}

              {/* ── CLI 가이드 탭 ── */}
              {srvTab === "guide" && (
                <div className="srv-form-body">
                  <div className="srv-guide-title">rustdb-client로 접속</div>
                  <code className="srv-guide-code">
                    {`cargo run -p rustdb-client -- -u ${srvUser} -p *** -h 127.0.0.1 -P ${portInput}`}
                  </code>

                  <div className="srv-guide-title" style={{ marginTop: 20 }}>인증 프로토콜 흐름</div>
                  <div className="srv-guide-flow">
                    <div className="srv-flow-row"><span className="srv-flow-arrow">→</span><code>AUTH {srvUser} &lt;password&gt;</code></div>
                    <div className="srv-flow-row"><span className="srv-flow-arrow">←</span><code>OK authenticated as '{srvUser}'</code></div>
                    <div className="srv-flow-row"><span className="srv-flow-arrow">→</span><code>SELECT * FROM t;</code></div>
                    <div className="srv-flow-row"><span className="srv-flow-arrow">←</span><code>OK{"\\n"}&lt;결과&gt;{"\\n"}(0.001 sec){"\\n"}---END---</code></div>
                  </div>

                  <div className="srv-guide-title" style={{ marginTop: 20 }}>Netcat / PowerShell</div>
                  <code className="srv-guide-code">nc 127.0.0.1 {portInput}</code>
                  <code className="srv-guide-code" style={{ marginTop: 8 }}>
                    {`$c = New-Object Net.Sockets.TcpClient('127.0.0.1',${portInput})`}
                  </code>
                </div>
              )}
            </div>
          </div>

          {/* ── 로그 패널 ── */}
          <div className="srv-log-panel">
            <div className="srv-log-header">
              <span className="srv-log-title">ACTIVITY LOG</span>
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
              <span className="status-item">RustDB v2.2.0</span>
              <span className="status-item">Server Manager</span>
            </div>
          </div>
        </div>
      )}

      </div> {/* app-body */}
    </div>
  );
}

export default App;
