// src/storage/composite_index.rs
//
// 복합 인덱스: 여러 컬럼을 조합한 B+Tree 인덱스
// 복합 키 형식: "val1\x00val2\x00..." (null byte 구분자)

use std::collections::HashMap;
use crate::storage::btree::BPlusTree;

pub type Row = HashMap<String, String>;

/// 복합 인덱스
pub struct CompositeIndex {
    /// 이 인덱스가 속한 테이블
    pub table: String,
    /// 인덱스를 구성하는 컬럼 순서 (순서 중요)
    pub columns: Vec<String>,
    /// 내부 B+Tree: key = "val1\x00val2\x00..."
    tree: BPlusTree,
}

impl CompositeIndex {
    pub fn new(table: String, columns: Vec<String>) -> Self {
        CompositeIndex { table, columns, tree: BPlusTree::new() }
    }

    /// 컬럼 값 슬라이스로 복합 키 생성
    pub fn make_key(values: &[&str]) -> String {
        values.join("\x00")
    }

    /// Row에서 이 인덱스의 복합 키 생성 (컬럼이 없으면 None)
    pub fn key_from_row(&self, row: &Row) -> Option<String> {
        let parts: Option<Vec<&str>> = self.columns.iter()
            .map(|col| row.get(col).map(|v| v.as_str()))
            .collect();
        parts.map(|p| Self::make_key(&p))
    }

    /// Row를 인덱스에 삽입
    pub fn insert_row(&mut self, row: &Row) {
        if let Some(key) = self.key_from_row(row) {
            let val = serde_json::to_string(row).unwrap_or_default();
            self.tree.insert(key, val);
        }
    }

    /// 모든 인덱스 컬럼에 대한 정확한 값으로 검색
    /// values 순서는 self.columns 순서와 동일해야 함
    pub fn search_exact(&self, values: &[&str]) -> Option<String> {
        let key = Self::make_key(values);
        self.tree.search(&key)
    }

    /// 인덱스 컬럼과 조건 맵이 완전히 일치하는지 확인
    /// eq_map: column -> value 매핑
    pub fn matches_conditions(&self, eq_map: &HashMap<String, String>) -> bool {
        self.columns.iter().all(|col| eq_map.contains_key(col))
    }

    /// eq_map에서 인덱스 키 순서대로 값을 추출하여 검색
    pub fn search_from_eq_map(&self, eq_map: &HashMap<String, String>) -> Option<String> {
        let values: Vec<&str> = self.columns.iter()
            .map(|col| eq_map.get(col).map(|v| v.as_str()).unwrap_or(""))
            .collect();
        self.search_exact(&values)
    }

    /// 기존 rows로 인덱스 전체 재빌드
    pub fn rebuild(&mut self, rows: &[Row]) {
        self.tree = BPlusTree::new();
        for row in rows {
            self.insert_row(row);
        }
    }
}
