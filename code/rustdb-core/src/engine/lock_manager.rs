// src/engine/lock_manager.rs
//
// Row-level lock manager with wait-for graph deadlock detection.
//
// 구조:
//   row_locks  : (table, pk) → txn_id (잠금 보유자)
//   wait_for   : txn_id → txn_id (대기 중인 트랜잭션 → 블로킹 트랜잭션)
//
// 데드락 감지:
//   acquire() 호출 시 wait_for 에 엣지를 추가하기 전에
//   wait_for 그래프에서 사이클 유무를 DFS 로 확인한다.
//   사이클이 감지되면 요청 트랜잭션을 희생자(victim)로 선택하고
//   DeadlockError 를 반환한다.

use std::collections::{HashMap, HashSet};

/// acquire() 의 반환 타입
pub enum LockResult {
    /// 잠금 획득 성공
    Granted,
    /// 다른 트랜잭션이 보유 중 → 대기 필요 (단순 충돌, 데드락 아님)
    Conflict { holder: u64 },
    /// 데드락 감지 → 호출 트랜잭션을 즉시 중단해야 함
    Deadlock { holder: u64 },
}

pub struct LockManager {
    /// (table, pk_val) → 잠금 보유 txn_id
    row_locks: HashMap<(String, String), u64>,
    /// wait-for 그래프: waiting_txn → blocking_txn
    wait_for: HashMap<u64, u64>,
    /// 데드락 이력: (victim_txn, blocker_txn)
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

    /// 행 잠금 획득 시도.
    ///
    /// - 잠금이 없으면 즉시 획득 → Granted
    /// - 이미 같은 txn이 보유 → Granted (재진입)
    /// - 다른 txn이 보유 중이면 wait-for 사이클 검사:
    ///     사이클 있음 → Deadlock (victim: 요청 txn)
    ///     사이클 없음 → Conflict (wait_for 엣지 추가)
    pub fn acquire(&mut self, table: &str, pk: &str, txn_id: u64) -> LockResult {
        let key = (table.to_string(), pk.to_string());
        match self.row_locks.get(&key).copied() {
            None => {
                self.row_locks.insert(key, txn_id);
                LockResult::Granted
            }
            Some(holder) if holder == txn_id => LockResult::Granted,
            Some(holder) => {
                // wait_for[txn_id] = holder 를 추가하면 사이클이 생기는지 확인
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

    /// txn_id 가 보유한 모든 잠금과 wait-for 엣지를 해제한다.
    /// 커밋/롤백 시 호출.
    pub fn release(&mut self, txn_id: u64) {
        self.row_locks.retain(|_, holder| *holder != txn_id);
        // 해제된 트랜잭션을 기다리던 대기 엣지도 제거
        self.wait_for.retain(|waiter, blocking| {
            *waiter != txn_id && *blocking != txn_id
        });
    }

    /// 특정 잠금 키에 대한 보유자를 직접 삽입 (FOR UPDATE 용).
    pub fn insert_lock(&mut self, table: &str, pk: &str, txn_id: u64) {
        self.row_locks.insert((table.to_string(), pk.to_string()), txn_id);
    }

    /// 잠금 보유자 조회 (충돌 검사용)
    pub fn holder(&self, table: &str, pk: &str) -> Option<u64> {
        self.row_locks.get(&(table.to_string(), pk.to_string())).copied()
    }

    /// SHOW LOCKS 용: 현재 보유 잠금 목록
    pub fn lock_rows(&self) -> Vec<(String, String, u64)> {
        let mut v: Vec<_> = self.row_locks.iter()
            .map(|((t, k), &txn)| (t.clone(), k.clone(), txn))
            .collect();
        v.sort_by(|a, b| a.0.cmp(&b.0).then(a.1.cmp(&b.1)));
        v
    }

    /// SHOW LOCKS 용: wait-for 그래프 엣지 목록
    pub fn wait_for_rows(&self) -> Vec<(u64, u64)> {
        let mut v: Vec<_> = self.wait_for.iter()
            .map(|(&w, &b)| (w, b))
            .collect();
        v.sort();
        v
    }

    /// 데드락 감지 이력
    pub fn deadlock_history(&self) -> &Vec<(u64, u64)> {
        &self.deadlock_history
    }

    pub fn is_empty(&self) -> bool {
        self.row_locks.is_empty()
    }

    // ── 내부 유틸 ──────────────────────────────────────────────────────────

    /// wait_for[from] = to 를 추가했을 때 사이클이 생기는지 DFS 로 검사.
    /// from 에서 시작해 to 를 따라가다가 다시 from 에 도달하면 사이클.
    fn creates_cycle(&self, from: u64, to: u64) -> bool {
        let mut current = to;
        let mut visited = HashSet::new();
        loop {
            if current == from {
                return true;
            }
            if !visited.insert(current) {
                // 이미 방문한 노드 → 더 이상 진행 불가, 사이클 없음
                return false;
            }
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
        // txn 2가 txn 1이 보유한 잠금을 요청 → Conflict
        assert!(matches!(lm.acquire("t", "1", 2), LockResult::Conflict { holder: 1 }));
    }

    #[test]
    fn test_deadlock_detection() {
        let mut lm = LockManager::new();
        // txn1 → "row1" 잠금 획득
        lm.acquire("t", "row1", 1);
        // txn2 → "row2" 잠금 획득
        lm.acquire("t", "row2", 2);
        // txn1 → "row2" 요청 → txn2 가 보유 → Conflict (wait_for[1] = 2)
        assert!(matches!(lm.acquire("t", "row2", 1), LockResult::Conflict { holder: 2 }));
        // txn2 → "row1" 요청 → txn1 이 보유 → wait_for[2]=1 추가 시 1→2→1 사이클 → Deadlock
        assert!(matches!(lm.acquire("t", "row1", 2), LockResult::Deadlock { holder: 1 }));
        // 이력에 기록되었는지 확인
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
}
