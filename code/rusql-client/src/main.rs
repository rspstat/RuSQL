use std::io::{BufRead, BufReader, Write};
use std::net::TcpStream;

// ANSI 색상 코드
const RESET: &str = "\x1b[0m";
const RED:   &str = "\x1b[31m";
const GREEN: &str = "\x1b[32m";
const CYAN:  &str = "\x1b[36m";
const BOLD:  &str = "\x1b[1m";
const DIM:   &str = "\x1b[2m";

fn get_arg(args: &[String], flag: &str) -> Option<String> {
    args.windows(2).find(|w| w[0] == flag).map(|w| w[1].clone())
}

// 서버 응답을 ---END--- 가 올 때까지 읽어 줄 목록 반환
fn read_response<R: BufRead>(reader: &mut R) -> Vec<String> {
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

// 입력에서 세미콜론 개수 카운트 (주석·문자열 안 제외)
fn count_semicolons(input: &str) -> usize {
    let chars: Vec<char> = input.chars().collect();
    let (mut count, mut i) = (0, 0);
    while i < chars.len() {
        match chars[i] {
            '-' if i + 1 < chars.len() && chars[i + 1] == '-' => {
                while i < chars.len() && chars[i] != '\n' { i += 1; }
            }
            '#' => { while i < chars.len() && chars[i] != '\n' { i += 1; } }
            '\'' => {
                i += 1;
                while i < chars.len() && chars[i] != '\'' { i += 1; }
                if i < chars.len() { i += 1; }
            }
            '/' if i + 1 < chars.len() && chars[i + 1] == '*' => {
                i += 2;
                while i + 1 < chars.len() && !(chars[i] == '*' && chars[i + 1] == '/') { i += 1; }
                if i + 1 < chars.len() { i += 2; }
            }
            ';' => { count += 1; i += 1; }
            _ => { i += 1; }
        }
    }
    count
}

// 서버 쿼리 응답 출력
// 형식: [OK|ERR]\n<출력>\n(x.xxx sec)\n---END---
fn display_response(lines: &[String]) {
    if lines.is_empty() { return; }
    let status = &lines[0];
    let rest   = &lines[1..];

    if status == "ERR" {
        eprint!("{}{}", RED, BOLD);
        for l in rest { eprintln!("{}", l); }
        eprint!("{}", RESET);
    } else {
        for l in rest { println!("{}", l); }
    }
}

fn print_help() {
    println!("{}Commands:{}", BOLD, RESET);
    println!("  SQL query{}; {}     — Execute SQL (end with semicolon)", GREEN, RESET);
    println!("  {}\\status{}         — Show server status", CYAN, RESET);
    println!("  {}exit | quit{}     — Disconnect", DIM, RESET);
    println!("  {}\\help{}           — This help", DIM, RESET);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    if args.iter().any(|a| a == "--help") {
        println!("Usage: rusql-client [-u user] [-p password] [-h host] [-P port]");
        println!("  -u  username  (default: root)");
        println!("  -p  password  (default: root)");
        println!("  -h  host      (default: 127.0.0.1)");
        println!("  -P  port      (default: 7878)");
        return;
    }

    let user = get_arg(&args, "-u").unwrap_or_else(|| "root".to_string());
    let pass = get_arg(&args, "-p").unwrap_or_else(|| "root".to_string());
    let host = get_arg(&args, "-h").unwrap_or_else(|| "127.0.0.1".to_string());
    let port: u16 = get_arg(&args, "-P")
        .and_then(|p| p.parse().ok())
        .unwrap_or(7878);

    let addr = format!("{}:{}", host, port);

    // ── 연결 ──
    let stream = match TcpStream::connect(&addr) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("{}{}Cannot connect to {}: {}{}", RED, BOLD, addr, e, RESET);
            std::process::exit(1);
        }
    };

    let mut writer = stream.try_clone().expect("stream clone failed");
    let mut reader = BufReader::new(stream);

    // ── 배너 수신 ──
    read_response(&mut reader);

    // ── AUTH 송신 ──
    if writeln!(writer, "AUTH {} {}", user, pass).is_err() || writer.flush().is_err() {
        eprintln!("{}Connection lost during auth.{}", RED, RESET);
        std::process::exit(1);
    }

    // ── AUTH 응답 수신 ──
    let auth_resp = read_response(&mut reader);
    let status = auth_resp.first().map(|s| s.as_str()).unwrap_or("");
    if !status.starts_with("OK") {
        let msg = auth_resp.get(1).map(|s| s.as_str()).unwrap_or("authentication failed");
        eprintln!("{}{}ERROR: {}{}", RED, BOLD, msg, RESET);
        std::process::exit(1);
    }

    // ── 세션 시작 메시지 ──
    println!("{}{}RuSQL{} {}[{}@{}:{}]{}",
        BOLD, CYAN, RESET, GREEN, user, host, port, RESET);
    println!("{}Type SQL queries ending with ';'. \\help for commands.{}\n", DIM, RESET);

    // ── REPL ──
    let stdin = std::io::stdin();
    let mut buf = String::new();

    loop {
        // 프롬프트
        if buf.trim().is_empty() {
            print!("{}rusql{}{}>{} ", BOLD, RESET, GREEN, RESET);
        } else {
            print!("       {}>{} ", GREEN, RESET);
        }
        let _ = std::io::stdout().flush();

        let mut line = String::new();
        match stdin.lock().read_line(&mut line) {
            Ok(0) | Err(_) => break,
            _ => {}
        }

        let trimmed = line.trim();

        // 빈 줄 무시
        if trimmed.is_empty() { continue; }

        // 버퍼가 비어있을 때만 단독 명령 처리
        if buf.trim().is_empty() {
            if trimmed.eq_ignore_ascii_case("exit") || trimmed.eq_ignore_ascii_case("quit") {
                if writeln!(writer, "exit").is_ok() { let _ = writer.flush(); }
                println!("Bye!");
                break;
            }
            if trimmed == "\\help" || trimmed.eq_ignore_ascii_case("help") {
                print_help();
                continue;
            }
            if trimmed == "\\status" {
                if writeln!(writer, "\\status").is_err() || writer.flush().is_err() {
                    eprintln!("{}Connection lost.{}", RED, RESET); break;
                }
                let resp = read_response(&mut reader);
                for l in &resp { println!("{}", l); }
                continue;
            }
        }

        buf.push_str(&line);

        // 세미콜론이 있을 때 실행
        if !buf.contains(';') { continue; }

        let n = count_semicolons(&buf).max(1);
        if writeln!(writer, "{}", buf.trim()).is_err() || writer.flush().is_err() {
            eprintln!("{}Connection lost.{}", RED, RESET);
            break;
        }
        buf.clear();

        // 쿼리 수만큼 응답 수신
        for _ in 0..n {
            let resp = read_response(&mut reader);
            if resp.is_empty() {
                eprintln!("{}Connection closed by server.{}", RED, RESET);
                return;
            }
            display_response(&resp);
        }
    }
}
