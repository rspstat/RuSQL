#![allow(dead_code)]

use std::io::{self, BufRead, Write};
use std::time::Instant;
use crossterm::style::{Color, Print, ResetColor, SetForegroundColor};
use crossterm::ExecutableCommand;

use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::Executor;

fn print_color(text: &str, color: Color) {
    let mut stdout = io::stdout();
    stdout.execute(SetForegroundColor(color)).unwrap();
    stdout.execute(Print(text)).unwrap();
    stdout.execute(ResetColor).unwrap();
    stdout.flush().unwrap();
}

fn print_banner() {
    print_color("+-------------------------------------------------+\n", Color::DarkCyan);
    print_color("|", Color::DarkCyan);
    print_color("         RustDB  v2.2.0  Custom RDBMS           ", Color::Cyan);
    print_color("|\n", Color::DarkCyan);
    print_color("+-------------------------------------------------+\n", Color::DarkCyan);
    print_color("| Engine  : B+Tree + WAL + Undo Log + Optimizer   |\n", Color::DarkGrey);
    print_color("| Storage : Binary .rdb  LZ4 Compressed           |\n", Color::DarkGrey);
    print_color("| Join    : Sort-Merge / Hash / Nested Loop        |\n", Color::DarkGrey);
    print_color("+-------------------------------------------------+\n", Color::DarkCyan);
    println!();
    print_color("Type SQL or 'help' for commands. 'exit' to quit.\n", Color::Grey);
    println!();
}

fn colorize_table(output: &str) {
    let mut stdout = io::stdout();
    for line in output.lines() {
        if line.starts_with('+') {
            stdout.execute(SetForegroundColor(Color::DarkCyan)).unwrap();
            println!("{}", line);
        } else if line.starts_with('|') {
            let parts: Vec<&str> = line.split('|').collect();
            stdout.execute(SetForegroundColor(Color::DarkCyan)).unwrap();
            print!("|");
            stdout.execute(SetForegroundColor(Color::Cyan)).unwrap();
            for (i, part) in parts.iter().enumerate() {
                if i == 0 || i == parts.len() - 1 { continue; }
                print!("{}", part);
                stdout.execute(SetForegroundColor(Color::DarkCyan)).unwrap();
                if i < parts.len() - 2 { print!("|"); }
            }
            stdout.execute(SetForegroundColor(Color::DarkCyan)).unwrap();
            println!("|");
        } else if line.contains("row(s) returned") {
            stdout.execute(SetForegroundColor(Color::Green)).unwrap();
            println!("{}", line);
        } else {
            stdout.execute(ResetColor).unwrap();
            println!("{}", line);
        }
    }
    stdout.execute(ResetColor).unwrap();
}

fn run_query(executor: &mut Executor, query: &str) {
    let start = Instant::now();
    let mut p = Parser::new(query);
    match p.parse() {
        Ok(stmt) => match executor.execute(stmt) {
            Ok(result) => {
                let elapsed = start.elapsed();
                colorize_table(&result);
                print_color(
                    &format!("({:.3} sec)\n", elapsed.as_secs_f64()),
                    Color::DarkGrey,
                );
            }
            Err(e) => print_color(&format!("ERROR: {}\n", e), Color::Red),
        },
        Err(e) if e.contains("Unknown statement: None") => {}
        Err(e) => print_color(&format!("PARSE ERROR: {}\n", e), Color::Red),
    }
}


fn main() {
    let args: Vec<String> = std::env::args().collect();
    let buffer_pool_size: usize = args.windows(2)
        .find(|w| w[0] == "--buffer-pool-size")
        .and_then(|w| w[1].parse().ok())
        .unwrap_or(64);

    let stdin = io::stdin();
    let mut executor = Executor::new_with_buffer_pool_size(buffer_pool_size);

    print_banner();

    let mut buf = String::new();

    for line in stdin.lock().lines() {
        let line = match line { Ok(l) => l, Err(_) => break };
        let trimmed = line.trim();

        if trimmed.is_empty() || trimmed.starts_with("--") { continue; }
        if trimmed == "exit" || trimmed == "quit" {
            print_color("\nBye!\n", Color::Cyan);
            break;
        }

        buf.push(' ');
        buf.push_str(trimmed);

        // BEGIN..END 인식 분리: depth=0인 ';' 위치만 분리점으로 사용
        loop {
            match find_stmt_end(&buf) {
                Some(pos) => {
                    let stmt_str = buf[..pos].trim().to_string();
                    buf = buf[pos + 1..].to_string();
                    if buf.trim_start().starts_with("--") { buf.clear(); }
                    if stmt_str.is_empty() { continue; }

                    print_color("rustdb", Color::Cyan);
                    print_color("> ", Color::White);
                    io::stdout().flush().unwrap();

                    run_query(&mut executor, &stmt_str);
                }
                None => break,
            }
        }
    }
}

/// BEGIN...END depth=0 에서의 첫 번째 ';' 바이트 오프셋을 반환
fn find_stmt_end(s: &str) -> Option<usize> {
    let bytes = s.as_bytes();
    let len = bytes.len();
    let mut begin_depth: i32 = 0;
    let mut i = 0;
    while i < len {
        // 문자열 리터럴 건너뜀 (ASCII-safe)
        if bytes[i] == b'\'' {
            i += 1;
            while i < len { let c = bytes[i]; i += 1; if c == b'\'' { break; } }
            continue;
        }
        // 키워드 추출
        if bytes[i].is_ascii_alphabetic() || bytes[i] == b'_' {
            let start = i;
            while i < len && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_') { i += 1; }
            let word = std::str::from_utf8(&bytes[start..i]).unwrap_or("").to_uppercase();
            match word.as_str() {
                "BEGIN" => { begin_depth += 1; }
                "END" => {
                    let mut j = i;
                    while j < len && bytes[j].is_ascii_whitespace() { j += 1; }
                    let next_is_sub = if j < len && (bytes[j].is_ascii_alphabetic() || bytes[j] == b'_') {
                        let s2 = j;
                        let mut k = j;
                        while k < len && (bytes[k].is_ascii_alphanumeric() || bytes[k] == b'_') { k += 1; }
                        let nw = std::str::from_utf8(&bytes[s2..k]).unwrap_or("").to_uppercase();
                        matches!(nw.as_str(), "IF" | "WHILE" | "LOOP" | "REPEAT" | "CASE")
                    } else { false };
                    if !next_is_sub && begin_depth > 0 { begin_depth -= 1; }
                }
                _ => {}
            }
            continue;
        }
        // ';': depth=0 이면 분리점 (바이트 오프셋 반환)
        if bytes[i] == b';' && begin_depth == 0 {
            return Some(i);
        }
        i += 1;
    }
    None
}