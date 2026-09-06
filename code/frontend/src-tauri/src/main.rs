#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::net::TcpStream;
use std::io::{BufRead, BufReader, Write};
use std::time::Instant;
use std::process::{Child, Command, Stdio};

use sha1::{Digest, Sha1};
use tauri::{Manager, State};

// ─── 세션 정보 ────────────────────────────────────────────────
#[derive(serde::Serialize, Clone)]
struct SessionInfo {
    addr:         String,
    user:         String,
    connected_at: u64,
    query_count:  usize,
}

// ─── 연결별로 노출 중인 포트 정보 (실제 리스너는 engine_server.exe 프로세스가 담당) ──
struct ServerEntry {
    running:    bool,
    port:       u16,
    mysql_port: Option<u16>,
}

// ─── 상태 구조체 ──────────────────────────────────────────────
struct UiStore {
    tab_content: Arc<Mutex<HashMap<String, String>>>, // tab_name → editor content
    tab_list:    Arc<Mutex<Vec<String>>>,             // ordered tab names
    last_result: Arc<Mutex<String>>,                  // last query result (TSV)
    current_db:  Arc<Mutex<String>>,                  // current database name
}

// ─── C++ engine_server.exe 프로세스 + 그 프로세스에 대한 제어용 TCP 연결 ──────
// UI는 더 이상 엔진을 프로세스 내부에 임베드하지 않고, 별도 프로세스로 띄운 뒤
// 그 자신도 여느 클라이언트와 마찬가지로 TCP로 접속해 SQL을 주고받는다.
struct EngineConn {
    child:            Child,
    writer:           TcpStream,
    reader:           BufReader<TcpStream>,
    port:             u16,
    mysql_port:       Option<u16>,
    data_dir:         String,
    buffer_pool_size: usize,
    user:             String,
    password:         String,
    current_db:       String,
    log:              Vec<String>,
}

impl Drop for EngineConn {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

struct AppState {
    db:      Arc<Mutex<Option<EngineConn>>>,
    servers: Mutex<HashMap<String, ServerEntry>>,
    ui:      Arc<UiStore>,
}

// Every `.lock()` call site in this file uses `.unwrap_or_else(|e| e.into_inner())`
// instead of `.unwrap()`: a panic inside any single Tauri command while holding one of
// these Mutexes would otherwise poison it, and every later `.lock().unwrap()` on the
// same Mutex would then also panic -- turning one unrelated bug into every command that
// touches shared state (i.e. almost the whole app) becoming permanently unusable until
// restart. Recovering the guard even when poisoned trades strict "never observe
// possibly-inconsistent state after a panic" correctness for availability, which is the
// right tradeoff here: a single-process desktop app where the underlying data lives in
// the separately-supervised `engine_server` process, not in these Mutexes themselves.

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
    sessions:     Vec<SessionInfo>,
}

// ─── code/ 디렉터리 위치 헬퍼 ─────────────────────────────────
// PLAN.md P1 "컴파일타임 개발자 경로 하드코딩 → 배포 불가" 수정: env!("CARGO_MANIFEST_DIR")는
// 빌드 당시 소스 체크아웃 경로를 실행 파일에 그대로 박아 넣어서, 빌드한 PC와 다른 곳에
// 복사하면 존재하지 않는 디렉터리를 가리키게 됐다. 디버그 빌드(`cargo tauri dev`)에서는
// 그대로 CARGO_MANIFEST_DIR 기준으로 계산하고(개발 중엔 항상 유효), 릴리즈 빌드에서는
// 실행 파일이 실제로 지금 있는 위치(`current_exe()`) 기준으로 계산한다. 이 프로젝트는
// 정식 인스톨러/사이드카 바이너리 번들링 파이프라인이 없고 "code/ 트리 전체를 복사해서
// 어디서든 실행"하는 모델이므로, 그 모델은 유지한 채 컴파일타임 하드코딩만 없앤다.
fn code_dir() -> std::path::PathBuf {
    if cfg!(debug_assertions) {
        // CARGO_MANIFEST_DIR = .../code/frontend/src-tauri → 두 단계 상위 = code/
        std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(2)
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|| std::path::PathBuf::from("code"))
    } else {
        // 실행 파일 위치: .../code/frontend/src-tauri/target/release/rusql-ui.exe
        // ancestors(): 0=exe 자신, 1=release/, 2=target/, 3=src-tauri/, 4=frontend/, 5=code/
        std::env::current_exe()
            .ok()
            .and_then(|p| p.ancestors().nth(5).map(|p| p.to_path_buf()))
            .unwrap_or_else(|| std::path::PathBuf::from("code"))
    }
}

// ─── C++ engine_server.exe 위치/포트 헬퍼 ─────────────────────
// MSVC Debug 빌드는 최적화가 전혀 없어 Release 대비 한 자릿수~두 자릿수 배 느리다
// (실측: 단건 INSERT 9x, Bulk DELETE 21x, SeqScan 22x 등) -- UI가 항상 Debug 바이너리를
// 띄우고 있었던 게 "Rust 버전보다 훨씬 느려 보인다"는 오해의 실제 원인이었다. Release를
// 우선하되, 개발 중 Debug만 새로 빌드했을 수도 있으니 두 바이너리 다 있으면 더 최근에
// 빌드된 쪽을 쓴다 (특정 설정 하나로 고정하면 반대 방향의 "왜 최신 빌드가 반영 안 되지"
// 혼란이 재발할 뿐이라 mtime 비교가 더 안전하다).
fn engine_server_path() -> std::path::PathBuf {
    let build_dir = code_dir().join("build").join("backend").join("server");
    let release = build_dir.join("Release").join("engine_server.exe");
    let debug = build_dir.join("Debug").join("engine_server.exe");
    let mtime = |p: &std::path::Path| p.metadata().and_then(|m| m.modified()).ok();

    match (mtime(&release), mtime(&debug)) {
        (Some(r), Some(d)) => if r >= d { release } else { debug },
        (Some(_), None) => release,
        (None, Some(_)) => debug,
        (None, None) => release, // 둘 다 없으면 에러 메시지에 Release 경로가 보이는 쪽이 낫다
    }
}

fn pick_free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0")
        .and_then(|l| l.local_addr())
        .map(|a| a.port())
        .unwrap_or(17878)
}

fn add_conn_log(conn: &mut EngineConn, msg: &str) {
    use std::time::{SystemTime, UNIX_EPOCH};
    let secs = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs();
    let entry = format!("[{:02}:{:02}:{:02}] {}", (secs % 86400) / 3600, (secs % 3600) / 60, secs % 60, msg);
    conn.log.push(entry);
    if conn.log.len() > 500 { conn.log.drain(0..100); }
}

// mysql_native_password 방식 challenge-response (MySQL 프로토콜이 이미 쓰는 것과 동일한
// 스킴, 엔진 쪽 구현은 SharedDatabase::verify_mysql_native_password) -- native 프로토콜의
// AUTH도 이 방식으로 바꿔 비밀번호 평문이 와이어에 절대 실리지 않게 한다.
fn hex_decode(hex: &str) -> Vec<u8> {
    (0..hex.len() / 2)
        .filter_map(|i| u8::from_str_radix(&hex[i * 2..i * 2 + 2], 16).ok())
        .collect()
}

fn hex_encode(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

// token = SHA1(password) XOR SHA1(nonce || SHA1(SHA1(password)))
fn compute_native_password_token(password: &str, nonce: &[u8]) -> String {
    let stage1 = Sha1::digest(password.as_bytes());
    let stage2 = Sha1::digest(&stage1);

    let mut concat = nonce.to_vec();
    concat.extend_from_slice(&stage2);
    let xor_key = Sha1::digest(&concat);

    let token: Vec<u8> = stage1.iter().zip(xor_key.iter()).map(|(a, b)| a ^ b).collect();
    hex_encode(&token)
}

// 배너 줄 목록에서 "NONCE <hex>" 줄을 찾아 20바이트로 디코딩한다.
fn extract_nonce(banner_lines: &[String]) -> Option<Vec<u8>> {
    banner_lines.iter().find_map(|line| {
        line.strip_prefix("NONCE ").and_then(|hex| {
            let bytes = hex_decode(hex);
            if bytes.len() == 20 { Some(bytes) } else { None }
        })
    })
}

// 서버 응답을 ---END--- 가 올 때까지 읽어 줄 목록 반환 (rusql-client와 동일한 프로토콜)
fn read_block(reader: &mut BufReader<TcpStream>) -> Vec<String> {
    let mut lines = Vec::new();
    loop {
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            _ => {}
        }
        let t = line.trim_end_matches('\n').trim_end_matches('\r').to_string();
        if t == "---END---" { break; }
        lines.push(t);
    }
    lines
}

// engine_server.exe 프로세스를 띄우고 AUTH까지 마친 EngineConn을 만든다.
// mysql_port가 Some이면 MySQL 프로토콜도 함께 열고, None이면 --no-mysql로 띄운다.
fn spawn_and_connect(
    data_dir: &str, buffer_pool_size: usize, user: &str, password: &str,
    port: u16, mysql_port: Option<u16>,
) -> Result<EngineConn, String> {
    let exe = engine_server_path();
    let mut args = vec![
        "--port".to_string(), port.to_string(),
        "--data-dir".to_string(), data_dir.to_string(),
        "--buffer-pool-size".to_string(), buffer_pool_size.to_string(),
    ];
    match mysql_port {
        Some(mp) => { args.push("--mysql-port".to_string()); args.push(mp.to_string()); }
        None => args.push("--no-mysql".to_string()),
    }

    // engine_server.exe의 작업 디렉터리를 data_dir로 고정한다. cargo/tauri dev의 CWD(src-tauri/)를
    // 그대로 물려받으면 BACKUP DATABASE 등이 만드는 상대경로 파일이 src-tauri/ 안에 떨어져
    // Tauri의 소스 파일 감시기가 이를 변경으로 인식해 앱 전체를 재시작해버리는 문제가 있었다.
    std::fs::create_dir_all(data_dir).map_err(|e| format!("data_dir 생성 실패: {}", e))?;
    let child = Command::new(&exe)
        .args(&args)
        .current_dir(data_dir)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| format!("engine_server.exe 실행 실패 ({}): {}", exe.display(), e))?;

    let addr = format!("127.0.0.1:{}", port);
    let mut stream = None;
    for _ in 0..50 {
        if let Ok(s) = TcpStream::connect(&addr) { stream = Some(s); break; }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    let stream = stream.ok_or_else(|| "엔진 서버에 연결할 수 없습니다 (시간 초과).".to_string())?;
    stream.set_nodelay(true).ok();
    let writer = stream.try_clone().map_err(|e| e.to_string())?;
    let mut reader = BufReader::new(stream);
    let banner_lines = read_block(&mut reader); // NONCE 줄 포함 (challenge-response 인증에 필요)
    let nonce = extract_nonce(&banner_lines)
        .ok_or_else(|| "서버가 인증 challenge(NONCE)를 보내지 않았습니다.".to_string())?;

    let mut conn = EngineConn {
        child, writer, reader, port, mysql_port,
        data_dir: data_dir.to_string(), buffer_pool_size,
        user: user.to_string(), password: password.to_string(),
        current_db: String::new(), log: Vec::new(),
    };

    let token = compute_native_password_token(password, &nonce);
    if writeln!(conn.writer, "AUTH {} {}", user, token).is_err() || conn.writer.flush().is_err() {
        return Err("AUTH 전송 실패.".to_string());
    }
    let resp = read_block(&mut conn.reader);
    if !resp.first().map(|s| s.starts_with("OK")).unwrap_or(false) {
        return Err(resp.into_iter().next().unwrap_or_else(|| "인증 실패".to_string()));
    }
    Ok(conn)
}

// 하나의 SQL 문(세미콜론 자동 보정)을 보내고 (성공여부, 본문, 경과초)를 반환한다.
fn send_one(conn: &mut EngineConn, stmt: &str) -> Result<(bool, String, f64), String> {
    let mut s = stmt.trim().to_string();
    if s.is_empty() { return Ok((true, String::new(), 0.0)); }
    if !s.ends_with(';') { s.push(';'); }
    writeln!(conn.writer, "{}", s).map_err(|e| e.to_string())?;
    conn.writer.flush().map_err(|e| e.to_string())?;
    let block = read_block(&mut conn.reader);
    if block.len() < 2 {
        return Err("서버와의 연결이 끊어졌습니다.".to_string());
    }
    let ok = block[0] == "OK";
    let body = block[1..block.len() - 1].join("\n");
    let elapsed = block.last()
        .and_then(|l| l.trim_start_matches('(').trim_end_matches(" sec)").parse::<f64>().ok())
        .unwrap_or(0.0);
    Ok((ok, body, elapsed))
}

// USE 문 실행 성공 시 conn.current_db를 갱신한다.
fn track_use_statement(conn: &mut EngineConn, query: &str) {
    let t = query.trim();
    if t.len() > 4 && t[..4].eq_ignore_ascii_case("use ") {
        let db = t[4..].trim().trim_end_matches(';').trim_matches(|c| c == '`' || c == '\'' || c == '"');
        conn.current_db = db.to_lowercase();
    }
}

// 표준 박스(+---+) 테이블 응답을 (헤더, 행목록)으로 파싱한다.
fn parse_box_table(body: &str) -> (Vec<String>, Vec<Vec<String>>) {
    let lines: Vec<&str> = body.lines().collect();
    let mut columns = vec![];
    let mut rows = vec![];
    for (i, line) in lines.iter().enumerate() {
        if line.starts_with('+') { continue; }
        if line.starts_with('|') {
            let cells: Vec<String> = line.split('|').filter(|s| !s.is_empty()).map(|s| s.trim().to_string()).collect();
            if i == 1 { columns = cells; } else { rows.push(cells); }
        }
    }
    (columns, rows)
}

// SHOW INDEX 응답(탭 구분, 박스 아님)을 행목록으로 파싱한다. 인덱스가 없으면 빈 벡터.
fn parse_tsv_rows(body: &str) -> Vec<Vec<String>> {
    if !body.contains('\t') { return Vec::new(); }
    body.lines().skip(1).map(|l| l.split('\t').map(|s| s.to_string()).collect()).collect()
}

fn col_index(header: &[String], name: &str) -> Option<usize> {
    header.iter().position(|h| h.eq_ignore_ascii_case(name))
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
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() {
        Some(c) => c,
        None => return MultiQueryResult {
            total_elapsed: 0.0,
            results: vec![QueryResult { columns: vec![], rows: vec![], message: "연결된 데이터베이스가 없습니다.".to_string(), elapsed: 0.0, success: false }],
        },
    };

    let queries = split_queries_smart(&query);

    let mut results = Vec::new();
    for q in &queries {
        let q_start = Instant::now();
        let result = match send_one(conn, q) {
            Ok((true, body, _)) => {
                track_use_statement(conn, q);
                parse_output(&body, q_start.elapsed().as_secs_f64())
            }
            Ok((false, body, _)) => QueryResult {
                columns: vec![], rows: vec![],
                message: body, elapsed: q_start.elapsed().as_secs_f64(), success: false,
            },
            Err(e) => QueryResult {
                columns: vec![], rows: vec![],
                message: e, elapsed: q_start.elapsed().as_secs_f64(), success: false,
            },
        };
        let preview: String = q.chars().take(60).collect();
        let preview = if q.chars().count() > 60 { format!("{}...", preview) } else { preview };
        add_conn_log(conn, &format!(
            "{} ({:.3}s) {}",
            if result.success { "OK" } else { "ERR" },
            result.elapsed,
            preview
        ));
        results.push(result);
    }

    MultiQueryResult { total_elapsed: start.elapsed().as_secs_f64(), results }
}

fn parse_output(output: &str, elapsed: f64) -> QueryResult {
    if output.starts_with('+') {
        let (columns, rows) = parse_box_table(output);
        QueryResult { columns, rows, message: String::new(), elapsed, success: true }
    } else {
        QueryResult { columns: vec![], rows: vec![], message: output.to_string(), elapsed, success: true }
    }
}

// db 전체를 SELECT ... FROM information_schema.<table> WHERE table_schema='db' [AND ..] 로 조회해
// 지정한 컬럼 하나의 값 목록을 정렬해서 반환하는 공용 헬퍼.
fn query_infoschema_col(conn: &mut EngineConn, sql: &str, col: &str) -> Vec<String> {
    let (header, rows) = match send_one(conn, sql) {
        Ok((true, body, _)) => parse_box_table(&body),
        _ => return Vec::new(),
    };
    let idx = match col_index(&header, col) { Some(i) => i, None => return Vec::new() };
    let mut out: Vec<String> = rows.iter().filter_map(|r| r.get(idx).cloned()).collect();
    out.sort();
    out
}

#[tauri::command]
fn get_databases(state: State<AppState>) -> Vec<String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() { Some(c) => c, None => return Vec::new() };
    match send_one(conn, "SHOW DATABASES;") {
        Ok((true, body, _)) => {
            let (header, rows) = parse_box_table(&body);
            let idx = col_index(&header, "Database").unwrap_or(0);
            let mut dbs: Vec<String> = rows.iter().filter_map(|r| r.get(idx).cloned()).collect();
            dbs.sort();
            dbs
        }
        _ => Vec::new(),
    }
}

#[tauri::command]
fn get_current_db(state: State<AppState>) -> String {
    state.db.lock().unwrap_or_else(|e| e.into_inner()).as_ref().map(|c| c.current_db.clone()).unwrap_or_default()
}

#[tauri::command]
fn get_tables_for_db(db: String, state: State<AppState>) -> Vec<String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() { Some(c) => c, None => return Vec::new() };
    let sql = format!(
        "SELECT table_name FROM information_schema.tables WHERE table_schema='{}' AND table_type='BASE TABLE';",
        db.to_lowercase().replace('\'', "''")
    );
    query_infoschema_col(conn, &sql, "table_name")
}

#[tauri::command]
fn get_views_for_db(db: String, state: State<AppState>) -> Vec<String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() { Some(c) => c, None => return Vec::new() };
    let sql = format!(
        "SELECT table_name FROM information_schema.tables WHERE table_schema='{}' AND table_type='VIEW';",
        db.to_lowercase().replace('\'', "''")
    );
    query_infoschema_col(conn, &sql, "table_name")
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

// db 안의 모든 테이블에 대해 SHOW INDEX FROM db.table; 를 돌려 단일/복합 인덱스를 모은다.
// 해시 인덱스는 SHOW INDEX/INFORMATION_SCHEMA 어디에도 노출되지 않아 (Rust 원본도 동일한 한계)
// 이 목록에는 나타나지 않는다.
fn fetch_indexes(conn: &mut EngineConn, db: &str, tables: &[String]) -> Vec<IndexInfo> {
    let mut result = Vec::new();
    for table in tables {
        let sql = format!("SHOW INDEX FROM `{}`.`{}`;", db, table);
        if let Ok((true, body, _)) = send_one(conn, &sql) {
            for row in parse_tsv_rows(&body) {
                // Table \t Key_name \t Column_name \t Index_type
                if row.len() < 3 { continue; }
                let key_name = row[1].clone();
                let col_field = row[2].clone();
                let (columns, kind) = if col_field.contains(", ") {
                    (col_field.split(", ").map(|s| s.to_string()).collect(), "composite".to_string())
                } else {
                    (vec![col_field], "single".to_string())
                };
                result.push(IndexInfo { name: key_name, table: table.clone(), columns, kind });
            }
        }
    }
    result.sort_by(|a, b| a.table.cmp(&b.table).then(a.name.cmp(&b.name)));
    result
}

#[tauri::command]
fn get_indexes_for_db(db: String, state: State<AppState>) -> Vec<IndexInfo> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() { Some(c) => c, None => return Vec::new() };
    let db_lc = db.to_lowercase();
    let sql = format!(
        "SELECT table_name FROM information_schema.tables WHERE table_schema='{}' AND table_type='BASE TABLE';",
        db_lc.replace('\'', "''")
    );
    let tables = query_infoschema_col(conn, &sql, "table_name");
    fetch_indexes(conn, &db_lc, &tables)
}

// 트리거 목록을 조회하는 SQL 문이 엔진 자체에 없어 (Rust 원본도 동일), 이 모드에서는
// 항상 빈 목록을 반환한다 — 알려진 한계.
#[tauri::command]
fn get_triggers_for_db(_db: String, _state: State<AppState>) -> Vec<TriggerInfo> {
    Vec::new()
}

#[tauri::command]
fn get_tables(state: State<AppState>) -> Vec<String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() { Some(c) => c, None => return Vec::new() };
    match send_one(conn, "SHOW TABLES;") {
        Ok((true, body, _)) => {
            let (_, rows) = parse_box_table(&body);
            let mut tables: Vec<String> = rows.into_iter().filter_map(|r| r.into_iter().next()).collect();
            tables.sort();
            tables
        }
        _ => Vec::new(),
    }
}

fn split_qualified(table: &str, current_db: &str) -> (String, String) {
    match table.split_once('.') {
        Some((db, t)) => (db.to_string(), t.to_string()),
        None => (current_db.to_string(), table.to_string()),
    }
}

#[tauri::command]
fn get_columns(table: String, state: State<AppState>) -> Vec<String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() { Some(c) => c, None => return Vec::new() };
    match send_one(conn, &format!("DESCRIBE {};", table)) {
        Ok((true, body, _)) => {
            let (header, rows) = parse_box_table(&body);
            let idx = col_index(&header, "Field").unwrap_or(0);
            rows.into_iter().filter_map(|r| r.get(idx).cloned()).collect()
        }
        _ => Vec::new(),
    }
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
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = match guard.as_mut() { Some(c) => c, None => return Vec::new() };
    let (db, bare) = split_qualified(&table, &conn.current_db);

    let (header, rows) = match send_one(conn, &format!("DESCRIBE {};", table)) {
        Ok((true, body, _)) => parse_box_table(&body),
        _ => return Vec::new(),
    };
    let fi = col_index(&header, "Field");
    let ti = col_index(&header, "Type");
    let pki = col_index(&header, "PK");
    let nni = col_index(&header, "NN");
    let aii = col_index(&header, "Auto Increment");
    let dfi = col_index(&header, "Default");

    // UNIQUE 여부는 information_schema.columns의 COLUMN_KEY로 보강
    let uniq_sql = format!(
        "SELECT column_name, column_key FROM information_schema.columns WHERE table_schema='{}' AND table_name='{}';",
        db.to_lowercase().replace('\'', "''"), bare.to_lowercase().replace('\'', "''")
    );
    let mut unique_cols: std::collections::HashSet<String> = std::collections::HashSet::new();
    if let Ok((true, body, _)) = send_one(conn, &uniq_sql) {
        let (h, rs) = parse_box_table(&body);
        if let (Some(ci), Some(ki)) = (col_index(&h, "column_name"), col_index(&h, "column_key")) {
            for r in &rs {
                if r.get(ki).map(|k| k == "UNI").unwrap_or(false) {
                    if let Some(name) = r.get(ci) { unique_cols.insert(name.clone()); }
                }
            }
        }
    }

    // FK 참조는 information_schema.key_column_usage로 보강
    let fk_sql = format!(
        "SELECT column_name, referenced_table_name, referenced_column_name FROM information_schema.key_column_usage WHERE table_schema='{}' AND table_name='{}';",
        db.to_lowercase().replace('\'', "''"), bare.to_lowercase().replace('\'', "''")
    );
    let mut fk_map: HashMap<String, String> = HashMap::new();
    if let Ok((true, body, _)) = send_one(conn, &fk_sql) {
        let (h, rs) = parse_box_table(&body);
        if let (Some(ci), Some(rti), Some(rci)) = (col_index(&h, "column_name"), col_index(&h, "referenced_table_name"), col_index(&h, "referenced_column_name")) {
            for r in &rs {
                let rt = r.get(rti).cloned().unwrap_or_default();
                if rt.is_empty() || rt == "NULL" { continue; }
                let rc = r.get(rci).cloned().unwrap_or_default();
                if let Some(name) = r.get(ci) {
                    fk_map.insert(name.clone(), format!("{}({})", rt, rc));
                }
            }
        }
    }

    rows.into_iter().map(|r| {
        let name = fi.and_then(|i| r.get(i)).cloned().unwrap_or_default();
        ColumnDetail {
            data_type:   ti.and_then(|i| r.get(i)).cloned().unwrap_or_default(),
            is_pk:       pki.and_then(|i| r.get(i)).map(|v| v == "YES").unwrap_or(false),
            is_not_null: nni.and_then(|i| r.get(i)).map(|v| v == "YES").unwrap_or(false),
            is_auto_inc: aii.and_then(|i| r.get(i)).map(|v| v == "YES").unwrap_or(false),
            default_val: dfi.and_then(|i| r.get(i)).filter(|v| v.as_str() != "NULL").cloned(),
            is_unique:   unique_cols.contains(&name),
            fk_ref:      fk_map.get(&name).cloned(),
            name,
        }
    }).collect()
}

// ─── Tauri 커맨드: 서버 관리 ─────────────────────────────────
// engine_server.exe는 독립 프로세스라 같은 data_dir을 두 프로세스가 동시에 열 수 없다.
// 그래서 "서버 시작/중지"는 지금 붙어있는 프로세스를 내려받고, 원하는 포트로 다시
// 띄운 뒤 제어용 연결을 재접속하는 방식으로 구현한다 (같은 data_dir이라 안전).
#[tauri::command]
fn start_server(conn_id: String, port: u16, mysql_port: u16, state: State<AppState>) -> Result<String, String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let old = guard.take().ok_or("연결된 데이터베이스가 없습니다.")?;
    let (data_dir, bp, user, password, current_db) =
        (old.data_dir.clone(), old.buffer_pool_size, old.user.clone(), old.password.clone(), old.current_db.clone());
    drop(old); // Drop impl이 이전 프로세스를 종료한다

    let mp = if mysql_port > 0 { Some(mysql_port) } else { None };
    let mut conn = spawn_and_connect(&data_dir, bp, &user, &password, port, mp)?;
    if !current_db.is_empty() {
        let _ = send_one(&mut conn, &format!("USE {};", current_db));
        conn.current_db = current_db;
    }
    add_conn_log(&mut conn, &format!(
        "서버 시작: 127.0.0.1:{}{}", port,
        mp.map(|m| format!(" / MySQL 0.0.0.0:{}", m)).unwrap_or_default()
    ));
    *guard = Some(conn);

    state.servers.lock().unwrap_or_else(|e| e.into_inner()).insert(conn_id, ServerEntry { running: true, port, mysql_port: mp });
    Ok(format!("포트 {}에서 서버를 시작합니다...", port))
}

#[tauri::command]
fn get_app_data_dir(_app: tauri::AppHandle) -> String {
    // code/ 폴더를 기준으로 사용 → UI와 CLI/서버가 같은 데이터 공유
    code_dir().join("data").to_string_lossy().to_string()
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

// data/ 바로 아래에는 각 연결(Connection)의 전용 dataDir만 있어야 하는데, 연결 추가 후
// 인증 실패/중도 취소나 UI 밖에서(CLI 등으로) 직접 띄운 서버 때문에 어떤 저장된 연결에도
// 속하지 않는 디렉토리가 남을 수 있다. 앱 시작 시 한 번, 현재 저장된 연결들의 dataDir(keep)
// 목록에 없는 하위 디렉토리를 정리한다. `_`로 시작하는 예약 디렉토리(_system, _backups 등)는
// list_databases()와 동일한 규칙으로 건드리지 않는다.
#[tauri::command]
fn cleanup_orphan_data_dirs(base: String, keep: Vec<String>) -> Vec<String> {
    let norm = |s: &str| s.trim_end_matches(['\\', '/']).to_lowercase();
    let keep_set: std::collections::HashSet<String> = keep.iter().map(|k| norm(k)).collect();
    let mut removed = Vec::new();
    let Ok(entries) = std::fs::read_dir(&base) else { return removed; };
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() { continue; }
        let name = entry.file_name().to_string_lossy().to_string();
        if name.starts_with('_') { continue; }
        if keep_set.contains(&norm(&path.to_string_lossy())) { continue; }
        if std::fs::remove_dir_all(&path).is_ok() {
            removed.push(path.to_string_lossy().to_string());
        }
    }
    removed
}

#[tauri::command]
fn authenticate(user: String, password: String, data_dir: String, buffer_pool_size: usize, state: State<AppState>) -> bool {
    let bp = if buffer_pool_size > 0 { buffer_pool_size } else { 64 };
    // 이전 연결이 있으면 정리 (Drop이 자식 프로세스를 종료한다)
    *state.db.lock().unwrap_or_else(|e| e.into_inner()) = None;

    let port = pick_free_port();
    match spawn_and_connect(&data_dir, bp, &user, &password, port, None) {
        Ok(conn) => { *state.db.lock().unwrap_or_else(|e| e.into_inner()) = Some(conn); true }
        Err(_) => false,
    }
}

// SHOW PROCESSLIST를 파싱해 실제 접속 중인 세션 목록을 만든다.
fn query_processlist(conn: &mut EngineConn) -> Vec<SessionInfo> {
    let (header, rows) = match send_one(conn, "SHOW PROCESSLIST;") {
        Ok((true, body, _)) => parse_box_table(&body),
        _ => return Vec::new(),
    };
    let ui = col_index(&header, "User");
    let hi = col_index(&header, "Host");
    let ti = col_index(&header, "Time");
    use std::time::{SystemTime, UNIX_EPOCH};
    let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs();
    rows.into_iter().map(|r| {
        let elapsed: u64 = ti.and_then(|i| r.get(i)).and_then(|v| v.parse().ok()).unwrap_or(0);
        SessionInfo {
            addr:         hi.and_then(|i| r.get(i)).cloned().unwrap_or_default(),
            user:         ui.and_then(|i| r.get(i)).cloned().unwrap_or_default(),
            connected_at: now.saturating_sub(elapsed),
            query_count:  0,
        }
    }).collect()
}

#[tauri::command]
fn stop_server(conn_id: String, state: State<AppState>) -> Result<String, String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let old = guard.take().ok_or("연결된 데이터베이스가 없습니다.")?;
    let (data_dir, bp, user, password, current_db) =
        (old.data_dir.clone(), old.buffer_pool_size, old.user.clone(), old.password.clone(), old.current_db.clone());
    drop(old);

    let port = pick_free_port();
    let mut conn = spawn_and_connect(&data_dir, bp, &user, &password, port, None)?;
    if !current_db.is_empty() {
        let _ = send_one(&mut conn, &format!("USE {};", current_db));
        conn.current_db = current_db;
    }
    *guard = Some(conn);

    if let Some(e) = state.servers.lock().unwrap_or_else(|e| e.into_inner()).get_mut(&conn_id) {
        e.running = false;
    }
    Ok("서버를 중지했습니다.".to_string())
}

#[tauri::command]
fn get_server_status(conn_id: String, state: State<AppState>) -> ServerStatus {
    let entry = state.servers.lock().unwrap_or_else(|e| e.into_inner()).get(&conn_id).map(|e| (e.running, e.port));
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let (sessions, log, fallback_port) = match guard.as_mut() {
        Some(conn) => (query_processlist(conn), conn.log.clone(), conn.port),
        None => (Vec::new(), Vec::new(), 7878),
    };
    let (running, port) = entry.unwrap_or((false, fallback_port));
    ServerStatus { running, port, client_count: sessions.len(), log, sessions }
}

#[tauri::command]
fn clear_server_log(_conn_id: String, state: State<AppState>) {
    if let Some(conn) = state.db.lock().unwrap_or_else(|e| e.into_inner()).as_mut() {
        conn.log.clear();
    }
}


// ─── CSV 내보내기 ─────────────────────────────────────────────
#[tauri::command]
fn export_csv(query: String, file_path: String, state: State<AppState>) -> Result<String, String> {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = guard.as_mut().ok_or("연결된 데이터베이스가 없습니다.")?;
    let (ok, body, _) = send_one(conn, &query)?;
    if !ok {
        return Err(body);
    }

    let qr = parse_output(&body, 0.0);
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
// 프런트가 <input type="file"> + FileReader로 이미 읽어 넘겨준 content를 그대로
// 파싱한다 (importSqlFile과 동일한 파일 읽기 방식 — 네이티브 dialog 플러그인 불필요,
// 그래서 실제 파일시스템 경로 대신 파일 내용 문자열을 받는다).
#[tauri::command]
fn import_csv(table: String, content: String, state: State<AppState>) -> Result<String, String> {
    let mut lines = content.lines();

    let header = match lines.next() {
        Some(h) => h,
        None => return Err("CSV file is empty.".to_string()),
    };
    let cols: Vec<&str> = header.split(',').map(|c| c.trim().trim_matches('"')).collect();
    let col_list = cols.iter().map(|c| format!("`{}`", c)).collect::<Vec<_>>().join(", ");

    let mut count = 0usize;
    let mut errors = 0usize;
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    let conn = guard.as_mut().ok_or("연결된 데이터베이스가 없습니다.")?;

    for line in lines {
        if line.trim().is_empty() { continue; }
        let vals: Vec<String> = csv_parse_row(line);
        let val_list = vals.iter().map(|v| format!("'{}'", v.replace('\'', "''"))).collect::<Vec<_>>().join(", ");
        let sql = format!("INSERT INTO {} ({}) VALUES ({});", table, col_list, val_list);
        match send_one(conn, &sql) {
            Ok((true, _, _)) => count += 1,
            _ => errors += 1,
        }
    }
    Ok(format!("Imported {} rows ({} errors).", count, errors))
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

// ─── MCP 자동 설정 ───────────────────────────────────────────
fn find_python_with_mcp() -> Result<String, String> {
    let where_out = std::process::Command::new("where")
        .arg("python")
        .output()
        .map_err(|_| "Python을 찾을 수 없습니다.".to_string())?;

    let paths_str = String::from_utf8_lossy(&where_out.stdout);
    for path in paths_str.lines() {
        let path = path.trim();
        if path.is_empty() { continue; }
        if let Ok(test) = std::process::Command::new(path)
            .args(["-c", "import mcp"])
            .output()
        {
            if test.status.success() {
                return Ok(path.to_string());
            }
        }
    }
    Err("mcp 패키지가 설치된 Python을 찾을 수 없습니다.\n설치 방법: pip install mcp".to_string())
}

fn write_mcp_into(config_path: &std::path::Path, entry: &serde_json::Value) -> Result<(), String> {
    // PLAN.md P1 "MCP 설정 병합 실패 시 기존 설정 덮어씀" 수정: 기존 설정 파일이 있는데
    // 파싱에 실패하면(트렁케이트/문법 오류 등) 조용히 빈 객체로 대체한 뒤 그대로 덮어써서
    // 사용자의 기존 설정(다른 MCP 서버 등록 등)이 전부 사라지던 것 -- 파일을 건드리지
    // 않고 즉시 에러로 반환한다.
    let mut config: serde_json::Value = if config_path.exists() {
        let content = std::fs::read_to_string(config_path).map_err(|e| e.to_string())?;
        serde_json::from_str(&content)
            .map_err(|e| format!("기존 MCP 설정 파일을 읽는 데 실패했습니다 ({}): {}", config_path.display(), e))?
    } else {
        serde_json::json!({})
    };
    if !config.get("mcpServers").map(|v| v.is_object()).unwrap_or(false) {
        config["mcpServers"] = serde_json::json!({});
    }
    config["mcpServers"]["RuSQL"] = entry.clone();
    // BOM 없는 UTF-8로 쓰기
    let json_str = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    std::fs::write(config_path, json_str.as_bytes()).map_err(|e| e.to_string())
}

#[tauri::command]
fn setup_mcp_config(host: String, port: u16, user: String, password: String) -> Result<String, String> {
    let mcp_server_path = code_dir().join("mcp").join("mcp_server.py");

    if !mcp_server_path.exists() {
        return Err(format!("mcp_server.py를 찾을 수 없습니다:\n{}", mcp_server_path.display()));
    }

    let python_path = find_python_with_mcp()?;
    // mcp_server.py의 접속정보(RUSQL_HOST/PORT/USER/PASS)는 하드코딩 기본값(127.0.0.1:7878,
    // root/root)만 있었고 이 UI가 실제로 연결 중인 서버의 host/port/계정을 전혀 전달하지
    // 않아, 기본값과 다른 연결에서는 MCP 도구가 항상 연결/인증에 실패했음. Claude Desktop
    // 설정의 "env" 필드로 지금 이 세션의 실제 값을 넘겨 mcp_server.py가 그대로 읽어 쓰도록 함
    // (mcp_server.py 쪽은 env가 없으면 기존 하드코딩 기본값으로 폴백 — 하위 호환 유지).
    let mcp_entry = serde_json::json!({
        "command": python_path,
        "args": ["-u", mcp_server_path.to_string_lossy().as_ref()],
        "env": {
            "RUSQL_HOST": host,
            "RUSQL_PORT": port.to_string(),
            "RUSQL_USER": user,
            "RUSQL_PASS": password,
        },
        "alwaysAllow": [
            "execute_sql", "list_databases", "list_tables", "get_table_schema",
            "explain_query", "get_indexes", "sample_data"
        ]
    });

    // 1. 일반 설치 버전 경로
    let appdata = std::env::var("APPDATA")
        .map_err(|_| "APPDATA 환경변수를 찾을 수 없습니다.".to_string())?;
    let real_dir = std::path::Path::new(&appdata).join("Claude");
    std::fs::create_dir_all(&real_dir).map_err(|e| e.to_string())?;
    write_mcp_into(&real_dir.join("claude_desktop_config.json"), &mcp_entry)?;

    // 2. Windows Store 버전 가상화 경로 (있으면 추가로 씀)
    if let Ok(localappdata) = std::env::var("LOCALAPPDATA") {
        let packages = std::path::Path::new(&localappdata).join("Packages");
        if let Ok(entries) = std::fs::read_dir(&packages) {
            for entry in entries.flatten() {
                if entry.file_name().to_string_lossy().starts_with("Claude_") {
                    let store_dir = entry.path()
                        .join("LocalCache").join("Roaming").join("Claude");
                    if store_dir.exists() {
                        let _ = write_mcp_into(&store_dir.join("claude_desktop_config.json"), &mcp_entry);
                    }
                }
            }
        }
    }

    Ok(format!("연결 완료! Claude Desktop을 재시작하세요.\n\nPython: {}", python_path))
}

#[tauri::command]
fn open_terminal() {
    let frontend_dir = code_dir().join("frontend");
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "cmd"])
        .current_dir(frontend_dir)
        .spawn();
}

#[tauri::command]
fn open_url(url: String) {
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "", &url])
        .spawn();
}

fn bench_dir() -> std::path::PathBuf {
    code_dir().join("test").join("perf")
}

#[tauri::command]
fn read_bench_result() -> String {
    std::fs::read_to_string(bench_dir().join("result.json")).unwrap_or_default()
}

#[tauri::command]
fn open_bench_terminal() {
    let dir = bench_dir();
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "cmd", "/k", "pip install -q -r requirements.txt && python bench.py"])
        .current_dir(dir)
        .spawn();
}

#[tauri::command]
fn sync_tab_content(name: String, content: String, state: State<AppState>) {
    state.ui.tab_content.lock().unwrap_or_else(|e| e.into_inner()).insert(name, content);
}

#[tauri::command]
fn sync_tab_list(names: Vec<String>, state: State<AppState>) {
    *state.ui.tab_list.lock().unwrap_or_else(|e| e.into_inner()) = names;
}

#[tauri::command]
fn sync_query_result(result: String, state: State<AppState>) {
    *state.ui.last_result.lock().unwrap_or_else(|e| e.into_inner()) = result;
}

#[tauri::command]
fn sync_current_db(db: String, state: State<AppState>) {
    *state.ui.current_db.lock().unwrap_or_else(|e| e.into_inner()) = db;
}

#[tauri::command]
fn open_bench_graph() {
    let path = bench_dir().join("benchmark_result.png");
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "", &path.to_string_lossy()])
        .spawn();
}


// ─── 엔트리포인트 ─────────────────────────────────────────────
fn main() {
    tauri::Builder::default()
        .manage(AppState {
            db: Arc::new(Mutex::new(None)),
            servers: Mutex::new(HashMap::new()),
            ui: Arc::new(UiStore {
                tab_content: Arc::new(Mutex::new(HashMap::new())),
                tab_list:    Arc::new(Mutex::new(Vec::new())),
                last_result: Arc::new(Mutex::new(String::new())),
                current_db:  Arc::new(Mutex::new(String::new())),
            }),
        })
        .setup(|app| {
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
            get_tables_for_db,
            get_views_for_db,
            get_indexes_for_db,
            get_triggers_for_db,
            authenticate,
            delete_conn_data,
            cleanup_orphan_data_dirs,
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
            read_bench_result,
            open_bench_terminal,
            open_bench_graph,
            sync_tab_content,
            sync_tab_list,
            sync_query_result,
            sync_current_db,
            setup_mcp_config,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::*;

    // Regression: write_mcp_into used to silently replace a malformed existing
    // claude_desktop_config.json with `serde_json::json!({})` on parse failure, then
    // overwrite the file with just the RuSQL entry -- destroying whatever else was in
    // there (other MCP servers, unrelated settings). It must now refuse to touch the
    // file at all when the existing content doesn't parse.
    #[test]
    fn write_mcp_into_does_not_overwrite_a_malformed_existing_config() {
        let dir = std::env::temp_dir().join(format!("rusql_mcp_test_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let config_path = dir.join("claude_desktop_config.json");

        let malformed = r#"{ "mcpServers": { "other": {"command": "x"} "#; // truncated, invalid JSON
        std::fs::write(&config_path, malformed).unwrap();

        let entry = serde_json::json!({"command": "python", "args": []});
        let result = write_mcp_into(&config_path, &entry);

        assert!(result.is_err(), "expected an error for malformed existing config, got {:?}", result);
        let after = std::fs::read_to_string(&config_path).unwrap();
        assert_eq!(after, malformed, "malformed config file must be left completely untouched");

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn write_mcp_into_merges_into_a_valid_existing_config() {
        let dir = std::env::temp_dir().join(format!("rusql_mcp_test_valid_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let config_path = dir.join("claude_desktop_config.json");

        std::fs::write(&config_path, r#"{"mcpServers": {"other": {"command": "x"}}}"#).unwrap();

        let entry = serde_json::json!({"command": "python", "args": []});
        write_mcp_into(&config_path, &entry).unwrap();

        let after: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(&config_path).unwrap()).unwrap();
        assert!(after["mcpServers"]["other"].is_object(), "pre-existing entry must survive the merge");
        assert!(after["mcpServers"]["RuSQL"].is_object(), "new entry must be added");

        std::fs::remove_dir_all(&dir).ok();
    }

    // Regression: every `state.db`/`state.servers`/`state.ui.*` `.lock()` call site used
    // to be `.lock().unwrap()` -- if any single Tauri command panicked while holding one
    // of those Mutexes, it would poison it, and every later `.lock().unwrap()` on the
    // same Mutex would then also panic, permanently bricking every command that touches
    // shared state until the app was restarted. `.unwrap_or_else(|e| e.into_inner())`
    // recovers the guard instead. This test proves the *pattern* (not AppState directly,
    // since its fields aren't independently constructible outside Tauri's own state
    // machinery) using a plain Mutex poisoned the same way a panicking command would.
    #[test]
    fn poisoned_mutex_is_recovered_instead_of_panicking_again() {
        let m = std::sync::Mutex::new(42);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _guard = m.lock().unwrap_or_else(|e| e.into_inner());
            panic!("simulated panic while holding the lock");
        }));
        assert!(result.is_err(), "the simulated panic should have propagated");
        assert!(m.is_poisoned(), "the Mutex should now be poisoned");

        // The old `.lock().unwrap()` pattern would panic here on the poisoned Mutex.
        // `.unwrap_or_else(|e| e.into_inner())` must instead recover the guard.
        let recovered = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            *m.lock().unwrap_or_else(|e| e.into_inner())
        }));
        assert_eq!(recovered.ok(), Some(42), "lock() must recover from poisoning, not panic again");
    }
}
