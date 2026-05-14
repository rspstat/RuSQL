use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::Path;
use std::collections::HashMap;
use serde::{Serialize, Deserialize};
use crate::engine::executor::Row;
use crate::storage::page::{PageHeader, FLAG_COMPRESSED};
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
        Self::new_with_dir("data")
    }

    pub fn new_with_dir(dir: &str) -> Self {
        fs::create_dir_all(dir).unwrap();
        DiskManager { data_dir: dir.to_string() }
    }

    /// "db.table" → ("db", "table").  No dot → ("rustdb", key)
    fn parse_key<'a>(key: &'a str) -> (&'a str, &'a str) {
        if let Some(pos) = key.find('.') {
            (&key[..pos], &key[pos+1..])
        } else {
            ("rustdb", key)
        }
    }

    fn table_dir(&self, db: &str) -> String {
        format!("{}/{}", self.data_dir, db)
    }

    fn ensure_db_dir(&self, db: &str) {
        fs::create_dir_all(self.table_dir(db)).ok();
    }

    /// 데이터베이스 디렉토리 생성
    pub fn create_db_dir(&self, db: &str) {
        fs::create_dir_all(self.table_dir(db)).ok();
    }

    /// 데이터베이스 디렉토리 삭제 (DB 전체 삭제)
    pub fn drop_db_dir(&self, db: &str) {
        let _ = fs::remove_dir_all(self.table_dir(db));
    }

    /// data/ 하위 서브디렉토리 목록 = 데이터베이스 목록
    pub fn list_databases(&self) -> Vec<String> {
        let mut dbs = Vec::new();
        if let Ok(entries) = fs::read_dir(&self.data_dir) {
            for entry in entries.flatten() {
                if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                    dbs.push(entry.file_name().to_string_lossy().to_string());
                }
            }
        }
        dbs
    }

    /// 전체 TableSchema를 JSON으로 저장 (PK, auto_increment, 타입 등 포함)
    /// table 인자는 "db.table" 또는 "table" (rustdb 기본)
    pub fn save_schema(&self, table: &str, schema: &TableSchema) {
        let (db, tbl) = Self::parse_key(table);
        self.ensure_db_dir(db);
        let path = format!("{}/{}.schema.json", self.table_dir(db), tbl);
        let json = serde_json::to_string_pretty(schema).unwrap();
        fs::write(path, json).unwrap();
    }

    /// 저장된 TableSchema 로드
    pub fn load_schema(&self, table: &str) -> Option<TableSchema> {
        let (db, tbl) = Self::parse_key(table);
        // 신규 경로: data/db/table.schema.json
        let path = format!("{}/{}.schema.json", self.table_dir(db), tbl);
        // 구버전 flat 경로: data/table.schema.json (rustdb 테이블만)
        let flat_path = format!("{}/{}.schema.json", self.data_dir, tbl);
        let path = if Path::new(&path).exists() { path }
                   else if db == "rustdb" && Path::new(&flat_path).exists() { flat_path }
                   else { return None; };
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
        let (db, tbl) = Self::parse_key(table);
        self.ensure_db_dir(db);
        let path = format!("{}/{}.schema.json", self.table_dir(db), tbl);
        let json = serde_json::to_string(columns).unwrap();
        fs::write(path, json).unwrap();
    }

    // 데이터는 LZ4-compressed 바이너리 .rdb 포맷
    pub fn save_table(&self, table: &str, rows: &[Row]) {
        let (db, tbl) = Self::parse_key(table);
        self.ensure_db_dir(db);
        let path = format!("{}/{}.rdb", self.table_dir(db), tbl);
        let mut file = OpenOptions::new()
            .write(true).create(true).truncate(true)
            .open(&path).unwrap();

        // 모든 행을 평탄한 바이트 스트림으로 직렬화
        let mut raw: Vec<u8> = Vec::new();
        for row in rows {
            let json = serde_json::to_string(row).unwrap();
            let bytes = json.as_bytes();
            raw.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
            raw.extend_from_slice(bytes);
        }

        // LZ4 압축 (원본 크기를 앞 4바이트에 저장)
        let compressed = lz4_flex::compress_prepend_size(&raw);

        let page_size = crate::storage::page::PAGE_SIZE;
        let mut header = PageHeader::new();
        header.row_count  = rows.len() as u32;
        header.flags      = FLAG_COMPRESSED;
        header.page_count = ((compressed.len() + page_size - 1) / page_size).max(1) as u32;

        file.write_all(&header.to_bytes()).unwrap();
        file.write_all(&compressed).unwrap();
        file.flush().unwrap();
    }

    pub fn load_table(&self, table: &str) -> Vec<Row> {
        let (db, tbl) = Self::parse_key(table);
        // 신규 경로: data/db/table.rdb
        let rdb_path = format!("{}/{}.rdb", self.table_dir(db), tbl);
        if Path::new(&rdb_path).exists() {
            return self.load_rdb(&rdb_path);
        }
        // 구버전 flat 경로 (rustdb만)
        if db == "rustdb" {
            let flat_rdb = format!("{}/{}.rdb", self.data_dir, tbl);
            if Path::new(&flat_rdb).exists() {
                return self.load_rdb(&flat_rdb);
            }
            let flat_json = format!("{}/{}.json", self.data_dir, tbl);
            if Path::new(&flat_json).exists() {
                let json = fs::read_to_string(&flat_json).unwrap_or_default();
                return serde_json::from_str(&json).unwrap_or_default();
            }
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

        // 압축 해제 (FLAG_COMPRESSED가 설정된 경우)
        let raw: Vec<u8> = if header.is_compressed() {
            match lz4_flex::decompress_size_prepended(&buf[32..]) {
                Ok(d) => d,
                Err(_) => return Vec::new(),
            }
        } else {
            buf[32..].to_vec()
        };

        let mut rows = Vec::new();
        let mut pos = 0usize;

        for _ in 0..header.row_count {
            if pos + 4 > raw.len() { break; }
            let len = u32::from_le_bytes(raw[pos..pos+4].try_into().unwrap()) as usize;
            pos += 4;
            if pos + len > raw.len() { break; }
            let json = std::str::from_utf8(&raw[pos..pos+len]).unwrap_or("{}");
            if let Ok(row) = serde_json::from_str::<Row>(json) {
                rows.push(row);
            }
            pos += len;
        }

        rows
    }

    pub fn delete_table(&self, table: &str) {
        let (db, tbl) = Self::parse_key(table);
        let dir = self.table_dir(db);
        let _ = fs::remove_file(format!("{}/{}.rdb", dir, tbl));
        let _ = fs::remove_file(format!("{}/{}.json", dir, tbl));
        let _ = fs::remove_file(format!("{}/{}.schema.json", dir, tbl));
        // 구버전 flat 파일도 정리
        if db == "rustdb" {
            let _ = fs::remove_file(format!("{}/{}.rdb", self.data_dir, tbl));
            let _ = fs::remove_file(format!("{}/{}.json", self.data_dir, tbl));
            let _ = fs::remove_file(format!("{}/{}.schema.json", self.data_dir, tbl));
        }
    }

    /// 모든 DB의 모든 테이블을 "db.table" 형식으로 반환.
    /// 구버전 flat 파일은 "rustdb.table" 형식으로 반환 (마이그레이션 지원).
    pub fn list_tables(&self) -> Vec<String> {
        let mut tables = Vec::new();
        if let Ok(entries) = fs::read_dir(&self.data_dir) {
            for entry in entries.flatten() {
                let ftype = entry.file_type().unwrap_or_else(|_| return entry.file_type().unwrap());
                let name = entry.file_name().to_string_lossy().to_string();
                if ftype.is_dir() {
                    // data/db/ 서브디렉토리: db 안의 테이블 스캔
                    let db = &name;
                    if let Ok(sub) = fs::read_dir(entry.path()) {
                        for sub_entry in sub.flatten() {
                            let fname = sub_entry.file_name().to_string_lossy().to_string();
                            if fname.ends_with(".schema.json") {
                                let tbl = fname.replace(".schema.json", "");
                                tables.push(format!("{}.{}", db, tbl));
                            }
                        }
                    }
                } else if name.ends_with(".schema.json") {
                    // 구버전 flat 파일 → "rustdb.table"
                    let tbl = name.replace(".schema.json", "");
                    let qualified = format!("rustdb.{}", tbl);
                    if !tables.contains(&qualified) {
                        tables.push(qualified);
                    }
                }
            }
        }
        tables
    }

    // ── 뷰 영속화 (db별) ──────────────────────────────────────────────────

    pub fn save_views(&self, db: &str, views: &HashMap<String, Statement>) {
        self.ensure_db_dir(db);
        let path = format!("{}/views.json", self.table_dir(db));
        let json = serde_json::to_string_pretty(views).unwrap_or_default();
        let _ = fs::write(path, json);
    }

    pub fn load_views(&self, db: &str) -> HashMap<String, Statement> {
        // 신규 경로
        let path = format!("{}/views.json", self.table_dir(db));
        // 구버전 flat 경로 (rustdb만)
        let flat = format!("{}/views.json", self.data_dir);
        let path = if Path::new(&path).exists() { path }
                   else if db == "rustdb" && Path::new(&flat).exists() { flat }
                   else { return HashMap::new(); };
        let json = fs::read_to_string(&path).unwrap_or_default();
        serde_json::from_str(&json).unwrap_or_default()
    }

    // ── 인덱스 메타데이터 영속화 (db별) ──────────────────────────────────

    pub fn save_index_meta(&self, db: &str, meta_list: &[IndexMeta]) {
        self.ensure_db_dir(db);
        let path = format!("{}/indexes.json", self.table_dir(db));
        let json = serde_json::to_string_pretty(meta_list).unwrap_or_default();
        let _ = fs::write(path, json);
    }

    pub fn load_index_meta(&self, db: &str) -> Vec<IndexMeta> {
        let path = format!("{}/indexes.json", self.table_dir(db));
        let flat = format!("{}/indexes.json", self.data_dir);
        let path = if Path::new(&path).exists() { path }
                   else if db == "rustdb" && Path::new(&flat).exists() { flat }
                   else { return Vec::new(); };
        let json = fs::read_to_string(&path).unwrap_or_default();
        serde_json::from_str(&json).unwrap_or_default()
    }

    // ── 사용자/권한 영속화 (전역: data/_users.json, data/_grants.json) ────

    pub fn save_users<T: Serialize>(&self, users: &T) {
        let path = format!("{}/_users.json", self.data_dir);
        let json = serde_json::to_string_pretty(users).unwrap_or_default();
        let _ = fs::write(path, json);
    }

    pub fn load_users<T: for<'de> serde::Deserialize<'de> + Default>(&self) -> T {
        let path = format!("{}/_users.json", self.data_dir);
        if !Path::new(&path).exists() { return T::default(); }
        let json = fs::read_to_string(&path).unwrap_or_default();
        serde_json::from_str(&json).unwrap_or_default()
    }

    pub fn save_grants<T: Serialize>(&self, grants: &T) {
        let path = format!("{}/_grants.json", self.data_dir);
        let json = serde_json::to_string_pretty(grants).unwrap_or_default();
        let _ = fs::write(path, json);
    }

    pub fn load_grants<T: for<'de> serde::Deserialize<'de> + Default>(&self) -> T {
        let path = format!("{}/_grants.json", self.data_dir);
        if !Path::new(&path).exists() { return T::default(); }
        let json = fs::read_to_string(&path).unwrap_or_default();
        serde_json::from_str(&json).unwrap_or_default()
    }
}