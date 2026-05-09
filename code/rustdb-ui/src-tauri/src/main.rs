#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::sync::{Arc, Mutex, RwLock};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::net::{TcpListener, TcpStream};
use std::io::{BufRead, BufReader, Write};
use std::time::Instant;
use tauri::State;
use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::{Executor, SharedDatabase};

// ─── 상태 구조체 ──────────────────────────────────────────────
struct AppState {
    db:          Arc<Mutex<Executor>>,        // UI 전용 세션 (트랜잭션·current_db)
    shared:      Arc<RwLock<SharedDatabase>>, // TCP 클라이언트들과 공유되는 DB 상태
    srv_running: Arc<AtomicBool>,
    srv_clients: Arc<AtomicUsize>,
    srv_log:     Arc<Mutex<Vec<String>>>,
    srv_port:    Arc<Mutex<u16>>,
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
// `;` 기준으로 분리하되, --, #, /* */ 주석과 '' 문자열 안의 `;` 는 무시.
// 주석만 남은 조각(실제 토큰 없음)은 빈 문자열로 반환 → 이후 필터링.
fn split_queries_smart(input: &str) -> Vec<String> {
    let chars: Vec<char> = input.chars().collect();
    let len = chars.len();
    let mut queries: Vec<String> = Vec::new();
    let mut current = String::new();
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
        // 문자열 리터럴 (안의 ; 는 구분자가 아님)
        if chars[i] == '\'' {
            current.push(chars[i]); i += 1;
            while i < len {
                let c = chars[i]; i += 1;
                current.push(c);
                if c == '\'' { break; }
            }
            continue;
        }
        // 세미콜론 → 쿼리 분리
        if chars[i] == ';' {
            let t = current.trim().to_string();
            if !t.is_empty() { queries.push(t); }
            current.clear();
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
    let s = state.shared.read().unwrap();
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
    let s = state.shared.read().unwrap();
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
    let s = state.shared.read().unwrap();
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
    kind:    String, // "single" | "composite"
}

#[tauri::command]
fn get_indexes_for_db(db: String, state: State<AppState>) -> Vec<IndexInfo> {
    let s = state.shared.read().unwrap();
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
    result.sort_by(|a, b| a.name.cmp(&b.name));
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
                .replace("Boolean", "BOOL")
                .replace("DateTime", "DATETIME")
                .replace("Timestamp", "TIMESTAMP")
                .replace("Double", "DOUBLE")
                .replace("Float", "FLOAT")
                .replace("Text", "TEXT")
                .replace("Date", "DATE")
                .replace("Time", "TIME")
                .replace("Year", "YEAR")
                .replace("Int", "INT");
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
fn start_server(port: u16, state: State<AppState>) -> Result<String, String> {
    if state.srv_running.load(Ordering::SeqCst) {
        return Err("서버가 이미 실행 중입니다.".to_string());
    }

    let running    = state.srv_running.clone();
    let clients    = state.srv_clients.clone();
    let log        = state.srv_log.clone();
    // TCP 클라이언트들은 UI 세션과 동일한 SharedDatabase를 공유
    let shared_db  = state.shared.clone();
    let port_store = state.srv_port.clone();

    *port_store.lock().unwrap() = port;
    running.store(true, Ordering::SeqCst);

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

    Ok(format!("포트 {}에서 서버를 시작합니다...", port))
}

#[tauri::command]
fn stop_server(state: State<AppState>) -> Result<String, String> {
    if !state.srv_running.load(Ordering::SeqCst) {
        return Err("실행 중인 서버가 없습니다.".to_string());
    }
    state.srv_running.store(false, Ordering::SeqCst);
    Ok("서버를 중지합니다...".to_string())
}

#[tauri::command]
fn get_server_status(state: State<AppState>) -> ServerStatus {
    ServerStatus {
        running:      state.srv_running.load(Ordering::SeqCst),
        port:         *state.srv_port.lock().unwrap(),
        client_count: state.srv_clients.load(Ordering::SeqCst),
        log:          state.srv_log.lock().unwrap().clone(),
    }
}

#[tauri::command]
fn clear_server_log(state: State<AppState>) {
    state.srv_log.lock().unwrap().clear();
}

// ─── 엔트리포인트 ─────────────────────────────────────────────
fn main() {
    // WAL 복구 포함 초기화 → UI 세션 executor + 공유 DB 상태 분리
    let exec   = Executor::new();
    let shared = exec.get_shared();
    // users가 없으면 root/root 자동 생성
    shared.write().unwrap().ensure_default_user();
    let db     = Arc::new(Mutex::new(exec));

    tauri::Builder::default()
        .manage(AppState {
            db,
            shared,
            srv_running: Arc::new(AtomicBool::new(false)),
            srv_clients: Arc::new(AtomicUsize::new(0)),
            srv_log:     Arc::new(Mutex::new(Vec::new())),
            srv_port:    Arc::new(Mutex::new(7878)),
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
            start_server,
            stop_server,
            get_server_status,
            clear_server_log,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
