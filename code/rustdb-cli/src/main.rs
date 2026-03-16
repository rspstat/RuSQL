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
    print_color("         RustDB  v0.1.0  Custom RDBMS           ", Color::Cyan);
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

fn main() {
    let stdin = io::stdin();
    let mut executor = Executor::new();

    print_banner();

    loop {
        print_color("rustdb", Color::Cyan);
        print_color("> ", Color::White);
        io::stdout().flush().unwrap();

        let mut input = String::new();
        stdin.lock().read_line(&mut input).unwrap();
        let input = input.trim();

        if input.is_empty() { continue; }

        if input == "exit" || input == "quit" {
            print_color("\nBye!\n", Color::Cyan);
            break;
        }

        let queries: Vec<&str> = input
            .split(';')
            .map(|q| q.trim())
            .filter(|q| !q.is_empty())
            .collect();

        for query in queries {
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
                Err(e) => print_color(&format!("PARSE ERROR: {}\n", e), Color::Red),
            }
        }
    }
}