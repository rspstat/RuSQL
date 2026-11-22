#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::net::TcpStream;
use std::io::{BufRead, BufReader, Write};
use std::time::Instant;
use std::process::{Child, Command, Stdio};
#[cfg(windows)]
use std::os::windows::process::CommandExt;

use sha1::{Digest, Sha1};
use tauri::{Emitter, Manager, State};

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
}

// ─── 상태 구조체 ──────────────────────────────────────────────
struct UiStore {
    tab_content: Arc<Mutex<HashMap<String, String>>>, // tab_name → editor content
    tab_list:    Arc<Mutex<Vec<String>>>,             // ordered tab names
    last_result: Arc<Mutex<String>>,                  // last query result (TSV)
    current_db:  Arc<Mutex<String>>,                  // current database name
    logged_in:   Arc<Mutex<bool>>,                    // 로그인(DB 세션 접속) 여부 — 다중 인스턴스 레지스트리용
}

// ─── C++ engine_server.exe 프로세스 + 그 프로세스에 대한 제어용 TCP 연결 ──────
// UI는 더 이상 엔진을 프로세스 내부에 임베드하지 않고, 별도 프로세스로 띄운 뒤
// 그 자신도 여느 클라이언트와 마찬가지로 TCP로 접속해 SQL을 주고받는다.
struct EngineConn {
    child:            Child,
    writer:           TcpStream,
    reader:           BufReader<TcpStream>,
    port:             u16,
    data_dir:         String,
    buffer_pool_size: usize,
    user:             String,
    password:         String,
    current_db:       String,
    log:              Vec<String>,
    ready_ms:         f64, // from starting the process until it accepted the sign-in: the engine opening its data, a crash recovery included
}

impl Drop for EngineConn {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

struct AppState {
    db:          Arc<Mutex<Option<EngineConn>>>,
    servers:    Mutex<HashMap<String, ServerEntry>>,
    ui:          Arc<UiStore>,
    instance_id: String, // 이 프로세스의 수명 동안 고정 — 다중 인스턴스 레지스트리/타깃팅용
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
    recovery:     ActivityTable, // information_schema.recovery: what the engine did to open its data the last time it started
    ready_ms:     f64,
}

// ─── code/ 디렉터리 위치 헬퍼 ─────────────────────────────────
// "컴파일타임 개발자 경로 하드코딩 → 배포 불가" 수정: env!("CARGO_MANIFEST_DIR")는
// 빌드 당시 소스 체크아웃 경로를 실행 파일에 그대로 박아 넣어서, 빌드한 PC와 다른 곳에
// 복사하면 존재하지 않는 디렉터리를 가리키게 됐다. 디버그 빌드(`cargo tauri dev`)에서는
// 그대로 CARGO_MANIFEST_DIR 기준으로 계산하고(개발 중엔 항상 유효), 릴리즈 빌드에서는
// 실행 파일이 실제로 지금 있는 위치(`current_exe()`) 기준으로 계산한다. "code/ 트리 전체를
// 복사해서 어디서든 실행"하는 모델은 그대로 두고 컴파일타임 하드코딩만 없앴다. 인스톨러로 설치한 앱은
// code/ 트리가 없으므로 아래 bundled_engine_path()/data_root()가 설치 폴더와 %APPDATA%를 쓴다.
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

// ─── 설치본(번들) 위치 ───────────────────────────────────────
// 설치본에는 code/ 트리가 없다. tauri.conf.json의 bundle.resources가 실행 파일 옆에 engine/(engine_server.exe와 그것이 쓰는 VC++ 런타임 DLL)와
// mcp/(mcp_server.py)를 같이 설치하므로, 그 모양이면 엔진과 MCP 스크립트를 거기서 찾고 데이터는 사용자별로 쓸 수 있는 %APPDATA%\RuSQL\data에 둔다
// (설치 폴더는 쓸 수 없을 수 있다). 소스 트리 안(디버그 빌드 cargo tauri dev, 트리 안에서 실행한 릴리즈 exe)에서는 번들을 쓰지 않고 예전 방식 그대로다.
fn exe_dir() -> Option<std::path::PathBuf> {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|p| p.to_path_buf()))
}

// 소스 트리(code/) 안에서 실행 중인가. 그 트리에는 방금 빌드한 엔진 · MCP 스크립트 · data 폴더가 있으므로 번들보다 그것을 쓴다 (target/release/rusql-ui.exe를 트리 안에서
// 그냥 실행하는 개발자의 흐름 -- UI · CLI · MCP가 같은 code/data를 본다). tauri-build는 target/release/engine/에도 번들 파일을 복사해 두므로 구분이 필요하다.
fn in_code_tree() -> bool {
    let root = code_dir();
    root.is_absolute() && root.join("mcp").join("mcp_server.py").is_file()
}

// 번들된 엔진: 실행 파일 옆 engine/engine_server.exe. 1 MB가 안 되는 파일은 build.rs가 만든 자리표시 파일(릴리즈 빌드 전 scripts/stage-engine.mjs가 진짜로 바꾼다)이라 없는 것으로 본다.
fn bundled_engine_path() -> Option<std::path::PathBuf> {
    if cfg!(debug_assertions) || in_code_tree() {
        return None;
    }
    let path = exe_dir()?.join("engine").join("engine_server.exe");
    let big_enough = path.metadata().map(|m| m.len() > 1 << 20).unwrap_or(false);
    big_enough.then_some(path)
}

// 앱 · MCP 서버 · 연결 목록이 함께 쓰는 data 폴더. RUSQL_DATA_DIR이 있으면 그것(MCP 서버에도 같은 값을 넘겨 두 프로세스가 같은 파일을 본다).
fn data_root() -> std::path::PathBuf {
    if let Ok(dir) = std::env::var("RUSQL_DATA_DIR") {
        if !dir.is_empty() {
            return std::path::PathBuf::from(dir);
        }
    }
    if bundled_engine_path().is_some() {
        if let Ok(appdata) = std::env::var("APPDATA") {
            return std::path::PathBuf::from(appdata).join("RuSQL").join("data");
        }
    }
    code_dir().join("data")
}

fn mcp_server_script() -> std::path::PathBuf {
    if bundled_engine_path().is_some() {
        if let Some(dir) = exe_dir() {
            let bundled = dir.join("mcp").join("mcp_server.py");
            if bundled.is_file() {
                return bundled;
            }
        }
    }
    code_dir().join("mcp").join("mcp_server.py")
}

// ─── C++ engine_server.exe 위치/포트 헬퍼 ─────────────────────
// MSVC Debug 빌드는 최적화가 전혀 없어 Release 대비 한 자릿수~두 자릿수 배 느리다
// (실측: 단건 INSERT 9x, Bulk DELETE 21x, SeqScan 22x 등) -- UI가 항상 Debug 바이너리를
// 띄우고 있었던 게 "Rust 버전보다 훨씬 느려 보인다"는 오해의 실제 원인이었다. Release를
// 우선하되, 개발 중 Debug만 새로 빌드했을 수도 있으니 두 바이너리 다 있으면 더 최근에
// 빌드된 쪽을 쓴다 (특정 설정 하나로 고정하면 반대 방향의 "왜 최신 빌드가 반영 안 되지"
// 혼란이 재발할 뿐이라 mtime 비교가 더 안전하다).
fn engine_server_path() -> std::path::PathBuf {
    if let Some(bundled) = bundled_engine_path() {
        return bundled; // 설치본: 실행 파일 옆에 같이 설치된 엔진
    }
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
    let started = Instant::now();
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
    let mut command = Command::new(&exe);
    command.args(&args).current_dir(data_dir).stdout(Stdio::null()).stderr(Stdio::null());
    // CREATE_NO_WINDOW: engine_server.exe는 콘솔 프로그램이라 이 옵션이 없으면 Windows가 엔진마다 빈 콘솔 창을 띄운다(그 창을 닫으면 엔진이 죽는다)
    #[cfg(windows)]
    command.creation_flags(0x0800_0000);
    let child = command
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
        child, writer, reader, port,
        data_dir: data_dir.to_string(), buffer_pool_size,
        user: user.to_string(), password: password.to_string(),
        current_db: String::new(), log: Vec::new(), ready_ms: 0.0,
    };

    let token = compute_native_password_token(password, &nonce);
    if writeln!(conn.writer, "AUTH {} {}", user, token).is_err() || conn.writer.flush().is_err() {
        return Err("AUTH 전송 실패.".to_string());
    }
    let resp = read_block(&mut conn.reader);
    if !resp.first().map(|s| s.starts_with("OK")).unwrap_or(false) {
        return Err(resp.into_iter().next().unwrap_or_else(|| "인증 실패".to_string()));
    }
    conn.ready_ms = started.elapsed().as_secs_f64() * 1000.0;
    Ok(conn)
}

// 하나의 SQL 문(세미콜론 자동 보정)을 보내고 (성공여부, 본문, 경과초)를 반환한다.
fn send_one(conn: &mut EngineConn, stmt: &str) -> Result<(bool, String, f64), String> {
    exchange(&mut conn.writer, &mut conn.reader, stmt)
}

fn exchange(writer: &mut TcpStream, reader: &mut BufReader<TcpStream>, stmt: &str) -> Result<(bool, String, f64), String> {
    let mut s = stmt.trim().to_string();
    if s.is_empty() { return Ok((true, String::new(), 0.0)); }
    if !s.ends_with(';') { s.push(';'); }
    writeln!(writer, "{}", s).map_err(|e| e.to_string())?;
    writer.flush().map_err(|e| e.to_string())?;
    let block = read_block(reader);
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

// 결과 표의 셀 하나에서 이스케이프를 푼다: 엔진은 값의 역슬래시를 \\, 줄바꿈을 \n, 막대를 \|, 값 양 끝의 공백/탭(패딩과 구별되지 않는)을 \s, \t 로 쓴다.
fn unescape_cell(cell: &str) -> String {
    let mut out = String::with_capacity(cell.len());
    let mut it = cell.chars().peekable();
    while let Some(c) = it.next() {
        if c == '\\' {
            match it.peek() {
                Some('\\') => { out.push('\\'); it.next(); }
                Some('|') => { out.push('|'); it.next(); }
                Some('n') => { out.push('\n'); it.next(); }
                Some('s') => { out.push(' '); it.next(); }
                Some('t') => { out.push('\t'); it.next(); }
                _ => out.push(c),
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn push_cell(piece: &str, cells: &mut Vec<String>) {
    if !piece.is_empty() {
        cells.push(unescape_cell(piece.trim_matches(|c| c == ' ' || c == '\t')));
    }
}

// 행 한 줄("| a   | b |")의 셀들. 이스케이프된 막대(\|)는 셀 경계가 아니고, 첫 막대 앞·마지막 막대 뒤 조각은 셀이 아니다.
fn split_row(line: &str) -> Vec<String> {
    let mut cells = Vec::new();
    let mut piece = String::new();
    let mut it = line.chars();
    while let Some(c) = it.next() {
        if c == '\\' {
            piece.push(c);
            if let Some(n) = it.next() { piece.push(n); }
        } else if c == '|' {
            push_cell(&piece, &mut cells);
            piece.clear();
        } else {
            piece.push(c);
        }
    }
    push_cell(&piece, &mut cells);
    cells
}

#[cfg(test)]
mod cell_tests {
    use super::split_row;

    #[test]
    fn cells_keep_their_spaces_and_special_characters() {
        assert_eq!(split_row("| 1  | \\sx\\s | a\\|b |"), vec!["1", " x ", "a|b"]);
        assert_eq!(split_row("| \\\\n |"), vec!["\\n"]);
        assert_eq!(split_row("|   | x |"), vec!["", "x"]);
        assert_eq!(split_row("| a\\nb | \\t\\tz |"), vec!["a\nb", "\t\tz"]);
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
            let cells: Vec<String> = split_row(line);
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
        db.to_lowercase().replace('\'', "''"), bare.replace('\'', "''")
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
        db.to_lowercase().replace('\'', "''"), bare.replace('\'', "''")
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
    log_recovery(&mut conn);
    *guard = Some(conn);

    state.servers.lock().unwrap_or_else(|e| e.into_inner()).insert(conn_id, ServerEntry { running: true, port });
    Ok(format!("포트 {}에서 서버를 시작합니다...", port))
}

#[tauri::command]
fn get_app_data_dir(_app: tauri::AppHandle) -> String {
    // code/ 폴더를 기준으로 사용 → UI와 CLI/서버가 같은 데이터 공유 (설치본은 %APPDATA%\RuSQL\data)
    data_root().to_string_lossy().to_string()
}

// ─── 저장된 연결 목록 ──────────────────────────────────────────
// code/data/connections.json 파일에 저장 — localStorage가 아니라 파일인 이유는,
// MCP 서버(별도 Python 프로세스, code/mcp/mcp_server.py)도 같은 파일을 직접 읽고 써서
// Claude가 Connections를 추가/삭제할 수 있게 하기 위함(웹뷰 localStorage는 다른 프로세스가
// 접근할 방법이 없음). 필드명은 프런트의 Connection 인터페이스와 그대로 맞춤.
#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct ConnectionEntry {
    id: String,
    name: String,
    host: String,
    port: u16,
    user: String,
    password: String,
    #[serde(rename = "autoLogin")]
    auto_login: bool,
    #[serde(rename = "dataDir")]
    data_dir: String,
    // 즐겨찾기 표시와 마지막으로 연결한 시각(epoch ms) — 이 필드가 없는 예전 파일·MCP가 쓴 항목은 기본값으로 읽힘
    #[serde(default)]
    favorite: bool,
    #[serde(default, rename = "lastUsed")]
    last_used: u64,
}

fn connections_file_path() -> std::path::PathBuf {
    data_root().join("connections.json")
}

#[tauri::command]
fn get_connections() -> Vec<ConnectionEntry> {
    std::fs::read_to_string(connections_file_path())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

#[tauri::command]
fn save_connections(connections: Vec<ConnectionEntry>) -> Result<(), String> {
    let path = connections_file_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let json = serde_json::to_string_pretty(&connections).map_err(|e| e.to_string())?;
    // 임시 파일에 쓴 뒤 원자적으로 교체 — MCP 서버가 거의 동시에 같은 파일을 쓰는
    // 경우에도 절반만 쓰인 손상된 JSON을 절대 남기지 않기 위함(WAL 원자적 재작성과 동일한 이유).
    let tmp_path = path.with_extension("json.tmp");
    std::fs::write(&tmp_path, json).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp_path, &path).map_err(|e| e.to_string())?;
    Ok(())
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
    // 새 엔진은 서버가 꺼진 채(빈 포트, MySQL 없이) 시작한다 -- 옛 엔진의 "서버 켜짐" 기록이 남으면 Server Manager와 홈 화면의 점이 거짓말을 한다
    state.servers.lock().unwrap_or_else(|e| e.into_inner()).clear();

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

// What the server is doing right now (the Sessions panel): the rows of the engine's information_schema tables processlist, innodb_trx, innodb_locks,
// innodb_lock_waits and innodb_deadlocks, as column names and rows of text.
#[derive(serde::Serialize, Default, Clone)]
struct ActivityTable {
    columns: Vec<String>,
    rows:    Vec<Vec<String>>,
}

#[derive(serde::Serialize, Default, Clone)]
struct Activity {
    sessions:     ActivityTable,
    transactions: ActivityTable,
    locks:        ActivityTable,
    waits:        ActivityTable,
    deadlocks:    ActivityTable,
    // the slow query log (R59): the newest statements that ran for at least long_query_time, the two switches, and how many statements were that slow
    slow:          ActivityTable,
    slow_settings: ActivityTable,
    slow_count:    ActivityTable,
}

fn activity_table(conn: &mut EngineConn, sql: &str) -> ActivityTable {
    match send_one(conn, sql) {
        Ok((true, body, _)) => {
            let (columns, rows) = parse_box_table(&body);
            ActivityTable { columns, rows }
        }
        _ => ActivityTable::default(),
    }
}

// What the engine did to open its data (information_schema.recovery, one row): the Server Manager's recovery card and the log line after a restart.
fn query_recovery(conn: &mut EngineConn) -> ActivityTable {
    activity_table(conn, "SELECT OPEN_MS, UNDO_MS, REDO_MS, WAL_TXNS_REPLAYED, WAL_TXNS_UNDONE, REDO_OPS, REDO_TABLES, CRASH_DETECTED FROM information_schema.recovery;")
}

fn log_recovery(conn: &mut EngineConn) {
    let t = query_recovery(conn);
    let Some(row) = t.rows.first() else { return };
    let get = |name: &str| t.columns.iter().position(|c| c.eq_ignore_ascii_case(name)).and_then(|i| row.get(i)).cloned().unwrap_or_default();
    let line = if get("CRASH_DETECTED") == "YES" {
        format!(
            "비정상 종료에서 복구: redo {}건({}개 테이블, {} ms), WAL 커밋 {}건 재적용·미완료 {}건 되돌림({} ms), 엔진 열기 {} ms (프로세스 시작부터 {:.0} ms)",
            get("REDO_OPS"), get("REDO_TABLES"), get("REDO_MS"), get("WAL_TXNS_REPLAYED"), get("WAL_TXNS_UNDONE"), get("UNDO_MS"), get("OPEN_MS"), conn.ready_ms
        )
    } else {
        format!("엔진 열기 {} ms, 복구할 것 없음 (프로세스 시작부터 {:.0} ms)", get("OPEN_MS"), conn.ready_ms)
    };
    add_conn_log(conn, &line);
}

#[tauri::command]
fn get_activity(state: State<AppState>) -> Activity {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    match guard.as_mut() {
        Some(conn) => Activity {
            sessions:     activity_table(conn, "SELECT ID, USER, HOST, DB, COMMAND, TIME, INFO FROM information_schema.processlist ORDER BY ID;"),
            transactions: activity_table(conn, "SELECT TRX_ID, TRX_STATE, TRX_MYSQL_THREAD_ID, TRX_ROWS_LOCKED, TRX_WAITING_FOR, TIMESTAMPDIFF(SECOND, TRX_STARTED, NOW()) AS TRX_AGE FROM information_schema.innodb_trx;"),
            locks:        activity_table(conn, "SELECT LOCK_TRX_ID, LOCK_TABLE, LOCK_DATA FROM information_schema.innodb_locks;"),
            waits:        activity_table(conn, "SELECT REQUESTING_TRX_ID, BLOCKING_TRX_ID FROM information_schema.innodb_lock_waits;"),
            deadlocks:    activity_table(conn, "SELECT REQUESTING_TRX_ID, BLOCKING_TRX_ID FROM information_schema.innodb_deadlocks;"),
            slow:          activity_table(conn, "SELECT START_TIME, QUERY_TIME, ROWS_SENT, DB, THREAD_ID, SQL_TEXT FROM information_schema.slow_log ORDER BY START_TIME DESC LIMIT 15;"),
            slow_settings: activity_table(conn, "SELECT @@slow_query_log AS LOG_ON, @@long_query_time AS SECONDS;"),
            slow_count:    activity_table(conn, "SHOW GLOBAL STATUS LIKE 'Slow_queries';"),
        },
        None => Activity::default(),
    }
}

// The counters of SHOW GLOBAL STATUS (statements by kind, sessions, buffer pool, redo log, row-lock waits ...): the Metrics panel of the Server Manager draws their trend.
#[tauri::command]
fn get_status_counters(state: State<AppState>) -> ActivityTable {
    let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
    match guard.as_mut() {
        Some(conn) => activity_table(conn, "SHOW GLOBAL STATUS;"),
        None => ActivityTable::default(),
    }
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
    add_conn_log(&mut conn, "서버 중지 (엔진 프로세스를 강제 종료하고 같은 데이터 폴더로 다시 열었습니다)");
    log_recovery(&mut conn);
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
    let (sessions, log, fallback_port, recovery, ready_ms) = match guard.as_mut() {
        Some(conn) => (query_processlist(conn), conn.log.clone(), conn.port, query_recovery(conn), conn.ready_ms),
        None => (Vec::new(), Vec::new(), 7878, ActivityTable::default(), 0.0),
    };
    let (running, port) = entry.unwrap_or((false, fallback_port));
    ServerStatus { running, port, client_count: sessions.len(), log, sessions, recovery, ready_ms }
}

#[tauri::command]
fn clear_server_log(_conn_id: String, state: State<AppState>) {
    if let Some(conn) = state.db.lock().unwrap_or_else(|e| e.into_inner()).as_mut() {
        conn.log.clear();
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
    let mut where_cmd = std::process::Command::new("where");
    where_cmd.arg("python");
    #[cfg(windows)]
    where_cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: 콘솔 창이 깜박이지 않게
    let where_out = where_cmd
        .output()
        .map_err(|_| "Python을 찾을 수 없습니다.".to_string())?;

    let paths_str = String::from_utf8_lossy(&where_out.stdout);
    for path in paths_str.lines() {
        let path = path.trim();
        if path.is_empty() { continue; }
        // `import mcp`만으로는 부족하다: 현재 폴더에 mcp라는 폴더가 있으면(설치 폴더에는 mcp/mcp_server.py가 있다) 파이썬이 그 폴더를 패키지로 읽어 mcp가 없는 파이썬도
        // 통과시키고, Claude Desktop이 그 파이썬으로 mcp_server.py를 띄우면 "No module named 'mcp'"로 죽는다. mcp_server.py가 실제로 가져오는 것을, 폴더가 없는 곳에서 가져와 본다.
        let mut probe = std::process::Command::new(path);
        probe.args(["-c", "from mcp.server.fastmcp import FastMCP"]).current_dir(std::env::temp_dir());
        #[cfg(windows)]
        probe.creation_flags(0x0800_0000);
        if let Ok(test) = probe.output() {
            if test.status.success() {
                return Ok(path.to_string());
            }
        }
    }
    Err("mcp 패키지가 설치된 Python을 찾을 수 없습니다.\n설치 방법: pip install mcp".to_string())
}

fn write_mcp_into(config_path: &std::path::Path, entry: &serde_json::Value) -> Result<(), String> {
    // "MCP 설정 병합 실패 시 기존 설정 덮어씀" 수정: 기존 설정 파일이 있는데
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
    let mcp_server_path = mcp_server_script();

    if !mcp_server_path.exists() {
        return Err(format!("mcp_server.py를 찾을 수 없습니다:\n{}", mcp_server_path.display()));
    }

    let python_path = find_python_with_mcp()?;
    // mcp_server.py의 접속정보(RUSQL_HOST/PORT/USER/PASS)는 하드코딩 기본값(127.0.0.1:7878,
    // root/root)만 있었고 이 UI가 실제로 연결 중인 서버의 host/port/계정을 전혀 전달하지
    // 않아, 기본값과 다른 연결에서는 MCP 도구가 항상 연결/인증에 실패했음. Claude Desktop
    // 설정의 "env" 필드로 지금 이 세션의 실제 값을 넘겨 mcp_server.py가 그대로 읽어 쓰도록 함
    // (mcp_server.py 쪽은 env가 없으면 기존 하드코딩 기본값으로 폴백 — 하위 호환 유지).
    // RUSQL_APP_PATH는 launch_app 도구가 앱이 안 켜져 있을 때 무엇을 실행할지 알려주는 값 —
    // 지금 이 커맨드를 실행 중인 프로세스 자신의 경로(현재 세션의 실제 설치 위치)를 그대로 준다.
    let app_path = std::env::current_exe().map(|p| p.to_string_lossy().into_owned()).unwrap_or_default();
    let mcp_entry = serde_json::json!({
        "command": python_path,
        "args": ["-u", mcp_server_path.to_string_lossy().as_ref()],
        "env": {
            "RUSQL_HOST": host,
            "RUSQL_PORT": port.to_string(),
            "RUSQL_USER": user,
            "RUSQL_PASS": password,
            "RUSQL_APP_PATH": app_path,
            // 앱과 같은 data 폴더(connections.json · ui_commands.json · app_instances.json) -- 설치본은 MCP 스크립트가 code/ 옆이 아니라 설치 폴더에 있어 알려 줘야 한다
            "RUSQL_DATA_DIR": data_root().to_string_lossy().to_string(),
        },
        // 파괴적이거나(삭제·자격증명 변경) 위험 SQL 확인 우회처럼 사람이 매번 다시 확인해야
        // 하는 도구는 일부러 뺐다 — Claude Desktop 자체 권한 팝업이 마지막 방어선.
        "alwaysAllow": [
            "execute_sql", "list_databases", "list_tables", "get_table_schema",
            "explain_query", "get_indexes", "sample_data",
            "health_report", "list_locks", "get_table_stats", "get_fk_graph",
            "slow_queries", "diff_schema",
            "list_connections", "add_connection",
            "write_to_editor", "new_editor_tab", "close_editor_tab", "switch_editor_tab",
            "list_editor_tabs", "get_editor_tab_content", "execute_in_editor",
            "login", "list_app_instances", "launch_app",
            "start_server", "get_server_status"
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

// ─── 생태계 보기: 저장된 모든 연결의 데이터 폴더를 디스크에서 읽는다 ─────────────────────────
// 엔진을 띄우지 않고(로그인 없이, 읽기만) 파일에서 연결 → 데이터베이스 → 테이블을 알아낸다. 데이터 폴더 = <연결의 dataDir>: 그 아래 `_`로 시작하지 않는 폴더가 데이터베이스,
// 그 안의 `<표>.schema.json`이 테이블(열 · 기본키 · 외래키), `<표>.rdb` 머리 32바이트의 row_count가 행 수(파일에 저장된 행 버전의 수), `indexes.json` · `views.json`이 인덱스 · 뷰,
// `_system/`의 `_users.json` · `_procedures.json` · `_functions.json` · `_triggers.json`이 사용자와 루틴. 아직 한 번도 연결하지 않은 연결은 폴더가 없다(exists: false).
#[derive(serde::Deserialize)]
struct EcoRequest {
    id: String,
    #[serde(rename = "dataDir")]
    data_dir: String,
}

#[derive(serde::Serialize)]
struct EcoTable {
    name: String,
    columns: usize,
    #[serde(rename = "primaryKey")]
    primary_key: Vec<String>,
    #[serde(rename = "foreignKeys")]
    foreign_keys: usize,
    indexes: usize,
    rows: u64,
    exact: bool, // rows is the number of live rows (false: a very big file, rows is the file's row versions)
    bytes: u64,
}

#[derive(serde::Serialize)]
struct EcoDatabase {
    name: String,
    tables: Vec<EcoTable>,
    views: usize,
    bytes: u64,
}

#[derive(serde::Serialize)]
struct EcoScan {
    id: String,
    exists: bool,
    users: usize,
    routines: usize,
    triggers: usize,
    bytes: u64,
    databases: Vec<EcoDatabase>,
}

fn json_len(path: &std::path::Path) -> usize {
    let value: Option<serde_json::Value> = std::fs::read_to_string(path).ok().and_then(|s| serde_json::from_str(&s).ok());
    match value {
        Some(serde_json::Value::Array(a)) => a.len(),
        Some(serde_json::Value::Object(o)) => o.len(),
        _ => 0,
    }
}

fn dir_bytes(path: &std::path::Path, depth: u32) -> u64 {
    let mut total = 0;
    if let Ok(entries) = std::fs::read_dir(path) {
        for entry in entries.flatten() {
            if let Ok(meta) = entry.metadata() {
                if meta.is_dir() {
                    if depth < 4 { total += dir_bytes(&entry.path(), depth + 1); }
                } else {
                    total += meta.len();
                }
            }
        }
    }
    total
}

// ── 행 수: 표 파일이 담은 행 버전 가운데 살아 있는 것 ─────────────────────────────────────────────
// .rdb = 32바이트 머리(0..4 매직 00 'B' 'D' 'R', 6 플래그(1: 압축), 8..12 row_count) + (압축이면 "원래 크기 u32 + LZ4 블록", 아니면 그대로) 본문. 본문은 [길이 u32][행 JSON]의 연속이다.
// 머리의 row_count는 행 *버전*의 수라서 지워진 행과 갱신으로 대체된 옛 행도 센다(VACUUM 전까지) -- 그대로 보여 주면 SELECT COUNT(*)와 다르다. 행은 `"_xmax":"0"`일 때 살아 있다
// (`_xmax`가 없으면 산 행). 압축을 풀어 세되, 파일이 바뀌지 않았으면(수정 시각과 크기가 같으면) 전에 센 값을 쓰고, 32MB가 넘는 파일은 머리의 값을 그대로 쓴다(exact: false).
const LIVE_COUNT_MAX_BYTES: u64 = 32 * 1024 * 1024;
const LIVE_COUNT_MAX_RAW: usize = 256 * 1024 * 1024;

// LZ4 블록(압축 형식은 토큰 · 글자 · 되돌아갈 거리 · 길이)을 푼다. 크기가 안 맞거나 깨졌으면 None.
fn lz4_block(src: &[u8], size: usize) -> Option<Vec<u8>> {
    let mut out: Vec<u8> = Vec::with_capacity(size);
    let mut i = 0usize;
    while i < src.len() {
        let token = src[i];
        i += 1;
        let mut literals = (token >> 4) as usize;
        if literals == 15 {
            loop {
                let b = *src.get(i)?;
                i += 1;
                literals += b as usize;
                if b != 255 { break; }
            }
        }
        out.extend_from_slice(src.get(i..i.checked_add(literals)?)?);
        i += literals;
        if i >= src.len() { break; } // 마지막 묶음은 글자뿐이다
        let offset = (*src.get(i)? as usize) | ((*src.get(i + 1)? as usize) << 8);
        i += 2;
        if offset == 0 || offset > out.len() { return None; }
        let mut length = (token & 15) as usize;
        if length == 15 {
            loop {
                let b = *src.get(i)?;
                i += 1;
                length += b as usize;
                if b != 255 { break; }
            }
        }
        length += 4;
        if out.len() + length > size { return None; }
        let start = out.len() - offset;
        for k in 0..length {
            let b = out[start + k]; // (거리가 길이보다 짧으면 방금 쓴 글자를 다시 베낀다)
            out.push(b);
        }
    }
    if out.len() == size { Some(out) } else { None }
}

fn is_live_row(row: &[u8]) -> bool {
    const KEY: &[u8] = b"\"_xmax\":\"";
    match row.windows(KEY.len()).position(|w| w == KEY) {
        Some(p) => row.get(p + KEY.len()) == Some(&b'0') && row.get(p + KEY.len() + 1) == Some(&b'"'),
        None => true,
    }
}

fn count_live_rows(raw: &[u8], row_count: u64) -> u64 {
    let (mut pos, mut live) = (0usize, 0u64);
    for _ in 0..row_count {
        if pos + 4 > raw.len() { break; }
        let len = u32::from_le_bytes([raw[pos], raw[pos + 1], raw[pos + 2], raw[pos + 3]]) as usize;
        pos += 4;
        if pos + len > raw.len() { break; }
        if is_live_row(&raw[pos..pos + len]) { live += 1; }
        pos += len;
    }
    live
}

type LiveCache = std::sync::Mutex<std::collections::HashMap<std::path::PathBuf, (std::time::SystemTime, u64, u64)>>;
fn live_cache() -> &'static LiveCache {
    static CACHE: std::sync::OnceLock<LiveCache> = std::sync::OnceLock::new();
    CACHE.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

// (행 수, 정확한가): 읽을 수 없는 파일은 (0, false)
fn rdb_rows(path: &std::path::Path) -> (u64, bool) {
    use std::io::Read;
    let Ok(meta) = std::fs::metadata(path) else { return (0, false) };
    let (len, modified) = (meta.len(), meta.modified().unwrap_or(std::time::UNIX_EPOCH));
    if let Some(&(m, l, live)) = live_cache().lock().unwrap_or_else(|e| e.into_inner()).get(path) {
        if m == modified && l == len { return (live, true); }
    }
    let Ok(mut file) = std::fs::File::open(path) else { return (0, false) };
    let mut head = [0u8; 32];
    if file.read_exact(&mut head).is_err() || &head[1..4] != b"BDR" { return (0, false); }
    let versions = u32::from_le_bytes([head[8], head[9], head[10], head[11]]) as u64;
    if len > LIVE_COUNT_MAX_BYTES { return (versions, false); }
    let mut body = Vec::with_capacity(len as usize);
    if file.read_to_end(&mut body).is_err() { return (versions, false); }
    let raw = if head[6] & 1 != 0 {
        if body.len() < 4 { return (versions, false); }
        let size = u32::from_le_bytes([body[0], body[1], body[2], body[3]]) as usize;
        if size > LIVE_COUNT_MAX_RAW { return (versions, false); }
        match lz4_block(&body[4..], size) {
            Some(raw) => raw,
            None => return (versions, false),
        }
    } else {
        body
    };
    let live = count_live_rows(&raw, versions);
    live_cache().lock().unwrap_or_else(|e| e.into_inner()).insert(path.to_path_buf(), (modified, len, live));
    (live, true)
}

fn scan_database(dir: &std::path::Path, name: String) -> EcoDatabase {
    let index_tables: Vec<String> = std::fs::read_to_string(dir.join("indexes.json")).ok()
        .and_then(|s| serde_json::from_str::<serde_json::Value>(&s).ok())
        .and_then(|v| v.as_array().map(|a| a.iter().filter_map(|i| i.get("table").and_then(|t| t.as_str()).map(|t| t.to_lowercase())).collect()))
        .unwrap_or_default();
    let mut tables = Vec::new();
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let file = entry.file_name().to_string_lossy().to_string();
            let Some(table) = file.strip_suffix(".schema.json") else { continue };
            let schema: serde_json::Value = match std::fs::read_to_string(entry.path()).ok().and_then(|s| serde_json::from_str(&s).ok()) {
                Some(v) => v,
                None => continue,
            };
            let columns = schema.get("columns").and_then(|c| c.as_array()).cloned().unwrap_or_default();
            let primary_key = columns.iter()
                .filter(|c| c.get("primary_key").and_then(|p| p.as_bool()).unwrap_or(false))
                .filter_map(|c| c.get("name").and_then(|n| n.as_str()).map(|n| n.to_string()))
                .collect();
            let foreign_keys = columns.iter().filter(|c| c.get("foreign_key").map(|f| !f.is_null()).unwrap_or(false)).count();
            let suffix = format!(".{}", table.to_lowercase());
            let indexes = index_tables.iter().filter(|t| t.ends_with(&suffix) || **t == table.to_lowercase()).count();
            let rdb = dir.join(format!("{}.rdb", table));
            let bytes = rdb.metadata().map(|m| m.len()).unwrap_or(0) + entry.metadata().map(|m| m.len()).unwrap_or(0);
            let (rows, exact) = rdb_rows(&rdb);
            tables.push(EcoTable { name: table.to_string(), columns: columns.len(), primary_key, foreign_keys, indexes, rows, exact, bytes });
        }
    }
    tables.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    EcoDatabase { name, tables, views: json_len(&dir.join("views.json")), bytes: dir_bytes(dir, 3) }
}

#[tauri::command(async)] // (동기 명령은 메인 스레드에서 돈다: 폴더가 크면 몇 초마다 창이 멈춘다 -- async는 스레드 풀에서 돈다)
fn scan_ecosystem(requests: Vec<EcoRequest>, flush: bool, state: State<AppState>) -> Vec<EcoScan> {
    // 엔진은 표 파일을 체크포인트 때만(redo 로그가 몇 MB 쌓였을 때) 다시 쓴다: 방금 한 일은 파일에 아직 없다. 로그아웃해도 마지막 연결의 엔진 프로세스는 살아 있으므로,
    // 창을 열 때와 새로고침을 누를 때(flush) 그 엔진이 이 연결 것이면 CHECKPOINT를 한 번 시켜 파일을 최신으로 만든다(바뀐 표만 쓰므로 싸다). 4초마다의 읽기는 시키지 않는다.
    if flush {
        let mut guard = state.db.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(conn) = guard.as_mut() {
            let same = |a: &str, b: &str| a == b || matches!((std::fs::canonicalize(a), std::fs::canonicalize(b)), (Ok(x), Ok(y)) if x == y);
            if requests.iter().any(|r| same(&r.data_dir, &conn.data_dir)) {
                let _ = send_one(conn, "CHECKPOINT;");
            }
        }
    }
    requests.into_iter().map(|req| {
        let root = std::path::PathBuf::from(&req.data_dir);
        if req.data_dir.is_empty() || !root.is_dir() {
            return EcoScan { id: req.id, exists: false, users: 0, routines: 0, triggers: 0, bytes: 0, databases: Vec::new() };
        }
        let system = root.join("_system");
        let mut databases = Vec::new();
        if let Ok(entries) = std::fs::read_dir(&root) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if name.starts_with('_') || !entry.path().is_dir() { continue; }
                databases.push(scan_database(&entry.path(), name));
            }
        }
        databases.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
        EcoScan {
            id: req.id,
            exists: true,
            users: json_len(&system.join("_users.json")),
            routines: json_len(&system.join("_procedures.json")) + json_len(&system.join("_functions.json")),
            triggers: json_len(&system.join("_triggers.json")),
            bytes: dir_bytes(&root, 0),
            databases,
        }
    }).collect()
}

#[tauri::command]
fn open_terminal() {
    let mut frontend_dir = code_dir().join("frontend");
    if !frontend_dir.is_dir() { // 설치본에는 소스 트리가 없다: 실행 파일 폴더에서 연다
        frontend_dir = exe_dir().unwrap_or_else(|| std::path::PathBuf::from("."));
    }
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

// ─── MCP용 UI 상태 파일 (읽기 전용 조회) ────────────────────────
// UiStore(state.ui)는 프런트가 sync_* 커맨드로 계속 채워왔지만, 예전엔 이걸 읽어가는
// 쪽(Phase 17에서 제거된 get_tab_content 등 9개 MCP 도구)이 전부 가짜였어서 사실상 아무도
// 안 읽는 죽은 상태였다. MCP 서버(별도 프로세스)는 이 프로세스의 메모리에 접근할 방법이
// 없으므로, sync_* 호출 시마다 code/data/ui_state.json에도 그대로 반영해 MCP가 직접
// 읽을 수 있게 한다(Connections와 동일한 "공유 파일" 다리 패턴).
#[derive(serde::Serialize)]
struct UiStateSnapshot {
    tabs: Vec<String>,
    #[serde(rename = "tabContent")]
    tab_content: HashMap<String, String>,
    #[serde(rename = "lastResult")]
    last_result: String,
    #[serde(rename = "currentDb")]
    current_db: String,
}

fn ui_state_file_path() -> std::path::PathBuf {
    code_dir().join("data").join("ui_state.json")
}

fn persist_ui_state(ui: &UiStore) {
    let snapshot = UiStateSnapshot {
        tabs: ui.tab_list.lock().unwrap_or_else(|e| e.into_inner()).clone(),
        tab_content: ui.tab_content.lock().unwrap_or_else(|e| e.into_inner()).clone(),
        last_result: ui.last_result.lock().unwrap_or_else(|e| e.into_inner()).clone(),
        current_db: ui.current_db.lock().unwrap_or_else(|e| e.into_inner()).clone(),
    };
    let path = ui_state_file_path();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if let Ok(json) = serde_json::to_string_pretty(&snapshot) {
        let tmp = path.with_extension("json.tmp");
        if std::fs::write(&tmp, json).is_ok() {
            let _ = std::fs::rename(&tmp, &path);
        }
    }
}

#[tauri::command]
fn sync_tab_content(name: String, content: String, state: State<AppState>) {
    state.ui.tab_content.lock().unwrap_or_else(|e| e.into_inner()).insert(name, content);
    persist_ui_state(&state.ui);
}

#[tauri::command]
fn sync_tab_list(names: Vec<String>, state: State<AppState>) {
    *state.ui.tab_list.lock().unwrap_or_else(|e| e.into_inner()) = names;
    persist_ui_state(&state.ui);
}

#[tauri::command]
fn sync_query_result(result: String, state: State<AppState>) {
    *state.ui.last_result.lock().unwrap_or_else(|e| e.into_inner()) = result;
    persist_ui_state(&state.ui);
}

#[tauri::command]
fn sync_current_db(db: String, state: State<AppState>) {
    *state.ui.current_db.lock().unwrap_or_else(|e| e.into_inner()) = db;
    persist_ui_state(&state.ui);
}

#[tauri::command]
fn sync_login_state(logged_in: bool, state: State<AppState>) {
    *state.ui.logged_in.lock().unwrap_or_else(|e| e.into_inner()) = logged_in;
}

// ─── MCP용 UI 명령 큐 (에디터/탭 조작, 쓰기 방향) ────────────────
// 읽기(UiStateSnapshot)와 반대 방향 — MCP가 "이 탭에 이 SQL 써줘" 같은 명령을 파일에
// 추가하면, 프런트가 실행 중인 동안 주기적으로(로그인 후 1초 간격) 이 파일을 폴링해
// 실제 화면에 반영하고 결과를 같은 항목에 채워 돌려준다. Phase 17에서 제거됐던
// UI 제어형 도구들과 달리, 이번엔 실제로 응답하는 쪽(이 폴링 루프)이 존재한다.
#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct UiCommand {
    id: String,
    action: String,
    #[serde(default)]
    params: serde_json::Value,
    status: String,
    #[serde(default)]
    result: Option<String>,
    // 비어있으면("") 아무 인스턴스나 처리 가능(기존 동작과 동일, 하위 호환).
    // 특정 인스턴스 id가 있으면 그 인스턴스만 pending -> in_progress로 집어갈 수 있음
    // (RuSQL 창을 여러 개 띄워 서로 다른 DB에 접속해 쓰는 경우, MCP가 list_app_instances로
    // 확인한 특정 인스턴스를 지정해 명령을 보낼 수 있게).
    #[serde(default)]
    target_instance: String,
}

fn ui_commands_file_path() -> std::path::PathBuf {
    data_root().join("ui_commands.json")
}

// ui_commands.json에 대한 파일 기반 상호 배제 락. 이 파일은 이 배경 스레드/
// complete_ui_command와 별도 Python(MCP) 프로세스 양쪽이 "읽고-고치고-통째로
// 다시 쓰기" 하므로, 락 없이 두 쓰기가 겹치면 나중에 쓰는 쪽이 상대가 방금 큐에
// 넣은 항목을 못 본 채 자기 스냅샷으로 덮어써 그 항목이 사라질 수 있다(Python
// 쪽 동시 도구 호출 2개로 실제 재현됨). mcp_server.py의 _UiCommandsLock과 동일한
// 파일명 규칙(O_CREAT|O_EXCL 방식의 락 파일)을 공유해 서로를 잠근다.
fn ui_lock_path() -> std::path::PathBuf {
    ui_commands_file_path().with_extension("json.lock")
}

struct UiCommandsLock;

impl UiCommandsLock {
    fn acquire() -> Self {
        let path = ui_lock_path();
        let mut deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        loop {
            match std::fs::OpenOptions::new().write(true).create_new(true).open(&path) {
                Ok(_) => return UiCommandsLock,
                Err(_) => {
                    if std::time::Instant::now() > deadline {
                        let _ = std::fs::remove_file(&path);
                        deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
                    }
                    std::thread::sleep(std::time::Duration::from_millis(10));
                }
            }
        }
    }
}

impl Drop for UiCommandsLock {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(ui_lock_path());
    }
}

// ─── 다중 앱 인스턴스 레지스트리 ──────────────────────────────────
// RuSQL 창을 여러 개 띄워 서로 다른 DB에 동시 접속해 쓰는 경우, MCP가 "어느 창에
// 명령을 보낼지" 고를 수 있도록 각 인스턴스가 자기 존재를 code/data/app_instances.json에
// 주기적으로(배경 스레드 하트비트, ~2초 간격) 기록한다. 하트비트가 오래된(15초 이상)
// 항목은 다음 쓰기 때 정리 — 창이 비정상 종료돼도 레지스트리가 죽은 항목으로 안 남게.
#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct AppInstance {
    id: String,
    pid: u32,
    #[serde(rename = "loggedIn")]
    logged_in: bool,
    #[serde(rename = "currentDb")]
    current_db: String,
    #[serde(rename = "lastHeartbeat")]
    last_heartbeat: u64,
    // 이 창이 TCP 서버를 켜 둔 연결의 id와 포트(서버가 꺼져 있으면 비어 있음/0) -- 다른 창의 홈 화면이 초록 점을 켜는 데 쓴다. 옛 항목에는 없어서 기본값을 둔다.
    #[serde(rename = "serverConnId", default)]
    server_conn_id: String,
    #[serde(rename = "serverPort", default)]
    server_port: u16,
}

// 하트비트가 이 시간(초) 넘게 없는 인스턴스는 죽은 창으로 본다
const INSTANCE_STALE_SECS: u64 = 15;

fn app_instances_file_path() -> std::path::PathBuf {
    data_root().join("app_instances.json")
}

fn instances_lock_path() -> std::path::PathBuf {
    app_instances_file_path().with_extension("json.lock")
}

struct InstancesLock;

impl InstancesLock {
    fn acquire() -> Self {
        let path = instances_lock_path();
        let mut deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        loop {
            match std::fs::OpenOptions::new().write(true).create_new(true).open(&path) {
                Ok(_) => return InstancesLock,
                Err(_) => {
                    if std::time::Instant::now() > deadline {
                        let _ = std::fs::remove_file(&path);
                        deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
                    }
                    std::thread::sleep(std::time::Duration::from_millis(10));
                }
            }
        }
    }
}

impl Drop for InstancesLock {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(instances_lock_path());
    }
}

fn read_app_instances() -> Vec<AppInstance> {
    std::fs::read_to_string(app_instances_file_path())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

fn write_app_instances(instances: &[AppInstance]) {
    let path = app_instances_file_path();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let Ok(json) = serde_json::to_string_pretty(instances) else { return };
    let tmp = path.with_extension(format!(
        "json.tmp.{:?}.{}",
        std::thread::current().id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
    ));
    if std::fs::write(&tmp, json).is_ok() {
        let _ = std::fs::rename(&tmp, &path);
    }
}

fn unix_now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

// 이 인스턴스의 하트비트를 기록하고, 15초 넘게 소식 없는(창이 비정상 종료된) 다른
// 인스턴스는 정리한다.
fn heartbeat_instance(instance_id: &str, ui: &UiStore, server: Option<(String, u16)>) {
    let _lock = InstancesLock::acquire();
    let now = unix_now();
    let mut instances = read_app_instances();
    instances.retain(|i| i.id != instance_id && now.saturating_sub(i.last_heartbeat) < INSTANCE_STALE_SECS);
    let (server_conn_id, server_port) = server.unwrap_or_default();
    instances.push(AppInstance {
        id: instance_id.to_string(),
        pid: std::process::id(),
        logged_in: *ui.logged_in.lock().unwrap_or_else(|e| e.into_inner()),
        current_db: ui.current_db.lock().unwrap_or_else(|e| e.into_inner()).clone(),
        last_heartbeat: now,
        server_conn_id,
        server_port,
    });
    write_app_instances(&instances);
}

// 이 창이 지금 TCP 서버를 켜 둔 연결: `start_server`가 적어 둔 것 중, 그 엔진 프로세스가 아직 살아 있는 것. (엔진이 질의를 돌리느라 잠금이 잡혀 있으면 살아 있는 것이다.)
fn own_server(state: &AppState) -> Option<(String, u16)> {
    fn alive(engine: &mut Option<EngineConn>) -> bool {
        engine.as_mut().map_or(false, |c| matches!(c.child.try_wait(), Ok(None)))
    }
    let engine_alive = match state.db.try_lock() {
        Ok(mut g) => alive(&mut *g),
        Err(std::sync::TryLockError::Poisoned(e)) => alive(&mut *e.into_inner()),
        Err(std::sync::TryLockError::WouldBlock) => true,
    };
    if !engine_alive { return None; }
    state.servers.lock().unwrap_or_else(|e| e.into_inner()).iter().find(|(_, s)| s.running).map(|(id, s)| (id.clone(), s.port))
}

// TCP 서버가 켜져 있는 연결들(연결 id -> 포트): 이 창의 것(`own`)과, 살아 있는 다른 창들이 app_instances.json에 적어 둔 것.
fn running_servers(own: Option<(String, u16)>, instances: &[AppInstance], own_id: &str, now: u64) -> HashMap<String, u16> {
    let mut out = HashMap::new();
    for i in instances {
        if i.id != own_id && !i.server_conn_id.is_empty() && now.saturating_sub(i.last_heartbeat) < INSTANCE_STALE_SECS {
            out.insert(i.server_conn_id.clone(), i.server_port);
        }
    }
    if let Some((id, port)) = own { out.insert(id, port); }
    out
}

// 홈 화면의 초록/빨간 점이 쓴다: 목록에 있으면 그 연결의 TCP 서버가 켜져 있다.
#[tauri::command]
fn get_running_servers(state: State<AppState>) -> HashMap<String, u16> {
    running_servers(own_server(&state), &read_app_instances(), &state.instance_id, unix_now())
}

fn read_ui_commands() -> Vec<UiCommand> {
    std::fs::read_to_string(ui_commands_file_path())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

fn write_ui_commands(cmds: &[UiCommand]) -> Result<(), String> {
    let path = ui_commands_file_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let json = serde_json::to_string_pretty(cmds).map_err(|e| e.to_string())?;
    // 이 파일은 Rust 배경 스레드와 별도 Python(MCP) 프로세스가 둘 다 자주 쓴다.
    // 고정된 이름의 임시 파일을 공유하면 두 쓰기가 겹칠 때 한쪽의 rename이
    // 상대가 이미 없애버린 파일을 찾다 FileNotFoundError 나는 경합이 생기므로,
    // 쓰기마다 고유한 임시 파일명을 사용한다.
    let tmp = path.with_extension(format!(
        "json.tmp.{:?}.{}",
        std::thread::current().id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
    ));
    std::fs::write(&tmp, json).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())?;
    Ok(())
}

// UiCommand.params -> "ui-command" 이벤트의 data 필드로 변환.
// 문자열이면 그대로(탭 이름처럼 프런트가 JSON.parse 없이 바로 쓰는 경우), 그 외(객체 등)는
// JSON 텍스트로 직렬화해 프런트 쪽에서 JSON.parse(data)로 풀어 쓰게 한다.
fn ui_command_data_string(v: &serde_json::Value) -> String {
    match v {
        serde_json::Value::String(s) => s.clone(),
        serde_json::Value::Null => String::new(),
        other => other.to_string(),
    }
}

#[tauri::command]
fn complete_ui_command(id: String, result: String) -> Result<(), String> {
    let _lock = UiCommandsLock::acquire();
    let mut cmds = read_ui_commands();
    if let Some(c) = cmds.iter_mut().find(|c| c.id == id) {
        c.status = "done".to_string();
        c.result = Some(result);
    }
    write_ui_commands(&cmds)
}

#[tauri::command]
fn open_bench_graph() {
    let path = bench_dir().join("benchmark_result.png");
    let _ = std::process::Command::new("cmd")
        .args(["/c", "start", "", &path.to_string_lossy()])
        .spawn();
}


// 이 프로세스만의 인스턴스 id — RuSQL 창을 여러 개 띄웠을 때 MCP가 서로 구분해
// 타깃팅할 수 있게. UUID 크레이트를 새로 추가하는 대신 pid+시각으로 충분히 유일하게 생성.
fn generate_instance_id() -> String {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    format!("{:x}-{:x}", std::process::id(), nanos)
}

// ─── 엔트리포인트 ─────────────────────────────────────────────
fn main() {
    // data 폴더가 없으면 UiCommandsLock::acquire()가 락 파일을 못 만들어 영원히 도므로(설치본의 첫 실행) 먼저 만든다
    let _ = std::fs::create_dir_all(data_root());
    tauri::Builder::default()
        .manage(AppState {
            db: Arc::new(Mutex::new(None)),
            servers: Mutex::new(HashMap::new()),
            ui: Arc::new(UiStore {
                tab_content: Arc::new(Mutex::new(HashMap::new())),
                tab_list:    Arc::new(Mutex::new(Vec::new())),
                last_result: Arc::new(Mutex::new(String::new())),
                current_db:  Arc::new(Mutex::new(String::new())),
                logged_in:   Arc::new(Mutex::new(false)),
            }),
            instance_id: generate_instance_id(),
        })
        .setup(|app| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.set_icon(tauri::include_image!("icons/icon.png"));
            }
            // MCP가 code/data/ui_commands.json에 넣어둔 "pending" 명령을 폴링해
            // 프런트로 "ui-command" 이벤트를 emit. 프런트는 처리 후 complete_ui_command로
            // 결과를 같은 파일에 채워 되돌려준다(쓰기는 프런트, 큐 관리는 여기).
            // 같은 스레드에서 ~2초(400ms*5)마다 이 인스턴스의 하트비트도 함께 기록한다.
            let app_handle = app.handle().clone();
            let state = app.state::<AppState>();
            let instance_id = state.instance_id.clone();
            let ui_for_thread = state.ui.clone();
            std::thread::spawn(move || {
                let mut tick: u32 = 0;
                loop {
                std::thread::sleep(std::time::Duration::from_millis(400));
                tick = tick.wrapping_add(1);
                if tick % 5 == 0 {
                    heartbeat_instance(&instance_id, &ui_for_thread, own_server(&app_handle.state::<AppState>()));
                }
                let _lock = UiCommandsLock::acquire();
                let mut cmds = read_ui_commands();
                let mut changed = false;
                for c in cmds.iter_mut() {
                    if c.status == "pending" && (c.target_instance.is_empty() || c.target_instance == instance_id) {
                        c.status = "in_progress".to_string();
                        changed = true;
                        let data = ui_command_data_string(&c.params);
                        let _ = app_handle.emit(
                            "ui-command",
                            serde_json::json!({ "id": c.id, "action": c.action, "data": data }),
                        );
                    }
                }
                if changed {
                    let _ = write_ui_commands(&cmds);
                }
                drop(_lock);
                }
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            scan_ecosystem,
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
            get_running_servers,
            get_activity,
            get_status_counters,
            clear_server_log,
            import_csv,
            open_terminal,
            open_url,
            get_app_data_dir,
            get_connections,
            save_connections,
            complete_ui_command,
            set_parallel_query,
            read_bench_result,
            open_bench_terminal,
            open_bench_graph,
            sync_tab_content,
            sync_tab_list,
            sync_query_result,
            sync_current_db,
            sync_login_state,
            setup_mcp_config,
        ])
        .build(tauri::generate_context!())
        .expect("error while building tauri application")
        .run(|app, event| {
            // 창을 닫으면 프로세스가 바로 끝나 AppState의 Drop이 돌지 않는다. 그러면 엔진 프로세스가 남아 engine_server.exe가 잠긴 채로
            // 다음 설치가 "Error opening file for writing"으로 실패하므로, 끝나기 직전에 엔진을 내려받아 종료한다.
            if let tauri::RunEvent::Exit = event {
                let engine = app.state::<AppState>().db.lock().unwrap_or_else(|e| e.into_inner()).take();
                drop(engine); // EngineConn의 Drop이 엔진 프로세스를 종료한다
            }
        });
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

    // ── 홈 화면의 서버 점: 어느 연결의 TCP 서버가 켜져 있나 ─────────────────────────────────
    fn instance(id: &str, beat: u64, conn: &str, port: u16) -> AppInstance {
        AppInstance { id: id.into(), pid: 1, logged_in: true, current_db: String::new(), last_heartbeat: beat, server_conn_id: conn.into(), server_port: port }
    }

    #[test]
    fn running_servers_lists_this_window_and_the_live_other_windows() {
        let now = 1_000;
        let all = [
            instance("me", now, "old-entry-of-this-window", 1), // this window is told by `own`, not by its own old file entry
            instance("b", now - 3, "conn-b", 7879),             // another window that is alive
            instance("dead", now - 60, "conn-dead", 7880),      // a window that stopped beating
            instance("quiet", now, "", 0),                      // a window with no server on
        ];
        let got = running_servers(Some(("conn-a".into(), 7878)), &all, "me", now);
        assert_eq!(got.len(), 2, "{:?}", got);
        assert_eq!(got["conn-a"], 7878);
        assert_eq!(got["conn-b"], 7879);
        // nothing on in this window: only the other window's server is listed
        let got = running_servers(None, &all, "me", now);
        assert_eq!(got.keys().cloned().collect::<Vec<_>>(), vec!["conn-b".to_string()]);
    }

    #[test]
    fn instances_written_before_the_server_fields_still_load() {
        let old = r#"[{"id":"x","pid":5,"loggedIn":true,"currentDb":"shop","lastHeartbeat":10}]"#;
        let list: Vec<AppInstance> = serde_json::from_str(old).unwrap();
        assert_eq!((list[0].server_conn_id.as_str(), list[0].server_port), ("", 0));
    }

    // ── 생태계 창의 행 수 (R94) ──────────────────────────────────────────────────────────
    #[test]
    fn lz4_block_decodes_literals_matches_and_long_lengths() {
        // "abc", then a match of 9 bytes at distance 3, then the literal "Z"
        assert_eq!(lz4_block(&[0x35, b'a', b'b', b'c', 3, 0, 0x10, b'Z'], 13).unwrap(), b"abcabcabcabcZ");
        // 20 literals: token 0xF0 and the extra length byte 5 (15 + 5)
        let mut long = vec![0xF0u8, 5];
        long.extend_from_slice(&[7u8; 20]);
        assert_eq!(lz4_block(&long, 20).unwrap(), vec![7u8; 20]);
        // a match that is longer than its distance copies what it has just written: "a", 8 more "a", "!"
        assert_eq!(lz4_block(&[0x14, b'a', 1, 0, 0x10, b'!'], 10).unwrap(), b"aaaaaaaaa!");
        // a wrong size, a distance of 0, a distance beyond what is written and a cut block are refused
        assert!(lz4_block(&[0x35, b'a', b'b', b'c', 3, 0, 0x10, b'Z'], 12).is_none());
        assert!(lz4_block(&[0x14, b'a', 0, 0, 0x10, b'!'], 10).is_none());
        assert!(lz4_block(&[0x14, b'a', 2, 0, 0x10, b'!'], 10).is_none());
        assert!(lz4_block(&[0x35, b'a', b'b'], 13).is_none());
    }

    fn rows_blob(rows: &[&str]) -> Vec<u8> {
        let mut raw = Vec::new();
        for r in rows {
            raw.extend_from_slice(&(r.len() as u32).to_le_bytes());
            raw.extend_from_slice(r.as_bytes());
        }
        raw
    }

    #[test]
    fn live_rows_are_the_rows_whose_xmax_is_zero() {
        let raw = rows_blob(&[
            r#"{"_xmax":"0","_xmin":"1","id":1}"#,
            r#"{"_xmax":"7","_xmin":"1","id":2}"#,
            r#"{"id":3}"#,
            r#"{"_xmax":"0","note":"a \"_xmax\":\"9\" b"}"#,
        ]);
        assert_eq!(count_live_rows(&raw, 4), 3, "rows 1, 3 (no _xmax) and 4 (the text in a string is escaped) are alive, row 2 was deleted");
        assert_eq!(count_live_rows(&raw, 2), 1, "only the first row_count rows are read");
        assert_eq!(count_live_rows(&raw, 9), 3, "a row_count past the end of the data stops at the end");
    }

    #[test]
    fn rdb_rows_counts_live_rows_of_a_plain_and_a_compressed_file() {
        let dir = std::env::temp_dir().join(format!("rusql_eco_rows_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let raw = rows_blob(&[r#"{"_xmax":"0","id":1}"#, r#"{"_xmax":"3","id":2}"#, r#"{"_xmax":"0","id":3}"#]);
        let header = |flags: u8| {
            let mut h = vec![0u8, b'B', b'D', b'R', 1, 0, flags, 0];
            h.extend_from_slice(&3u32.to_le_bytes());
            h.resize(32, 0);
            h
        };
        // plain
        let plain = dir.join("plain.rdb");
        let mut file = header(0);
        file.extend_from_slice(&raw);
        std::fs::write(&plain, &file).unwrap();
        assert_eq!(rdb_rows(&plain), (2, true));
        // compressed: size, then a block that is all literals (token with the length in the high nibble, extra bytes of 255 and a rest)
        let compressed = dir.join("compressed.rdb");
        let mut block = vec![0xF0u8];
        let mut rest = raw.len() - 15;
        while rest >= 255 { block.push(255); rest -= 255; }
        block.push(rest as u8);
        block.extend_from_slice(&raw);
        let mut file = header(1);
        file.extend_from_slice(&(raw.len() as u32).to_le_bytes());
        file.extend_from_slice(&block);
        std::fs::write(&compressed, &file).unwrap();
        assert_eq!(rdb_rows(&compressed), (2, true));
        assert_eq!(rdb_rows(&compressed), (2, true), "the second time the cached count is used");
        // not a table file, a missing file
        std::fs::write(dir.join("junk.rdb"), b"hello").unwrap();
        assert_eq!(rdb_rows(&dir.join("junk.rdb")), (0, false));
        assert_eq!(rdb_rows(&dir.join("missing.rdb")), (0, false));
        std::fs::remove_dir_all(&dir).ok();
    }
}
