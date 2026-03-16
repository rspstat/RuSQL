#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// mod lib;

use std::sync::Mutex;
use std::time::Instant;
use tauri::State;
use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::Executor;

struct DbState(Mutex<Executor>);

#[derive(serde::Serialize)]
struct QueryResult {
    columns: Vec<String>,
    rows: Vec<Vec<String>>,
    message: String,
    elapsed: f64,
    success: bool,
}

#[tauri::command]
fn execute_query(query: String, state: State<DbState>) -> QueryResult {
    let start = Instant::now();
    let mut exec = state.0.lock().unwrap();

    let queries: Vec<&str> = query
        .split(';')
        .map(|q| q.trim())
        .filter(|q| !q.is_empty())
        .collect();

    let mut last_result = QueryResult {
        columns: vec![],
        rows: vec![],
        message: String::new(),
        elapsed: 0.0,
        success: true,
    };

    for q in &queries {
        let mut p = Parser::new(q);
        match p.parse() {
            Ok(stmt) => match exec.execute(stmt) {
                Ok(output) => {
                    last_result = parse_output(&output, start.elapsed().as_secs_f64());
                }
                Err(e) => {
                    last_result = QueryResult {
                        columns: vec![],
                        rows: vec![],
                        message: e,
                        elapsed: start.elapsed().as_secs_f64(),
                        success: false,
                    };
                }
            },
            Err(e) => {
                last_result = QueryResult {
                    columns: vec![],
                    rows: vec![],
                    message: format!("Parse Error: {}", e),
                    elapsed: start.elapsed().as_secs_f64(),
                    success: false,
                };
            }
        }
    }
    last_result
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
                    .split('|')
                    .filter(|s| !s.is_empty())
                    .map(|s| s.trim().to_string())
                    .collect();
                if i == 1 {
                    columns = cells;
                } else {
                    rows.push(cells);
                }
            }
        }

        QueryResult { columns, rows, message: String::new(), elapsed, success: true }
    } else {
        QueryResult { columns: vec![], rows: vec![], message: output.to_string(), elapsed, success: true }
    }
}

#[tauri::command]
fn get_tables(state: State<DbState>) -> Vec<String> {
    let exec = state.0.lock().unwrap();
    exec.catalog.tables.keys().cloned().collect()
}

fn main() {
    tauri::Builder::default()
        .manage(DbState(Mutex::new(Executor::new())))
        .invoke_handler(tauri::generate_handler![execute_query, get_tables])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}