 // src/transaction/txn_manager.rs

use std::collections::HashMap;
use crate::transaction::wal::{Wal, LogRecord};
use crate::engine::executor::Row;

#[derive(Debug, Clone)]
pub enum UndoOp {
    Insert { table: String, key: String },
    Update { table: String, key: String, old_value: Row },
    Delete { table: String, key: String, old_value: Row },
}

pub struct TransactionManager {
    pub next_txn_id: u64,
    pub active_txn: Option<u64>,
    pub undo_log: Vec<UndoOp>,
    pub wal: Wal,
}

impl TransactionManager {
    pub fn new() -> Self {
        TransactionManager {
            next_txn_id: 1,
            active_txn: None,
            undo_log: Vec::new(),
            wal: Wal::new("data/wal.log"),
        }
    }

    pub fn begin(&mut self) -> Result<u64, String> {
        if self.active_txn.is_some() {
            return Err("Transaction already active. COMMIT or ROLLBACK first.".to_string());
        }
        let txn_id = self.next_txn_id;
        self.next_txn_id += 1;
        self.active_txn = Some(txn_id);
        self.undo_log.clear();
        self.wal.log(LogRecord::Begin(txn_id));
        Ok(txn_id)
    }

    pub fn commit(&mut self) -> Result<(), String> {
        let txn_id = self.active_txn
            .ok_or("No active transaction.".to_string())?;
        self.wal.log(LogRecord::Commit(txn_id));
        self.active_txn = None;
        self.undo_log.clear();
        Ok(())
    }

    pub fn abort(&mut self) -> Result<Vec<UndoOp>, String> {
        let txn_id = self.active_txn
            .ok_or("No active transaction.".to_string())?;
        self.wal.log(LogRecord::Abort(txn_id));
        self.active_txn = None;
        let ops = self.undo_log.drain(..).rev().collect();
        Ok(ops)
    }

    pub fn log_insert(&mut self, table: &str, key: &str, value: &str) {
        if let Some(txn_id) = self.active_txn {
            self.wal.log(LogRecord::Insert {
                txn_id,
                table: table.to_string(),
                key: key.to_string(),
                value: value.to_string(),
            });
            self.undo_log.push(UndoOp::Insert {
                table: table.to_string(),
                key: key.to_string(),
            });
        }
    }

    pub fn log_update(&mut self, table: &str, key: &str, old_value: Row, new_value: &str) {
        if let Some(txn_id) = self.active_txn {
            self.wal.log(LogRecord::Update {
                txn_id,
                table: table.to_string(),
                key: key.to_string(),
                old_value: serde_json::to_string(&old_value).unwrap(),
                new_value: new_value.to_string(),
            });
            self.undo_log.push(UndoOp::Update {
                table: table.to_string(),
                key: key.to_string(),
                old_value,
            });
        }
    }

    pub fn log_delete(&mut self, table: &str, key: &str, old_value: Row) {
        if let Some(txn_id) = self.active_txn {
            self.wal.log(LogRecord::Delete {
                txn_id,
                table: table.to_string(),
                key: key.to_string(),
                old_value: serde_json::to_string(&old_value).unwrap(),
            });
            self.undo_log.push(UndoOp::Delete {
                table: table.to_string(),
                key: key.to_string(),
                old_value,
            });
        }
    }

    pub fn is_active(&self) -> bool {
        self.active_txn.is_some()
    }
}
