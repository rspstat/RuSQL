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

    /// 단일 행을 인덱스에 추가한다 (O(1)).
    pub fn insert_row(&mut self, row: &Row) {
        if let Some(val) = row.get(&self.column) {
            self.data.entry(val.clone()).or_default().push(row.clone());
        }
    }

    /// PK 값이 `pk_val`인 행을 `col_val` 버킷에서 제거한다 (O(bucket size)).
    pub fn remove_row(&mut self, col_val: &str, pk_col: &str, pk_val: &str) {
        if let Some(bucket) = self.data.get_mut(col_val) {
            bucket.retain(|r| r.get(pk_col).map(|v| v != pk_val).unwrap_or(true));
            if bucket.is_empty() {
                self.data.remove(col_val);
            }
        }
    }

    /// 전체 버킷 수 (고유 키 수)
    pub fn bucket_count(&self) -> usize { self.data.len() }

    /// 전체 행 수
    pub fn row_count(&self) -> usize { self.data.values().map(|v| v.len()).sum() }
}
