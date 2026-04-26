use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::Executor;

// ─── 타임스탬프 ──────────────────────────────────────────────
fn timestamp() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let hh = (secs % 86400) / 3600;
    let mm = (secs % 3600) / 60;
    let ss = secs % 60;
    format!("{:02}:{:02}:{:02}", hh, mm, ss)
}

fn log(msg: &str) {
    println!("[{}] {}", timestamp(), msg);
}

// ─── 주석 인식 쿼리 분리 ────────────────────────────────────
// `;` 기준 분리, --, #, /* */ 주석 및 '' 문자열 안의 `;` 무시
fn split_queries_smart(input: &str) -> Vec<String> {
    let chars: Vec<char> = input.chars().collect();
    let len = chars.len();
    let mut queries: Vec<String> = Vec::new();
    let mut current = String::new();
    let mut i = 0;

    while i < len {
        if chars[i] == '-' && i + 1 < len && chars[i + 1] == '-' {
            while i < len && chars[i] != '\n' { i += 1; }
            continue;
        }
        if chars[i] == '#' {
            while i < len && chars[i] != '\n' { i += 1; }
            continue;
        }
        if chars[i] == '/' && i + 1 < len && chars[i + 1] == '*' {
            i += 2;
            while i + 1 < len {
                if chars[i] == '*' && chars[i + 1] == '/' { i += 2; break; }
                i += 1;
            }
            continue;
        }
        if chars[i] == '\'' {
            current.push(chars[i]); i += 1;
            while i < len {
                let c = chars[i]; i += 1;
                current.push(c);
                if c == '\'' { break; }
            }
            continue;
        }
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

// ─── 내장 명령 처리 ─────────────────────────────────────────
fn handle_builtin(cmd: &str, client_count: usize, uptime: &Instant) -> Option<String> {
    match cmd.to_lowercase().trim() {
        "\\help" | "help" => Some(format!(
            "RustDB Server v2.2.0\n\
             Commands:\n\
               \\help           — 이 도움말\n\
               \\status         — 서버 상태\n\
               exit | quit     — 연결 종료\n\
             SQL:\n\
               SHOW TABLES;    — 테이블 목록\n\
               DESCRIBE <t>;   — 테이블 구조\n\
               SHOW BUFFER POOL; SHOW WAL; SHOW LOCKS;"
        )),
        "\\status" => {
            let elapsed = uptime.elapsed().as_secs();
            Some(format!(
                "Status: RUNNING\n\
                 Uptime: {}h {}m {}s\n\
                 Connections: {}",
                elapsed / 3600,
                (elapsed % 3600) / 60,
                elapsed % 60,
                client_count,
            ))
        }
        _ => None,
    }
}

// ─── 클라이언트 핸들러 ───────────────────────────────────────
fn handle_client(
    stream: TcpStream,
    db: Arc<Mutex<Executor>>,
    client_count: Arc<AtomicUsize>,
    server_start: Arc<Instant>,
) {
    let peer = match stream.peer_addr() {
        Ok(a) => a.to_string(),
        Err(_) => "unknown".to_string(),
    };

    let count = client_count.fetch_add(1, Ordering::SeqCst) + 1;
    log(&format!("Client connected: {} (total: {})", peer, count));

    let mut writer = match stream.try_clone() {
        Ok(s) => s,
        Err(_) => {
            client_count.fetch_sub(1, Ordering::SeqCst);
            return;
        }
    };
    let reader = BufReader::new(stream);

    // 환영 메시지
    let _ = writeln!(writer, "+-----------------------------------------+");
    let _ = writeln!(writer, "|   RustDB Server v2.2.0                  |");
    let _ = writeln!(writer, "|   Connected: {}{}|",
        peer, " ".repeat(23usize.saturating_sub(peer.len())));
    let _ = writeln!(writer, "+-----------------------------------------+");
    let _ = writeln!(writer, "Type SQL queries ending with ';'");
    let _ = writeln!(writer, "\\help for commands, exit to quit.");
    let _ = writeln!(writer, "---END---");
    let _ = writer.flush();

    // 멀티라인 쿼리 버퍼
    let mut buf = String::new();

    for line in reader.lines() {
        let input = match line {
            Ok(l) => l,
            Err(_) => break,
        };

        let trimmed = input.trim();

        // exit / quit
        if trimmed.eq_ignore_ascii_case("exit") || trimmed.eq_ignore_ascii_case("quit") {
            let _ = writeln!(writer, "Bye!");
            let _ = writeln!(writer, "---END---");
            break;
        }

        // 내장 명령 (세미콜론 없이 바로 실행)
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

        // 세미콜론이 있으면 실행
        if !buf.contains(';') { continue; }

        let queries = split_queries_smart(&buf);
        buf.clear();

        if queries.is_empty() {
            let _ = writeln!(writer, "---END---");
            let _ = writer.flush();
            continue;
        }

        let mut exec = db.lock().unwrap();
        for q in &queries {
            let preview = if q.len() > 60 { format!("{}...", &q[..60]) } else { q.clone() };
            log(&format!("[{}] Query: {}", peer, preview));

            let q_start = Instant::now();
            let mut p = Parser::new(q.as_str());
            let (status, output) = match p.parse() {
                Ok(stmt) => match exec.execute(stmt) {
                    Ok(r)  => ("OK",  r),
                    Err(e) => ("ERR", e),
                },
                Err(e) => ("ERR", format!("Parse Error: {}", e)),
            };
            let elapsed = q_start.elapsed().as_secs_f64();

            let _ = writeln!(writer, "{}", status);
            let _ = writeln!(writer, "{}", output);
            let _ = writeln!(writer, "({:.3} sec)", elapsed);
            let _ = writeln!(writer, "---END---");
            let _ = writer.flush();
        }
    }

    let remaining = client_count.fetch_sub(1, Ordering::SeqCst) - 1;
    log(&format!("Client disconnected: {} (remaining: {})", peer, remaining));
}

// ─── 메인 ────────────────────────────────────────────────────
fn main() {
    // 포트 인수 파싱 (--port 1234 또는 1234)
    let args: Vec<String> = std::env::args().collect();
    let port: u16 = args.windows(2)
        .find(|w| w[0] == "--port")
        .and_then(|w| w[1].parse().ok())
        .or_else(|| args.get(1).and_then(|a| a.parse().ok()))
        .unwrap_or(7878);

    let addr = format!("127.0.0.1:{}", port);

    let listener = match TcpListener::bind(&addr) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("Failed to bind {}: {}", addr, e);
            std::process::exit(1);
        }
    };

    // non-blocking: Ctrl+C 감지용
    listener.set_nonblocking(true).ok();

    let db            = Arc::new(Mutex::new(Executor::new()));
    let client_count  = Arc::new(AtomicUsize::new(0));
    let running       = Arc::new(AtomicBool::new(true));
    let server_start  = Arc::new(Instant::now());

    // Ctrl+C 핸들러
    {
        let running = running.clone();
        ctrlc::set_handler(move || {
            println!("\n[!] Ctrl+C received. Shutting down...");
            running.store(false, Ordering::SeqCst);
        }).expect("Error setting Ctrl+C handler");
    }

    println!("+-----------------------------------------+");
    println!("|   RustDB Server v2.2.0                  |");
    println!("|   Listening on {:<24}|", addr);
    println!("|   Press Ctrl+C to stop                  |");
    println!("+-----------------------------------------+");
    log("Server started.");

    while running.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, _)) => {
                stream.set_nonblocking(false).ok();
                let db2    = db.clone();
                let cc     = client_count.clone();
                let start  = server_start.clone();
                thread::spawn(move || handle_client(stream, db2, cc, start));
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(std::time::Duration::from_millis(50));
            }
            Err(e) => {
                eprintln!("Accept error: {}", e);
                break;
            }
        }
    }

    log(&format!(
        "Server stopped. Total uptime: {}s",
        server_start.elapsed().as_secs()
    ));
}
