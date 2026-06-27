use std::collections::{HashMap, HashSet};

const DEFAULT_CAPACITY: usize = 512;

/// SELECT 결과 LRU 캐시.
///
/// - 키: "{current_db}::{sql}"
/// - `table_to_keys`: 테이블 → 해당 테이블을 참조하는 캐시 키 집합 (O(k) 무효화)
/// - `insert_order`: 캐시 키 → 삽입 tick (용량 초과 시 oldest-first 교체, O(n))
/// - 트랜잭션 내부 쿼리는 캐시하지 않음 (호출자가 skip)
///
/// 이전 VecDeque 기반 구현에서는 invalidate_table 시 O(n×k) retain 루프가 발생했다.
/// 이 구현은 insert_order HashMap에서 O(1) remove를 사용하므로 invalidate_table이 O(k).
pub struct QueryResultCache {
    entries: HashMap<String, String>,
    table_to_keys: HashMap<String, HashSet<String>>,
    /// 키 → 삽입 순서 tick. 용량 초과 시 최솟값을 찾아 교체.
    insert_order: HashMap<String, u64>,
    tick: u64,
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
            insert_order: HashMap::new(),
            tick: 0,
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
        // 용량 초과 시 가장 먼저 삽입된 항목 교체 (O(n) 스캔, 교체는 드물게 발생)
        if self.entries.len() >= self.capacity {
            if let Some(oldest_key) = self.insert_order.iter()
                .min_by_key(|(_, &t)| t)
                .map(|(k, _)| k.clone())
            {
                self.entries.remove(&oldest_key);
                self.insert_order.remove(&oldest_key);
                for keys in self.table_to_keys.values_mut() {
                    keys.remove(&oldest_key);
                }
            }
        }
        self.tick += 1;
        for table in tables {
            self.table_to_keys
                .entry(table.clone())
                .or_default()
                .insert(key.clone());
        }
        self.entries.insert(key.clone(), result);
        self.insert_order.insert(key, self.tick);
    }

    /// 특정 테이블을 참조하는 캐시 항목 전체 무효화.
    /// 복잡도: O(k) — k는 해당 테이블을 참조하는 캐시 항목 수.
    /// 이전 VecDeque retain 방식의 O(n×k) 에서 개선.
    pub fn invalidate_table(&mut self, table: &str) {
        if let Some(keys) = self.table_to_keys.remove(table) {
            for key in &keys {
                self.entries.remove(key);
                self.insert_order.remove(key); // O(1) HashMap remove
            }
        }
    }

    pub fn clear(&mut self) {
        self.entries.clear();
        self.table_to_keys.clear();
        self.insert_order.clear();
        self.tick = 0;
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }
}
