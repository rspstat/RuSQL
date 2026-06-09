// src/engine/lock_manager.rs
//
// Row-level lock manager with shared/exclusive locks and deadlock detection.
//
// 잠금 종류:
//   Exclusive : FOR UPDATE / DML — 하나의 트랜잭션만 보유
//   Shared    : FOR SHARE       — 여러 트랜잭션이 동시에 보유 가능, 쓰기 잠금과 충돌
//
// 데드락 감지: wait-for 그래프 DFS (기존과 동일)

use std::collections::{HashMap, HashSet};

#[derive(Debug, Clone)]
enum LockEntry {
    Exclusive(u64),
    Shared(HashSet<u64>),
}

/// acquire() / acquire_shared() 의 반환 타입
pub enum LockResult {
    Granted,
    Conflict { holder: u64 },
    Deadlock { holder: u64 },
}

pub struct LockManager {
    /// (table, pk_val) → LockEntry
    row_locks: HashMap<(String, String), LockEntry>,
    /// wait-for 그래프: waiting_txn → blocking_txn
    wait_for: HashMap<u64, u64>,
    /// 데드락 이력
    deadlock_history: Vec<(u64, u64)>,
}

impl LockManager {
    pub fn new() -> Self {
        LockManager {
            row_locks: HashMap::new(),
            wait_for: HashMap::new(),
            deadlock_history: Vec::new(),
        }
    }

    // ── 배타 잠금 (FOR UPDATE / DML) ────────────────────────────

    pub fn acquire(&mut self, table: &str, pk: &str, txn_id: u64) -> LockResult {
        let key = (table.to_string(), pk.to_string());
        match self.row_locks.get(&key) {
            None => {
                self.row_locks.insert(key, LockEntry::Exclusive(txn_id));
                LockResult::Granted
            }
            Some(LockEntry::Exclusive(holder)) if *holder == txn_id => LockResult::Granted,
            Some(LockEntry::Exclusive(holder)) => {
                let holder = *holder;
                if self.creates_cycle(txn_id, holder) {
                    self.deadlock_history.push((txn_id, holder));
                    LockResult::Deadlock { holder }
                } else {
                    self.wait_for.insert(txn_id, holder);
                    LockResult::Conflict { holder }
                }
            }
            Some(LockEntry::Shared(holders)) => {
                // 이 트랜잭션만 공유 잠금을 보유하고 있으면 배타로 업그레이드
                if holders.len() == 1 && holders.contains(&txn_id) {
                    self.row_locks.insert(key, LockEntry::Exclusive(txn_id));
                    return LockResult::Granted;
                }
                // 다른 트랜잭션이 공유 잠금을 보유 중 → 충돌
                let holder = *holders.iter().find(|&&h| h != txn_id).unwrap_or(&txn_id);
                if self.creates_cycle(txn_id, holder) {
                    self.deadlock_history.push((txn_id, holder));
                    LockResult::Deadlock { holder }
                } else {
                    self.wait_for.insert(txn_id, holder);
                    LockResult::Conflict { holder }
                }
            }
        }
    }

    // ── 공유 잠금 (FOR SHARE) ────────────────────────────────────

    pub fn acquire_shared(&mut self, table: &str, pk: &str, txn_id: u64) -> LockResult {
        let key = (table.to_string(), pk.to_string());
        match self.row_locks.get_mut(&key) {
            None => {
                let mut holders = HashSet::new();
                holders.insert(txn_id);
                self.row_locks.insert(key, LockEntry::Shared(holders));
                LockResult::Granted
            }
            Some(LockEntry::Exclusive(holder)) if *holder == txn_id => {
                // 이미 배타 잠금 보유 중 → 재진입
                LockResult::Granted
            }
            Some(LockEntry::Exclusive(holder)) => {
                let holder = *holder;
                if self.creates_cycle(txn_id, holder) {
                    self.deadlock_history.push((txn_id, holder));
                    LockResult::Deadlock { holder }
                } else {
                    self.wait_for.insert(txn_id, holder);
                    LockResult::Conflict { holder }
                }
            }
            Some(LockEntry::Shared(holders)) => {
                holders.insert(txn_id);
                LockResult::Granted
            }
        }
    }

    // ── 잠금 해제 ────────────────────────────────────────────────

    pub fn release(&mut self, txn_id: u64) {
        self.row_locks.retain(|_, entry| match entry {
            LockEntry::Exclusive(holder) => *holder != txn_id,
            LockEntry::Shared(holders) => {
                holders.remove(&txn_id);
                !holders.is_empty()
            }
        });
        self.wait_for.retain(|waiter, blocking| {
            *waiter != txn_id && *blocking != txn_id
        });
    }

    /// FOR UPDATE 즉시 삽입 (검증 없이)
    pub fn insert_lock(&mut self, table: &str, pk: &str, txn_id: u64) {
        self.row_locks.insert(
            (table.to_string(), pk.to_string()),
            LockEntry::Exclusive(txn_id),
        );
    }

    /// 배타 잠금 보유자 조회 (충돌 검사용)
    pub fn holder(&self, table: &str, pk: &str) -> Option<u64> {
        match self.row_locks.get(&(table.to_string(), pk.to_string())) {
            Some(LockEntry::Exclusive(h)) => Some(*h),
            _ => None,
        }
    }

    pub fn lock_rows(&self) -> Vec<(String, String, u64)> {
        let mut v: Vec<_> = self.row_locks.iter().flat_map(|((t, k), entry)| {
            match entry {
                LockEntry::Exclusive(h) => vec![(t.clone(), k.clone(), *h)],
                LockEntry::Shared(hs) => hs.iter().map(|&h| (t.clone(), k.clone(), h)).collect(),
            }
        }).collect();
        v.sort_by(|a, b| a.0.cmp(&b.0).then(a.1.cmp(&b.1)));
        v
    }

    pub fn wait_for_rows(&self) -> Vec<(u64, u64)> {
        let mut v: Vec<_> = self.wait_for.iter().map(|(&w, &b)| (w, b)).collect();
        v.sort();
        v
    }

    pub fn deadlock_history(&self) -> &Vec<(u64, u64)> {
        &self.deadlock_history
    }

    pub fn is_empty(&self) -> bool {
        self.row_locks.is_empty()
    }

    fn creates_cycle(&self, from: u64, to: u64) -> bool {
        let mut current = to;
        let mut visited = HashSet::new();
        loop {
            if current == from { return true; }
            if !visited.insert(current) { return false; }
            match self.wait_for.get(&current) {
                Some(&next) => current = next,
                None => return false,
            }
        }
    }
}


#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_grant_lock() {
        let mut lm = LockManager::new();
        assert!(matches!(lm.acquire("t", "1", 1), LockResult::Granted));
    }

    #[test]
    fn test_reentrant_lock() {
        let mut lm = LockManager::new();
        lm.acquire("t", "1", 1);
        assert!(matches!(lm.acquire("t", "1", 1), LockResult::Granted));
    }

    #[test]
    fn test_conflict() {
        let mut lm = LockManager::new();
        lm.acquire("t", "1", 1);
        assert!(matches!(lm.acquire("t", "1", 2), LockResult::Conflict { holder: 1 }));
    }

    #[test]
    fn test_deadlock_detection() {
        let mut lm = LockManager::new();
        lm.acquire("t", "row1", 1);
        lm.acquire("t", "row2", 2);
        assert!(matches!(lm.acquire("t", "row2", 1), LockResult::Conflict { holder: 2 }));
        assert!(matches!(lm.acquire("t", "row1", 2), LockResult::Deadlock { holder: 1 }));
        assert_eq!(lm.deadlock_history().len(), 1);
        assert_eq!(lm.deadlock_history()[0], (2, 1));
    }

    #[test]
    fn test_release() {
        let mut lm = LockManager::new();
        lm.acquire("t", "1", 1);
        lm.acquire("t", "2", 1);
        lm.release(1);
        assert!(lm.is_empty());
        assert!(lm.wait_for_rows().is_empty());
    }

    #[test]
    fn test_shared_lock_multi_readers() {
        let mut lm = LockManager::new();
        assert!(matches!(lm.acquire_shared("t", "1", 1), LockResult::Granted));
        assert!(matches!(lm.acquire_shared("t", "1", 2), LockResult::Granted));
        assert!(matches!(lm.acquire_shared("t", "1", 3), LockResult::Granted));
    }

    #[test]
    fn test_shared_exclusive_conflict() {
        let mut lm = LockManager::new();
        lm.acquire_shared("t", "1", 1);
        lm.acquire_shared("t", "1", 2);
        // 배타 잠금은 공유 잠금(다수)과 충돌
        assert!(matches!(lm.acquire("t", "1", 3), LockResult::Conflict { .. }));
    }

    #[test]
    fn test_exclusive_blocks_shared() {
        let mut lm = LockManager::new();
        lm.acquire("t", "1", 1);
        assert!(matches!(lm.acquire_shared("t", "1", 2), LockResult::Conflict { holder: 1 }));
    }
}
