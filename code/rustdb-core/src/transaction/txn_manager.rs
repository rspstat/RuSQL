use crate::transaction::wal::{WalManager, WalRecord, WalOp};
use crate::engine::executor::Row;

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
}

impl TransactionManager {
    pub fn new() -> Self {
        TransactionManager {
            active: false,
            txn_id: 0,
            undo_log: Vec::new(),
            wal: WalManager::new(),
        }
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

    pub fn commit(&mut self) -> Result<(), String> {
        if !self.active {
            return Err("No active transaction.".to_string());
        }
        self.wal.log_commit();
        self.wal.log_checkpoint();
        self.wal.clear();
        self.undo_log.clear();
        self.active = false;
        Ok(())
    }

    pub fn rollback(&mut self) -> Vec<UndoEntry> {
        self.wal.log_rollback();
        self.wal.clear();
        let entries = self.undo_log.drain(..).rev().collect();
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
        self.active = false;
        Ok(entries)
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
}