use std::collections::HashMap;
use crate::engine::executor::Row;
use crate::storage::disk::DiskManager;

pub const BUFFER_POOL_SIZE: usize = 64;

#[derive(Debug, Clone)]
pub struct Page {
    pub table_name: String,
    pub rows: Vec<Row>,
    pub is_dirty: bool,
}

/// LRU Buffer Pool.
///
/// 기존 VecDeque 기반 구현은 move_to_front 시 O(n) retain을 사용했다.
/// 이 구현은 각 페이지마다 단조 증가하는 tick을 저장해 히트 시 O(1)로 갱신하고,
/// 교체(eviction)가 필요할 때만 O(n) min-scan을 수행한다.
/// 교체는 pool이 만석일 때 새 페이지 삽입 시에만 발생하므로 전체 히트 경로는 O(1).
pub struct BufferPool {
    /// table_name → (Page, last_access_tick)
    cache: HashMap<String, (Page, u64)>,
    /// 단조 증가 논리 시계
    tick: u64,
    pub capacity: usize,
    pub hit_count: u64,
    pub miss_count: u64,
}

impl BufferPool {
    pub fn new() -> Self {
        Self::with_capacity(BUFFER_POOL_SIZE)
    }

    pub fn with_capacity(capacity: usize) -> Self {
        BufferPool {
            cache: HashMap::new(),
            tick: 0,
            capacity,
            hit_count: 0,
            miss_count: 0,
        }
    }

    /// 페이지 읽기 (캐시 히트: O(1) tick 갱신, 미스: 디스크 로드 후 삽입)
    pub fn get_page(&mut self, table_name: &str, disk: &DiskManager) -> Vec<Row> {
        if self.cache.contains_key(table_name) {
            self.hit_count += 1;
            self.tick += 1;
            let tick = self.tick;
            // 안전: contains_key로 존재 확인 후 get_mut — 패닉 없음
            self.cache.get_mut(table_name).unwrap().1 = tick;
            return self.cache[table_name].0.rows.clone();
        }

        self.miss_count += 1;
        let rows = disk.load_table(table_name);
        self.insert_page(table_name.to_string(), rows.clone(), false, Some(disk));
        rows
    }

    /// 페이지 쓰기 (dirty 마킹, O(1))
    pub fn write_page(&mut self, table_name: &str, rows: Vec<Row>) {
        if self.cache.contains_key(table_name) {
            self.tick += 1;
            let tick = self.tick;
            let entry = self.cache.get_mut(table_name).unwrap();
            entry.0.rows = rows;
            entry.0.is_dirty = true;
            entry.1 = tick;
        } else {
            self.insert_page(table_name.to_string(), rows, true, None);
        }
    }

    /// dirty 페이지를 디스크에 flush
    pub fn flush_page(&mut self, table_name: &str, disk: &DiskManager) {
        if let Some((page, _)) = self.cache.get_mut(table_name) {
            if page.is_dirty {
                disk.save_table(table_name, &page.rows);
                page.is_dirty = false;
            }
        }
    }

    /// 모든 dirty 페이지 flush
    pub fn flush_all(&mut self, disk: &DiskManager) {
        // 선 수집 후 처리 (borrow 충돌 회피)
        let dirty: Vec<String> = self.cache.iter()
            .filter(|(_, (p, _))| p.is_dirty)
            .map(|(k, _)| k.clone())
            .collect();
        for name in dirty {
            if let Some((page, _)) = self.cache.get_mut(&name) {
                disk.save_table(&name, &page.rows);
                page.is_dirty = false;
            }
        }
    }

    /// 특정 테이블 캐시 무효화
    pub fn invalidate(&mut self, table_name: &str) {
        self.cache.remove(table_name);
    }

    /// 캐시 적중률 (%)
    pub fn hit_rate(&self) -> f64 {
        let total = self.hit_count + self.miss_count;
        if total == 0 { return 0.0; }
        (self.hit_count as f64 / total as f64) * 100.0
    }

    /// 현재 캐시 사용량
    pub fn usage(&self) -> usize {
        self.cache.len()
    }

    /// 새 페이지 삽입 (용량 초과 시 LRU 교체)
    ///
    /// 교체 시 최소 tick 항목을 O(n) 스캔으로 찾는다.
    /// 교체는 pool이 만석일 때만 발생하므로 히트 경로에 영향 없음.
    fn insert_page(&mut self, table_name: String, rows: Vec<Row>, is_dirty: bool, disk: Option<&DiskManager>) {
        if self.cache.len() >= self.capacity {
            // LRU: 가장 오래 전에 접근된 항목(최소 tick) 교체
            let evict_key = self.cache.iter()
                .min_by_key(|(_, (_, t))| *t)
                .map(|(k, _)| k.clone());
            if let Some(key) = evict_key {
                if let Some((page, _)) = self.cache.remove(&key) {
                    if page.is_dirty {
                        if let Some(d) = disk {
                            d.save_table(&key, &page.rows);
                        }
                    }
                }
            }
        }
        self.tick += 1;
        let tick = self.tick;
        self.cache.insert(table_name.clone(), (Page { table_name, rows, is_dirty }, tick));
    }
}
