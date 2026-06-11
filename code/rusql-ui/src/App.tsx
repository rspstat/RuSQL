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
interface SessionInfo {
  addr: string;
  user: string;
  connected_at: number;
  query_count: number;
}
interface ServerStatus {
  running: boolean;
  port: number;
  client_count: number;
  log: string[];
  sessions: SessionInfo[];
}
interface IndexInfo {
  name: string;
  table: string;
  columns: string[];
  kind: "single" | "composite" | "hash";
}
interface TriggerInfo {
  name: string;
  table: string;
  timing: string;
  event: string;
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
type ActiveView = "editor" | "erd" | "server";
const PAGE_SIZE = 100;

// ─── ERD 타입/상수 ────────────────────────────────────────────
interface ErdPos { x: number; y: number; }
const ERD_CARD_W = 360;
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

// ─── ERD 레이아웃 계산 (pure) ─────────────────────────────────
function computeErdLayout(columns: Record<string, ColumnDetail[]>): Record<string, ErdPos> {
  const allTables = Object.keys(columns);
  if (allTables.length === 0) return {};
  const deps: Record<string, Set<string>> = {};
  for (const t of allTables) deps[t] = new Set();
  for (const [tbl, cols] of Object.entries(columns)) {
    for (const col of cols) {
      if (col.fk_ref) {
        const parsed = parseRef(col.fk_ref);
        if (parsed) { const ref = unqualify(parsed.table); if (allTables.includes(ref)) deps[tbl].add(ref); }
      }
    }
  }
  const depth: Record<string, number> = {};
  const computing = new Set<string>();
  const getDepth = (t: string): number => {
    if (depth[t] !== undefined) return depth[t];
    if (computing.has(t)) { depth[t] = 0; return 0; }
    computing.add(t);
    depth[t] = deps[t].size === 0 ? 0 : Math.max(...Array.from(deps[t]).map(d => getDepth(d) + 1));
    computing.delete(t);
    return depth[t];
  };
  for (const t of allTables) getDepth(t);
  const byDepth: Record<number, string[]> = {};
  for (const t of allTables) { const d = depth[t]; (byDepth[d] = byDepth[d] ?? []).push(t); }
  const maxDepth = Math.max(...Object.keys(byDepth).map(Number));
  const cardH = (t: string) => ERD_HEADER_H + (columns[t]?.length ?? 0) * ERD_COL_H + 8;
  const COL_W = 480, ROW_GAP = 56;
  // 바리센터 정렬: 부모 노드 평균 위치 기준으로 자식 노드를 정렬해 교차선 최소화
  const sortedByDepth: Record<number, string[]> = { 0: [...(byDepth[0] ?? [])] };
  for (let d = 1; d <= maxDepth; d++) {
    const tables = [...(byDepth[d] ?? [])];
    const prevSorted = sortedByDepth[d - 1] ?? [];
    const bc: Record<string, number> = {};
    for (const t of tables) {
      const parents = [...deps[t]].filter(p => prevSorted.includes(p));
      bc[t] = parents.length === 0 ? prevSorted.length / 2 : parents.reduce((s, p) => s + prevSorted.indexOf(p), 0) / parents.length;
    }
    sortedByDepth[d] = tables.sort((a, b) => bc[a] - bc[b]);
  }
  // 열 높이 계산 후 수직 중앙 정렬
  const colH = (d: number) => (sortedByDepth[d] ?? []).reduce((s, t) => s + cardH(t) + ROW_GAP, -ROW_GAP);
  const maxColH = Math.max(0, ...Array.from({ length: maxDepth + 1 }, (_, d) => colH(d)));
  const positions: Record<string, ErdPos> = {};
  for (let d = 0; d <= maxDepth; d++) {
    let y = 60 + Math.max(0, (maxColH - colH(d)) / 2);
    for (const t of sortedByDepth[d] ?? []) {
      positions[t] = { x: 60 + d * COL_W, y };
      y += cardH(t) + ROW_GAP;
    }
  }
  return positions;
}

// ─── 탭 타입 ──────────────────────────────────────────────────
interface Tab { id: string; name: string; content: string; }

const MAX_HISTORY = 200;

function loadTabs(connId: string): Tab[] {
  try {
    const saved = localStorage.getItem(`rusql_tabs_${connId}`);
    if (saved) return JSON.parse(saved);
  } catch {}
  return [{ id: "1", name: "query.sql", content: localStorage.getItem(`rusql_query_${connId}`) ?? "SHOW TABLES;" }];
}
function loadActiveTabId(connId: string): string {
  return localStorage.getItem(`rusql_active_tab_${connId}`) ?? "1";
}
function loadHistory(connId: string): HistoryEntry[] {
  try {
    const saved = localStorage.getItem(`rusql_history_${connId}`);
    if (saved) return JSON.parse(saved);
  } catch {}
  return [];
}
function saveHistory(connId: string, h: HistoryEntry[]) {
  localStorage.setItem(`rusql_history_${connId}`, JSON.stringify(h.slice(0, MAX_HISTORY)));
}

// ─── 텍스트 너비 측정 (한글/CJK 포함, canvas 사용) ──────────
let _measureCtx: CanvasRenderingContext2D | null = null;
const measureTextPx = (text: string): number => {
  if (!_measureCtx) {
    const c = document.createElement('canvas');
    _measureCtx = c.getContext('2d');
    if (!_measureCtx) return text.length * 8;
    _measureCtx.font = '13px Consolas, "Malgun Gothic", monospace';
  }
  return _measureCtx.measureText(text).width;
};

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
  const [editingCell, setEditingCell] = useState<{
    resultIdx: number; rowIdx: number; colIdx: number;
    tableName: string; pkColName: string; pkValue: string;
  } | null>(null);
  const [editingValue, setEditingValue] = useState("");
  const cellEditCommittedRef = useRef(false);
  const [editTableModal, setEditTableModal] = useState<{ table: string; cols: ColumnDetail[] } | null>(null);
  const [editTableNewCol, setEditTableNewCol] = useState({ name: "", type: "VARCHAR(50)", notNull: false, defaultVal: "" });
  const [sidebarSearch, setSidebarSearch] = useState("");
  const [bookmarks, setBookmarks] = useState<{ id: string; name: string; sql: string }[]>(() => {
    try { return JSON.parse(localStorage.getItem("rusql_bookmarks") ?? "[]"); } catch { return []; }
  });
  const [databases, setDatabases] = useState<string[]>(["rusql"]);
  const [expandedTables, setExpandedTables] = useState<Set<string>>(new Set());
  const [tableColumns, setTableColumns] = useState<Record<string, ColumnDetail[]>>({});
  const [expandedViews, setExpandedViews] = useState<Set<string>>(new Set());
  const [dbAllExpanded, setDbAllExpanded] = useState<Record<string, boolean>>({});
  const [viewColumns, setViewColumns] = useState<Record<string, string[]>>({});
  // expandedIndexes: 인덱스 컨텍스트 메뉴용으로만 유지 (사이드바 트리는 table-level로 이동)
  const [, setExpandedIndexes] = useState<Set<string>>(new Set());
  const [currentDb, setCurrentDb] = useState<string>("rusql");
  const [expandedDbs, setExpandedDbs] = useState<Set<string>>(new Set(["rusql"]));
  // DB별 Tables/Views/Indexes/Triggers 데이터
  interface DbData { tables: string[]; views: string[]; indexes: IndexInfo[]; triggers: TriggerInfo[]; }
  const [dbData, setDbData] = useState<Record<string, DbData>>({});
  const [tablesOpen, setTablesOpen] = useState<Record<string, boolean>>({});
  const [viewsOpen, setViewsOpen] = useState<Record<string, boolean>>({});
  const [, ] = useState<Record<string, boolean>>({});
  // 테이블 하위 섹션 열림 상태: key = "${table}::columns|indexes|fkeys|triggers"
  const [expandedTableSections, setExpandedTableSections] = useState<Record<string, boolean>>({});
  const [ctxMenu, setCtxMenu] = useState<{ x: number; y: number } | null>(null);
  const [tableCtxMenu, setTableCtxMenu] = useState<{ x: number; y: number; table: string } | null>(null);
  const [dbCtxMenu, setDbCtxMenu] = useState<{ x: number; y: number; db: string } | null>(null);
  const [viewCtxMenu, setViewCtxMenu] = useState<{ x: number; y: number; view: string } | null>(null);
  const [indexCtxMenu, setIndexCtxMenu] = useState<{ x: number; y: number; index: string; table: string; kind: "single" | "composite" | "hash" } | null>(null);
  const [tabCtxMenu, setTabCtxMenu] = useState<{ x: number; y: number; tabId: string; source: "main" | "split" } | null>(null);
  const [pinnedTabs, setPinnedTabs] = useState<Set<string>>(new Set());
  const [splitTabId, setSplitTabId] = useState<string | null>(null);
  const [splitTabStash, setSplitTabStash] = useState<(Tab & { insertIdx: number }) | null>(null);
  const [splitLeftPct, setSplitLeftPct] = useState(50);
  const runQueryRef = useRef<() => Promise<void>>(async () => {});
  const splitEditorRef = useRef<Monaco.editor.IStandaloneCodeEditor | null>(null);
  const splitQueryRef = useRef<string>("");
  const isSplitSwitching = useRef(false);
  const isSplitDragging = useRef(false);
  const [editingTabId, setEditingTabId] = useState<string | null>(null);
  const [editingTabName, setEditingTabName] = useState("");
  const [isRunning, setIsRunning] = useState(false);
  const [queryHistory, setQueryHistory] = useState<HistoryEntry[]>([]);
  const [resultTab, setResultTab] = useState<"results" | "history">("results");
  const editorRef = useRef<Monaco.editor.IStandaloneCodeEditor | null>(null);
  const [resultHeight, setResultHeight] = useState(260);
  const [sidebarWidth, setSidebarWidth] = useState(240);
  const isDragging = useRef(false);
  const isSidebarDragging = useRef(false);
  const lastResultHeightRef = useRef(260);
  const [homeSidebarWidth, setHomeSidebarWidth] = useState(210);

  // ERD 상태
  const [erdColumns, setErdColumns] = useState<Record<string, ColumnDetail[]>>({});
  const [erdPositions, setErdPositions] = useState<Record<string, ErdPos>>({});
  const [isAutoLayout, setIsAutoLayout] = useState(false);
  const erdOriginalPositions = useRef<Record<string, ErdPos>>({});
  const [erdLoading, setErdLoading] = useState(false);
  const [erdPan, setErdPan] = useState<ErdPos>({ x: 40, y: 40 });
  const [erdZoom, setErdZoom] = useState(1);
  const erdCanvasRef = useRef<HTMLDivElement>(null);
  const erdCardDragRef = useRef<{ table: string; startMX: number; startMY: number; startCX: number; startCY: number; zoom: number } | null>(null);
  const erdCanvasDragRef = useRef<{ startMX: number; startMY: number; startPX: number; startPY: number } | null>(null);
  const erdCardWasDragged = useRef(false);
  const erdCanvasWasDragged = useRef(false);
  const [erdSelectedTable, setErdSelectedTable] = useState<string>("");
  const [erdTableData, setErdTableData] = useState<QueryResult | null>(null);
  const [erdTableLoading, setErdTableLoading] = useState(false);
  const [erdFilter, setErdFilter] = useState("");
  const [erdDataHeight, setErdDataHeight] = useState(0);
  const erdDataDragging = useRef(false);
  const [erdHoveredTable, setErdHoveredTable] = useState<string | null>(null);
  const [erdAnimating, setErdAnimating] = useState(false);

  // ─── 연결 관리 ───────────────────────────────────────────────
  interface Connection {
    id: string; name: string; host: string; port: number;
    user: string; password: string; autoLogin: boolean; dataDir: string;
  }
  const loadConnections = (): Connection[] => {
    try {
      const s = localStorage.getItem("rusql_connections");
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
    return [{ id: "1", name: "RuSQL Local", host: "localhost", port: 7878, user: "root", password: "root", autoLogin: false, dataDir: "local" }];
  };
  const [connections, setConnections] = useState<Connection[]>(loadConnections);
  const saveConnections = (c: Connection[]) => { localStorage.setItem("rusql_connections", JSON.stringify(c)); setConnections(c); };
  const [appDataBase, setAppDataBase] = useState("");

  // 앱 데이터 디렉터리 조회 및 기존 상대경로 연결 마이그레이션

  useEffect(() => {
    invoke<string>("get_app_data_dir").then(base => {
      setAppDataBase(base);
      setConnections(prev => {
        const migrated = prev.map(c => {
          // 이미 절대경로면 그대로
          if (c.dataDir.includes("\\") || c.dataDir.includes("/") || c.dataDir.startsWith(".")) return c;
          // 모든 상대경로 → code/data/{relative}
          return { ...c, dataDir: `${base}\\${c.dataDir}` };
        });
        // 변경이 있으면 localStorage에도 저장
        if (migrated.some((c, i) => c.dataDir !== prev[i].dataDir)) {
          localStorage.setItem("rusql_connections", JSON.stringify(migrated));
        }
        return migrated;
      });
    });
  }, []);

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
    setServerStatus({ running: false, port: conn.port, client_count: 0, log: [], sessions: [] });
    setPortInput(conn.port.toString());
    setSrvConnName(conn.name);
    setServerMsg("");
    setActiveView("editor");
    setLoggedIn(true);
    setConnectingTo(null);
    setDlgPass("");
  };

  const saveBookmarkList = (bks: { id: string; name: string; sql: string }[]) => {
    localStorage.setItem("rusql_bookmarks", JSON.stringify(bks));
    setBookmarks(bks);
  };
  const addBookmark = () => {
    const sql = queryRef.current.trim();
    if (!sql) return;
    const name = sql.split("\n")[0].trim().slice(0, 40) || "Bookmark";
    saveBookmarkList([...bookmarks, { id: Date.now().toString(), name, sql }]);
  };
  const removeBookmark = (id: string) => saveBookmarkList(bookmarks.filter(b => b.id !== id));

  const importSqlFile = () => {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".sql,.txt";
    input.onchange = (e) => {
      const file = (e.target as HTMLInputElement).files?.[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = ev => {
        const content = ev.target?.result as string;
        if (content) setEditorQuery(content);
      };
      reader.readAsText(file);
    };
    input.click();
  };

  const toggleResultPanel = () => {
    if (resultHeight > 0) {
      lastResultHeightRef.current = resultHeight;
      setResultHeight(0);
    } else {
      setResultHeight(lastResultHeightRef.current);
    }
  };

  const handleAutoLogin = async (conn: Connection) => {
    const ok = await invoke<boolean>("authenticate", { user: conn.user, password: conn.password, dataDir: conn.dataDir, bufferPoolSize: parseInt(bufferPoolInput) || 64 });
    if (ok) doLogin(conn);
  };

  const handleConnect = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!connectingTo) return;
    setDlgLoading(true);
    setDlgError("");
    const ok = await invoke<boolean>("authenticate", { user: connectingTo.user, password: dlgPass, dataDir: connectingTo.dataDir, bufferPoolSize: parseInt(bufferPoolInput) || 64 });
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
      dataDir: appDataBase ? `${appDataBase}\\data_${id}` : `data\\data_${id}`,
    };
    saveConnections([...connections, conn]);
    setShowNewConn(false);
    setNewName("New Connection"); setNewHost("localhost"); setNewPort("7878");
    setNewUser("root"); setNewPass("root"); setNewAutoLogin(false); setNewPassVisible(false);
  };

  // 뷰 전환
  const [activeView, setActiveView] = useState<ActiveView>("editor");

  // 서버 상태

  const serverStatus_init: ServerStatus = { running: false, port: 7878, client_count: 0, log: [], sessions: [] };
  const [serverStatus, setServerStatus] = useState<ServerStatus>(serverStatus_init);
  const [portInput, setPortInput] = useState("7878");
  const [mysqlPortInput, setMysqlPortInput] = useState("13306");
  const [bufferPoolInput, setBufferPoolInput] = useState(() => localStorage.getItem("rusql_bp_size") || "64");
  const [parallelQuery, setParallelQuery] = useState(() => localStorage.getItem("rusql_parallel") !== "0");
  const [serverMsg, setServerMsg] = useState("");
  const [srvConnName, setSrvConnName] = useState("RuSQL Local");
  const [srvUser, setSrvUser] = useState("root");
  const [srvPass, setSrvPass] = useState("root");
  const [srvRightPanel, setSrvRightPanel] = useState<"none" | "cli" | "mysql" | "bench" | "sessions" | "mcp">("none");
  const [mcpSetupMsg, setMcpSetupMsg] = useState<{ ok: boolean; text: string } | null>(null);
  const [benchResult, setBenchResult] = useState<Record<string, unknown> | null | undefined>(undefined);
  const [benchLoading, setBenchLoading] = useState(false);
  const [srvPassVisible, setSrvPassVisible] = useState(false);
  const logEndRef = useRef<HTMLDivElement>(null);

  // ─── DB 하나의 데이터 로드 ────────────────────────────────────
  const loadDbData = async (db: string) => {
    const [tbls, vws, idxs, trgs] = await Promise.all([
      invoke<string[]>("get_tables_for_db", { db }),
      invoke<string[]>("get_views_for_db", { db }),
      invoke<IndexInfo[]>("get_indexes_for_db", { db }),
      invoke<TriggerInfo[]>("get_triggers_for_db", { db }),
    ]);
    setDbData(prev => ({ ...prev, [db]: { tables: tbls, views: vws, indexes: idxs, triggers: trgs } }));
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


  // ─── 메뉴바 상태 ────────────────────────────────────────────
  const [openMenu, setOpenMenu] = useState<string | null>(null);

  // ─── 컨텍스트 메뉴 + 메뉴바 닫기 ────────────────────────────
  useEffect(() => {
    const h = () => { setCtxMenu(null); setTableCtxMenu(null); setDbCtxMenu(null); setViewCtxMenu(null); setIndexCtxMenu(null); setTabCtxMenu(null); setOpenMenu(null); };
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
      if (isSplitDragging.current) {
        const el = document.querySelector(".editor-area") as HTMLElement;
        if (!el) return;
        const rect = el.getBoundingClientRect();
        const pct = Math.max(20, Math.min(80, ((e.clientX - rect.left) / rect.width) * 100));
        setSplitLeftPct(pct);
      }
    };
    const onUp = () => {
      isDragging.current = false;
      isSidebarDragging.current = false;
      isSplitDragging.current = false;
      document.body.style.cursor = "";
      document.body.style.userSelect = "";
    };
    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onUp);
    return () => { window.removeEventListener("mousemove", onMove); window.removeEventListener("mouseup", onUp); };
  }, []);

  // ─── ERD 자동 레이아웃 ──────────────────────────────────────
  const autoLayoutErd = () => {
    if (isAutoLayout) {
      setErdAnimating(true);
      setErdPositions(erdOriginalPositions.current);
      setIsAutoLayout(false);
      setTimeout(() => setErdAnimating(false), 500);
      return;
    }
    if (Object.keys(erdColumns).length === 0) return;
    erdOriginalPositions.current = { ...erdPositions };
    setErdAnimating(true);
    setErdPositions(computeErdLayout(erdColumns));
    setIsAutoLayout(true);
    setTimeout(() => setErdAnimating(false), 500);
  };

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
        erdCanvasWasDragged.current = true;
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
  const runQuery = async (forceAll = false) => {
    const sel = !forceAll && editorRef.current?.getSelection()
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
      // 최소 400ms 표시 보장 (빠른 쿼리도 로딩 바가 보이도록)
      setTimeout(() => setIsRunning(false), 400);
    }
  };
  // addCommand가 첫 렌더링의 runQuery를 캡처하는 stale closure 방지
  useEffect(() => { runQueryRef.current = runQuery; });

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
    localStorage.setItem(`rusql_tabs_${connIdRef.current}`, JSON.stringify(next));
  };


  // 현재 에디터 내용을 탭에 저장한 후 탭 전환
  const switchTab = (id: string) => {
    if (id === activeTabId) return;
    const currentContent = editorRef.current?.getValue() ?? queryRef.current;
    const updated = tabs.map(t => t.id === activeTabId ? { ...t, content: currentContent } : t);
    saveTabs(updated);
    setActiveTabId(id);
    localStorage.setItem(`rusql_active_tab_${connIdRef.current}`, id);
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
    localStorage.setItem(`rusql_active_tab_${connIdRef.current}`, newId);
    queryRef.current = "";
    isSwitchingTab.current = true;
    editorRef.current?.setValue("");
    isSwitchingTab.current = false;
  };

  // 탭 닫기
  const closeTab = (id: string, e?: React.MouseEvent) => {
    e?.stopPropagation();
    if (tabs.length === 1) return; // 마지막 탭은 닫지 않음
    if (pinnedTabs.has(id)) return; // 고정된 탭은 닫지 않음
    if (id === splitTabId) setSplitTabId(null); // 분할 탭 닫힐 때 분할도 해제
    const idx = tabs.findIndex(t => t.id === id);
    const next = tabs.filter(t => t.id !== id);
    saveTabs(next);
    if (activeTabId === id) {
      const newActive = next[Math.min(idx, next.length - 1)];
      setActiveTabId(newActive.id);
      localStorage.setItem("rusql_active_tab", newActive.id);
      queryRef.current = newActive.content;
      editorRef.current?.setValue(newActive.content);
    }
  };

  const closeOtherTabs = (id: string) => {
    const currentContent = editorRef.current?.getValue() ?? queryRef.current;
    const target = tabs.find(t => t.id === id);
    if (!target) return;
    const next = [{ ...target, content: id === activeTabId ? currentContent : target.content }];
    saveTabs(next);
    setActiveTabId(id);
    localStorage.setItem(`rusql_active_tab_${connIdRef.current}`, id);
    queryRef.current = next[0].content;
    isSwitchingTab.current = true;
    editorRef.current?.setValue(next[0].content);
    isSwitchingTab.current = false;
    setPinnedTabs(prev => { const s = new Set<string>(); if (prev.has(id)) s.add(id); return s; });
  };

  const closeTabsToRight = (id: string) => {
    const currentContent = editorRef.current?.getValue() ?? queryRef.current;
    const updated = tabs.map(t => t.id === activeTabId ? { ...t, content: currentContent } : t);
    const idx = updated.findIndex(t => t.id === id);
    if (idx < 0) return;
    const next = updated.slice(0, idx + 1);
    saveTabs(next);
    if (!next.find(t => t.id === activeTabId)) {
      const last = next[next.length - 1];
      setActiveTabId(last.id);
      localStorage.setItem(`rusql_active_tab_${connIdRef.current}`, last.id);
      queryRef.current = last.content;
      isSwitchingTab.current = true;
      editorRef.current?.setValue(last.content);
      isSwitchingTab.current = false;
    }
    setPinnedTabs(prev => { const s = new Set<string>(); next.forEach(t => { if (prev.has(t.id)) s.add(t.id); }); return s; });
  };

  const closeAllTabs = () => {
    const newId = Date.now().toString();
    const newTab: Tab = { id: newId, name: "query.sql", content: "" };
    saveTabs([newTab]);
    setActiveTabId(newId);
    localStorage.setItem(`rusql_active_tab_${connIdRef.current}`, newId);
    queryRef.current = "";
    isSwitchingTab.current = true;
    editorRef.current?.setValue("");
    isSwitchingTab.current = false;
    setPinnedTabs(new Set());
  };

  const downloadTab = (id: string) => {
    const currentContent = editorRef.current?.getValue() ?? queryRef.current;
    const tab = tabs.find(t => t.id === id);
    if (!tab) return;
    const content = id === activeTabId ? currentContent : tab.content;
    const blob = new Blob([content], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = tab.name.endsWith(".sql") ? tab.name : `${tab.name}.sql`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  };

  const togglePin = (id: string) => {
    setPinnedTabs(prev => {
      const s = new Set(prev);
      if (s.has(id)) s.delete(id); else s.add(id);
      return s;
    });
  };

  const openSplitPaneWith = (targetId: string) => {
    // 이미 분할 중이면 기존 stash를 tabs[]에 복원한 뒤 새 stash 생성
    let baseTabs = tabs;
    if (splitTabStash) {
      const stashContent = splitEditorRef.current?.getValue() ?? splitQueryRef.current;
      const insertIdx = Math.min(splitTabStash.insertIdx, tabs.length);
      const restored = { id: splitTabStash.id, name: splitTabStash.name, content: stashContent };
      baseTabs = [...tabs.slice(0, insertIdx), restored, ...tabs.slice(insertIdx)];
      setSplitTabStash(null);
    }

    const idx = baseTabs.findIndex(t => t.id === targetId);
    if (idx === -1) return;
    const tab = baseTabs[idx];

    splitQueryRef.current = tab.content;
    setSplitTabId(targetId);
    setSplitLeftPct(50);

    // 탭을 왼쪽 바에서 제거하고 stash에 보관
    const nextTabs = baseTabs.filter(t => t.id !== targetId);
    setSplitTabStash({ ...tab, insertIdx: idx });
    setTabs(nextTabs);
    localStorage.setItem(`rusql_tabs_${connIdRef.current}`, JSON.stringify(nextTabs));

    // 분할된 탭이 활성 탭이었으면 인접 탭으로 전환
    if (targetId === activeTabId && nextTabs.length > 0) {
      const newActive = nextTabs[Math.min(idx, nextTabs.length - 1)];
      setActiveTabId(newActive.id);
      queryRef.current = newActive.content;
      isSwitchingTab.current = true;
      editorRef.current?.setValue(newActive.content);
      isSwitchingTab.current = false;
    }
  };

  // 오른쪽으로 분할: 우클릭한 탭이 오른쪽 분할창에 열림 (같은 탭도 허용)
  const doSplitRight = (tabId: string) => {
    openSplitPaneWith(tabId);
  };

  // 왼쪽으로 분할: 현재 활성 탭이 오른쪽, 우클릭한 탭이 왼쪽(메인)으로
  const doSplitLeft = (tabId: string) => {
    const prevActiveId = activeTabId;
    openSplitPaneWith(prevActiveId); // 현재 활성 탭을 오른쪽 분할창으로
    if (tabId !== prevActiveId) switchTab(tabId); // 클릭한 탭을 왼쪽(메인)으로
  };

  // 분할 및 이동: 우클릭한 탭을 오른쪽으로 이동, 왼쪽은 다른 탭으로 전환
  const doSplitAndMove = (tabId: string) => {
    openSplitPaneWith(tabId);
    if (tabId === activeTabId) {
      const other = tabs.find(t => t.id !== tabId);
      if (other) switchTab(other.id);
    }
  };

  const closeSplit = () => {
    const content = splitEditorRef.current?.getValue() ?? splitQueryRef.current;
    if (splitTabStash) {
      const stash = splitTabStash;
      const restored = { id: stash.id, name: stash.name, content };
      setTabs(prev => {
        const insertIdx = Math.min(stash.insertIdx, prev.length);
        const next = [...prev.slice(0, insertIdx), restored, ...prev.slice(insertIdx)];
        localStorage.setItem(`rusql_tabs_${connIdRef.current}`, JSON.stringify(next));
        return next;
      });
      setActiveTabId(stash.id);
      queryRef.current = content;
      isSwitchingTab.current = true;
      editorRef.current?.setValue(content);
      isSwitchingTab.current = false;
      setSplitTabStash(null);
    }
    setSplitTabId(null);
  };

  const switchSplitTab = (id: string) => {
    if (id === splitTabId) return;
    const content = splitEditorRef.current?.getValue() ?? splitQueryRef.current;
    if (splitTabStash?.id === splitTabId) {
      setSplitTabStash(prev => prev ? { ...prev, content } : prev);
    } else {
      setTabs(prev => {
        const next = prev.map(t => t.id === splitTabId ? { ...t, content } : t);
        localStorage.setItem(`rusql_tabs_${connIdRef.current}`, JSON.stringify(next));
        return next;
      });
    }
    const newContent = tabs.find(t => t.id === id)?.content ?? "";
    splitQueryRef.current = newContent;
    isSplitSwitching.current = true;
    splitEditorRef.current?.setValue(newContent);
    isSplitSwitching.current = false;
    setSplitTabId(id);
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

  const toggleDbExpandAll = async (dbName: string, e: React.MouseEvent) => {
    e.stopPropagation();
    const isExpanded = dbAllExpanded[dbName] ?? false;

    if (isExpanded) {
      // 접기: Columns/Indexes/FK/Triggers 서브섹션만 닫기, 테이블 노드는 그대로
      const data = dbData[dbName] ?? { tables: [], views: [], indexes: [], triggers: [] };
      setExpandedTableSections(prev => {
        const next = { ...prev };
        for (const t of data.tables) {
          delete next[`${t}::columns`];
          delete next[`${t}::indexes`];
          delete next[`${t}::fkeys`];
          delete next[`${t}::triggers`];
        }
        return next;
      });
      setDbAllExpanded(p => ({ ...p, [dbName]: false }));
    } else {
      // DB 노드가 닫혀있으면 먼저 열기
      setExpandedDbs(prev => new Set(prev).add(dbName));

      // DB 데이터 로드 (없으면)
      let data = dbData[dbName];
      if (!data) {
        const [tables, views, indexes, triggers] = await Promise.all([
          invoke<string[]>("get_tables_for_db", { db: dbName }),
          invoke<string[]>("get_views_for_db", { db: dbName }),
          invoke<IndexInfo[]>("get_indexes_for_db", { db: dbName }),
          invoke<TriggerInfo[]>("get_triggers_for_db", { db: dbName }),
        ]);
        data = { tables, views, indexes, triggers };
        setDbData(prev => ({ ...prev, [dbName]: data! }));
      }

      if (data.tables.length > 0) setTablesOpen(p => ({ ...p, [dbName]: true }));
      if (data.views.length > 0) setViewsOpen(p => ({ ...p, [dbName]: true }));

      // 미로드 테이블 컬럼 일괄 로드
      const allCols: Record<string, ColumnDetail[]> = { ...tableColumns };
      await Promise.all(
        data.tables
          .filter(t => !allCols[t])
          .map(async t => { allCols[t] = await invoke<ColumnDetail[]>("get_columns_detail", { table: t }); })
      );
      setTableColumns(prev => ({ ...prev, ...allCols }));

      // 모든 테이블 펼치기
      setExpandedTables(prev => {
        const s = new Set(prev);
        for (const t of data!.tables) s.add(t);
        return s;
      });

      // 항목이 있는 서브섹션만 오픈
      setExpandedTableSections(prev => {
        const next = { ...prev };
        for (const t of data!.tables) {
          const cols = allCols[t] ?? [];
          const tIdxs = data!.indexes.filter(i => i.table === t);
          const tFkeys = cols.filter(c => c.fk_ref);
          const tTrgs = data!.triggers.filter(tr => tr.table === t);
          if (cols.length > 0) next[`${t}::columns`] = true;
          if (tIdxs.length > 0) next[`${t}::indexes`] = true;
          if (tFkeys.length > 0) next[`${t}::fkeys`] = true;
          if (tTrgs.length > 0) next[`${t}::triggers`] = true;
        }
        return next;
      });

      setDbAllExpanded(p => ({ ...p, [dbName]: true }));
    }
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

  const runViewCtxQuery = async (q: string, dropView?: string) => {
    setViewCtxMenu(null);
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
      if (dropView) setExpandedViews(prev => { const s = new Set(prev); s.delete(dropView); return s; });
    } catch (e) {
      setTabResults(p => ({ ...p, [activeTabId]: [{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }] }));
    } finally {
      setIsRunning(false);
    }
  };

  const runIndexCtxQuery = async (q: string, dropIndex?: string) => {
    setIndexCtxMenu(null);
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
      if (dropIndex) setExpandedIndexes(prev => { const s = new Set(prev); s.delete(dropIndex); return s; });
    } catch (e) {
      setTabResults(p => ({ ...p, [activeTabId]: [{ columns: [], rows: [], message: String(e), elapsed: 0, success: false }] }));
    } finally {
      setIsRunning(false);
    }
  };

  // ─── Edit Table 모달 ──────────────────────────────────────────
  const openEditTableModal = async (table: string) => {
    setTableCtxMenu(null);
    let cols = tableColumns[table];
    if (!cols) {
      cols = await invoke<ColumnDetail[]>("get_columns_detail", { table });
      setTableColumns(p => ({ ...p, [table]: cols }));
    }
    setEditTableModal({ table, cols });
    setEditTableNewCol({ name: "", type: "VARCHAR(50)", notNull: false, defaultVal: "" });
  };

  const refreshEditTableModal = async (table: string) => {
    const cols = await invoke<ColumnDetail[]>("get_columns_detail", { table });
    setTableColumns(p => ({ ...p, [table]: cols }));
    setEditTableModal({ table, cols });
  };

  const dropColumn = async (table: string, colName: string) => {
    if (!window.confirm(`"${table}.${colName}" 컬럼을 삭제하시겠습니까?`)) return;
    try {
      await invoke<MultiQueryResult>("execute_query", { query: `ALTER TABLE ${table} DROP COLUMN ${colName};`, ts: Date.now() });
      await refreshEditTableModal(table);
      await refreshSidebar();
    } catch (e) { window.alert(String(e)); }
  };

  const addColumn = async () => {
    if (!editTableModal || !editTableNewCol.name.trim()) return;
    const { table } = editTableModal;
    let sql = `ALTER TABLE ${table} ADD COLUMN ${editTableNewCol.name.trim()} ${editTableNewCol.type}`;
    if (editTableNewCol.notNull) sql += " NOT NULL";
    if (editTableNewCol.defaultVal.trim()) sql += ` DEFAULT ${editTableNewCol.defaultVal.trim()}`;
    sql += ";";
    try {
      await invoke<MultiQueryResult>("execute_query", { query: sql, ts: Date.now() });
      setEditTableNewCol({ name: "", type: "VARCHAR(50)", notNull: false, defaultVal: "" });
      await refreshEditTableModal(table);
      await refreshSidebar();
    } catch (e) { window.alert(String(e)); }
  };

  const extractTableName = (sql: string): string | null => {
    if (/\bJOIN\b/i.test(sql)) return null;
    const m = sql.match(/\bFROM\s+(\w+)\b/i);
    return m ? m[1] : null;
  };

  const handleCellDoubleClick = async (
    resultIdx: number, rowIdx: number, colIdx: number,
    currentValue: string, row: string[], columns: string[]
  ) => {
    const sql = editorRef.current?.getValue() ?? queryRef.current;
    const tableName = extractTableName(sql);
    if (!tableName) return;

    let cols = tableColumns[tableName];
    if (!cols) {
      cols = await invoke<ColumnDetail[]>("get_columns_detail", { table: tableName });
      setTableColumns(p => ({ ...p, [tableName]: cols }));
    }

    const pkCol = cols.find(c => c.is_pk);
    if (!pkCol) return;

    const pkColIdx = columns.indexOf(pkCol.name);
    if (pkColIdx === -1) return;

    cellEditCommittedRef.current = false;
    setEditingCell({ resultIdx, rowIdx, colIdx, tableName, pkColName: pkCol.name, pkValue: row[pkColIdx] });
    setEditingValue(currentValue);
  };

  const commitCellEdit = async () => {
    if (!editingCell || cellEditCommittedRef.current) return;
    cellEditCommittedRef.current = true;
    const { tableName, pkColName, pkValue, resultIdx, colIdx } = editingCell;
    const colName = results[resultIdx].columns[colIdx];
    const newVal = editingValue;
    setEditingCell(null);
    const isNum = newVal.trim() !== "" && !isNaN(Number(newVal));
    const quoted = isNum ? newVal : `'${newVal.replace(/'/g, "''")}'`;
    const q = `UPDATE ${tableName} SET ${colName} = ${quoted} WHERE ${pkColName} = ${pkValue};`;
    try {
      await invoke<MultiQueryResult>("execute_query", { query: q, ts: Date.now() });
      await runQuery();
    } catch {}
  };

  // ─── ERD 로드 ────────────────────────────────────────────────
  const loadErd = async () => {
    setErdLoading(true);
    setIsAutoLayout(false);
    erdOriginalPositions.current = {};
    try {
      const tblList = await invoke<string[]>("get_tables");
      if (tblList.length === 0) { setErdColumns({}); setErdPositions({}); return; }
      const entries = await Promise.all(
        tblList.map(async t => {
          const cols = await invoke<ColumnDetail[]>("get_columns_detail", { table: t });
          return [t, cols] as [string, ColumnDetail[]];
        })
      );
      const cols = Object.fromEntries(entries);
      setErdColumns(cols);
      setErdPositions(prev => {
        const computed = computeErdLayout(cols);
        const prevTables = new Set(Object.keys(prev));
        const sameTables = tblList.every(t => prevTables.has(t)) && tblList.length === prevTables.size;
        if (sameTables) {
          // 같은 테이블이면 수동 위치 유지
          const next: Record<string, ErdPos> = {};
          for (const t of tblList) next[t] = prev[t] ?? computed[t] ?? { x: 60, y: 60 };
          return next;
        }
        return computed;
      });
    } finally {
      setErdLoading(false);
    }
  };

  // ─── 서버 제어 ──────────────────────────────────────────────
  const handleStartServer = async () => {
    const port = parseInt(portInput) || 7878;
    const mysqlPort = parseInt(mysqlPortInput) || 0;
    try {
      const msg = await invoke<string>("start_server", { connId: connIdRef.current, port, mysqlPort });
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
            <span style={{ color: "#999", fontSize: 12 }}>RuSQL</span>
          </div>
        </div>

        {/* ── 레이아웃 (사이드바 + 메인) ── */}
        <div className="home-layout" onClick={() => setOpenMenu(null)}>

          {/* 왼쪽 액티비티 바 (유저 + 설정만) */}
          <div className="activity-bar">
            <div className="activity-bar-bottom">
              <div className="activity-icon" title="Account">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor">
                  <path d="M12 12c2.7 0 4.8-2.1 4.8-4.8S14.7 2.4 12 2.4 7.2 4.5 7.2 7.2 9.3 12 12 12zm0 2.4c-3.2 0-9.6 1.6-9.6 4.8v2.4h19.2v-2.4c0-3.2-6.4-4.8-9.6-4.8z"/>
                </svg>
              </div>
              <div className="activity-icon" title="Settings">
                <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
                  <path d="M19.43 12.98c.04-.32.07-.64.07-.98s-.03-.66-.07-.98l2.11-1.65c.19-.15.24-.42.12-.64l-2-3.46c-.12-.22-.39-.3-.61-.22l-2.49 1c-.52-.4-1.08-.73-1.69-.98l-.38-2.65C14.46 2.18 14.25 2 14 2h-4c-.25 0-.46.18-.49.42l-.38 2.65c-.61.25-1.17.59-1.69.98l-2.49-1c-.23-.09-.49 0-.61.22l-2 3.46c-.13.22-.07.49.12.64l2.11 1.65c-.04.32-.07.65-.07.98s.03.66.07.98l-2.11 1.65c-.19.15-.24.42-.12.64l2 3.46c.12.22.39.3.61.22l2.49-1c.52.4 1.08.73 1.69.98l.38 2.65c.03.24.24.42.49.42h4c.25 0 .46-.18.49-.42l.38-2.65c.61-.25 1.17-.59 1.69-.98l2.49 1c.23.09.49 0 .61-.22l2-3.46c.12-.22.07-.49-.12-.64l-2.11-1.65zM12 15.5c-1.93 0-3.5-1.57-3.5-3.5s1.57-3.5 3.5-3.5 3.5 1.57 3.5 3.5-1.57 3.5-3.5 3.5z"/>
                </svg>
              </div>
            </div>
          </div>

          {/* 사이드바 */}
          <div className="home-right">
          <div className="home-right-inner">
          <div className="home-sidebar" style={{ width: homeSidebarWidth }}>
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
                  <defs><linearGradient id="dbGradSidebar" x1="0" y1="2" x2="0" y2="22" gradientUnits="userSpaceOnUse"><stop offset="0%" stopColor="#8b5cf6"/><stop offset="100%" stopColor="#4ec9b0"/></linearGradient></defs>
                  <ellipse cx="12" cy="5" rx="7" ry="3" stroke="url(#dbGradSidebar)" strokeWidth="1.6" vectorEffect="non-scaling-stroke"/>
                  <path d="M5 5v6c0 1.66 3.13 3 7 3s7-1.34 7-3V5" stroke="url(#dbGradSidebar)" strokeWidth="1.6" fill="none" vectorEffect="non-scaling-stroke"/>
                  <path d="M5 11v6c0 1.66 3.13 3 7 3s7-1.34 7-3v-6" stroke="url(#dbGradSidebar)" strokeWidth="1.2" fill="none" vectorEffect="non-scaling-stroke"/>
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

          {/* 사이드바 리사이즈 핸들 */}
          <div
            className="home-sidebar-resize-handle"
            onMouseDown={e => {
              e.preventDefault();
              const startX = e.clientX;
              const startW = homeSidebarWidth;
              document.body.style.cursor = "col-resize";
              document.body.style.userSelect = "none";
              const onMove = (me: MouseEvent) => {
                setHomeSidebarWidth(Math.max(140, Math.min(400, startW + me.clientX - startX)));
              };
              const onUp = () => {
                document.removeEventListener("mousemove", onMove);
                document.removeEventListener("mouseup", onUp);
                document.body.style.cursor = "";
                document.body.style.userSelect = "";
              };
              document.addEventListener("mousemove", onMove);
              document.addEventListener("mouseup", onUp);
            }}
          />

          <div className="home-main">

            {/* 헤더 */}
            <div className="home-header">
              <div className="home-header-icon">
                <img src="/icon.png" alt="RuSQL" style={{ width: 110, height: 110, objectFit: "contain" }} />
              </div>
              <div>
                <h1 className="home-title">RuSQL Connections</h1>
                <p className="home-desc">연결할 데이터베이스를 선택하거나 새 연결을 추가하세요.</p>
              </div>
            </div>

            {/* RDBMS 설명 */}
            <div className="home-rdbms-desc">
              <p>
                Welcome to our project! — a relational database management system built entirely from the ground up in Rust, designed for reliability.<br/>
                Connect to a database and feel free to explore, query, update, and manage your data — there are no limits on how you interact with it.<br/>
                Every part of the interface is crafted to be intuitive and approachable, so you can always stay focused on your data rather than the tool.<br/>
                Whenever you need a hand, the built-in AI assistant is always there — ready to help you write queries, understand results, and go further.
              </p>
            </div>

            {/* 퀵 액션 */}
            <div className="home-quick-actions">
              <button className="home-quick-btn" onClick={() => setShowNewConn(true)}>
                <svg width="52" height="52" viewBox="0 0 24 24" fill="currentColor">
                  <path d="M19 13H13v6h-2v-6H5v-2h6V5h2v6h6v2z"/>
                </svg>
                새 연결
              </button>
              <button className="home-quick-btn" onClick={() => invoke("open_terminal")}>
                <svg width="52" height="52" viewBox="0 0 24 24" fill="currentColor">
                  <path d="M20 4H4C2.9 4 2 4.9 2 6V18C2 19.1 2.9 20 4 20H20C21.1 20 22 19.1 22 18V6C22 4.9 21.1 4 20 4ZM20 18H4V6H20V18ZM6 10L8.5 12.5L6 15L7.5 16.5L11.5 12.5L7.5 8.5L6 10ZM12 15H18V17H12V15Z"/>
                </svg>
                터미널 열기
              </button>
              <button className="home-quick-btn" onClick={() => invoke("open_url", { url: "https://github.com/rspstat/RuSQL" })}>
                <svg width="52" height="52" viewBox="0 0 24 24" fill="currentColor">
                  <path d="M12 2C6.477 2 2 6.477 2 12c0 4.42 2.865 8.166 6.839 9.489.5.092.682-.217.682-.482 0-.237-.009-.868-.013-1.703-2.782.604-3.369-1.342-3.369-1.342-.454-1.154-1.11-1.462-1.11-1.462-.908-.62.069-.608.069-.608 1.003.07 1.531 1.03 1.531 1.03.892 1.529 2.341 1.088 2.91.832.092-.647.35-1.088.636-1.338-2.22-.253-4.555-1.11-4.555-4.943 0-1.091.39-1.984 1.029-2.683-.103-.253-.446-1.27.098-2.647 0 0 .84-.269 2.75 1.025A9.578 9.578 0 0112 6.836a9.59 9.59 0 012.504.337c1.909-1.294 2.747-1.025 2.747-1.025.546 1.377.202 2.394.1 2.647.64.699 1.028 1.592 1.028 2.683 0 3.842-2.339 4.687-4.566 4.935.359.309.678.919.678 1.852 0 1.336-.012 2.415-.012 2.741 0 .267.18.577.688.479C19.138 20.162 22 16.418 22 12c0-5.523-4.477-10-10-10z"/>
                </svg>
                GitHub 방문
              </button>
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
                    <svg width="52" height="52" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                      <defs><linearGradient id="dbGradCard" x1="0" y1="0" x2="0" y2="24" gradientUnits="userSpaceOnUse"><stop offset="0%" stopColor="#8b5cf6"/><stop offset="100%" stopColor="#4ec9b0"/></linearGradient></defs>
                      <ellipse cx="12" cy="3" rx="8" ry="2.5" stroke="url(#dbGradCard)" strokeWidth="1.4" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 3v6c0 1.38 3.58 2.5 8 2.5s8-1.12 8-2.5v-6" stroke="url(#dbGradCard)" strokeWidth="1.4" fill="none" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 9v6c0 1.38 3.58 2.5 8 2.5s8-1.12 8-2.5v-6" stroke="url(#dbGradCard)" strokeWidth="1.4" fill="none" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 15v6c0 1.38 3.58 2.5 8 2.5s8-1.12 8-2.5v-6" stroke="url(#dbGradCard)" strokeWidth="1.4" fill="none" vectorEffect="non-scaling-stroke"/>
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
                        {conn.dataDir.split(/[\\/]/).pop()}
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
                      localStorage.removeItem(`rusql_tabs_${conn.id}`);
                      localStorage.removeItem(`rusql_active_tab_${conn.id}`);
                      localStorage.removeItem(`rusql_history_${conn.id}`);
                      localStorage.removeItem(`rusql_query_${conn.id}`);
                      // 디스크 데이터 디렉토리 삭제
                      invoke("delete_conn_data", { dataDir: conn.dataDir });
                      saveConnections(connections.filter(c => c.id !== conn.id));
                    }}
                  >✕</button>
                </div>
              ))}
            </div>

          </div>{/* home-main */}
          </div>{/* home-right-inner */}

          {/* ── 하단 상태바 ── */}
          <div className="status-bar">
            <div className="status-left">
              <span className="status-item">⎇ main</span>
              <span className="status-item" style={{ color: "#9cdcfe" }}>RuSQL v2.2.0</span>
            </div>
            <div className="status-right">
              <span className="status-item">MySQL 호환 RDBMS</span>
              <span className="status-item">포트 7878 / 3306</span>
              <span className="status-item">B+Tree · WAL · MVCC</span>
            </div>
          </div>

          </div>{/* home-right */}
        </div>{/* home-layout */}

        {/* ── 연결 다이얼로그 ── */}
        {connectingTo && (
          <div className="dlg-overlay" onClick={() => setConnectingTo(null)}>
            <div className="dlg-box" onClick={e => e.stopPropagation()}>
              <div className="dlg-header">
                <svg width="18" height="22" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                  <defs><linearGradient id="dbGradDlg1" x1="0" y1="1" x2="0" y2="23" gradientUnits="userSpaceOnUse"><stop offset="0%" stopColor="#8b5cf6"/><stop offset="100%" stopColor="#4ec9b0"/></linearGradient></defs>
                  <ellipse cx="12" cy="5" rx="8" ry="3.5" stroke="url(#dbGradDlg1)" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 5v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5V5" stroke="url(#dbGradDlg1)" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 11v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5v-6" stroke="url(#dbGradDlg1)" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
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
                  <defs><linearGradient id="dbGradDlg2" x1="0" y1="1" x2="0" y2="23" gradientUnits="userSpaceOnUse"><stop offset="0%" stopColor="#8b5cf6"/><stop offset="100%" stopColor="#4ec9b0"/></linearGradient></defs>
                  <ellipse cx="12" cy="5" rx="8" ry="3.5" stroke="url(#dbGradDlg2)" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 5v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5V5" stroke="url(#dbGradDlg2)" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                  <path d="M4 11v6c0 1.93 3.58 3.5 8 3.5s8-1.57 8-3.5v-6" stroke="url(#dbGradDlg2)" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
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


        <div className="activity-bar-bottom">
          <div className="activity-icon" title="Account">
            <svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor">
              <path d="M12 12c2.7 0 4.8-2.1 4.8-4.8S14.7 2.4 12 2.4 7.2 4.5 7.2 7.2 9.3 12 12 12zm0 2.4c-3.2 0-9.6 1.6-9.6 4.8v2.4h19.2v-2.4c0-3.2-6.4-4.8-9.6-4.8z"/>
            </svg>
          </div>
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
            <div className="sidebar-title-row">
              <span className="sidebar-title">SCHEMAS</span>
              <button
                className="sidebar-refresh-btn"
                onClick={refreshSidebar}
                title="새로고침 (Refresh)"
              >⟳</button>
            </div>
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
                      <svg className="sidebar-db-icon" viewBox="0 0 24 24" width="13" height="16" preserveAspectRatio="none" fill="none">
                        <ellipse cx="12" cy="5" rx="9" ry="3.5" stroke="currentColor" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                        <path d="M3 5v6c0 1.93 4.03 3.5 9 3.5s9-1.57 9-3.5V5" stroke="currentColor" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                        <path d="M3 11v6c0 1.93 4.03 3.5 9 3.5s9-1.57 9-3.5v-6" stroke="currentColor" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                      </svg>
                      <span className="sidebar-db-name">{dbName}{isActive ? " ◀" : ""}</span>
                      <button
                        className={`sidebar-db-expand-btn${dbAllExpanded[dbName] ? " active" : ""}`}
                        onClick={e => toggleDbExpandAll(dbName, e)}
                        title={dbAllExpanded[dbName] ? "모두 접기" : "모두 펼치기"}
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
              <div className="sidebar-info-item"><span className="col-icon">◉</span> v2.2.0</div>
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

          {/* DB 우클릭 컨텍스트 메뉴 */}
          {dbCtxMenu && (
            <>
              <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={() => setDbCtxMenu(null)} />
              <div
                className="ctx-menu table-ctx-menu"
                style={{ top: dbCtxMenu.y, left: dbCtxMenu.x, zIndex: 1000 }}
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
            </>
          )}

          {/* 테이블 우클릭 컨텍스트 메뉴 */}
          {tableCtxMenu && (
            <>
              <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={() => setTableCtxMenu(null)} />
              <div
                className="ctx-menu table-ctx-menu"
                style={{ top: tableCtxMenu.y, left: tableCtxMenu.x, zIndex: 1000 }}
              >
                <div className="ctx-menu-header">{tableCtxMenu.table}</div>
                <div className="ctx-divider" />
                <div onClick={() => openEditTableModal(tableCtxMenu.table)}>Edit Table...</div>
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
            </>
          )}

          {/* 뷰 우클릭 컨텍스트 메뉴 */}
          {viewCtxMenu && (
            <>
              <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={() => setViewCtxMenu(null)} />
              <div
                className="ctx-menu table-ctx-menu"
                style={{ top: viewCtxMenu.y, left: viewCtxMenu.x, zIndex: 1000 }}
              >
                <div className="ctx-menu-header">{viewCtxMenu.view}</div>
                <div className="ctx-divider" />
                <div onClick={() => runViewCtxQuery(`SELECT * FROM ${viewCtxMenu.view};`)}>Select Rows</div>
                <div onClick={() => runViewCtxQuery(`SELECT * FROM ${viewCtxMenu.view} LIMIT 100;`)}>Select Rows (LIMIT 100)</div>
                <div onClick={() => runViewCtxQuery(`SHOW CREATE VIEW ${viewCtxMenu.view};`)}>Show Create View</div>
                <div className="ctx-divider" />
                <div onClick={() => { navigator.clipboard.writeText(viewCtxMenu.view); setViewCtxMenu(null); }}>Copy View Name</div>
                <div className="ctx-divider" />
                <div className="ctx-item-danger" onClick={() => runViewCtxQuery(`DROP VIEW ${viewCtxMenu.view};`, viewCtxMenu.view)}>
                  Drop View
                </div>
              </div>
            </>
          )}

          {/* 인덱스 우클릭 컨텍스트 메뉴 */}
          {indexCtxMenu && (
            <>
              <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={() => setIndexCtxMenu(null)} />
              <div
                className="ctx-menu table-ctx-menu"
                style={{ top: indexCtxMenu.y, left: indexCtxMenu.x, zIndex: 1000 }}
              >
                <div className="ctx-menu-header">{indexCtxMenu.index}</div>
                <div className="ctx-divider" />
                <div onClick={() => runIndexCtxQuery(`SHOW INDEX FROM ${indexCtxMenu.table};`)}>Show Index Info</div>
                <div className="ctx-divider" />
                <div onClick={() => { navigator.clipboard.writeText(indexCtxMenu.index); setIndexCtxMenu(null); }}>Copy Index Name</div>
                <div className="ctx-divider" />
                <div className="ctx-item-danger" onClick={() => runIndexCtxQuery(`DROP INDEX ${indexCtxMenu.index} ON ${indexCtxMenu.table};`, indexCtxMenu.index)}>
                  Drop Index
                </div>
              </div>
            </>
          )}

          {/* 탭 컨텍스트 메뉴 */}
          {tabCtxMenu && (() => {
            const ctxTab = tabs.find(t => t.id === tabCtxMenu.tabId);
            if (!ctxTab) return null;
            const isPinned = pinnedTabs.has(tabCtxMenu.tabId);
            const tabIdx = tabs.findIndex(t => t.id === tabCtxMenu.tabId);
            const hasTabsToRight = tabIdx < tabs.length - 1;
            const hasOtherTabs = tabs.length > 1;
            const isFromSplit = tabCtxMenu.source === "split";
            const close = () => setTabCtxMenu(null);
            return (
              <>
                <div style={{ position: "fixed", inset: 0, zIndex: 999 }} onClick={close} />
                <div className="ctx-menu tab-ctx-menu" style={{ top: tabCtxMenu.y, left: tabCtxMenu.x, zIndex: 1000 }}>
                  <div className="ctx-menu-header">{ctxTab.name}</div>
                  <div className="ctx-divider" />
                  <div className="ctx-item-with-shortcut" onClick={() => {
                    close();
                    if (isFromSplit) closeSplit(); else closeTab(tabCtxMenu.tabId);
                  }}>
                    <span>닫기</span><span className="ctx-shortcut">Ctrl+W</span>
                  </div>
                  <div
                    className={`ctx-item-with-shortcut${(!hasOtherTabs || isFromSplit) ? " ctx-item-disabled" : ""}`}
                    onClick={() => { if (!hasOtherTabs || isFromSplit) return; close(); closeOtherTabs(tabCtxMenu.tabId); }}
                  >
                    <span>다른 탭 닫기</span>
                  </div>
                  <div
                    className={`ctx-item-with-shortcut${(!hasTabsToRight || isFromSplit) ? " ctx-item-disabled" : ""}`}
                    onClick={() => { if (!hasTabsToRight || isFromSplit) return; close(); closeTabsToRight(tabCtxMenu.tabId); }}
                  >
                    <span>오른쪽 탭 닫기</span>
                  </div>
                  <div
                    className={`ctx-item-with-shortcut${(tabs.length <= 1 || isFromSplit) ? " ctx-item-disabled" : ""}`}
                    onClick={() => { if (tabs.length <= 1 || isFromSplit) return; close(); closeAllTabs(); }}
                  >
                    <span>모두 닫기</span><span className="ctx-shortcut">Ctrl+K W</span>
                  </div>
                  <div className="ctx-divider" />
                  <div className="ctx-item-with-shortcut" onClick={() => {
                    close();
                    setEditingTabId(tabCtxMenu.tabId);
                    setEditingTabName(ctxTab.name);
                    if (isFromSplit) { if (tabCtxMenu.tabId !== splitTabId) switchSplitTab(tabCtxMenu.tabId); }
                    else { if (tabCtxMenu.tabId !== activeTabId) switchTab(tabCtxMenu.tabId); }
                  }}>
                    <span>이름 변경</span>
                  </div>
                  <div className="ctx-item-with-shortcut" onClick={() => { close(); togglePin(tabCtxMenu.tabId); }}>
                    <span>{isPinned ? "고정 해제" : "고정"}</span><span className="ctx-shortcut">Ctrl+K ⇧Enter</span>
                  </div>
                  <div className="ctx-divider" />
                  <div className="ctx-item-with-shortcut" onClick={() => { close(); doSplitRight(tabCtxMenu.tabId); }}>
                    <span>오른쪽으로 분할</span><span className="ctx-shortcut">Ctrl+\</span>
                  </div>
                  <div className="ctx-item-with-shortcut" onClick={() => { close(); doSplitLeft(tabCtxMenu.tabId); }}>
                    <span>왼쪽으로 분할</span>
                  </div>
                  <div className="ctx-item-with-shortcut" onClick={() => { close(); doSplitAndMove(tabCtxMenu.tabId); }}>
                    <span>분할 및 이동</span>
                  </div>
                </div>
              </>
            );
          })()}

          {/* Edit Table 모달 */}
          {editTableModal && (
            <div className="modal-overlay" onClick={() => setEditTableModal(null)}>
              <div className="edit-table-modal" onClick={e => e.stopPropagation()}>
                <div className="edit-table-header">
                  <span>Edit Table: <strong>{editTableModal.table}</strong></span>
                  <button className="edit-table-close" onClick={() => setEditTableModal(null)}>✕</button>
                </div>
                <div className="edit-table-body">
                  <div className="edit-table-section">Columns</div>
                  <table className="edit-table-cols">
                    <thead><tr><th>Name</th><th>Type</th><th>Constraints</th><th></th></tr></thead>
                    <tbody>
                      {editTableModal.cols.map(col => (
                        <tr key={col.name}>
                          <td>{col.is_pk ? "🔑 " : ""}{col.name}</td>
                          <td>{col.data_type}</td>
                          <td className="edit-table-constraints">
                            {[col.is_pk && "PK", col.is_not_null && "NOT NULL", col.is_unique && !col.is_pk && "UNIQUE", col.is_auto_inc && "AUTO_INC"].filter(Boolean).join(", ")}
                          </td>
                          <td>
                            {!col.is_pk && (
                              <button className="drop-col-btn" onClick={() => dropColumn(editTableModal.table, col.name)}>Drop</button>
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
                      value={editTableNewCol.name}
                      onChange={e => setEditTableNewCol(p => ({ ...p, name: e.target.value }))}
                      onKeyDown={e => { if (e.key === "Enter") addColumn(); }}
                    />
                    <select
                      className="add-col-select"
                      value={editTableNewCol.type}
                      onChange={e => setEditTableNewCol(p => ({ ...p, type: e.target.value }))}
                    >
                      {["INT","BIGINT","VARCHAR(50)","VARCHAR(100)","VARCHAR(255)","TEXT","FLOAT","DOUBLE","DECIMAL(10,2)","BOOLEAN","DATE","DATETIME","TIMESTAMP"].map(t => (
                        <option key={t} value={t}>{t}</option>
                      ))}
                    </select>
                    <label className="add-col-check">
                      <input type="checkbox" checked={editTableNewCol.notNull} onChange={e => setEditTableNewCol(p => ({ ...p, notNull: e.target.checked }))} />
                      NOT NULL
                    </label>
                    <input
                      className="add-col-input"
                      placeholder="DEFAULT value (optional)"
                      value={editTableNewCol.defaultVal}
                      onChange={e => setEditTableNewCol(p => ({ ...p, defaultVal: e.target.value }))}
                      onKeyDown={e => { if (e.key === "Enter") addColumn(); }}
                    />
                    <button className="add-col-btn" onClick={addColumn}>Add Column</button>
                  </div>
                </div>
              </div>
            </div>
          )}

          <div className="main">
            <div className="tabs-row">
            <div className="tab-bar" style={splitTabId ? { width: `${splitLeftPct}%`, flex: 'none' } : {}}>
              <div className="tab-list">
                {tabs.map(tab => (
                  <div
                    key={tab.id}
                    className={`tab ${tab.id === activeTabId ? "active" : ""}${pinnedTabs.has(tab.id) ? " pinned" : ""}`}
                    onClick={() => switchTab(tab.id)}
                    onDoubleClick={e => {
                      e.stopPropagation();
                      setEditingTabId(tab.id);
                      setEditingTabName(tab.name);
                    }}
                    onContextMenu={e => {
                      e.preventDefault();
                      e.stopPropagation();
                      setTabCtxMenu({ x: e.clientX, y: e.clientY, tabId: tab.id, source: "main" });
                    }}
                  >
                    {pinnedTabs.has(tab.id) && <span className="tab-pin-icon" title="고정됨">📌</span>}
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
                    {!pinnedTabs.has(tab.id) && (
                      <span
                        className="tab-close"
                        onClick={e => closeTab(tab.id, e)}
                        title="Close tab"
                      >×</span>
                    )}
                  </div>
                ))}
                <div className="tab-add-wrap">
                  <button className="tab-add-btn" onClick={addTab} title="New query tab">+</button>
                </div>
              </div>
            </div>
            {splitTabId && (() => {
              const splitTab = splitTabStash?.id === splitTabId
                ? splitTabStash
                : tabs.find(t => t.id === splitTabId);
              return splitTab ? (
                <>
                  <div className="tabs-split-gap" />
                  <div className="split-tab-bar">
                    <div className="split-tab-list">
                      <div
                        className="split-tab active"
                        title={splitTab.name}
                        onContextMenu={e => {
                          e.preventDefault();
                          e.stopPropagation();
                          setTabCtxMenu({ x: e.clientX, y: e.clientY, tabId: splitTab.id, source: "split" });
                        }}
                      >
                        <span className="tab-icon">⊞</span>
                        {splitTab.name}
                        <span className="tab-close" onClick={e => { e.stopPropagation(); closeSplit(); }} title="분할 닫기">×</span>
                      </div>
                    </div>
                  </div>
                </>
              ) : null;
            })()}
            <div className="tab-bar-right">
              <button className="bookmark-btn" onClick={addBookmark} title="현재 쿼리 북마크 추가 (★)">★</button>
              <div className="panel-toggle-group">
                <button
                  className={`panel-toggle-btn${sidebarWidth > 0 ? " active" : ""}`}
                  onClick={() => setSidebarWidth(w => w > 0 ? 0 : 240)}
                  title="사이드바 토글"
                >
                  <svg width="16" height="12" viewBox="0 0 16 12" fill="none">
                    <rect x="0.5" y="0.5" width="15" height="11" rx="1.5" stroke="currentColor" strokeWidth="1"/>
                    <rect x="0.5" y="0.5" width="5" height="11" rx="1.5" fill="currentColor"/>
                  </svg>
                </button>
                <button
                  className={`panel-toggle-btn${resultHeight > 0 ? " active" : ""}`}
                  onClick={toggleResultPanel}
                  title="결과창 토글"
                >
                  <svg width="16" height="12" viewBox="0 0 16 12" fill="none">
                    <rect x="0.5" y="0.5" width="15" height="11" rx="1.5" stroke="currentColor" strokeWidth="1"/>
                    <rect x="0.5" y="6.5" width="15" height="5" rx="0" fill="currentColor"/>
                  </svg>
                </button>
                <button className="panel-toggle-btn" title="오른쪽 패널 (미지원)" style={{ opacity: 0.4 }}>
                  <svg width="16" height="12" viewBox="0 0 16 12" fill="none">
                    <rect x="0.5" y="0.5" width="15" height="11" rx="1.5" stroke="currentColor" strokeWidth="1"/>
                    <rect x="10.5" y="0.5" width="5" height="11" rx="1.5" fill="currentColor"/>
                  </svg>
                </button>
              </div>
            </div>
            </div>

            <div className="breadcrumb">
              <span>{currentDb}</span>
              <span className="breadcrumb-sep">›</span>
              <span>query</span>
              <span className="breadcrumb-sep">›</span>
              <span className="breadcrumb-active">{activeTab?.name ?? "query.sql"}</span>
            </div>

            <div className="editor-toolbar">
              <div className="editor-toolbar-group">
                <button className="editor-toolbar-btn" onClick={importSqlFile} title="SQL 파일 열기">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                    <path d="M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v9a2 2 0 01-2 2H5a2 2 0 01-2-2V7z" stroke="currentColor" strokeWidth="1.6"/>
                    <line x1="12" y1="12" x2="12" y2="18" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/>
                    <polyline points="9,15 12,18 15,15" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round"/>
                  </svg>
                </button>
                <button className="editor-toolbar-btn" onClick={() => activeTabId && downloadTab(activeTabId)} title="SQL 파일로 저장">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                    <path d="M19 21H5a2 2 0 01-2-2V5a2 2 0 012-2h11l5 5v11a2 2 0 01-2 2z" stroke="currentColor" strokeWidth="1.6"/>
                    <polyline points="17,21 17,13 7,13 7,21" stroke="currentColor" strokeWidth="1.4"/>
                    <polyline points="7,3 7,8 15,8" stroke="currentColor" strokeWidth="1.4"/>
                  </svg>
                </button>
              </div>
              <div className="editor-toolbar-sep"/>
              <div className="editor-toolbar-group">
                <button className="editor-toolbar-btn editor-toolbar-run" onClick={() => runQuery()} disabled={isRunning} title="실행 (Ctrl+Enter)">
                  {isRunning
                    ? <span style={{ fontSize: 13 }}>⏳</span>
                    : <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
                        <polygon points="13,2 4,13 11,13 11,22 20,11 13,11"/>
                      </svg>
                  }
                </button>
                <button className="editor-toolbar-btn editor-toolbar-run" onClick={() => runQuery(true)} disabled={isRunning} title="전체 실행">
                  {isRunning
                    ? <span style={{ fontSize: 13 }}>⏳</span>
                    : <span style={{ position: "relative", display: "inline-flex", width: 16, height: 16 }}>
                        <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
                          <polygon points="13,2 4,13 11,13 11,22 20,11 13,11"/>
                        </svg>
                        <span style={{ position: "absolute", bottom: -2, right: -3, fontSize: 8, fontWeight: 700, lineHeight: 1 }}>A</span>
                      </span>
                  }
                </button>
              </div>
            </div>

            <div className="editor-and-chat">
            <div className="editor-area">
              <div className="editor-pane" style={splitTabId ? { width: `${splitLeftPct}%` } : {}}>
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
                    localStorage.setItem(`rusql_tabs_${connIdRef.current}`, JSON.stringify(next));
                    return next;
                  });
                }}
                theme="rusql-dark"
                options={{
                  fontSize: 14,
                  fontFamily: "Consolas, 'Courier New', monospace",
                  minimap: { enabled: true },
                  scrollBeyondLastLine: false,
                  lineNumbers: "on",
                  renderLineHighlight: "all",
                  automaticLayout: true,
                  padding: { top: 12 },
                  quickSuggestions: false,
                  suggestOnTriggerCharacters: false,
                  wordBasedSuggestions: "off",
                }}
                beforeMount={monaco => {
                  monaco.editor.defineTheme("rusql-dark", {
                    base: "vs-dark",
                    inherit: true,
                    rules: [
                      // 키워드 (DML/DDL/제어): 파란색
                      { token: "keyword",              foreground: "569CD6", fontStyle: "bold" },
                      // 데이터 타입: 청록색
                      { token: "keyword.type",         foreground: "4EC9B0" },
                      // 내장 함수: 노란색
                      { token: "keyword.other",        foreground: "DCDCAA" },
                      // 문자열: 핑크색
                      { token: "string",               foreground: "F92672" },
                      { token: "string.sql",           foreground: "F92672" },
                      { token: "string.invalid",       foreground: "F92672" },
                      { token: "string.invalid.sql",   foreground: "F92672" },
                      // 숫자: 연두색
                      { token: "number",               foreground: "B5CEA8" },
                      // 주석: 초록회색
                      { token: "comment",              foreground: "6A6A6A" },
                      { token: "comment.line",         foreground: "6A6A6A" },
                      { token: "comment.block",        foreground: "6A6A6A" },
                      // 연산자
                      { token: "operator",             foreground: "D4D4D4" },
                      // 구분자
                      { token: "delimiter",            foreground: "D4D4D4" },
                      { token: "delimiter.parenthesis",foreground: "D4D4D4" },
                    ],
                    colors: {
                      "editor.background":              "#1e1e1e",
                      "editor.foreground":              "#D4D4D4",
                      "editorLineNumber.foreground":    "#858585",
                      "editor.lineHighlightBackground": "#2a2d2e",
                    },
                  });

                  monaco.languages.setMonarchTokensProvider("sql", {
                    defaultToken: "",
                    tokenPostfix: ".sql",
                    ignoreCase: true,
                    brackets: [
                      { open: "(", close: ")", token: "delimiter.parenthesis" },
                      { open: "[", close: "]", token: "delimiter.square" },
                    ],
                    keywords: [
                      "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","DELETE",
                      "CREATE","TABLE","DROP","ALTER","ADD","COLUMN","RENAME","TO",
                      "JOIN","LEFT","RIGHT","INNER","OUTER","CROSS","FULL","NATURAL","ON","USING",
                      "AND","OR","NOT","IN","BETWEEN","LIKE","REGEXP","IS","NULL","EXISTS",
                      "ORDER","GROUP","BY","ASC","DESC","LIMIT","OFFSET","HAVING","DISTINCT","ALL",
                      "UNION","INTERSECT","EXCEPT","AS","WITH","RECURSIVE",
                      "CASE","WHEN","THEN","ELSE","END",
                      "INDEX","UNIQUE","VIEW","PRIMARY","KEY","FOREIGN","REFERENCES",
                      "CONSTRAINT","CASCADE","RESTRICT","DEFAULT","CHECK","AUTO","INCREMENT",
                      "SHOW","TABLES","DESCRIBE","TRUNCATE","EXPLAIN","ANALYZE",
                      "BEGIN","COMMIT","ROLLBACK","TRANSACTION","SAVEPOINT","RELEASE",
                      "ISOLATION","LEVEL","UNCOMMITTED","COMMITTED","REPEATABLE","SERIALIZABLE",
                      "FOR","SHARE","LOCK","LOCKS","CHECKPOINT","VACUUM",
                      "PROCEDURE","FUNCTION","TRIGGER","RETURNS","RETURN","CALL",
                      "DECLARE","IF","THEN","ELSEIF","WHILE","DO","LOOP","LEAVE","ITERATE","REPEAT","UNTIL",
                      "GRANT","REVOKE","USER","ROLE","IDENTIFIED","PRIVILEGES","DATABASE","USE","DATABASES",
                      "BACKUP","MERGE","MATCHED","FETCH","NEXT","ROWS","ONLY",
                      "PARTITION","OVER","WINDOW","RANGE","PRECEDING","FOLLOWING","UNBOUNDED","CURRENT","ROW",
                      "PREPARE","EXECUTE","DEALLOCATE","OUTFILE","REPLACE","IGNORE",
                      "SYNONYM","SET","AFTER","BEFORE","EACH","EACH",
                    ],
                    datatypes: [
                      "INT","INTEGER","BIGINT","SMALLINT","TINYINT","MEDIUMINT",
                      "FLOAT","DOUBLE","DECIMAL","NUMERIC","REAL",
                      "CHAR","VARCHAR","TEXT","TINYTEXT","MEDIUMTEXT","LONGTEXT",
                      "BINARY","VARBINARY","BLOB","TINYBLOB","MEDIUMBLOB","LONGBLOB",
                      "DATE","DATETIME","TIMESTAMP","TIME","YEAR",
                      "BOOLEAN","BOOL","BIT","JSON","ENUM","SET",
                    ],
                    builtins: [
                      "COUNT","SUM","AVG","MIN","MAX","GROUP_CONCAT","STDDEV","VARIANCE",
                      "ROW_NUMBER","RANK","DENSE_RANK","NTILE","LAG","LEAD",
                      "FIRST_VALUE","LAST_VALUE","NTH_VALUE","PERCENT_RANK","CUME_DIST",
                      "UPPER","LOWER","LENGTH","CONCAT","SUBSTR","SUBSTRING","REPLACE","TRIM",
                      "LTRIM","RTRIM","LPAD","RPAD","CHAR_LENGTH","LEFT","RIGHT","REVERSE",
                      "REPEAT","INSTR","ASCII","HEX","FORMAT","REGEXP_LIKE","REGEXP_REPLACE",
                      "ROUND","ABS","CEIL","CEILING","FLOOR","MOD","SQRT","POW","POWER",
                      "LOG","LOG2","LOG10","PI","SIGN","TRUNCATE","RAND","EXP",
                      "NOW","CURDATE","CURTIME","YEAR","MONTH","DAY","DAYOFWEEK",
                      "HOUR","MINUTE","SECOND","DATE_ADD","DATE_SUB","DATE_FORMAT",
                      "DATEDIFF","TIMESTAMPDIFF","EXTRACT",
                      "COALESCE","IFNULL","NULLIF","ISNULL","GREATEST","LEAST",
                      "CAST","CONVERT","IF","MD5","UUID","LAST_INSERT_ID",
                      "DATABASE","VERSION","USER","CURRENT_USER","SESSION_USER","SYSTEM_USER",
                      "JSON_EXTRACT","JSON_VALUE","JSON_OBJECT","JSON_ARRAY",
                    ],
                    tokenizer: {
                      root: [
                        { include: "@comments" },
                        [/[a-zA-Z_]\w*/, { cases: {
                          "@keywords":  "keyword",
                          "@datatypes": "keyword.type",
                          "@builtins":  "keyword.other",
                          "@default":   "identifier",
                        }}],
                        [/'([^'\\]|\\.)*'/, "string"],
                        [/'([^'\\]|\\.)*$/, "string.invalid"],
                        [/\d+(\.\d+)?/,     "number"],
                        [/[=!<>]+/,         "operator"],
                        [/[(),;.]/,         "delimiter"],
                        [/\s+/,             "white"],
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
                  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter, () => runQueryRef.current());
                  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyMod.Shift | monaco.KeyCode.KeyF, () => {
                    try {
                      const fmt = sqlFormat(editor.getValue(), { language: 'sql', tabWidth: 2, keywordCase: 'upper' });
                      editor.setValue(fmt);
                    } catch {}
                  });
                }}
              />
            </div>
            </div>
            {splitTabId && (() => {
              const splitTab = splitTabStash?.id === splitTabId
                ? splitTabStash
                : tabs.find(t => t.id === splitTabId);
              return splitTab ? (
                <>
                  <div
                    className="split-divider"
                    onMouseDown={() => {
                      isSplitDragging.current = true;
                      document.body.style.cursor = "col-resize";
                      document.body.style.userSelect = "none";
                    }}
                  />
                  <div className="editor-pane" style={{ width: `${100 - splitLeftPct}%` }}>
                    <div className="editor-container">
                      <Editor
                        key={splitTabId}
                        height="100%"
                        defaultLanguage="sql"
                        defaultValue={splitQueryRef.current}
                        onChange={val => {
                          if (isSplitSwitching.current) return;
                          splitQueryRef.current = val ?? "";
                          // stash된 탭이면 stash를 업데이트, 아니면 tabs[]를 업데이트
                          if (splitTabStash?.id === splitTabId) {
                            setSplitTabStash(prev => prev ? { ...prev, content: splitQueryRef.current } : prev);
                          } else {
                            setTabs(prev => {
                              const next = prev.map(t => t.id === splitTabId ? { ...t, content: splitQueryRef.current } : t);
                              localStorage.setItem(`rusql_tabs_${connIdRef.current}`, JSON.stringify(next));
                              return next;
                            });
                          }
                        }}
                        theme="rusql-dark"
                        options={{
                          fontSize: 14,
                          fontFamily: "Consolas, 'Courier New', monospace",
                          minimap: { enabled: true },
                          scrollBeyondLastLine: false,
                          lineNumbers: "on",
                          renderLineHighlight: "all",
                          automaticLayout: true,
                          padding: { top: 12 },
                          quickSuggestions: false,
                          suggestOnTriggerCharacters: false,
                          wordBasedSuggestions: "off",
                        }}
                        onMount={(editor) => {
                          splitEditorRef.current = editor;
                        }}
                      />
                    </div>
                  </div>
                </>
              ) : null;
            })()}
            </div>
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
                {isRunning && <div className="query-progress-bar"><div className="query-progress-fill" /></div>}
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
                  <div className="result-empty">{isRunning ? "쿼리 실행 중..." : "Ctrl+Enter 또는 ▶ Run 으로 쿼리를 실행하세요"}</div>
                ) : results.map((r, i) => (
                  <div key={i} className="result-block">
                    {!r.success ? (
                      <div className="result-error">
                        <div>❌ {r.message}</div>
                      </div>
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
                        // 컬럼별 자동 너비: canvas measureText로 실제 픽셀 너비 측정 (한글/CJK 지원)
                        const CELL_PAD = 36; // 좌우 패딩 + 정렬 아이콘 여유
                        const sampleRows = r.rows.slice(0, 200);
                        const autoWidths = r.columns.map((col, ci) => {
                          const maxDataPx = sampleRows.reduce(
                            (max, row) => Math.max(max, measureTextPx(row[ci] ?? '')), 0
                          );
                          const w = Math.max(measureTextPx(col + ' ⇅'), maxDataPx);
                          return Math.min(500, Math.max(60, Math.round(w + CELL_PAD)));
                        });
                        const effectiveWidths = colWidths[i] ?? autoWidths;
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
                            <table className="result-table" style={{ tableLayout: 'fixed', width: 'auto' }}>
                              <thead><tr>
                                <th className="result-rownum" style={{ width: 40 }}>#</th>
                                {r.columns.map((c, ci) => (
                                <th key={c} style={{ width: effectiveWidths[ci], position: 'relative', userSelect: 'none', cursor: 'pointer' }}
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
                                      // index 0은 # 열이므로 데이터 열은 index 1부터 시작 → ci+1
                                      const allThs = Array.from(thEl.parentElement!.querySelectorAll<HTMLTableCellElement>('th'));
                                      const initWidths = allThs.slice(1).map(th => th.getBoundingClientRect().width);
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
                                  {row.map((cell, ci) => {
                                    const isEditing = editingCell?.resultIdx === i && editingCell.rowIdx === ri && editingCell.colIdx === ci;
                                    return (
                                      <td
                                        key={ci}
                                        style={colWidths[i] ? { overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' } : undefined}
                                        onDoubleClick={() => handleCellDoubleClick(i, ri, ci, cell, row, r.columns)}
                                      >
                                        {isEditing ? (
                                          <input
                                            className="cell-edit-input"
                                            value={editingValue}
                                            autoFocus
                                            onChange={e => setEditingValue(e.target.value)}
                                            onBlur={commitCellEdit}
                                            onKeyDown={e => {
                                              if (e.key === "Enter") { e.preventDefault(); commitCellEdit(); }
                                              if (e.key === "Escape") { e.preventDefault(); setEditingCell(null); }
                                            }}
                                            onClick={e => e.stopPropagation()}
                                          />
                                        ) : cell}
                                      </td>
                                    );
                                  })}
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
                <span className="status-item">RuSQL v2.2.0</span>
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
              <div className="erd-zoom-slider">
                <input
                  type="range"
                  min={0}
                  max={200}
                  value={Math.round(erdZoom * 100)}
                  onChange={e => setErdZoom(Number(e.target.value) / 100)}
                  title="확대/축소 (0~200%)"
                />
                <span className="erd-zoom-label">{Math.round(erdZoom * 100)}%</span>
              </div>
              <button className="erd-tool-btn" onClick={autoLayoutErd} title={isAutoLayout ? "원래 위치로 복원" : "FK 기반 자동 배치"}>
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
              <span className="status-item">RuSQL v2.2.0</span>
              <span className="status-item">ERD Editor</span>
            </div>
          </div>
        </div>
      )}


      {/* ── 서버 관리 뷰 ───────────────────────────────────────── */}
      {activeView === "server" && (
        <div className="server-view">
          {/* ── 메인 영역 (좌측 콘텐츠 + 슬라이드 패널 + 우측 버튼바) ── */}
          <div className="srv-main-area">

            {/* ── 좌측 컬럼 (연결 폼 + 로그) ── */}
            <div className="srv-left-col">
              <div className="srv-scroll-area">
                <div className="srv-conn-panel">

                  {/* 아이콘 + 제목 */}
                  <div className="srv-conn-header">
                    <svg className="srv-db-icon" width="65" height="65" viewBox="0 0 24 24" preserveAspectRatio="none" fill="none">
                      <defs><linearGradient id="dbGradSrv" x1="0" y1="0" x2="0" y2="24" gradientUnits="userSpaceOnUse"><stop offset="0%" stopColor="#f5c26b"/><stop offset="100%" stopColor="#c97a1a"/></linearGradient></defs>
                      <ellipse cx="12" cy="3" rx="8" ry="2.5" stroke="url(#dbGradSrv)" strokeWidth="1.5" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 3v6c0 1.38 3.58 2.5 8 2.5s8-1.12 8-2.5v-6" stroke="url(#dbGradSrv)" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 9v6c0 1.38 3.58 2.5 8 2.5s8-1.12 8-2.5v-6" stroke="url(#dbGradSrv)" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                      <path d="M4 15v6c0 1.38 3.58 2.5 8 2.5s8-1.12 8-2.5v-6" stroke="url(#dbGradSrv)" strokeWidth="1.5" fill="none" vectorEffect="non-scaling-stroke"/>
                    </svg>
                    <div>
                      <div className="srv-conn-title">서버에 연결</div>
                      <div className="srv-conn-sub">RuSQL TCP Server</div>
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

                  {/* 폼 본문 */}
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
                          <button className="srv-port-btn" onClick={() => setPortInput(p => String(Math.max(1024, parseInt(p || "7878") - 1)))} disabled={serverStatus.running}>−</button>
                          <input className="srv-form-input srv-port-num" value={portInput} onChange={e => setPortInput(e.target.value)} disabled={serverStatus.running}/>
                          <button className="srv-port-btn" onClick={() => setPortInput(p => String(Math.min(65535, parseInt(p || "7878") + 1)))} disabled={serverStatus.running}>+</button>
                        </div>
                      </div>
                      <div className="srv-form-col" style={{ maxWidth: 160 }}>
                        <label className="srv-form-label">MySQL 포트 <span style={{ color: "var(--text-muted)", fontWeight: 400 }}>(0=비활성)</span></label>
                        <div className="srv-port-wrap">
                          <button className="srv-port-btn" onClick={() => setMysqlPortInput(p => String(Math.max(0, parseInt(p || "0") - 1)))} disabled={serverStatus.running}>−</button>
                          <input className="srv-form-input srv-port-num" value={mysqlPortInput} onChange={e => setMysqlPortInput(e.target.value)} disabled={serverStatus.running}/>
                          <button className="srv-port-btn" onClick={() => setMysqlPortInput(p => String(Math.min(65535, parseInt(p || "0") + 1)))} disabled={serverStatus.running}>+</button>
                        </div>
                      </div>
                    </div>

                    {/* 사용자 / 비밀번호 */}
                    <div className="srv-form-row">
                      <div className="srv-form-col">
                        <label className="srv-form-label"><span className="srv-req">*</span> 사용자 이름</label>
                        <input className="srv-form-input" value={srvUser} onChange={e => setSrvUser(e.target.value)} placeholder="root"/>
                      </div>
                      <div className="srv-form-col">
                        <label className="srv-form-label"><span className="srv-req">*</span> 비밀번호</label>
                        <div className="srv-pass-wrap">
                          <input className="srv-form-input" type={srvPassVisible ? "text" : "password"} value={srvPass} onChange={e => setSrvPass(e.target.value)} placeholder="비밀번호"/>
                          <button className="srv-pass-toggle" onClick={() => setSrvPassVisible(v => !v)} title={srvPassVisible ? "숨기기" : "표시"}>{srvPassVisible ? "🙈" : "👁"}</button>
                        </div>
                      </div>
                    </div>

                    {/* 버퍼 풀 크기 + 병렬 쿼리 토글 + 터미널 버튼 */}
                    <div className="srv-form-row srv-inline-row">
                      <div className="srv-inline-group">
                        <label className="srv-form-label">
                          버퍼 풀 크기
                          <span style={{ color: "var(--text-muted)", fontWeight: 400, marginLeft: 4 }}>페이지 (×16KB)</span>
                        </label>
                        <div className="srv-port-wrap">
                          <button className="srv-port-btn" onClick={() => { const v = String(Math.max(8, (parseInt(bufferPoolInput)||64) - 8)); setBufferPoolInput(v); localStorage.setItem("rusql_bp_size", v); }}>−</button>
                          <input className="srv-form-input srv-port-num" value={bufferPoolInput} onChange={e => { setBufferPoolInput(e.target.value); localStorage.setItem("rusql_bp_size", e.target.value); }}/>
                          <button className="srv-port-btn" onClick={() => { const v = String(Math.min(4096, (parseInt(bufferPoolInput)||64) + 8)); setBufferPoolInput(v); localStorage.setItem("rusql_bp_size", v); }}>+</button>
                        </div>
                      </div>
                      <div className="srv-inline-group">
                        <label className="srv-form-label">병렬 쿼리</label>
                        <div className="srv-toggle-row">
                          <button
                            className={`srv-toggle-btn ${parallelQuery ? "on" : "off"}`}
                            onClick={() => { const next = !parallelQuery; setParallelQuery(next); localStorage.setItem("rusql_parallel", next ? "1" : "0"); invoke("set_parallel_query", { enabled: next }); }}
                          >
                            <span className="srv-toggle-knob" />
                          </button>
                          <span className="srv-toggle-label">
                            {parallelQuery ? "ON — 10,000행+ SeqScan 자동 병렬 처리" : "OFF — 단일 스레드"}
                          </span>
                        </div>
                      </div>
                      <div className="srv-inline-group" style={{ marginLeft: "auto" }}>
                        <label className="srv-form-label" style={{ opacity: 0 }}>.</label>
                        <button className="srv-terminal-btn" onClick={() => invoke("open_terminal")} title="터미널 열기">
                          <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
                            <path d="M20 4H4C2.9 4 2 4.9 2 6V18C2 19.1 2.9 20 4 20H20C21.1 20 22 19.1 22 18V6C22 4.9 21.1 4 20 4ZM20 18H4V6H20V18ZM6 10L8.5 12.5L6 15L7.5 16.5L11.5 12.5L7.5 8.5L6 10ZM12 15H18V17H12V15Z"/>
                          </svg>
                          터미널 열기
                        </button>
                      </div>
                    </div>

                    {/* 인증 방식 */}
                    <div className="srv-form-row">
                      <div className="srv-form-col srv-full-col">
                        <label className="srv-form-label">인증 방식</label>
                        <div className="srv-auth-badge">RuSQL AUTH Protocol v1</div>
                      </div>
                    </div>

                    <div className="srv-divider" />

                    {/* 상태 */}
                    <div className="srv-status-strip">
                      <span className={`srv-dot ${serverStatus.running ? "running" : "stopped"}`} />
                      <span className={`srv-strip-text ${serverStatus.running ? "running" : "stopped"}`}>
                        {serverStatus.running ? `RUNNING · 127.0.0.1:${serverStatus.port} · ${serverStatus.client_count} 클라이언트` : "STOPPED"}
                      </span>
                    </div>

                    {serverMsg && <div className="srv-feedback">{serverMsg}</div>}

                    {/* 버튼 행 */}
                    <div className="srv-action-row">
                      <button
                        className={`srv-action-btn ${serverStatus.running ? "danger" : "primary"}`}
                        onClick={serverStatus.running ? handleStopServer : handleStartServer}
                      >
                        {serverStatus.running
                          ? <><svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><rect x="4" y="4" width="16" height="16" rx="1"/></svg>서버 중지</>
                          : <><svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor"><polygon points="5,3 19,12 5,21"/></svg>서버 시작</>
                        }
                      </button>
                      <div style={{ flex: 1 }} />
                      <button className="srv-action-btn save" onClick={() => setServerMsg("설정이 저장되었습니다.")}>
                        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                          <path d="M19 21H5a2 2 0 01-2-2V5a2 2 0 012-2h11l5 5v11a2 2 0 01-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/>
                        </svg>
                        저장
                      </button>
                    </div>
                  </div>
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
            </div>{/* /srv-left-col */}

            {/* ── 슬라이드 패널 ── */}
            {srvRightPanel !== "none" && (
              <div className="srv-slide-panel">
                <div className="srv-slide-header">
                  {srvRightPanel === "cli" ? "CLI 가이드" : srvRightPanel === "bench" ? "벤치마크" : srvRightPanel === "sessions" ? "접속 세션" : srvRightPanel === "mcp" ? "MCP Agent" : "MySQL 연결"}
                  <button className="srv-slide-close" onClick={() => setSrvRightPanel("none")}>✕</button>
                </div>
                <div className="srv-slide-body">
                  {srvRightPanel === "cli" && (<>
                    <div className="srv-slide-section" style={{ textTransform: 'none' }}>RuSQL-Client</div>
                    <code className="srv-slide-code">{`cargo run -p rusql-client -- \\
  -u ${srvUser} -p <password> \\
  -h 127.0.0.1 -P ${portInput}`}</code>

                    <div className="srv-slide-section" style={{ marginTop: 18 }}>다른 컴퓨터에서 접속</div>
                    <div className="srv-slide-flow">
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow in">1.</span><code>서버 PC에서 rusql-server 실행</code></div>
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow out">2.</span><code>접속 PC에서 서버 IP로 연결</code></div>
                    </div>
                    <code className="srv-slide-code" style={{ marginTop: 8 }}>{`cargo run -p rusql-client -- \\\n  -u ${srvUser} -p <password> \\\n  -h <server-ip> -P ${portInput}`}</code>

                    <div className="srv-slide-section" style={{ marginTop: 18, textTransform: 'none' }}>인증 흐름 (RuSQL AUTH Protocol v1)</div>
                    <div className="srv-slide-flow">
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow out">→</span><code>{`AUTH ${srvUser} <password>`}</code></div>
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow in">←</span><code>{`OK authenticated as '${srvUser}'`}</code></div>
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow out">→</span><code>SELECT * FROM t;</code></div>
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow in">←</span><code>{"OK\n<결과>\n(0.001 sec)\n---END---"}</code></div>
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow out">→</span><code>exit</code></div>
                      <div className="srv-slide-flow-row"><span className="srv-slide-arrow in">←</span><code>Bye!</code></div>
                    </div>

                    <div className="srv-slide-section" style={{ marginTop: 18 }}>PowerShell 직접 접속</div>
                    <code className="srv-slide-code">{`$c = New-Object Net.Sockets.TcpClient('127.0.0.1',${portInput})\n$s = $c.GetStream()\n$w = New-Object IO.StreamWriter($s)\n$w.WriteLine("AUTH ${srvUser} <password>")\n$w.Flush()`}</code>

                    <div className="srv-slide-section" style={{ marginTop: 18 }}>netcat</div>
                    <code className="srv-slide-code">{`nc 127.0.0.1 ${portInput}`}</code>
                  </>)}

                  {srvRightPanel === "sessions" && (() => {
                    const now = Math.floor(Date.now() / 1000);
                    const fmtDuration = (secs: number) => {
                      if (secs < 60) return `${secs}s`;
                      if (secs < 3600) return `${Math.floor(secs / 60)}m ${secs % 60}s`;
                      return `${Math.floor(secs / 3600)}h ${Math.floor((secs % 3600) / 60)}m`;
                    };
                    return (
                      <>
                        <div className="srv-slide-section">접속 중인 세션</div>
                        {serverStatus.sessions.length === 0 ? (
                          <div className="srv-slide-text">현재 접속 중인 세션이 없습니다.</div>
                        ) : serverStatus.sessions.map((s, i) => (
                          <div key={i} style={{ marginBottom: 10, padding: "8px 10px", background: "#2d2d2d", borderRadius: 5, border: "1px solid #3a3d41" }}>
                            <div style={{ display: "flex", alignItems: "center", gap: 6, marginBottom: 4 }}>
                              <span style={{ width: 8, height: 8, borderRadius: "50%", background: "#4ec9b0", display: "inline-block", flexShrink: 0 }} />
                              <span style={{ fontSize: 13, fontWeight: 600, color: "#d4d4d4" }}>{s.user}</span>
                              <span style={{ marginLeft: "auto", fontSize: 11, color: "#858585" }}>{fmtDuration(now - s.connected_at)}</span>
                            </div>
                            <div style={{ fontSize: 11, color: "#858585", paddingLeft: 14 }}>{s.addr}</div>
                            <div style={{ fontSize: 11, color: "#858585", paddingLeft: 14, marginTop: 2 }}>쿼리 {s.query_count}건</div>
                          </div>
                        ))}
                        <div style={{ marginTop: 6, fontSize: 11, color: "#555", textAlign: "center" }}>1.5s 자동 갱신</div>
                      </>
                    );
                  })()}

                  {srvRightPanel === "bench" && (<>
                    <div className="srv-slide-section">성능 벤치마크</div>
                    <div className="srv-slide-text" style={{ marginBottom: 10 }}>
                      단순/Bulk 처리량 · 인덱스 가속 · 병렬 집계
                    </div>
                    <div style={{ display: "flex", gap: 8, marginBottom: 14 }}>
                      <button
                        className="srv-action-btn primary"
                        style={{ fontSize: 12 }}
                        disabled={benchLoading}
                        onClick={async () => {
                          setBenchLoading(true);
                          try {
                            const raw: string = await invoke("read_bench_result");
                            setBenchResult(raw ? JSON.parse(raw) : null);
                          } catch { setBenchResult(null); }
                          setBenchLoading(false);
                        }}
                      >{benchLoading ? "불러오는 중..." : "결과 불러오기"}</button>
                      <button
                        className="srv-action-btn save"
                        style={{ fontSize: 12 }}
                        onClick={() => invoke("open_bench_terminal")}
                      >터미널 실행</button>
                    </div>
                    {benchResult !== undefined && benchResult !== null ? (() => {
                      const r = benchResult as Record<string, unknown>;
                      const fms  = (v: unknown) => typeof v === "number" ? v.toFixed(3) : "—";
                      const fmx  = (v: unknown) => typeof v === "number" ? v.toFixed(1) : "—";
                      const fmtps = (rows: number, secs: number) =>
                        secs > 0 ? Math.round(rows / secs).toLocaleString("ko-KR") + " rows/s" : "—";
                      const fmtime = (v: unknown) => typeof v === "number" ? v.toFixed(2) + "초" : "—";
                      const greenBadge = (label: string) => (
                        <span style={{ background: "#4ec9b0", color: "#1e1e1e", fontWeight: 700, padding: "1px 6px", borderRadius: 3, fontSize: 11, marginLeft: 6 }}>{label}</span>
                      );
                      const row = (label: string, val: string) => (
                        <div key={label} style={{ display: "flex", justifyContent: "space-between", paddingLeft: 8 }}>
                          <span style={{ color: "#9cdcfe" }}>{label}</span>
                          <strong>{val}</strong>
                        </div>
                      );
                      const sg = r.single as Record<string,number> | undefined;
                      const bk = r.bulk   as Record<string,number> | undefined;
                      const pl = r.point_lookup as Record<string,number> | undefined;
                      const rq = r.range_query  as Record<string,number> | undefined;
                      const tk = r.top_k        as Record<string,number> | undefined;
                      const pa = r.parallel     as Record<string,number> | undefined;
                      return (
                        <div style={{ fontSize: 12, lineHeight: 1.8 }}>
                          {sg && <div style={{ marginBottom: 10 }}>
                            <div style={{ color: "#4ec9b0", fontWeight: 600, marginBottom: 4 }}>
                              단순 처리 ({(sg.rows ?? 10000).toLocaleString()}건 · 단건 I/O)
                            </div>
                            {row("INSERT", `${fmtps(sg.rows, sg.insert_s)}  (${fmtime(sg.insert_s)})`)}
                            {row("DELETE", `${fmtps(sg.rows, sg.delete_s)}  (${fmtime(sg.delete_s)})`)}
                          </div>}
                          {bk && (() => {
                            const perRowSpeedup = sg && sg.insert_s > 0 && bk.insert_s > 0
                              ? (sg.insert_s / sg.rows) / (bk.insert_s / bk.rows)
                              : 0;
                            return (
                              <div style={{ marginBottom: 10 }}>
                                <div style={{ color: "#4ec9b0", fontWeight: 600, marginBottom: 4 }}>
                                  Bulk 처리 ({(bk.rows ?? 100000).toLocaleString()}건 · 500행 묶음)
                                  {perRowSpeedup >= 10 && greenBadge(`단건 대비 ${Math.round(perRowSpeedup)}배 효율`)}
                                </div>
                                {row("INSERT", `${fmtps(bk.rows, bk.insert_s)}  (${fmtime(bk.insert_s)})`)}
                                {row("DELETE", `${fmtps(bk.rows, bk.delete_s)}  (${fmtime(bk.delete_s)})`)}
                              </div>
                            );
                          })()}
                          {pl && <div style={{ marginBottom: 10 }}>
                            <div style={{ color: "#4ec9b0", fontWeight: 600, marginBottom: 2 }}>포인트 조회 (등호){greenBadge(`${fmx(pl.speedup)}x 빠름`)}</div>
                            {row("SeqScan", `${fms(pl.seq_ms)} ms/q`)}
                            {row("BTree Index", `${fms(pl.idx_ms)} ms/q`)}
                          </div>}
                          {rq && <div style={{ marginBottom: 10 }}>
                            <div style={{ color: "#4ec9b0", fontWeight: 600, marginBottom: 2 }}>범위 쿼리 (BETWEEN){greenBadge(`${fmx(rq.speedup)}x 빠름`)}</div>
                            {row("No-Index", `${fms(rq.seq_ms)} ms/q`)}
                            {row("BTree Index", `${fms(rq.idx_ms)} ms/q`)}
                          </div>}
                          {tk && <div style={{ marginBottom: 10 }}>
                            <div style={{ color: "#4ec9b0", fontWeight: 600, marginBottom: 2 }}>Top-K (ORDER BY LIMIT){greenBadge(`${fmx(tk.speedup)}x 빠름`)}</div>
                            {row("SeqScan+Sort", `${fms(tk.seq_ms)} ms/q`)}
                            {row("Index Fast-Path", `${fms(tk.idx_ms)} ms/q`)}
                          </div>}
                          {pa && <div>
                            <div style={{ color: "#4ec9b0", fontWeight: 600, marginBottom: 2 }}>병렬 집계 (GROUP BY)</div>
                            {row("PARALLEL OFF", `${fmx(pa.off_ms)} ms/q`)}
                            {row("PARALLEL ON",  `${fmx(pa.on_ms)} ms/q`)}
                          </div>}
                        </div>
                      );
                    })() : benchResult === null ? (
                      <div className="srv-slide-text" style={{ color: "#f48771" }}>result.json 없음 — 터미널 실행으로 bench.py를 먼저 완료하세요.</div>
                    ) : null}
                  </>)}

                  {srvRightPanel === "mysql" && (<>
                    <div className="srv-slide-section">mysql CLI</div>
                    <code className="srv-slide-code">{`mysql -h 127.0.0.1 -P ${mysqlPortInput || 13306} \\\n  -u ${srvUser} -p<password> \\\n  --ssl-mode=DISABLED`}</code>

                    <div className="srv-slide-section" style={{ marginTop: 18 }}>Python (mysql-connector)</div>
                    <code className="srv-slide-code">{`import mysql.connector\nconn = mysql.connector.connect(\n  host="127.0.0.1",\n  port=${mysqlPortInput || 13306},\n  user="${srvUser}",\n  password="<password>"\n)\ncur = conn.cursor()\ncur.execute("SHOW DATABASES")\nfor row in cur: print(row)`}</code>

                    <div className="srv-slide-section" style={{ marginTop: 18 }}>DBeaver</div>
                    <code className="srv-slide-code">{`New Connection → MySQL\nHost: 127.0.0.1\nPort: ${mysqlPortInput || 13306}\nUser: ${srvUser}\nSSL: 비활성화 (allowPublicKeyRetrieval=true)`}</code>

                    <div className="srv-slide-section" style={{ marginTop: 18 }}>인증 방식</div>
                    <code className="srv-slide-code">{`mysql_native_password\nSHA1(SHA1(pw)) 챌린지-응답\n포트 ${mysqlPortInput || 13306}에서 수신`}</code>
                  </>)}

                  {srvRightPanel === "mcp" && (<>
                    <div className="srv-slide-section">사용 방법</div>
                    <div style={{ display: "flex", flexDirection: "column", gap: 6, marginTop: 6 }}>
                      <div className="srv-slide-desc" style={{ margin: 0 }}>① RuSQL 서버 시작 (이 탭에서)</div>
                      <div className="srv-slide-desc" style={{ margin: 0 }}>② 아래 버튼으로 Claude Desktop에 자동 등록</div>
                      <div className="srv-slide-desc" style={{ margin: 0 }}>③ Claude Desktop 재시작</div>
                    </div>

                    <button
                      className="srv-action-btn save"
                      style={{ marginTop: 14, width: "100%" }}
                      onClick={async () => {
                        setMcpSetupMsg(null);
                        try {
                          const msg = await invoke<string>("setup_mcp_config");
                          setMcpSetupMsg({ ok: true, text: msg });
                        } catch (e) {
                          setMcpSetupMsg({ ok: false, text: String(e) });
                        }
                      }}
                    >
                      Claude Desktop 자동 연결
                    </button>
                    {mcpSetupMsg && (
                      <div style={{
                        marginTop: 8, padding: "7px 10px", borderRadius: 4, fontSize: 11,
                        background: mcpSetupMsg.ok ? "rgba(78,201,176,0.08)" : "rgba(244,135,113,0.08)",
                        border: `1px solid ${mcpSetupMsg.ok ? "rgba(78,201,176,0.3)" : "rgba(244,135,113,0.3)"}`,
                        color: mcpSetupMsg.ok ? "#4ec9b0" : "#f48771",
                        whiteSpace: "pre-wrap", lineHeight: 1.6, wordBreak: "break-all",
                      }}>
                        {mcpSetupMsg.text}
                      </div>
                    )}

                    <div className="srv-slide-section" style={{ marginTop: 18 }}>제공 도구</div>
                    <div style={{ display: "flex", flexWrap: "wrap", gap: 4, marginTop: 6 }}>
                      {["execute_sql", "list_databases", "list_tables", "get_table_schema", "explain_query", "get_indexes", "sample_data"].map(t => (
                        <span key={t} style={{ fontSize: 11, background: "rgba(78,201,176,0.12)", color: "#4ec9b0", border: "1px solid rgba(78,201,176,0.25)", borderRadius: 3, padding: "2px 7px", fontFamily: "monospace" }}>{t}</span>
                      ))}
                    </div>
                  </>)}
                </div>
              </div>
            )}

            {/* ── 우측 버튼바 ── */}
            <div className="srv-right-bar">
              <button
                className={`srv-rbar-btn ${srvRightPanel === "cli" ? "active" : ""}`}
                onClick={() => setSrvRightPanel(p => p === "cli" ? "none" : "cli")}
                title="CLI 가이드"
              >
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
                  <polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/>
                </svg>
                <span>CLI</span>
              </button>
              <button
                className={`srv-rbar-btn ${srvRightPanel === "mysql" ? "active" : ""}`}
                onClick={() => setSrvRightPanel(p => p === "mysql" ? "none" : "mysql")}
                title="MySQL 연결 방법"
              >
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
                  <ellipse cx="12" cy="5" rx="9" ry="3"/><path d="M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5"/><path d="M3 12c0 1.66 4 3 9 3s9-1.34 9-3"/>
                </svg>
                <span>MySQL</span>
              </button>
              <button
                className={`srv-rbar-btn ${srvRightPanel === "bench" ? "active" : ""}`}
                onClick={() => setSrvRightPanel(p => p === "bench" ? "none" : "bench")}
                title="벤치마크"
              >
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
                  <line x1="18" y1="20" x2="18" y2="10"/><line x1="12" y1="20" x2="12" y2="4"/><line x1="6" y1="20" x2="6" y2="14"/>
                </svg>
                <span>Bench</span>
              </button>
              <button
                className={`srv-rbar-btn ${srvRightPanel === "sessions" ? "active" : ""}`}
                onClick={() => setSrvRightPanel(p => p === "sessions" ? "none" : "sessions")}
                title="접속 세션"
              >
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
                  <circle cx="9" cy="7" r="4"/><path d="M3 21v-2a4 4 0 014-4h4a4 4 0 014 4v2"/>
                  <circle cx="19" cy="7" r="2"/><path d="M23 21v-1a3 3 0 00-2-2.83"/>
                </svg>
                <span>Session</span>
              </button>
              <button
                className={`srv-rbar-btn ${srvRightPanel === "mcp" ? "active" : ""}`}
                onClick={() => setSrvRightPanel(p => p === "mcp" ? "none" : "mcp")}
                title="MCP Agent"
              >
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
                  <path d="M12 2l1.5 4.5L18 8l-4.5 1.5L12 14l-1.5-4.5L6 8l4.5-1.5L12 2z"/>
                  <path d="M19 14l.75 2.25L22 17l-2.25.75L19 20l-.75-2.25L16 17l2.25-.75L19 14z"/>
                </svg>
                <span>AI MCP</span>
              </button>
            </div>

          </div>{/* /srv-main-area */}

          {/* 하단 상태바 */}
          <div className="status-bar">
            <div className="status-left">
              <span className="status-item">⎇ main</span>
              <span className="status-item" style={{ color: serverStatus.running ? "#4ec9b0" : "#858585" }}>
                {serverStatus.running ? `● TCP :${serverStatus.port} (${serverStatus.client_count} clients)` : "○ TCP Stopped"}
              </span>
            </div>
            <div className="status-right">
              <span className="status-item">RuSQL v2.2.0</span>
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
