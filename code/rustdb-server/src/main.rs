use std::net::{TcpListener, TcpStream};
use std::io::{BufRead, BufReader, Write};
use std::sync::{Arc, Mutex};
use std::thread;

use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::Executor;

fn handle_client(stream: TcpStream, executor: Arc<Mutex<Executor>>) {
    let peer = stream.peer_addr().unwrap();
    println!("[+] Client connected: {}", peer);

    let mut writer = stream.try_clone().unwrap();
    let reader = BufReader::new(stream);

    writer.write_all(b"RustDB v2.1.3\n").unwrap();
    writer.write_all(b"Ready.\n").unwrap();
    writer.flush().unwrap();

    for line in reader.lines() {
        let input = match line {
            Ok(l) => l.trim().to_string(),
            Err(_) => break,
        };

        if input.is_empty() { continue; }

        if input == "exit" || input == "quit" {
            writer.write_all(b"Bye!\n").unwrap();
            break;
        }

        let queries: Vec<&str> = input
            .split(';')
            .map(|q| q.trim())
            .filter(|q| !q.is_empty())
            .collect();

        let mut exec = executor.lock().unwrap();
        for query in &queries {
            let mut p = Parser::new(query);
            let result = match p.parse() {
                Ok(stmt) => match exec.execute(stmt) {
                    Ok(r)  => format!("OK\n{}\n", r),
                    Err(e) => format!("ERR\n{}\n", e),
                },
                Err(e) => format!("ERR\nParse Error: {}\n", e),
            };
            writer.write_all(result.as_bytes()).unwrap();
            writer.write_all(b"END\n").unwrap();
            writer.flush().unwrap();
        }
    }

    println!("[-] Client disconnected: {}", peer);
}

fn main() {
    let addr = "127.0.0.1:7878";
    let listener = TcpListener::bind(addr).unwrap();
    let executor = Arc::new(Mutex::new(Executor::new()));

    println!("+-----------------------------------------+");
    println!("|   RustDB Server v0.1.0                  |");
    println!("|   Listening on {}              |", addr);
    println!("+-----------------------------------------+");

    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let executor_clone = Arc::clone(&executor);
                thread::spawn(move || {
                    handle_client(s, executor_clone);
                });
            }
            Err(e) => eprintln!("Connection error: {}", e),
        }
    }
}