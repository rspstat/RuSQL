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
    print_color("         RustDB  v2.1.3  Custom RDBMS           ", Color::Cyan);
    print_color("|\n", Color::DarkCyan);
    print_color("+-------------------------------------------------+\n", Color::DarkCyan);
    print_color("| Engine  : B+Tree + WAL + Undo Log               |\n", Color::DarkGrey);
    print_color("| Storage : JSON (Binary planned)                 |\n", Color::DarkGrey);
    print_color("| MCP     : Natural Language Query (planned)      |\n", Color::DarkGrey);
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
    let stdin = io::stdin();
    let mut executor = Executor::new();

    print_banner();

    // Buffer to accumulate multi-line statements
    let mut buf = String::new();

    for line in stdin.lock().lines() {
        let line = match line { Ok(l) => l, Err(_) => break };
        let trimmed = line.trim();

        // Skip pure comment lines and empty lines
        if trimmed.is_empty() || trimmed.starts_with("--") {
            continue;
        }

        if trimmed == "exit" || trimmed == "quit" {
            print_color("\nBye!\n", Color::Cyan);
            break;
        }

        buf.push(' ');
        buf.push_str(trimmed);

        // Process complete statements (split on ';')
        while let Some(pos) = buf.find(';') {
            let stmt_str = buf[..pos].trim().to_string();
            buf = buf[pos + 1..].to_string();

            if stmt_str.is_empty() { continue; }

            print_color("rustdb", Color::Cyan);
            print_color("> ", Color::White);
            io::stdout().flush().unwrap();

            run_query(&mut executor, &stmt_str);
        }
    }
}