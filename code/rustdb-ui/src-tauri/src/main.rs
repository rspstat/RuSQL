#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod mysql;

use std::collections::HashMap;
use std::sync::{Arc, Mutex, RwLock};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::net::{TcpListener, TcpStream};
use std::io::{BufRead, BufReader, Write};
use std::time::Instant;
use std::process::{Child, Command};
use tauri::{Manager, State};
use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::{Executor, SharedDatabase};

struct McpServer(Mutex<Option<Child>>);

fn start_mcp_server() -> Option<Child> {
    #[cfg(debug_assertions)]
    let server_dir = {
        let manifest = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        manifest.parent()?.parent()?.join("rustdb-mcp")
    };
    #[cfg(not(debug_assertions))]
    let server_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();

    Command::new("python")
        .args(["-m", "uvicorn", "server:app",
               "--host", "127.0.0.1", "--port", "8765", "--log-level", "error"])
        .current_dir(server_dir)
        .spawn()
        .ok()
}

// ─── 연결별 서버 상태 ─────────────────────────────────────────
struct ServerEntry {
    running: Arc<AtomicBool>,
    clients: Arc<AtomicUsize>,
    log:     Arc<Mutex<Vec<String>>>,
    port:    Arc<Mutex<u16>>,
}

// ─── 상태 구조체 ──────────────────────────────────────────────
struct AppState {
    db:      Arc<Mutex<Executor>>,              // 현재 UI 세션 (연결마다 교체됨)
    servers: Mutex<HashMap<String, ServerEntry>>, // conn_id → 서버 상태
}

// ─── 직렬화 타입 ──────────────────────────────────────────────
#[derive(serde::Serialize)]
struct QueryResult {
    columns: Vec<String>,
    rows:    Vec<Vec<String>>,
    message: String,
    elapsed: f64,
    success: bool,
}

#[derive(serde::Serialize)]
struct MultiQueryResult {
    results:       Vec<QueryResult>,
    total_elapsed: f64,
}

#[derive(serde::Serialize)]
struct ServerStatus {
    running:      bool,
    port:         u16,
    client_count: usize,
    log:          Vec<String>,
}

// ─── 헬퍼: 로그 추가 ──────────────────────────────────────────
fn add_log(log: &Arc<Mutex<Vec<String>>>, msg: &str) {
    use std::time::{SystemTime, UNIX_EPOCH};
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let hh = (secs % 86400) / 3600;
    let mm = (secs % 3600) / 60;
    let ss = secs % 60;
    let entry = format!("[{:02}:{:02}:{:02}] {}", hh, mm, ss, msg);
    let mut guard = log.lock().unwrap();
    guard.push(entry);
    if guard.len() > 500 {
        guard.drain(0..100);
    }
}

// ─── 서버 활동 로그 영속화 경로 ───────────────────────────────
// conn_id별 로그 파일 (실행 파일 옆 server_logs/{conn_id}.log).
// 앱을 껐다 켜도 ACTIVITY LOG가 유지되도록 파일에 저장/로드한다.
fn server_log_path(conn_id: &str) -> std::path::PathBuf {
    let safe: String = conn_id.chars()
        .map(|c| if c.is_alphanumeric() { c } else { '_' })
        .collect();
    let base = std::env::current_exe().ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
        .unwrap_or_else(|| std::path::PathBuf::from("."));
    let dir = base.join("server_logs");
    let _ = std::fs::create_dir_all(&dir);
    dir.join(format!("{}.log", safe))
}

fn load_server_log(conn_id: &str) -> Vec<String> {
    std::fs::read_to_string(server_log_path(conn_id))
        .ok()
        .map(|s| s.lines().map(String::from).collect())
        .unwrap_or_default()
}

// ─── TCP 클라이언트 핸들러 ────────────────────────────────────
// 각 TCP 클라이언트는 독립적인 Executor(트랜잭션·current_db)를 가진다.
fn handle_client(stream: TcpStream, shared: Arc<RwLock<SharedDatabase>>, log: Arc<Mutex<Vec<String>>>) {
    let mut writer = match stream.try_clone() {
        Ok(s) => s,
        Err(_) => return,
    };
    let reader = BufReader::new(stream);

    // 1. 배너
    let _ = writeln!(writer, "+-----------------------------------------+");
    let _ = writeln!(writer, "|   RustDB Server v2.2.0                  |");
    let _ = writeln!(writer, "+-----------------------------------------+");
    let _ = writeln!(writer, "---END---");
    let _ = writer.flush();

    // 2. AUTH 핸드셰이크
    let mut lines_iter = reader.lines();
    let auth_line = match lines_iter.next() {
        Some(Ok(l)) => l,
        _ => return,
    };
    let parts: Vec<&str> = auth_line.splitn(3, ' ').collect();
    let cmd       = parts.first().copied().unwrap_or("");
    let auth_user = parts.get(1).copied().unwrap_or("").trim();
    let auth_pass = parts.get(2).copied().unwrap_or("").trim();

    if !cmd.eq_ignore_ascii_case("auth") || auth_user.is_empty() {
        let _ = writeln!(writer, "ERR expected: AUTH <user> <password>");
        let _ = writeln!(writer, "---END---");
        return;
    }

    let ok = shared.read().unwrap().validate_credentials(auth_user, auth_pass);
    if !ok {
        add_log(&log, &format!("AUTH failed: '{}'", auth_user));
        let _ = writeln!(writer, "ERR Access denied for user '{}'", auth_user);
        let _ = writeln!(writer, "---END---");
        let _ = writer.flush();
        return;
    }

    add_log(&log, &format!("Authenticated: '{}'", auth_user));
    let _ = writeln!(writer, "OK authenticated as '{}'", auth_user);
    let _ = writeln!(writer, "---END---");
    let _ = writer.flush();

    // 3. 쿼리 세션
    let mut exec = Executor::new_session(Arc::clone(&shared));
    let mut buf  = String::new();

    for line in lines_iter {
        let input = match line { Ok(l) => l, Err(_) => break };
        let trimmed = input.trim();

        if trimmed.eq_ignore_ascii_case("exit") || trimmed.eq_ignore_ascii_case("quit") {
            let _ = writeln!(writer, "Bye!");
            let _ = writeln!(writer, "---END---");
            break;
        }

        buf.push_str(&input);
        buf.push('\n');
        if !buf.contains(';') { continue; }

        let queries = split_queries_smart(&buf);
        buf.clear();

        for q in &queries {
            let preview = if q.len() > 60 { format!("{}...", &q[..60]) } else { q.clone() };
            add_log(&log, &format!("[{}] {}", auth_user, preview));

            let t0 = std::time::Instant::now();
            let mut parser = Parser::new(q.as_str());
            let (status, output) = match parser.parse() {
                Ok(stmt) => match exec.execute(stmt) {
                    Ok(r)  => ("OK",  r),
                    Err(e) => ("ERR", e),
                },
                Err(e) => ("ERR", format!("Parse Error: {}", e)),
            };
            let _ = writeln!(writer, "{}", status);
            let _ = writeln!(writer, "{}", output);
            let _ = writeln!(writer, "({:.3} sec)", t0.elapsed().as_secs_f64());
            let _ = writeln!(writer, "---END---");
            let _ = writer.flush();
        }
    }
}

// ─── 주석 인식 쿼리 분리 ─────────────────────────────────────
// BEGIN...END 블록 안의 `;` 는 분리하지 않음 (저장 프로시저/트리거 지원).
// BEGIN; / BEGIN WORK; 는 트랜잭션 마커로 depth 증가 안 함.
fn split_queries_smart(input: &str) -> Vec<String> {
    let chars: Vec<char> = input.chars().collect();
    let len = chars.len();
    let mut queries: Vec<String> = Vec::new();
    let mut current = String::new();
    let mut begin_depth: i32 = 0;
    let mut i = 0;

    while i < len {
        // -- 한 줄 주석
        if chars[i] == '-' && i + 1 < len && chars[i + 1] == '-' {
            while i < len && chars[i] != '\n' { i += 1; }
            continue;
        }
        // # 한 줄 주석
        if chars[i] == '#' {
            while i < len && chars[i] != '\n' { i += 1; }
            continue;
        }
        // /* */ 블록 주석
        if chars[i] == '/' && i + 1 < len && chars[i + 1] == '*' {
            i += 2;
            while i + 1 < len {
                if chars[i] == '*' && chars[i + 1] == '/' { i += 2; break; }
                i += 1;
            }
            continue;
        }
        // 문자열 리터럴
        if chars[i] == '\'' {
            current.push(chars[i]); i += 1;
            while i < len {
                let c = chars[i]; i += 1;
                current.push(c);
                if c == '\'' { break; }
            }
            continue;
        }
        // 키워드 추출 (BEGIN / END depth 추적)
        if chars[i].is_alphabetic() || chars[i] == '_' {
            let start = i;
            while i < len && (chars[i].is_alphanumeric() || chars[i] == '_') { i += 1; }
            let word: String = chars[start..i].iter().collect();
            match word.to_uppercase().as_str() {
                "BEGIN" => {
                    // BEGIN; or BEGIN WORK → 트랜잭션, depth 증가 안 함
                    let mut j = i;
                    while j < len && chars[j].is_whitespace() { j += 1; }
                    let is_transaction = if j >= len || chars[j] == ';' {
                        true
                    } else if chars[j].is_alphabetic() {
                        let s2 = j;
                        let mut k = j;
                        while k < len && (chars[k].is_alphanumeric() || chars[k] == '_') { k += 1; }
                        let nw: String = chars[s2..k].iter().collect();
                        nw.to_uppercase() == "WORK"
                    } else { false };
                    if !is_transaction { begin_depth += 1; }
                }
                "END" => {
                    let mut j = i;
                    while j < len && chars[j].is_whitespace() { j += 1; }
                    let next_is_sub = if j < len && (chars[j].is_alphabetic() || chars[j] == '_') {
                        let s2 = j;
                        let mut k = j;
                        while k < len && (chars[k].is_alphanumeric() || chars[k] == '_') { k += 1; }
                        let nw: String = chars[s2..k].iter().collect();
                        matches!(nw.to_uppercase().as_str(), "IF" | "WHILE" | "LOOP" | "REPEAT" | "CASE")
                    } else { false };
                    if !next_is_sub && begin_depth > 0 { begin_depth -= 1; }
                }
                _ => {}
            }
            current.push_str(&word);
            continue;
        }
        // 세미콜론: BEGIN 블록 밖에서만 분리
        if chars[i] == ';' {
            if begin_depth == 0 {
                let t = current.trim().to_string();
                if !t.is_empty() { queries.push(t); }
                current.clear();
            } else {
                current.push(';');
            }
            i += 1;
            continue;
        }
        current.push(chars[i]);
        i += 1;
    }
    let t = current.trim().to_string();
    if !t.is_empty() { queries.push(t); }
    queries
}

// ─── Tauri 커맨드: SQL 실행 ───────────────────────────────────
#[tauri::command]
fn execute_query(query: String, _ts: Option<u64>, state: State<AppState>) -> MultiQueryResult {
    let start = Instant::now();
    let mut exec = state.db.lock().unwrap();

    let queries = split_queries_smart(&query);

    let mut results = Vec::new();
    for q in &queries {
        let q_start = Instant::now();
        let mut p = Parser::new(q.as_str());
        let result = match p.parse() {
            Ok(stmt) => match exec.execute(stmt) {
                Ok(out) => parse_output(&out, q_start.elapsed().as_secs_f64()),
                Err(e)  => QueryResult {
                    columns: vec![], rows: vec![],
                    message: e, elapsed: q_start.elapsed().as_secs_f64(), success: false,
                },
            },
            Err(e) => QueryResult {
                columns: vec![], rows: vec![],
                message: format!("Parse Error: {}", e),
                elapsed: q_start.elapsed().as_secs_f64(), success: false,
            },
        };
        results.push(result);
    }

    MultiQueryResult { total_elapsed: start.elapsed().as_secs_f64(), results }
}

fn parse_output(output: &str, elapsed: f64) -> QueryResult {
    let lines: Vec<&str> = output.lines().collect();
    if lines.first().map(|l| l.starts_with('+')).unwrap_or(false) {
        let mut columns = vec![];
        let mut rows = vec![];
        for (i, line) in lines.iter().enumerate() {
            if line.starts_with('+') { continue; }
            if line.starts_with('|') {
                let cells: Vec<String> = line
                    .split('|').filter(|s| !s.is_empty())
                    .map(|s| s.trim().to_string()).collect();
                if i == 1 { columns = cells; } else { rows.push(cells); }
            }
        }
        QueryResult { columns, rows, message: String::new(), elapsed, success: true }
    } else {
        QueryResult { columns: vec![], rows: vec![], message: output.to_string(), elapsed, success: true }
    }
}

#[tauri::command]
fn get_databases(state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let mut dbs: Vec<String> = s.databases.iter().cloned().collect();
    dbs.sort();
    dbs
}

#[tauri::command]
fn get_current_db(state: State<AppState>) -> String {
    state.db.lock().unwrap().current_db.clone()
}

#[tauri::command]
fn get_tables_for_db(db: String, state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let prefix = format!("{}.", db.to_lowercase());
    let mut tables: Vec<String> = s.catalog.tables.keys()
        .filter(|k| k.starts_with(&prefix))
        .map(|k| k[prefix.len()..].to_string())
        .collect();
    tables.sort();
    tables
}

#[tauri::command]
fn get_views_for_db(db: String, state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let prefix = format!("{}.", db.to_lowercase());
    let mut views: Vec<String> = s.views.keys()
        .filter(|k| k.starts_with(&prefix))
        .map(|k| k[prefix.len()..].to_string())
        .collect();
    views.sort();
    views
}

#[derive(serde::Serialize)]
struct IndexInfo {
    name:    String,
    table:   String,
    columns: Vec<String>,
    kind:    String, // "single" | "composite" | "hash"
}

#[derive(serde::Serialize)]
struct TriggerInfo {
    name:   String,
    table:  String,
    timing: String,
    event:  String,
}

#[tauri::command]
fn get_indexes_for_db(db: String, state: State<AppState>) -> Vec<IndexInfo> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let prefix = format!("{}.", db.to_lowercase());
    let mut result = Vec::new();
    for (name, (table, column)) in &s.index_meta {
        if table.starts_with(&prefix) {
            result.push(IndexInfo {
                name: name.clone(),
                table: table[prefix.len()..].to_string(),
                columns: vec![column.clone()],
                kind: "single".to_string(),
            });
        }
    }
    for (name, ci) in &s.composite_indexes {
        if ci.table.starts_with(&prefix) {
            result.push(IndexInfo {
                name: name.clone(),
                table: ci.table[prefix.len()..].to_string(),
                columns: ci.columns.clone(),
                kind: "composite".to_string(),
            });
        }
    }
    for (name, (table, column)) in &s.hash_index_meta {
        if table.starts_with(&prefix) {
            result.push(IndexInfo {
                name: name.clone(),
                table: table[prefix.len()..].to_string(),
                columns: vec![column.clone()],
                kind: "hash".to_string(),
            });
        }
    }
    result.sort_by(|a, b| a.table.cmp(&b.table).then(a.name.cmp(&b.name)));
    result
}

#[tauri::command]
fn get_triggers_for_db(db: String, state: State<AppState>) -> Vec<TriggerInfo> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let prefix = format!("{}.", db.to_lowercase());
    let mut result: Vec<TriggerInfo> = s.triggers.iter()
        .filter(|(_, (table, _, _, _))| table.starts_with(&prefix))
        .map(|(name, (table, timing, event, _))| TriggerInfo {
            name:   name.clone(),
            table:  table[prefix.len()..].to_string(),
            timing: timing.clone(),
            event:  event.clone(),
        })
        .collect();
    result.sort_by(|a, b| a.table.cmp(&b.table).then(a.name.cmp(&b.name)));
    result
}

#[tauri::command]
fn get_tables(state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let prefix = format!("{}.", exec.current_db);
    let mut tables: Vec<String> = s.catalog.tables.keys()
        .filter(|k| k.starts_with(&prefix))
        .map(|k| k[prefix.len()..].to_string())
        .collect();
    tables.sort();
    tables
}

#[tauri::command]
fn get_columns(table: String, state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let qualified = if table.contains('.') {
        table.clone()
    } else {
        format!("{}.{}", exec.current_db, table)
    };
    s.catalog.get_table(&qualified)
        .map(|sch| sch.columns.iter().map(|c| c.name.clone()).collect())
        .unwrap_or_default()
}

#[derive(serde::Serialize)]
struct ColumnDetail {
    name:        String,
    data_type:   String,
    is_pk:       bool,
    is_not_null: bool,
    is_unique:   bool,
    is_auto_inc: bool,
    default_val: Option<String>,
    fk_ref:      Option<String>, // "table(col)"
}

#[tauri::command]
fn get_columns_detail(table: String, state: State<AppState>) -> Vec<ColumnDetail> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let qualified = if table.contains('.') {
        table.clone()
    } else {
        format!("{}.{}", exec.current_db, table)
    };
    s.catalog.get_table(&qualified)
        .map(|sch| sch.columns.iter().map(|c| {
            let type_str = format!("{:?}", c.data_type)
                .replace("Varchar(", "VARCHAR(")
                .replace("Decimal(", "DECIMAL(")
                .replace("Enum(", "ENUM(")
                .replace("Set(", "SET(")
                .replace("Boolean", "BOOL")
                .replace("DateTime", "DATETIME")
                .replace("Timestamp", "TIMESTAMP")
                .replace("Double", "DOUBLE")
                .replace("Float", "FLOAT")
                .replace("Text", "TEXT")
                .replace("Date", "DATE")
                .replace("Time", "TIME")
                .replace("Year", "YEAR")
                .replace("Int", "INT")
                // ENUM/SET 값 리스트를 SQL 표기로: ["a", "b", "c"] → 'a','b','c'
                .replace("[\"", "'")
                .replace("\", \"", "','")
                .replace("\"]", "'");
            ColumnDetail {
                name:        c.name.clone(),
                data_type:   type_str,
                is_pk:       c.primary_key,
                is_not_null: c.not_null,
                is_unique:   c.unique,
                is_auto_inc: c.auto_increment,
                default_val: c.default.clone(),
                fk_ref:      c.foreign_key.as_ref().map(|fk|
                    format!("{}({})", fk.ref_table, fk.ref_column)),
            }
        }).collect())
        .unwrap_or_default()
}

#[tauri::command]
fn get_views(state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let prefix = format!("{}.", exec.current_db);
    let mut views: Vec<String> = s.views.keys()
        .filter(|k| k.starts_with(&prefix))
        .map(|k| k[prefix.len()..].to_string())
        .collect();
    views.sort();
    views
}

#[tauri::command]
fn get_indexes(state: State<AppState>) -> Vec<IndexInfo> {
    let exec = state.db.lock().unwrap();
    let s = exec.shared.read().unwrap();
    let prefix = format!("{}.", exec.current_db);
    let mut result = Vec::new();

    // 단일 컬럼 인덱스
    for (name, (table, column)) in &s.index_meta {
        if table.starts_with(&prefix) {
            result.push(IndexInfo {
                name:    name.clone(),
                table:   table[prefix.len()..].to_string(),
                columns: vec![column.clone()],
                kind:    "single".to_string(),
            });
        }
    }
    // 복합 인덱스
    for (name, ci) in &s.composite_indexes {
        if ci.table.starts_with(&prefix) {
            result.push(IndexInfo {
                name:    name.clone(),
                table:   ci.table[prefix.len()..].to_string(),
                columns: ci.columns.clone(),
                kind:    "composite".to_string(),
            });
        }
    }
    result.sort_by(|a, b| a.name.cmp(&b.name));
    result
}

// ─── Tauri 커맨드: 서버 관리 ─────────────────────────────────
#[tauri::command]
fn start_server(conn_id: String, port: u16, mysql_port: u16, state: State<AppState>) -> Result<String, String> {
    {
        let servers = state.servers.lock().unwrap();
        if let Some(e) = servers.get(&conn_id) {
            if e.running.load(Ordering::SeqCst) {
                return Err("서버가 이미 실행 중입니다.".to_string());
            }
        }
    }

    // 현재 연결의 SharedDatabase를 캡처 (연결 전환 후에도 이 서버는 이 DB를 계속 사용)
    let shared_db = state.db.lock().unwrap().get_shared();
    let shared_db_mysql = Arc::clone(&shared_db); // MySQL 리스너용 사전 복제

    let running = Arc::new(AtomicBool::new(true));
    let clients = Arc::new(AtomicUsize::new(0));
    // 이전 세션 로그를 파일에서 복원해 이어서 기록한다.
    let log     = Arc::new(Mutex::new(load_server_log(&conn_id)));
    let log_mysql = Arc::clone(&log); // MySQL 로그용 사전 복제
    let port_store = Arc::new(Mutex::new(port));

    state.servers.lock().unwrap().insert(conn_id, ServerEntry {
        running: running.clone(),
        clients: clients.clone(),
        log:     log.clone(),
        port:    port_store,
    });

    thread::spawn(move || {
        let addr = format!("127.0.0.1:{}", port);
        let listener = match TcpListener::bind(&addr) {
            Ok(l)  => { add_log(&log, &format!("서버 시작: {}", addr)); l }
            Err(e) => {
                running.store(false, Ordering::SeqCst);
                add_log(&log, &format!("바인딩 실패: {}", e));
                return;
            }
        };
        listener.set_nonblocking(true).ok();

        loop {
            if !running.load(Ordering::SeqCst) { break; }
            match listener.accept() {
                Ok((stream, addr)) => {
                    clients.fetch_add(1, Ordering::SeqCst);
                    add_log(&log, &format!("클라이언트 접속: {}", addr));
                    let sh2  = Arc::clone(&shared_db);
                    let cc   = clients.clone();
                    let log2 = log.clone();
                    let astr = addr.to_string();
                    thread::spawn(move || {
                        handle_client(stream, sh2, log2.clone());
                        cc.fetch_sub(1, Ordering::SeqCst);
                        add_log(&log2, &format!("클라이언트 종료: {}", astr));
                    });
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    thread::sleep(std::time::Duration::from_millis(50));
                }
                Err(_) => break,
            }
        }
        add_log(&log, "서버가 중지되었습니다.");
    });

    if mysql_port > 0 {
        mysql::start_mysql_listener(mysql_port, shared_db_mysql);
        add_log(&log_mysql, &format!("MySQL 프로토콜 시작: 0.0.0.0:{}", mysql_port));
    }

    Ok(format!("포트 {}에서 서버를 시작합니다...", port))
}

#[tauri::command]
fn get_app_data_dir(_app: tauri::AppHandle) -> String {
    // code/ 폴더를 기준으로 사용 → UI와 CLI/서버가 같은 데이터 공유
    let manifest = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // CARGO_MANIFEST_DIR = .../code/rustdb-ui/src-tauri → 두 단계 상위 = code/
    manifest.parent().and_then(|p| p.parent())
        .map(|p| p.join("data").to_string_lossy().to_string())
        .unwrap_or_else(|| "data".to_string())
}

#[tauri::command]
fn set_parallel_query(enabled: bool) {
    std::env::set_var("RUSTDB_PARALLEL", if enabled { "1" } else { "0" });
}

#[tauri::command]
fn delete_conn_data(data_dir: String) -> bool {
    if data_dir.is_empty() { return false; }
    match std::fs::remove_dir_all(&data_dir) {
        Ok(_)  => true,
        Err(_) => false, // 디렉토리가 없으면 무시
    }
}

#[tauri::command]
fn authenticate(user: String, password: String, data_dir: String, buffer_pool_size: usize, state: State<AppState>) -> bool {
    // 해당 data_dir의 Executor를 로드 (WAL 복구 포함)
    let bp = if buffer_pool_size > 0 { buffer_pool_size } else { 64 };
    let new_exec = rustdb_core::engine::executor::Executor::new_with_options(&data_dir, bp);
    // 사용자 없으면 root/root 자동 생성
    new_exec.shared.write().unwrap().ensure_default_user();
    // 자격증명 검증
    let ok = new_exec.shared.read().unwrap().validate_credentials(&user, &password);
    if ok {
        // mysql_native_hash 없는 레거시 계정 자동 마이그레이션
        new_exec.shared.write().unwrap().migrate_mysql_hash(&user, &password);
        // UI 세션 executor 교체
        *state.db.lock().unwrap() = new_exec;
    }
    ok
}

#[tauri::command]
fn stop_server(conn_id: String, state: State<AppState>) -> Result<String, String> {
    let servers = state.servers.lock().unwrap();
    match servers.get(&conn_id) {
        Some(e) if e.running.load(Ordering::SeqCst) => {
            e.running.store(false, Ordering::SeqCst);
            Ok("서버를 중지합니다...".to_string())
        }
        _ => Err("실행 중인 서버가 없습니다.".to_string()),
    }
}

#[tauri::command]
fn get_server_status(conn_id: String, state: State<AppState>) -> ServerStatus {
    let servers = state.servers.lock().unwrap();
    match servers.get(&conn_id) {
        Some(e) => {
            let log = e.log.lock().unwrap().clone();
            // 메모리 로그를 파일에 저장 (앱 종료 후에도 유지)
            let _ = std::fs::write(server_log_path(&conn_id), log.join("\n"));
            ServerStatus {
                running:      e.running.load(Ordering::SeqCst),
                port:         *e.port.lock().unwrap(),
                client_count: e.clients.load(Ordering::SeqCst),
                log,
            }
        }
        // 서버 미실행 시에도 이전 세션 로그를 파일에서 읽어 표시
        None => ServerStatus { running: false, port: 7878, client_count: 0, log: load_server_log(&conn_id) },
    }
}

#[tauri::command]
fn clear_server_log(conn_id: String, state: State<AppState>) {
    if let Some(e) = state.servers.lock().unwrap().get(&conn_id) {
        e.log.lock().unwrap().clear();
    }
    // 영속화된 로그 파일도 삭제
    let _ = std::fs::remove_file(server_log_path(&conn_id));
}


// ─── CSV 내보내기 ─────────────────────────────────────────────
#[tauri::command]
fn export_csv(query: String, file_path: String, state: State<AppState>) -> Result<String, String> {
    let mut exec = state.db.lock().unwrap();
    let result = {
        let mut p = rustdb_core::parser::parser::Parser::new(&query);
        match p.parse() {
            Ok(stmt) => exec.execute(stmt),
            Err(e) => return Err(format!("Parse Error: {}", e)),
        }
    }?;

    let qr = parse_output(&result, 0.0);
    if qr.columns.is_empty() {
        return Err("Query returned no columns.".to_string());
    }

    let mut csv = String::new();
    csv.push_str(&qr.columns.iter().map(|c| csv_escape(c)).collect::<Vec<_>>().join(","));
    csv.push('\n');
    for row in &qr.rows {
        csv.push_str(&row.iter().map(|v| csv_escape(v)).collect::<Vec<_>>().join(","));
        csv.push('\n');
    }

    std::fs::write(&file_path, &csv).map_err(|e| e.to_string())?;
    Ok(format!("Exported {} rows to '{}'.", qr.rows.len(), file_path))
}

fn csv_escape(s: &str) -> String {
    if s.contains(',') || s.contains('"') || s.contains('\n') {
        format!("\"{}\"", s.replace('"', "\"\""))
    } else {
        s.to_string()
    }
}

// ─── CSV 가져오기 ─────────────────────────────────────────────
#[tauri::command]
fn import_csv(table: String, file_path: String, state: State<AppState>) -> Result<String, String> {
    let content = std::fs::read_to_string(&file_path).map_err(|e| e.to_string())?;
    let mut lines = content.lines();

    let header = match lines.next() {
        Some(h) => h,
        None => return Err("CSV file is empty.".to_string()),
    };
    let cols: Vec<&str> = header.split(',').map(|c| c.trim().trim_matches('"')).collect();
    let col_list = cols.iter().map(|c| format!("`{}`", c)).collect::<Vec<_>>().join(", ");

    let mut count = 0usize;
    let mut errors = 0usize;
    let mut exec = state.db.lock().unwrap();

    for line in lines {
        if line.trim().is_empty() { continue; }
        let vals: Vec<String> = csv_parse_row(line);
        let val_list = vals.iter().map(|v| format!("'{}'", v.replace('\'', "''"))).collect::<Vec<_>>().join(", ");
        let sql = format!("INSERT INTO {} ({}) VALUES ({});", table, col_list, val_list);
        let mut p = rustdb_core::parser::parser::Parser::new(&sql);
        match p.parse().and_then(|stmt| exec.execute(stmt).map_err(|e| e)) {
            Ok(_) => count += 1,
            Err(_) => errors += 1,
        }
    }
    Ok(format!("Imported {} rows ({} errors) from '{}'.", count, errors, file_path))
}

fn csv_parse_row(line: &str) -> Vec<String> {
    let mut fields = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;
    let chars: Vec<char> = line.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        match chars[i] {
            '"' if !in_quotes => { in_quotes = true; }
            '"' if in_quotes => {
                if i + 1 < chars.len() && chars[i+1] == '"' { current.push('"'); i += 1; }
                else { in_quotes = false; }
            }
            ',' if !in_quotes => { fields.push(current.clone()); current.clear(); }
            c => current.push(c),
        }
        i += 1;
    }
    fields.push(current);
    fields
}

#[tauri::command]
fn open_terminal() {
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "cmd"])
        .current_dir(r"C:\Users\win11\Desktop\projects\dbe\code")
        .spawn();
}

#[tauri::command]
fn open_url(url: String) {
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "", &url])
        .spawn();
}

// ─── 엔트리포인트 ─────────────────────────────────────────────
fn main() {
    let exec = Executor::new();
    exec.shared.write().unwrap().ensure_default_user();
    let db = Arc::new(Mutex::new(exec));

    tauri::Builder::default()
        .manage(AppState {
            db,
            servers: Mutex::new(HashMap::new()),
        })
        .manage(McpServer(Mutex::new(None)))
        .setup(|app| {
            *app.state::<McpServer>().0.lock().unwrap() = start_mcp_server();
            // 창 아이콘 설정
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.set_icon(tauri::include_image!("icons/icon.png"));
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            execute_query,
            get_databases,
            get_current_db,
            get_tables,
            get_columns,
            get_columns_detail,
            get_views,
            get_indexes,
            get_tables_for_db,
            get_views_for_db,
            get_indexes_for_db,
            get_triggers_for_db,
            authenticate,
            delete_conn_data,
            start_server,
            stop_server,
            get_server_status,
            clear_server_log,
            export_csv,
            import_csv,
            open_terminal,
            open_url,
            get_app_data_dir,
            set_parallel_query,
        ])
        .build(tauri::generate_context!())
        .expect("error while building tauri application")
        .run(|app, event| {
            if let tauri::RunEvent::Exit = event {
                if let Some(mut child) = app.state::<McpServer>().0.lock().unwrap().take() {
                    let _ = child.kill();
                    let _ = child.wait();
                }
            }
        });
}
