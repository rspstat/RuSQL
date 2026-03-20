use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::Path;
use crate::engine::executor::Row;
use crate::storage::page::PageHeader;

pub struct DiskManager {
    data_dir: String,
}

impl DiskManager {
    pub fn new() -> Self {
        let data_dir = "data".to_string();
        fs::create_dir_all(&data_dir).unwrap();
        DiskManager { data_dir }
    }

    // 스키마는 그대로 JSON 유지 (스키마는 작아서 바이너리 불필요)
    pub fn save_schema(&self, table: &str, columns: &[String]) {
        let path = format!("{}/{}.schema.json", self.data_dir, table);
        let json = serde_json::to_string(columns).unwrap();
        fs::write(path, json).unwrap();
    }

    pub fn load_schema(&self, table: &str) -> Option<Vec<String>> {
        let path = format!("{}/{}.schema.json", self.data_dir, table);
        let json = fs::read_to_string(path).ok()?;
        serde_json::from_str(&json).ok()
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
}