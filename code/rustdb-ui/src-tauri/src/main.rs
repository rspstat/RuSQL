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

#[derive(serde::Serialize)]
struct MultiQueryResult {
    results: Vec<QueryResult>,
    total_elapsed: f64,
}

#[tauri::command]
fn execute_query(query: String, ts: Option<u64>, state: State<DbState>) -> MultiQueryResult {
    let start = Instant::now();
    let mut exec = state.0.lock().unwrap();

    let queries: Vec<&str> = query
        .split(';')
        .map(|q| q.trim())
        .filter(|q| !q.is_empty())
        .collect();

    let mut results = Vec::new();

    for q in &queries {
        let q_start = Instant::now();
        let mut p = Parser::new(q);
        let result = match p.parse() {
            Ok(stmt) => match exec.execute(stmt) {
                Ok(output) => parse_output(&output, q_start.elapsed().as_secs_f64()),
                Err(e) => QueryResult {
                    columns: vec![],
                    rows: vec![],
                    message: e,
                    elapsed: q_start.elapsed().as_secs_f64(),
                    success: false,
                },
            },
            Err(e) => QueryResult {
                columns: vec![],
                rows: vec![],
                message: format!("Parse Error: {}", e),
                elapsed: q_start.elapsed().as_secs_f64(),
                success: false,
            },
        };
        results.push(result);
    }

    MultiQueryResult {
        total_elapsed: start.elapsed().as_secs_f64(),
        results,
    }
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

#[tauri::command]
fn get_columns(table: String, state: State<DbState>) -> Vec<String> {
    let exec = state.0.lock().unwrap();
    exec.catalog.get_table(&table)
        .map(|s| s.columns.iter().map(|c| c.name.clone()).collect())
        .unwrap_or_default()
}

fn main() {
    tauri::Builder::default()
        .manage(DbState(Mutex::new(Executor::new())))
        .invoke_handler(tauri::generate_handler![execute_query, get_tables, get_columns])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}