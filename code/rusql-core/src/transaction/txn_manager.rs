use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::{Write, Read};
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicU64, Ordering};
use crate::transaction::wal::{WalManager, WalRecord};
use crate::parser::ast::IsolationLevel;

pub type Row = HashMap<String, String>;

const UNDO_LOG_PATH: &str = "data/_undo.log";

/// 같은 data_dir을 공유하는 모든 세션(TransactionManager)이 함께 사용하는 상태.
/// - `next_txn_id`: 전역 유일 트랜잭션 ID 발급 (세션별 로컬 카운터였던 과거 구현은
///   서로 다른 세션의 트랜잭션이 같은 ID를 받아 lock_mgr/`_xmin` 태깅이 충돌하는 문제가 있었음)
/// - `wal_lock` / `undo_lock`: WAL·Undo 파일은 여러 세션이 물리적으로 공유하는 파일이므로,
///   각 세션의 commit/rollback이 다른 세션의 진행 중인 트랜잭션 레코드를 읽다가 서로 덮어쓰지
///   않도록 read-modify-write 전체를 직렬화한다.
pub struct TxnIoShared {
    next_txn_id: AtomicU64,
    pub wal_lock:  Mutex<()>,
    pub undo_lock: Mutex<()>,
}

impl TxnIoShared {
    pub fn new() -> Self {
        TxnIoShared {
            next_txn_id: AtomicU64::new(1),
            wal_lock: Mutex::new(()),
            undo_lock: Mutex::new(()),
        }
    }

    pub fn next_id(&self) -> u64 {
        self.next_txn_id.fetch_add(1, Ordering::SeqCst)
    }
}

#[derive(Debug, Clone)]
pub struct UndoEntry {
    /// 이 엔트리를 발생시킨 트랜잭션의 전역 유일 ID.
    pub txn_id: u64,
    pub operation: String,
    pub table: String,
    pub key: String,
    pub old_data: Option<String>,
}

/// 미완료 트랜잭션의 Undo Log를 디스크에 영속화하는 관리자.
/// 크래시 발생 시 재시작 후 미완료 트랜잭션을 롤백하는 데 사용된다.
struct UndoLogFile {
    path: String,
    io: Arc<TxnIoShared>,
}

impl UndoLogFile {
    fn new_with_dir(dir: &str, io: Arc<TxnIoShared>) -> Self {
        UndoLogFile { path: format!("{}/_undo.log", dir), io }
    }

    /// UndoEntry를 바이너리로 인코딩
    /// [ op(1) | txn_id(8,LE) | table_len(4) | table | key_len(4) | key | has_data(1) | [data_len(4) | data] ]
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
        buf.extend_from_slice(&entry.txn_id.to_le_bytes());
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
        if *pos + 8 > buf.len() { return None; }
        let txn_id = u64::from_le_bytes(buf[*pos..*pos+8].try_into().ok()?);
        *pos += 8;
        let table = Self::read_string(buf, pos)?;
        let key   = Self::read_string(buf, pos)?;
        if *pos >= buf.len() { return None; }
        let has_data = buf[*pos]; *pos += 1;
        let old_data = if has_data == 1 {
            Some(Self::read_string(buf, pos)?)
        } else {
            None
        };
        Some(UndoEntry { txn_id, operation, table, key, old_data })
    }

    /// 호출자가 io.undo_lock을 이미 보유하고 있어야 한다.
    fn append_locked(&self, entry: &UndoEntry) {
        let encoded = Self::encode(entry);
        let mut file = OpenOptions::new()
            .create(true).append(true)
            .open(&self.path)
            .expect("Undo log 파일 열기 실패");
        file.write_all(&encoded).expect("Undo log 기록 실패");
    }

    fn append(&self, entry: &UndoEntry) {
        let _g = self.io.undo_lock.lock().unwrap();
        self.append_locked(entry);
    }

    /// 호출자가 io.undo_lock을 이미 보유하고 있어야 한다.
    fn read_all_locked(&self) -> Vec<UndoEntry> {
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

    fn read_all(&self) -> Vec<UndoEntry> {
        let _g = self.io.undo_lock.lock().unwrap();
        self.read_all_locked()
    }

    fn clear_locked(&self) {
        if Path::new(&self.path).exists() {
            fs::remove_file(&self.path).ok();
        }
    }

    /// Undo 파일 전체 삭제. 서버 부팅 시 1회 실행되는 크래시 복구 완료 후에만 안전 —
    /// 그 외에는 다른 세션의 진행 중인 트랜잭션 undo 기록까지 함께 사라진다.
    fn clear(&self) {
        let _g = self.io.undo_lock.lock().unwrap();
        self.clear_locked();
    }

    fn exists(&self) -> bool {
        Path::new(&self.path).exists()
    }

    /// 지정한 트랜잭션의 엔트리만 제거한다 (commit/rollback/abort 시 clear() 대신 사용 —
    /// 같은 파일을 공유하는 다른 세션의 엔트리를 보존한다).
    fn remove_txn(&self, txn_id: u64) {
        let _g = self.io.undo_lock.lock().unwrap();
        let remaining: Vec<UndoEntry> = self.read_all_locked()
            .into_iter()
            .filter(|e| e.txn_id != txn_id)
            .collect();
        self.clear_locked();
        for e in &remaining {
            self.append_locked(e);
        }
    }

    /// SAVEPOINT 롤백 후: 이 트랜잭션이 파일에 남긴 기존 엔트리를 지우고 최신 상태(entries)로
    /// 교체한다. 다른 세션의 엔트리는 그대로 보존한다 (과거 rewrite()의 전체 파괴 문제 대체).
    fn rewrite_txn(&self, txn_id: u64, entries: &[UndoEntry]) {
        let _g = self.io.undo_lock.lock().unwrap();
        let mut all: Vec<UndoEntry> = self.read_all_locked()
            .into_iter()
            .filter(|e| e.txn_id != txn_id)
            .collect();
        all.extend(entries.iter().cloned());
        self.clear_locked();
        for e in &all {
            self.append_locked(e);
        }
    }
}

pub struct TransactionManager {
    active: bool,
    txn_id: u64,
    /// 세션 간 공유되는 전역 txn_id 발급기 + WAL/Undo 파일 접근 락.
    io: Arc<TxnIoShared>,
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
        Self::new_with_shared("data", Arc::new(TxnIoShared::new()))
    }

    pub fn new_with_dir(dir: &str) -> Self {
        Self::new_with_shared(dir, Arc::new(TxnIoShared::new()))
    }

    /// 같은 data_dir을 공유하는 다른 세션들과 `io`(전역 txn_id 발급기 + 파일 락)를 공유한다.
    pub fn new_with_shared(dir: &str, io: Arc<TxnIoShared>) -> Self {
        TransactionManager {
            active: false,
            txn_id: 0,
            wal: WalManager::new_with_dir(dir, Arc::clone(&io)),
            undo_log_file: UndoLogFile::new_with_dir(dir, Arc::clone(&io)),
            io,
            undo_log: Vec::new(),
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
        self.txn_id = self.io.next_id();
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
        self.txn_id = self.io.next_id();
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
        self.wal.log_commit(self.txn_id);
        self.wal.remove_txn(self.txn_id);
        self.undo_log.clear();
        self.undo_log_file.remove_txn(self.txn_id);
        self.snapshot = None;
        self.savepoints.clear();
        self.active = false;
        Ok(())
    }

    /// Group Commit Phase 1: COMMIT 레코드를 WAL에 기록 (fsync 없음).
    /// 호출 후 GroupCommitCoordinator::sync_commit()으로 fsync를 완료해야 한다.
    pub fn commit_write_record(&mut self) -> Result<(), String> {
        if !self.active {
            return Err("No active transaction.".to_string());
        }
        self.wal.log_commit_no_sync(self.txn_id);
        Ok(())
    }

    /// Group Commit Phase 3: fsync 완료 후 상태 정리.
    /// 이 트랜잭션이 남긴 WAL/Undo 레코드만 제거하고, 다른 세션의 진행 중인 트랜잭션
    /// 레코드는 보존한다.
    pub fn commit_finalize(&mut self) {
        self.wal.remove_txn(self.txn_id);
        self.undo_log.clear();
        self.undo_log_file.remove_txn(self.txn_id);
        self.snapshot = None;
        self.savepoints.clear();
        self.active = false;
    }

    pub fn rollback(&mut self) -> Vec<UndoEntry> {
        self.wal.log_rollback(self.txn_id);
        self.wal.remove_txn(self.txn_id);
        let entries = self.undo_log.drain(..).rev().collect();
        self.undo_log_file.remove_txn(self.txn_id);
        self.snapshot = None;
        self.savepoints.clear();
        self.active = false;
        entries
    }

    pub fn abort(&mut self) -> Result<Vec<UndoEntry>, String> {
        if !self.active {
            return Err("No active transaction.".to_string());
        }
        self.wal.log_rollback(self.txn_id);
        self.wal.remove_txn(self.txn_id);
        let entries = self.undo_log.drain(..).rev().collect();
        self.undo_log_file.remove_txn(self.txn_id);
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
        // undo log 파일도 savepoint 이전 상태로 재기록 (이 트랜잭션 몫만 교체, 다른 세션은 보존)
        self.undo_log_file.rewrite_txn(self.txn_id, &self.undo_log);
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
            self.wal.log_insert(self.txn_id, table, key, data);
            let entry = UndoEntry {
                txn_id: self.txn_id,
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
            self.wal.log_update(self.txn_id, table, key, new_data);
            let entry = UndoEntry {
                txn_id: self.txn_id,
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
            self.wal.log_delete(self.txn_id, table, key);
            let entry = UndoEntry {
                txn_id: self.txn_id,
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

    /// WAL 파일 전체 삭제. 서버 부팅 시 1회 실행되는 크래시 복구 완료 후에만 호출해야 한다
    /// (그 외의 시점에는 다른 세션의 진행 중인 트랜잭션 레코드까지 함께 사라짐).
    pub fn wal_clear(&self) {
        self.wal.clear();
    }

    /// 명시적 체크포인트:
    /// `safe_to_truncate`가 false이면(다른 세션에 아직 활성 트랜잭션이 있으면) 아무 것도 하지
    /// 않고 연기한다 — 체크포인트 마커를 남기면서 WAL을 자르면, 아직 커밋되지 않은 다른
    /// 세션의 트랜잭션이 남긴 이전 레코드가 마커보다 앞에 있어 잘려나가기 때문이다.
    /// 버퍼풀 flush는 호출 전에 executor가 직접 수행해야 한다.
    pub fn do_checkpoint(&mut self, safe_to_truncate: bool) {
        if !safe_to_truncate {
            return;
        }
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

    /// 디스크의 Undo Log 파일 전체를 삭제. 서버 부팅 시 1회 실행되는 크래시 복구 완료
    /// 후에만 호출해야 한다.
    pub fn clear_undo_log_file(&self) {
        self.undo_log_file.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_dir(name: &str) -> String {
        let dir = format!("test_tmp_txn_{}_{}", name, std::process::id());
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn global_txn_id_is_unique_across_managers() {
        let dir = test_dir("unique_id");
        let io = Arc::new(TxnIoShared::new());
        let mut a = TransactionManager::new_with_shared(&dir, Arc::clone(&io));
        let mut b = TransactionManager::new_with_shared(&dir, Arc::clone(&io));

        let id_a = a.begin().unwrap();
        let id_b = b.begin().unwrap();
        assert_ne!(id_a, id_b);

        a.abort().ok();
        b.abort().ok();
        fs::remove_dir_all(&dir).ok();
    }

    /// 핵심 회귀 테스트: 세션 B의 COMMIT이 세션 A의 진행 중인(미커밋) 트랜잭션의
    /// WAL/Undo 기록을 파괴하지 않아야 한다. 수정 전에는 clear()가 파일 전체를
    /// 지웠기 때문에 이 테스트가 실패했다.
    #[test]
    fn concurrent_commit_preserves_other_open_transaction() {
        let dir = test_dir("concurrent_commit");
        let io = Arc::new(TxnIoShared::new());
        let mut a = TransactionManager::new_with_shared(&dir, Arc::clone(&io));
        let mut b = TransactionManager::new_with_shared(&dir, Arc::clone(&io));

        let id_a = a.begin().unwrap();
        a.log_insert("t", "k1", "{\"a\":1}");

        let id_b = b.begin().unwrap();
        b.log_insert("t", "k2", "{\"a\":2}");
        b.commit().unwrap();

        let a_records = a.wal_records();
        assert!(a_records.iter().any(|r| r.txn_id == id_a && r.key == "k1"),
            "A's still-open transaction record must survive B's commit");
        assert!(!a_records.iter().any(|r| r.txn_id == id_b),
            "B's own committed record should be gone after commit_finalize-equivalent removal");

        let a_undo = a.read_undo_log_file();
        assert_eq!(a_undo.iter().filter(|e| e.txn_id == id_a).count(), 1);
        assert_eq!(a_undo.iter().filter(|e| e.txn_id == id_b).count(), 0);

        a.commit().unwrap();
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn rollback_to_savepoint_preserves_other_transaction_undo() {
        let dir = test_dir("savepoint_preserve");
        let io = Arc::new(TxnIoShared::new());
        let mut a = TransactionManager::new_with_shared(&dir, Arc::clone(&io));
        let mut b = TransactionManager::new_with_shared(&dir, Arc::clone(&io));

        let id_b = b.begin().unwrap();
        b.log_insert("t", "kb", "{}");

        let _id_a = a.begin().unwrap();
        a.log_insert("t", "ka1", "{}");
        a.create_savepoint("sp1").unwrap();
        a.log_insert("t", "ka2", "{}");
        a.rollback_to_savepoint("sp1").unwrap();

        // B's undo entry must still be on disk after A's savepoint rewrite.
        let entries = a.read_undo_log_file();
        assert_eq!(entries.iter().filter(|e| e.txn_id == id_b).count(), 1);
        // A should only have its pre-savepoint entry (ka1) left.
        assert_eq!(entries.iter().filter(|e| e.key == "ka2").count(), 0);
        assert_eq!(entries.iter().filter(|e| e.key == "ka1").count(), 1);

        a.commit().unwrap();
        b.commit().unwrap();
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn do_checkpoint_deferred_when_unsafe_is_noop() {
        let dir = test_dir("checkpoint_deferred");
        let io = Arc::new(TxnIoShared::new());
        let mut a = TransactionManager::new_with_shared(&dir, Arc::clone(&io));
        a.begin().unwrap();
        a.log_insert("t", "k1", "{}");
        let before = a.wal_records().len();

        a.do_checkpoint(false);

        let after = a.wal_records();
        assert_eq!(after.len(), before, "deferred checkpoint must not touch the WAL");
        assert!(!after.iter().any(|r| matches!(r.op, crate::transaction::wal::WalOp::Checkpoint)));

        a.abort().ok();
        fs::remove_dir_all(&dir).ok();
    }
}
