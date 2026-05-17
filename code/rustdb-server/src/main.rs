mod mysql;

use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, RwLock};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::{Executor, SharedDatabase};

// ─── 타임스탬프 ──────────────────────────────────────────────
fn timestamp() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    format!("{:02}:{:02}:{:02}", (secs % 86400) / 3600, (secs % 3600) / 60, secs % 60)
}

fn log(msg: &str) {
    println!("[{}] {}", timestamp(), msg);
}

// ─── 주석·BEGIN..END 인식 쿼리 분리 ──────────────────────────
fn split_queries_smart(input: &str) -> Vec<String> {
    split_proc_aware(input)
}

/// BEGIN...END 블록 안의 ';'를 분리하지 않는 스마트 분리기
fn split_proc_aware(input: &str) -> Vec<String> {
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
                "BEGIN" => { begin_depth += 1; }
                "END" => {
                    // END IF / END WHILE / END LOOP / END REPEAT / END CASE → 안쪽 블록 종료
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

// ─── 내장 명령 처리 ─────────────────────────────────────────
fn handle_builtin(cmd: &str, client_count: usize, uptime: &Instant) -> Option<String> {
    match cmd.to_lowercase().trim() {
        "\\help" | "help" => Some(
            "RustDB Server v2.2.0\n\
             Commands:\n\
               \\help           — 이 도움말\n\
               \\status         — 서버 상태\n\
               exit | quit     — 연결 종료\n\
             SQL:\n\
               SHOW TABLES;    — 테이블 목록\n\
               DESCRIBE <t>;   — 테이블 구조\n\
               SHOW BUFFER POOL; SHOW WAL; SHOW LOCKS;".to_string()
        ),
        "\\status" => {
            let elapsed = uptime.elapsed().as_secs();
            Some(format!(
                "Status: RUNNING\nUptime: {}h {}m {}s\nConnections: {}",
                elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60, client_count,
            ))
        }
        _ => None,
    }
}

// ─── 응답 전송 헬퍼 ─────────────────────────────────────────
fn send(w: &mut TcpStream, status: &str, body: &str, elapsed: f64) {
    let _ = writeln!(w, "{}", status);
    let _ = writeln!(w, "{}", body);
    let _ = writeln!(w, "({:.3} sec)", elapsed);
    let _ = writeln!(w, "---END---");
    let _ = w.flush();
}

// ─── 클라이언트 핸들러 ───────────────────────────────────────
fn handle_client(
    stream: TcpStream,
    shared: Arc<RwLock<SharedDatabase>>,
    client_count: Arc<AtomicUsize>,
    server_start: Arc<Instant>,
) {
    let peer = stream.peer_addr().map(|a| a.to_string()).unwrap_or_else(|_| "unknown".into());
    let count = client_count.fetch_add(1, Ordering::SeqCst) + 1;
    log(&format!("Client connected: {} (total: {})", peer, count));

    let mut writer = match stream.try_clone() {
        Ok(s) => s,
        Err(_) => { client_count.fetch_sub(1, Ordering::SeqCst); return; }
    };
    let reader = BufReader::new(stream);

    // ── 1. 배너 전송 ──
    let _ = writeln!(writer, "+-----------------------------------------+");
    let _ = writeln!(writer, "|   RustDB Server v2.2.0                  |");
    let _ = writeln!(writer, "|   Connected: {}{}|",
        peer, " ".repeat(23usize.saturating_sub(peer.len())));
    let _ = writeln!(writer, "+-----------------------------------------+");
    let _ = writeln!(writer, "---END---");
    let _ = writer.flush();

    // ── 2. AUTH 핸드셰이크 ──
    let mut lines_iter = reader.lines();

    let auth_line = match lines_iter.next() {
        Some(Ok(l)) => l,
        _ => {
            client_count.fetch_sub(1, Ordering::SeqCst);
            return;
        }
    };

    // "AUTH username password" 파싱 (password는 공백 포함 가능)
    let parts: Vec<&str> = auth_line.splitn(3, ' ').collect();
    let cmd       = parts.first().copied().unwrap_or("");
    let auth_user = parts.get(1).copied().unwrap_or("").trim();
    let auth_pass = parts.get(2).copied().unwrap_or("").trim();

    if !cmd.eq_ignore_ascii_case("auth") || auth_user.is_empty() {
        let _ = writeln!(writer, "ERR expected: AUTH <user> <password>");
        let _ = writeln!(writer, "---END---");
        let _ = writer.flush();
        client_count.fetch_sub(1, Ordering::SeqCst);
        return;
    }

    let ok = shared.read().unwrap().validate_credentials(auth_user, auth_pass);
    if !ok {
        log(&format!("[{}] AUTH failed: '{}'", peer, auth_user));
        let _ = writeln!(writer, "ERR Access denied for user '{}'", auth_user);
        let _ = writeln!(writer, "---END---");
        let _ = writer.flush();
        client_count.fetch_sub(1, Ordering::SeqCst);
        return;
    }

    log(&format!("[{}] Authenticated as '{}'", peer, auth_user));
    let _ = writeln!(writer, "OK authenticated as '{}'", auth_user);
    let _ = writeln!(writer, "---END---");
    let _ = writer.flush();

    // ── 3. 쿼리 세션 ──
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

        if trimmed.starts_with('\\') || trimmed.eq_ignore_ascii_case("help") {
            let cnt = client_count.load(Ordering::SeqCst);
            if let Some(resp) = handle_builtin(trimmed, cnt, &server_start) {
                let _ = writeln!(writer, "{}", resp);
                let _ = writeln!(writer, "---END---");
                let _ = writer.flush();
            }
            continue;
        }

        buf.push_str(&input);
        buf.push('\n');

        if !buf.contains(';') { continue; }

        let queries = split_queries_smart(&buf);
        buf.clear();

        if queries.is_empty() {
            let _ = writeln!(writer, "---END---");
            let _ = writer.flush();
            continue;
        }

        for q in &queries {
            let preview = if q.len() > 60 { format!("{}...", &q[..60]) } else { q.clone() };
            log(&format!("[{}@{}] {}", auth_user, peer, preview));

            let t0 = Instant::now();
            let mut p = Parser::new(q.as_str());
            let (status, output) = match p.parse() {
                Ok(stmt) => match exec.execute(stmt) {
                    Ok(r)  => ("OK",  r),
                    Err(e) => ("ERR", e),
                },
                Err(e) => ("ERR", format!("Parse Error: {}", e)),
            };
            send(&mut writer, status, &output, t0.elapsed().as_secs_f64());
        }
    }

    let remaining = client_count.fetch_sub(1, Ordering::SeqCst) - 1;
    log(&format!("Client disconnected: {} (remaining: {})", peer, remaining));
}

// ─── 메인 ────────────────────────────────────────────────────
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let port: u16 = args.windows(2)
        .find(|w| w[0] == "--port")
        .and_then(|w| w[1].parse().ok())
        .or_else(|| args.get(1).and_then(|a| a.parse().ok()))
        .unwrap_or(7878);

    let no_mysql = args.iter().any(|a| a == "--no-mysql");
    let mysql_port: u16 = args.windows(2)
        .find(|w| w[0] == "--mysql-port")
        .and_then(|w| w[1].parse().ok())
        .unwrap_or(3306);
    let buffer_pool_size: usize = args.windows(2)
        .find(|w| w[0] == "--buffer-pool-size")
        .and_then(|w| w[1].parse().ok())
        .unwrap_or(64);

    let addr = format!("127.0.0.1:{}", port);
    let listener = match TcpListener::bind(&addr) {
        Ok(l) => l,
        Err(e) => { eprintln!("Failed to bind {}: {}", addr, e); std::process::exit(1); }
    };
    listener.set_nonblocking(true).ok();

    // WAL 복구 포함 초기화 후 공유 상태 추출
    let shared = Executor::new_with_buffer_pool_size(buffer_pool_size).get_shared();

    // users가 없으면 root/root 자동 생성
    if shared.write().unwrap().ensure_default_user() {
        log("No users found. Created default user 'root'@'%' with password 'root'.");
    }

    let client_count = Arc::new(AtomicUsize::new(0));
    let running      = Arc::new(AtomicBool::new(true));
    let server_start = Arc::new(Instant::now());

    {
        let running = running.clone();
        ctrlc::set_handler(move || {
            println!("\n[!] Ctrl+C received. Shutting down...");
            running.store(false, Ordering::SeqCst);
        }).expect("Error setting Ctrl+C handler");
    }

    println!("+-----------------------------------------+");
    println!("|   RustDB Server v2.2.0                  |");
    println!("|   Native protocol on 127.0.0.1:{:<9}|", port);
    if !no_mysql {
        println!("|   MySQL protocol on 0.0.0.0:{:<12}|", mysql_port);
    }
    println!("|   Buffer pool size: {:<20}|", buffer_pool_size);
    println!("|   Press Ctrl+C to stop                  |");
    println!("+-----------------------------------------+");

    if !no_mysql {
        mysql::start_mysql_listener(mysql_port, Arc::clone(&shared));
    }

    log("Server started.");

    while running.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, _)) => {
                stream.set_nonblocking(false).ok();
                let sh2   = Arc::clone(&shared);
                let cc    = client_count.clone();
                let start = server_start.clone();
                thread::spawn(move || handle_client(stream, sh2, cc, start));
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(std::time::Duration::from_millis(50));
            }
            Err(e) => { eprintln!("Accept error: {}", e); break; }
        }
    }

    log(&format!("Server stopped. Total uptime: {}s", server_start.elapsed().as_secs()));
}
