use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::{Write, Read};
use std::path::Path;
use crate::transaction::wal::{WalManager, WalRecord};
use crate::parser::ast::IsolationLevel;

pub type Row = HashMap<String, String>;

const UNDO_LOG_PATH: &str = "data/_undo.log";

#[derive(Debug, Clone)]
pub struct UndoEntry {
    pub operation: String,
    pub table: String,
    pub key: String,
    pub old_data: Option<String>,
}

/// 미완료 트랜잭션의 Undo Log를 디스크에 영속화하는 관리자.
/// 크래시 발생 시 재시작 후 미완료 트랜잭션을 롤백하는 데 사용된다.
struct UndoLogFile {
    path: String,
}

impl UndoLogFile {
    fn new() -> Self {
        UndoLogFile { path: UNDO_LOG_PATH.to_string() }
    }

    /// UndoEntry를 바이너리로 인코딩
    /// [ op(1) | table_len(4) | table | key_len(4) | key | has_data(1) | [data_len(4) | data] ]
    fn encode(entry: &UndoEntry) -> Vec<u8> {
        let op: u8 = match entry.operation.as_str() {
            "INSERT" => 0x01,
            "UPDATE" => 0x02,
            "DELETE" => 0x03,
            _        => 0x00,
        };
        let table_b = entry.table.as_bytes();
        let key_b   = entry.key.as_bytes();
        let mut buf = Vec::new();
        buf.push(op);
        buf.extend_from_slice(&(table_b.len() as u32).to_le_bytes());
        buf.extend_from_slice(table_b);
        buf.extend_from_slice(&(key_b.len() as u32).to_le_bytes());
        buf.extend_from_slice(key_b);
        if let Some(ref data) = entry.old_data {
            buf.push(1u8);
            let data_b = data.as_bytes();
            buf.extend_from_slice(&(data_b.len() as u32).to_le_bytes());
            buf.extend_from_slice(data_b);
        } else {
            buf.push(0u8);
        }
        buf
    }

    fn read_string(buf: &[u8], pos: &mut usize) -> Option<String> {
        if *pos + 4 > buf.len() { return None; }
        let len = u32::from_le_bytes(buf[*pos..*pos+4].try_into().ok()?) as usize;
        *pos += 4;
        if *pos + len > buf.len() { return None; }
        let s = String::from_utf8(buf[*pos..*pos+len].to_vec()).ok()?;
        *pos += len;
        Some(s)
    }

    fn decode(buf: &[u8], pos: &mut usize) -> Option<UndoEntry> {
        if *pos >= buf.len() { return None; }
        let op_byte = buf[*pos]; *pos += 1;
        let operation = match op_byte {
            0x01 => "INSERT",
            0x02 => "UPDATE",
            0x03 => "DELETE",
            _    => return None,
        }.to_string();
        let table = Self::read_string(buf, pos)?;
        let key   = Self::read_string(buf, pos)?;
        if *pos >= buf.len() { return None; }
        let has_data = buf[*pos]; *pos += 1;
        let old_data = if has_data == 1 {
            Some(Self::read_string(buf, pos)?)
        } else {
            None
        };
        Some(UndoEntry { operation, table, key, old_data })
    }

    fn append(&self, entry: &UndoEntry) {
        let encoded = Self::encode(entry);
        let mut file = OpenOptions::new()
            .create(true).append(true)
            .open(&self.path)
            .expect("Undo log 파일 열기 실패");
        file.write_all(&encoded).expect("Undo log 기록 실패");
    }

    fn read_all(&self) -> Vec<UndoEntry> {
        if !Path::new(&self.path).exists() { return vec![]; }
        let mut file = match File::open(&self.path) {
            Ok(f)  => f,
            Err(_) => return vec![],
        };
        let mut buf = Vec::new();
        let _ = file.read_to_end(&mut buf);
        let mut entries = Vec::new();
        let mut pos = 0;
        while let Some(e) = Self::decode(&buf, &mut pos) {
            entries.push(e);
        }
        entries
    }

    fn rewrite(&self, entries: &[UndoEntry]) {
        if Path::new(&self.path).exists() {
            fs::remove_file(&self.path).ok();
        }
        for e in entries {
            self.append(e);
        }
    }

    fn clear(&self) {
        if Path::new(&self.path).exists() {
            fs::remove_file(&self.path).ok();
        }
    }

    fn exists(&self) -> bool {
        Path::new(&self.path).exists()
    }
}

pub struct TransactionManager {
    active: bool,
    txn_id: u64,
    undo_log: Vec<UndoEntry>,
    wal: WalManager,
    undo_log_file: UndoLogFile,
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
            undo_log_file: UndoLogFile::new(),
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
        self.undo_log_file.clear();
        self.snapshot = None;
        self.savepoints.clear();
        self.active = false;
        Ok(())
    }

    pub fn rollback(&mut self) -> Vec<UndoEntry> {
        self.wal.log_rollback();
        self.wal.clear();
        let entries = self.undo_log.drain(..).rev().collect();
        self.undo_log_file.clear();
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
        self.undo_log_file.clear();
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
        // undo log 파일도 savepoint 이전 상태로 재기록
        self.undo_log_file.rewrite(&self.undo_log);
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
            // 트랜잭션 중 → WAL 기록 + Undo Log 추가 (메모리 + 디스크)
            self.wal.log_insert(table, key, data);
            let entry = UndoEntry {
                operation: "INSERT".to_string(),
                table: table.to_string(),
                key: key.to_string(),
                old_data: None,
            };
            self.undo_log_file.append(&entry);
            self.undo_log.push(entry);
        }
        // 트랜잭션 없으면 WAL 기록 안 함 (즉시 flush는 executor에서 처리)
    }

    pub fn log_update(&mut self, table: &str, key: &str, old_data: &str, new_data: &str) {
        if self.active {
            self.wal.log_update(table, key, new_data);
            let entry = UndoEntry {
                operation: "UPDATE".to_string(),
                table: table.to_string(),
                key: key.to_string(),
                old_data: Some(old_data.to_string()),
            };
            self.undo_log_file.append(&entry);
            self.undo_log.push(entry);
        }
    }

    pub fn log_delete(&mut self, table: &str, key: &str, old_data: &str) {
        if self.active {
            self.wal.log_delete(table, key);
            let entry = UndoEntry {
                operation: "DELETE".to_string(),
                table: table.to_string(),
                key: key.to_string(),
                old_data: Some(old_data.to_string()),
            };
            self.undo_log_file.append(&entry);
            self.undo_log.push(entry);
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

    // ── Undo Log 파일 접근자 (크래시 복구용) ────────────────────────────────

    /// 디스크의 Undo Log 파일에 엔트리가 존재하는지 확인
    pub fn has_undo_log_file(&self) -> bool {
        self.undo_log_file.exists()
    }

    /// 디스크의 Undo Log 파일에서 모든 엔트리를 읽어 반환
    pub fn read_undo_log_file(&self) -> Vec<UndoEntry> {
        self.undo_log_file.read_all()
    }

    /// 디스크의 Undo Log 파일을 삭제
    pub fn clear_undo_log_file(&self) {
        self.undo_log_file.clear();
    }
}