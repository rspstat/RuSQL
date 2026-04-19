use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::Path;
use std::collections::HashMap;
use serde::{Serialize, Deserialize};
use crate::engine::executor::Row;
use crate::storage::page::PageHeader;
use crate::catalog::schema::TableSchema;
use crate::parser::ast::Statement;

/// 인덱스 메타데이터 — 재시작 시 인덱스 재빌드에 사용
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IndexMeta {
    pub name: String,
    pub table: String,
    pub columns: Vec<String>,
}

pub struct DiskManager {
    data_dir: String,
}

impl DiskManager {
    pub fn new() -> Self {
        let data_dir = "data".to_string();
        fs::create_dir_all(&data_dir).unwrap();
        DiskManager { data_dir }
    }

    /// 전체 TableSchema를 JSON으로 저장 (PK, auto_increment, 타입 등 포함)
    pub fn save_schema(&self, table: &str, schema: &TableSchema) {
        let path = format!("{}/{}.schema.json", self.data_dir, table);
        let json = serde_json::to_string_pretty(schema).unwrap();
        fs::write(path, json).unwrap();
    }

    /// 저장된 TableSchema 로드. 구버전(컬럼명만 있는) 파일도 호환
    pub fn load_schema(&self, table: &str) -> Option<TableSchema> {
        let path = format!("{}/{}.schema.json", self.data_dir, table);
        let json = fs::read_to_string(&path).ok()?;

        // 신버전: TableSchema JSON
        if let Ok(schema) = serde_json::from_str::<TableSchema>(&json) {
            return Some(schema);
        }

        // 구버전 폴백: 컬럼명 배열 ["col1", "col2", ...]
        if let Ok(col_names) = serde_json::from_str::<Vec<String>>(&json) {
            use crate::parser::ast::DataType;
            use crate::catalog::schema::ColumnDef;
            let columns = col_names.iter().map(|c| ColumnDef {
                name: c.clone(),
                data_type: DataType::Text,
                primary_key: false,
                not_null: false,
                unique: false,
                unique_constraint_name: None,
                auto_increment: false,
                default: None,
                foreign_key: None,
                check_expr: None,
            }).collect();
            return Some(TableSchema {
                name: table.to_string(),
                columns,
                auto_increment_counters: std::collections::HashMap::new(),
                primary_key_columns: Vec::new(),
                check_constraints: Vec::new(),
            });
        }

        None
    }

    /// 구버전 호환: 컬럼명만 저장 (내부용)
    pub fn save_schema_columns(&self, table: &str, columns: &[String]) {
        let path = format!("{}/{}.schema.json", self.data_dir, table);
        let json = serde_json::to_string(columns).unwrap();
        fs::write(path, json).unwrap();
    }

    // 데이터는 바이너리 .rdb 포맷
    pub fn save_table(&self, table: &str, rows: &[Row]) {
        let path = format!("{}/{}.rdb", self.data_dir, table);
        let mut file = OpenOptions::new()
            .write(true).create(true).truncate(true)
            .open(&path).unwrap();

        // 헤더 작성
        let mut header = PageHeader::new();
        header.row_count = rows.len() as u32;

        // 각 행을 직렬화
        let mut row_data: Vec<Vec<u8>> = Vec::new();
        for row in rows {
            let json = serde_json::to_string(row).unwrap();
            let bytes = json.as_bytes();
            let mut entry = Vec::new();
            // 행 크기(4 bytes) + 데이터
            entry.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
            entry.extend_from_slice(bytes);
            row_data.push(entry);
        }

        // 전체 데이터 크기 계산해서 페이지 수 결정
        let total_data: usize = row_data.iter().map(|r| r.len()).sum();
        let page_size = crate::storage::page::PAGE_SIZE;
        header.page_count = ((total_data + page_size - 1) / page_size).max(1) as u32;

        // 헤더 쓰기
        file.write_all(&header.to_bytes()).unwrap();

        // 데이터 쓰기
        for entry in &row_data {
            file.write_all(entry).unwrap();
        }

        // 마지막 페이지 패딩
        let data_written: usize = row_data.iter().map(|r| r.len()).sum();
        let remainder = data_written % page_size;
        if remainder != 0 {
            let padding = vec![0u8; page_size - remainder];
            file.write_all(&padding).unwrap();
        }

        file.flush().unwrap();
    }

    pub fn load_table(&self, table: &str) -> Vec<Row> {
        // .rdb 파일 먼저 시도
        let rdb_path = format!("{}/{}.rdb", self.data_dir, table);
        if Path::new(&rdb_path).exists() {
            return self.load_rdb(&rdb_path);
        }

        // 구버전 .json 파일 폴백
        let json_path = format!("{}/{}.json", self.data_dir, table);
        if Path::new(&json_path).exists() {
            let json = fs::read_to_string(&json_path).unwrap_or_default();
            return serde_json::from_str(&json).unwrap_or_default();
        }

        Vec::new()
    }

    fn load_rdb(&self, path: &str) -> Vec<Row> {
        let mut file = match File::open(path) {
            Ok(f) => f,
            Err(_) => return Vec::new(),
        };

        let mut buf = Vec::new();
        file.read_to_end(&mut buf).unwrap();

        if buf.len() < 32 { return Vec::new(); }

        let header = match PageHeader::from_bytes(&buf[..32]) {
            Some(h) => h,
            None => return Vec::new(),
        };

        let mut rows = Vec::new();
        let mut pos = 32usize;

        for _ in 0..header.row_count {
            if pos + 4 > buf.len() { break; }
            let len = u32::from_le_bytes(buf[pos..pos+4].try_into().unwrap()) as usize;
            pos += 4;
            if pos + len > buf.len() { break; }
            let json = std::str::from_utf8(&buf[pos..pos+len]).unwrap_or("{}");
            if let Ok(row) = serde_json::from_str::<Row>(json) {
                rows.push(row);
            }
            pos += len;
        }

        rows
    }

    pub fn delete_table(&self, table: &str) {
        let _ = fs::remove_file(format!("{}/{}.rdb", self.data_dir, table));
        let _ = fs::remove_file(format!("{}/{}.json", self.data_dir, table));
        let _ = fs::remove_file(format!("{}/{}.schema.json", self.data_dir, table));
    }

    pub fn list_tables(&self) -> Vec<String> {
        let mut tables = Vec::new();
        if let Ok(entries) = fs::read_dir(&self.data_dir) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if name.ends_with(".schema.json") {
                    tables.push(name.replace(".schema.json", ""));
                }
            }
        }
        tables
    }

    // ── 뷰 영속화 ─────────────────────────────────────────────────────────

    /// 모든 뷰 정의를 data/views.json에 저장
    pub fn save_views(&self, views: &HashMap<String, Statement>) {
        let path = format!("{}/views.json", self.data_dir);
        let json = serde_json::to_string_pretty(views).unwrap_or_default();
        let _ = fs::write(path, json);
    }

    /// data/views.json에서 뷰 정의 로드
    pub fn load_views(&self) -> HashMap<String, Statement> {
        let path = format!("{}/views.json", self.data_dir);
        let json = match fs::read_to_string(&path) {
            Ok(s) => s,
            Err(_) => return HashMap::new(),
        };
        serde_json::from_str(&json).unwrap_or_default()
    }

    // ── 인덱스 메타데이터 영속화 ──────────────────────────────────────────

    /// 모든 인덱스 메타데이터를 data/indexes.json에 저장
    pub fn save_index_meta(&self, meta_list: &[IndexMeta]) {
        let path = format!("{}/indexes.json", self.data_dir);
        let json = serde_json::to_string_pretty(meta_list).unwrap_or_default();
        let _ = fs::write(path, json);
    }

    /// data/indexes.json에서 인덱스 메타데이터 로드
    pub fn load_index_meta(&self) -> Vec<IndexMeta> {
        let path = format!("{}/indexes.json", self.data_dir);
        let json = match fs::read_to_string(&path) {
            Ok(s) => s,
            Err(_) => return Vec::new(),
        };
        serde_json::from_str(&json).unwrap_or_default()
    }
}