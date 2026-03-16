 // src/transaction/wal.rs

use std::fs::{File, OpenOptions};
use std::io::{Write, BufWriter};

#[derive(Debug, Clone)]
pub enum LogRecord {
    Begin(u64),
    Commit(u64),
    Abort(u64),
    Insert { txn_id: u64, table: String, key: String, value: String },
    Update { txn_id: u64, table: String, key: String, old_value: String, new_value: String },
    Delete { txn_id: u64, table: String, key: String, old_value: String },
}

pub struct Wal {
    writer: BufWriter<File>,
}

impl Wal {
    pub fn new(path: &str) -> Self {
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
            .unwrap();
        Wal { writer: BufWriter::new(file) }
    }

    pub fn log(&mut self, record: LogRecord) {
        let line = match &record {
            LogRecord::Begin(id)   => format!("BEGIN {}\n", id),
            LogRecord::Commit(id)  => format!("COMMIT {}\n", id),
            LogRecord::Abort(id)   => format!("ABORT {}\n", id),
            LogRecord::Insert { txn_id, table, key, value } =>
                format!("INSERT {} {} {} {}\n", txn_id, table, key, value),
            LogRecord::Update { txn_id, table, key, old_value, new_value } =>
                format!("UPDATE {} {} {} {} {}\n", txn_id, table, key, old_value, new_value),
            LogRecord::Delete { txn_id, table, key, old_value } =>
                format!("DELETE {} {} {} {}\n", txn_id, table, key, old_value),
        };
        self.writer.write_all(line.as_bytes()).unwrap();
        self.writer.flush().unwrap();
    }
}
