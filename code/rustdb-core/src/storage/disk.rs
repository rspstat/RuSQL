// src/storage/disk.rs

use std::collections::HashMap;
use std::fs;
use std::path::Path;

pub type Row = HashMap<String, String>;

pub struct DiskManager;

impl DiskManager {
    pub fn new() -> Self {
        DiskManager
    }

    // 데이터 폴더 없으면 생성
    fn ensure_dir() {
        if !Path::new("data").exists() {
            fs::create_dir("data").unwrap();
        }
    }

    fn table_path(table: &str) -> String {
        format!("data/{}.json", table)
    }

    // 테이블 전체 행 저장
    pub fn save_table(&self, table: &str, rows: &Vec<Row>) {
        Self::ensure_dir();
        let path = Self::table_path(table);
        let json = serde_json::to_string_pretty(rows).unwrap();
        fs::write(path, json).unwrap();
    }

    // 테이블 전체 행 불러오기
    pub fn load_table(&self, table: &str) -> Vec<Row> {
        let path = Self::table_path(table);
        if !Path::new(&path).exists() {
            return Vec::new();
        }
        let json = fs::read_to_string(path).unwrap();
        serde_json::from_str(&json).unwrap_or_default()
    }

    // 스키마 저장 (컬럼 이름 목록)
    pub fn save_schema(&self, table: &str, columns: &Vec<String>) {
        Self::ensure_dir();
        let path = format!("data/{}.schema.json", table);
        let json = serde_json::to_string_pretty(columns).unwrap();
        fs::write(path, json).unwrap();
    }

    // 스키마 불러오기
    pub fn load_schema(&self, table: &str) -> Option<Vec<String>> {
        let path = format!("data/{}.schema.json", table);
        if !Path::new(&path).exists() {
            return None;
        }
        let json = fs::read_to_string(path).unwrap();
        serde_json::from_str(&json).ok()
    }

    // 테이블 삭제
    pub fn delete_table(&self, table: &str) {
        let _ = fs::remove_file(Self::table_path(table));
        let _ = fs::remove_file(format!("data/{}.schema.json", table));
    }

    // 저장된 모든 테이블 이름 목록
    pub fn list_tables(&self) -> Vec<String> {
        Self::ensure_dir();
        fs::read_dir("data").unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|name| name.ends_with(".schema.json"))
            .map(|name| name.replace(".schema.json", ""))
            .collect()
    }
}