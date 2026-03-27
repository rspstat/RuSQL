use std::collections::HashMap;
use std::collections::VecDeque;
use crate::engine::executor::Row;
use crate::storage::disk::DiskManager;

pub const BUFFER_POOL_SIZE: usize = 64; // 최대 64 페이지 캐시

#[derive(Debug, Clone)]
pub struct Page {
    pub table_name: String,
    pub rows: Vec<Row>,
    pub is_dirty: bool, // 수정됐지만 디스크에 안 쓴 상태
}

pub struct BufferPool {
    /// 캐시된 페이지 (table_name → Page)
    cache: HashMap<String, Page>,
    /// LRU 순서 추적 (앞 = 최근, 뒤 = 오래됨)
    lru_queue: VecDeque<String>,
    /// 최대 캐시 크기
    capacity: usize,
    /// 통계
    pub hit_count: u64,
    pub miss_count: u64,
}

impl BufferPool {
    pub fn new() -> Self {
        BufferPool {
            cache: HashMap::new(),
            lru_queue: VecDeque::new(),
            capacity: BUFFER_POOL_SIZE,
            hit_count: 0,
            miss_count: 0,
        }
    }

    pub fn with_capacity(capacity: usize) -> Self {
        BufferPool {
            cache: HashMap::new(),
            lru_queue: VecDeque::new(),
            capacity,
            hit_count: 0,
            miss_count: 0,
        }
    }

    /// 페이지 읽기 (캐시 히트 or 디스크에서 로드)
    pub fn get_page(&mut self, table_name: &str, disk: &DiskManager) -> Vec<Row> {
        if self.cache.contains_key(table_name) {
            // 캐시 히트
            self.hit_count += 1;
            self.move_to_front(table_name);
            return self.cache[table_name].rows.clone();
        }

        // 캐시 미스 → 디스크에서 로드
        self.miss_count += 1;
        let rows = disk.load_table(table_name);
        self.load_page(table_name.to_string(), rows.clone(), false);
        rows
    }

    /// 페이지 쓰기 (dirty 마킹)
    pub fn write_page(&mut self, table_name: &str, rows: Vec<Row>) {
        if self.cache.contains_key(table_name) {
            self.move_to_front(table_name);
            let page = self.cache.get_mut(table_name).unwrap();
            page.rows = rows;
            page.is_dirty = true;
        } else {
            self.load_page(table_name.to_string(), rows, true);
        }
    }

    /// dirty 페이지를 디스크에 flush
    pub fn flush_page(&mut self, table_name: &str, disk: &DiskManager) {
        if let Some(page) = self.cache.get_mut(table_name) {
            if page.is_dirty {
                disk.save_table(table_name, &page.rows);
                page.is_dirty = false;
            }
        }
    }

    /// 모든 dirty 페이지 flush
    pub fn flush_all(&mut self, disk: &DiskManager) {
        let dirty_tables: Vec<String> = self.cache.iter()
            .filter(|(_, p)| p.is_dirty)
            .map(|(k, _)| k.clone())
            .collect();

        for table_name in dirty_tables {
            let rows = self.cache[&table_name].rows.clone();
            disk.save_table(&table_name, &rows);
            self.cache.get_mut(&table_name).unwrap().is_dirty = false;
        }
    }

    /// 특정 테이블 캐시 무효화
    pub fn invalidate(&mut self, table_name: &str) {
        self.cache.remove(table_name);
        self.lru_queue.retain(|t| t != table_name);
    }

    /// 캐시 적중률
    pub fn hit_rate(&self) -> f64 {
        let total = self.hit_count + self.miss_count;
        if total == 0 { return 0.0; }
        (self.hit_count as f64 / total as f64) * 100.0
    }

    /// 현재 캐시 사용량
    pub fn usage(&self) -> usize {
        self.cache.len()
    }

    /// 캐시에 페이지 로드 (LRU 관리 포함)
    fn load_page(&mut self, table_name: String, rows: Vec<Row>, is_dirty: bool) {
        // 용량 초과 시 LRU 제거
        while self.cache.len() >= self.capacity {
            if let Some(evict) = self.lru_queue.pop_back() {
                // dirty 페이지는 제거 전 flush 필요 (여기선 단순히 제거)
                self.cache.remove(&evict);
            }
        }

        self.lru_queue.push_front(table_name.clone());
        self.cache.insert(table_name.clone(), Page {
            table_name,
            rows,
            is_dirty,
        });
    }

    /// LRU 큐에서 해당 테이블을 앞으로 이동
    fn move_to_front(&mut self, table_name: &str) {
        self.lru_queue.retain(|t| t != table_name);
        self.lru_queue.push_front(table_name.to_string());
    }
}