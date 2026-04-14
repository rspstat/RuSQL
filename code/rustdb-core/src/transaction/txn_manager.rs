use std::collections::HashMap;
use crate::transaction::wal::{WalManager, WalRecord};
use crate::parser::ast::IsolationLevel;

pub type Row = HashMap<String, String>;

#[derive(Debug, Clone)]
pub struct UndoEntry {
    pub operation: String,
    pub table: String,
    pub key: String,
    pub old_data: Option<String>,
}

pub struct TransactionManager {
    active: bool,
    txn_id: u64,
    undo_log: Vec<UndoEntry>,
    wal: WalManager,
    /// 현재 세션의 격리 수준 (BEGIN 전에 설정)
    pub isolation_level: IsolationLevel,
    /// REPEATABLE READ / SERIALIZABLE: BEGIN 시점의 테이블 스냅샷
    snapshot: Option<HashMap<String, Vec<Row>>>,
    /// SAVEPOINT 스택: (이름, undo_log 길이)
    savepoints: Vec<(String, usize)>,
}

impl TransactionManager {
    pub fn new() -> Self {
        TransactionManager {
            active: false,
            txn_id: 0,
            undo_log: Vec::new(),
            wal: WalManager::new(),
            isolation_level: IsolationLevel::ReadCommitted,
            snapshot: None,
            savepoints: Vec::new(),
        }
    }

    /// 현재 트랜잭션 ID 반환. 트랜잭션 밖이면 0
    pub fn current_txn_id(&self) -> u64 {
        if self.active { self.txn_id } else { 0 }
    }

    pub fn set_isolation_level(&mut self, level: IsolationLevel) {
        if self.active {
            eprintln!("[TxnManager] 경고: 활성 트랜잭션 중 격리 수준 변경은 다음 트랜잭션부터 적용됩니다.");
        }
        self.isolation_level = level;
    }

    /// BEGIN 시 호출: REPEATABLE READ 이상이면 스냅샷을 저장
    pub fn begin_with_snapshot(&mut self, tables: &HashMap<String, Vec<Row>>) -> Result<u64, String> {
        if self.active {
            return Err("Transaction already active. COMMIT or ROLLBACK first.".to_string());
        }
        self.txn_id += 1;
        self.active = true;
        self.undo_log.clear();

        self.snapshot = match self.isolation_level {
            IsolationLevel::RepeatableRead | IsolationLevel::Serializable => {
                Some(tables.clone())
            }
            _ => None,
        };

        Ok(self.txn_id)
    }

    /// SELECT 시 사용할 테이블 데이터를 반환
    /// REPEATABLE READ+ 이면 스냅샷, 아니면 None (live 테이블 사용)
    pub fn get_snapshot_table(&self, table: &str) -> Option<&Vec<Row>> {
        match self.isolation_level {
            IsolationLevel::RepeatableRead | IsolationLevel::Serializable => {
                self.snapshot.as_ref()?.get(table)
            }
            _ => None,
        }
    }

    /// SERIALIZABLE: 커밋 전 스냅샷과 현재 테이블 상태를 비교
    /// 행 수가 달라졌으면 팬텀 읽기로 간주해 실패
    pub fn validate_serializable(&self, live_tables: &HashMap<String, Vec<Row>>) -> Result<(), String> {
        if self.isolation_level != IsolationLevel::Serializable {
            return Ok(());
        }
        if let Some(snapshot) = &self.snapshot {
            for (table, snap_rows) in snapshot {
                if let Some(live_rows) = live_tables.get(table) {
                    if live_rows.len() != snap_rows.len() {
                        return Err(format!(
                            "Serialization failure: table '{}' was modified since transaction started. ROLLBACK required.",
                            table
                        ));
                    }
                }
            }
        }
        Ok(())
    }

    pub fn begin(&mut self) -> Result<u64, String> {
        if self.active {
            return Err("Transaction already active. COMMIT or ROLLBACK first.".to_string());
        }
        self.txn_id += 1;
        self.active = true;
        self.undo_log.clear();
        Ok(self.txn_id)
    }

    /// 트랜잭션 중 수정된 테이블 목록 반환 (커밋 전 플러시용)
    pub fn dirty_tables(&self) -> Vec<String> {
        let mut tables: Vec<String> = self.undo_log.iter()
            .map(|e| e.table.clone())
            .collect();
        tables.sort();
        tables.dedup();
        tables
    }

    pub fn commit(&mut self) -> Result<(), String> {
        if !self.active {
            return Err("No active transaction.".to_string());
        }
        self.wal.log_commit();
        self.wal.log_checkpoint();
        self.wal.clear();
        self.undo_log.clear();
        self.snapshot = None;
        self.savepoints.clear();
        self.active = false;
        Ok(())
    }

    pub fn rollback(&mut self) -> Vec<UndoEntry> {
        self.wal.log_rollback();
        self.wal.clear();
        let entries = self.undo_log.drain(..).rev().collect();
        self.snapshot = None;
        self.savepoints.clear();
        self.active = false;
        entries
    }

    pub fn abort(&mut self) -> Result<Vec<UndoEntry>, String> {
        if !self.active {
            return Err("No active transaction.".to_string());
        }
        self.wal.log_rollback();
        self.wal.clear();
        let entries = self.undo_log.drain(..).rev().collect();
        self.snapshot = None;
        self.savepoints.clear();
        self.active = false;
        Ok(entries)
    }

    /// SAVEPOINT name — 현재 undo_log 길이를 저장
    pub fn create_savepoint(&mut self, name: &str) -> Result<(), String> {
        if !self.active {
            return Err("No active transaction. Use BEGIN first.".to_string());
        }
        // 동일 이름이 있으면 덮어씀 (MySQL 동작과 동일)
        self.savepoints.retain(|(n, _)| n != name);
        self.savepoints.push((name.to_string(), self.undo_log.len()));
        Ok(())
    }

    /// ROLLBACK TO name — savepoint 이후의 undo 엔트리 반환 (역순)
    pub fn rollback_to_savepoint(&mut self, name: &str) -> Result<Vec<UndoEntry>, String> {
        if !self.active {
            return Err("No active transaction.".to_string());
        }
        let pos = self.savepoints.iter().rposition(|(n, _)| n == name)
            .ok_or(format!("Savepoint '{}' not found", name))?;
        let (_, undo_len) = self.savepoints[pos].clone();
        // savepoint 이후에 기록된 undo 엔트리를 역순으로 꺼냄
        let entries: Vec<UndoEntry> = self.undo_log[undo_len..].iter().cloned().rev().collect();
        self.undo_log.truncate(undo_len);
        // savepoint 이후의 savepoint들 제거 (중첩 savepoint 처리)
        self.savepoints.truncate(pos + 1);
        Ok(entries)
    }

    /// RELEASE SAVEPOINT name — savepoint 삭제
    pub fn release_savepoint(&mut self, name: &str) -> Result<(), String> {
        if !self.active {
            return Err("No active transaction.".to_string());
        }
        let pos = self.savepoints.iter().rposition(|(n, _)| n == name)
            .ok_or(format!("Savepoint '{}' not found", name))?;
        self.savepoints.remove(pos);
        Ok(())
    }

    pub fn log_insert(&mut self, table: &str, key: &str, data: &str) {
        if self.active {
            // 트랜잭션 중 → WAL 기록 + Undo Log 추가
            self.wal.log_insert(table, key, data);
            self.undo_log.push(UndoEntry {
                operation: "INSERT".to_string(),
                table: table.to_string(),
                key: key.to_string(),
                old_data: None,
            });
        }
        // 트랜잭션 없으면 WAL 기록 안 함 (즉시 flush는 executor에서 처리)
    }

    pub fn log_update(&mut self, table: &str, key: &str, old_data: &str, new_data: &str) {
        if self.active {
            self.wal.log_update(table, key, new_data);
            self.undo_log.push(UndoEntry {
                operation: "UPDATE".to_string(),
                table: table.to_string(),
                key: key.to_string(),
                old_data: Some(old_data.to_string()),
            });
        }
    }

    pub fn log_delete(&mut self, table: &str, key: &str, old_data: &str) {
        if self.active {
            self.wal.log_delete(table, key);
            self.undo_log.push(UndoEntry {
                operation: "DELETE".to_string(),
                table: table.to_string(),
                key: key.to_string(),
                old_data: Some(old_data.to_string()),
            });
        }
    }

    pub fn is_active(&self) -> bool {
        self.active
    }

    pub fn txn_id(&self) -> u64 {
        self.txn_id
    }

    pub fn wal_records(&self) -> Vec<WalRecord> {
        self.wal.read_all()
    }

    pub fn wal_size(&self) -> u64 {
        self.wal.file_size()
    }

    pub fn wal_clear(&self) {
        self.wal.clear();
    }

    /// 명시적 체크포인트:
    /// WAL에 CHECKPOINT 레코드를 기록하고, 이전 커밋된 레코드를 정리한다.
    /// 버퍼풀 flush는 호출 전에 executor가 직접 수행해야 한다.
    pub fn do_checkpoint(&mut self) {
        self.wal.log_checkpoint();
        self.wal.truncate_to_last_checkpoint();
    }

    /// WAL 크기가 자동 체크포인트 임계값을 초과했는지 확인
    pub fn needs_auto_checkpoint(&self) -> bool {
        self.wal.needs_auto_checkpoint()
    }
}