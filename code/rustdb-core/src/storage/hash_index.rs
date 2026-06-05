use std::collections::HashMap;
use crate::engine::executor::Row;

/// 등호 조건 O(1) 검색을 위한 Hash Index.
/// key(컬럼 값) → 해당 행 목록을 HashMap으로 유지한다.
pub struct HashIndex {
    pub table:  String,
    pub column: String,
    data: HashMap<String, Vec<Row>>,
}

impl HashIndex {
    pub fn new(table: &str, column: &str) -> Self {
        Self { table: table.to_string(), column: column.to_string(), data: HashMap::new() }
    }

    /// 테이블 전체 행으로 인덱스를 (재)빌드한다.
    pub fn rebuild(&mut self, rows: &[Row]) {
        self.data.clear();
        for row in rows {
            if let Some(val) = row.get(&self.column) {
                self.data.entry(val.clone()).or_default().push(row.clone());
            }
        }
    }

    /// key에 해당하는 행 슬라이스를 반환한다.
    pub fn get(&self, key: &str) -> &[Row] {
        self.data.get(key).map(|v| v.as_slice()).unwrap_or(&[])
    }

    /// 전체 버킷 수 (고유 키 수)
    pub fn bucket_count(&self) -> usize { self.data.len() }

    /// 전체 행 수
    pub fn row_count(&self) -> usize { self.data.values().map(|v| v.len()).sum() }
}
