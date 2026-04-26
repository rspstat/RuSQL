#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::net::{TcpListener, TcpStream};
use std::io::{BufRead, BufReader, Write};
use std::time::Instant;
use tauri::State;
use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::Executor;

// ─── 상태 구조체 ──────────────────────────────────────────────
struct AppState {
    db:             Arc<Mutex<Executor>>,
    srv_running:    Arc<AtomicBool>,
    srv_clients:    Arc<AtomicUsize>,
    srv_log:        Arc<Mutex<Vec<String>>>,
    srv_port:       Arc<Mutex<u16>>,
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
    // UTC 기준 시간 (로컬 타임존 오프셋 없음)
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
fn handle_client(stream: TcpStream, db: Arc<Mutex<Executor>>, log: Arc<Mutex<Vec<String>>>) {
    let mut writer = match stream.try_clone() {
        Ok(s) => s,
        Err(_) => return,
    };
    let reader = BufReader::new(stream);

    let _ = writeln!(writer, "RustDB Server v2.2.0 — Ready");
    let _ = writeln!(writer, "---END---");

    for line in reader.lines() {
        let query = match line {
            Ok(q) => q.trim().to_string(),
            Err(_) => break,
        };
        if query.is_empty() { continue; }

        let preview = if query.len() > 60 { format!("{}...", &query[..60]) } else { query.clone() };
        add_log(&log, &format!("Query: {}", preview));

        let output = {
            let mut exec = db.lock().unwrap();
            let mut parser = Parser::new(&query);
            match parser.parse() {
                Ok(stmt) => exec.execute(stmt).unwrap_or_else(|e| format!("Error: {}", e)),
                Err(e)   => format!("Parse Error: {}", e),
            }
        };

        let _ = writeln!(writer, "{}", output);
        let _ = writeln!(writer, "---END---");
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
fn get_tables(state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    exec.catalog.tables.keys().cloned().collect()
}

#[tauri::command]
fn get_columns(table: String, state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    exec.catalog.get_table(&table)
        .map(|s| s.columns.iter().map(|c| c.name.clone()).collect())
        .unwrap_or_default()
}

#[derive(serde::Serialize)]
struct IndexInfo {
    name:    String,
    table:   String,
    columns: Vec<String>,
    kind:    String, // "single" | "composite"
}

#[tauri::command]
fn get_views(state: State<AppState>) -> Vec<String> {
    let exec = state.db.lock().unwrap();
    exec.views.keys().cloned().collect()
}

#[tauri::command]
fn get_indexes(state: State<AppState>) -> Vec<IndexInfo> {
    let exec = state.db.lock().unwrap();
    let mut result = Vec::new();

    // 단일 컬럼 인덱스
    for (name, (table, column)) in &exec.index_meta {
        result.push(IndexInfo {
            name:    name.clone(),
            table:   table.clone(),
            columns: vec![column.clone()],
            kind:    "single".to_string(),
        });
    }
    // 복합 인덱스
    for (name, ci) in &exec.composite_indexes {
        result.push(IndexInfo {
            name:    name.clone(),
            table:   ci.table.clone(),
            columns: ci.columns.clone(),
            kind:    "composite".to_string(),
        });
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

    let running     = state.srv_running.clone();
    let clients     = state.srv_clients.clone();
    let log         = state.srv_log.clone();
    let db          = state.db.clone();
    let port_store  = state.srv_port.clone();

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
                    let db2  = db.clone();
                    let cc   = clients.clone();
                    let log2 = log.clone();
                    let astr = addr.to_string();
                    thread::spawn(move || {
                        handle_client(stream, db2, log2.clone());
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
    let db = Arc::new(Mutex::new(Executor::new()));
    tauri::Builder::default()
        .manage(AppState {
            db,
            srv_running: Arc::new(AtomicBool::new(false)),
            srv_clients: Arc::new(AtomicUsize::new(0)),
            srv_log:     Arc::new(Mutex::new(Vec::new())),
            srv_port:    Arc::new(Mutex::new(7878)),
        })
        .invoke_handler(tauri::generate_handler![
            execute_query,
            get_tables,
            get_columns,
            get_views,
            get_indexes,
            start_server,
            stop_server,
            get_server_status,
            clear_server_log,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
