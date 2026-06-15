use std::collections::{HashMap, HashSet, VecDeque};

const DEFAULT_CAPACITY: usize = 512;

/// SELECT 결과 LRU 캐시.
/// - 키: "{current_db}::{sql}"
/// - 값: 결과 문자열
/// - 테이블별 무효화 인덱스(`table_to_keys`)로 DML 발생 시 O(k) 즉시 제거 (k = 해당 테이블 참조 쿼리 수)
/// - 트랜잭션 내부 쿼리는 캐시하지 않음 (호출자가 skip)
pub struct QueryResultCache {
    entries: HashMap<String, String>,
    table_to_keys: HashMap<String, HashSet<String>>,
    order: VecDeque<String>,
    pub capacity: usize,
}

impl QueryResultCache {
    pub fn new() -> Self {
        Self::with_capacity(DEFAULT_CAPACITY)
    }

    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            entries: HashMap::new(),
            table_to_keys: HashMap::new(),
            order: VecDeque::new(),
            capacity,
        }
    }

    pub fn get(&self, key: &str) -> Option<&String> {
        self.entries.get(key)
    }

    /// 캐시에 결과 저장. `tables`는 해당 쿼리가 참조하는 qualified table name 목록.
    pub fn put(&mut self, key: String, result: String, tables: &[String]) {
        if self.entries.contains_key(&key) {
            return;
        }
        // 용량 초과 시 가장 오래된 항목 제거 (FIFO 근사 LRU)
        if self.entries.len() >= self.capacity {
            if let Some(oldest) = self.order.pop_front() {
                self.entries.remove(&oldest);
                for keys in self.table_to_keys.values_mut() {
                    keys.remove(&oldest);
                }
            }
        }
        for table in tables {
            self.table_to_keys
                .entry(table.clone())
                .or_default()
                .insert(key.clone());
        }
        self.entries.insert(key.clone(), result);
        self.order.push_back(key);
    }

    /// 특정 테이블을 참조하는 캐시 항목 전체 무효화.
    pub fn invalidate_table(&mut self, table: &str) {
        if let Some(keys) = self.table_to_keys.remove(table) {
            for key in &keys {
                self.entries.remove(key);
                self.order.retain(|k| k != key);
            }
        }
    }

    pub fn clear(&mut self) {
        self.entries.clear();
        self.table_to_keys.clear();
        self.order.clear();
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }
}
