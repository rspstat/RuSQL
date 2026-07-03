use std::fs::{File, OpenOptions};
use std::io::{Write, Read};
use std::path::Path;
use std::sync::Arc;
use crate::transaction::txn_manager::TxnIoShared;

pub const WAL_PATH: &str = "rusql.wal";

/// WAL 자동 체크포인트 임계값 (512KB)
pub const AUTO_CHECKPOINT_BYTES: u64 = 512 * 1024;

/// WAL 레코드 op 코드
#[repr(u8)]
#[derive(Debug, Clone, PartialEq)]
pub enum WalOp {
    Insert    = 0x01,
    Update    = 0x02,
    Delete    = 0x03,
    Commit    = 0xFF,
    Rollback  = 0xFE,
    Checkpoint= 0xFD,
}

impl WalOp {
    pub fn from_u8(v: u8) -> Option<WalOp> {
        match v {
            0x01 => Some(WalOp::Insert),
            0x02 => Some(WalOp::Update),
            0x03 => Some(WalOp::Delete),
            0xFF => Some(WalOp::Commit),
            0xFE => Some(WalOp::Rollback),
            0xFD => Some(WalOp::Checkpoint),
            _    => None,
        }
    }
}

/// 바이너리 WAL 레코드
#[derive(Debug, Clone)]
pub struct WalRecord {
    pub op:         WalOp,
    /// 이 레코드를 발생시킨 트랜잭션의 전역 유일 ID (Checkpoint 레코드는 0 고정).
    pub txn_id:     u64,
    pub table_name: String,
    pub key:        String,
    pub data:       String, // JSON
}

pub struct WalManager {
    path: String,
    /// 세션 간 공유되는 WAL 파일 접근 직렬화 락 (같은 data_dir을 쓰는 모든 세션이 공유).
    io: Arc<TxnIoShared>,
}

impl WalManager {
    pub fn new_with_dir(dir: &str, io: Arc<TxnIoShared>) -> Self {
        WalManager { path: format!("{}/rusql.wal", dir), io }
    }

    /// 레코드 바이너리 인코딩
    /// [ op(1) | txn_id(8,LE) | table_len(4) | table(n) | key_len(4) | key(n) | data_len(4) | data(n) ]
    fn encode(record: &WalRecord) -> Vec<u8> {
        let mut buf = Vec::new();
        let table_bytes = record.table_name.as_bytes();
        let key_bytes   = record.key.as_bytes();
        let data_bytes  = record.data.as_bytes();

        buf.push(record.op.clone() as u8);
        buf.extend_from_slice(&record.txn_id.to_le_bytes());
        buf.extend_from_slice(&(table_bytes.len() as u32).to_le_bytes());
        buf.extend_from_slice(table_bytes);
        buf.extend_from_slice(&(key_bytes.len() as u32).to_le_bytes());
        buf.extend_from_slice(key_bytes);
        buf.extend_from_slice(&(data_bytes.len() as u32).to_le_bytes());
        buf.extend_from_slice(data_bytes);
        buf
    }

    /// 레코드 바이너리 디코딩
    fn decode(buf: &[u8], pos: &mut usize) -> Option<WalRecord> {
        if *pos >= buf.len() { return None; }

        let op = WalOp::from_u8(buf[*pos])?;
        *pos += 1;

        if *pos + 8 > buf.len() { return None; }
        let txn_id = u64::from_le_bytes(buf[*pos..*pos + 8].try_into().ok()?);
        *pos += 8;

        let table_name = Self::read_string(buf, pos)?;
        let key        = Self::read_string(buf, pos)?;
        let data       = Self::read_string(buf, pos)?;

        Some(WalRecord { op, txn_id, table_name, key, data })
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

    /// 인코딩된 바이트를 WAL 파일에 기록. sync=true 이면 커널 버퍼 → 디스크 fsync.
    /// 호출자가 io.wal_lock을 이미 보유하고 있어야 한다.
    fn write_encoded_locked(&self, encoded: &[u8], sync: bool) {
        let mut file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            .expect("WAL 파일 열기 실패");
        file.write_all(encoded).expect("WAL 기록 실패");
        if sync {
            file.sync_all().expect("WAL fsync 실패");
        }
    }

    /// WAL에 레코드 기록 (fsync 없음 — 데이터 변경 레코드용)
    pub fn append(&self, record: WalRecord) {
        let _g = self.io.wal_lock.lock().unwrap();
        self.write_encoded_locked(&Self::encode(&record), false);
    }

    pub fn log_insert(&self, txn_id: u64, table: &str, key: &str, data: &str) {
        self.append(WalRecord {
            op: WalOp::Insert,
            txn_id,
            table_name: table.to_string(),
            key: key.to_string(),
            data: data.to_string(),
        });
    }

    pub fn log_update(&self, txn_id: u64, table: &str, key: &str, data: &str) {
        self.append(WalRecord {
            op: WalOp::Update,
            txn_id,
            table_name: table.to_string(),
            key: key.to_string(),
            data: data.to_string(),
        });
    }

    pub fn log_delete(&self, txn_id: u64, table: &str, key: &str) {
        self.append(WalRecord {
            op: WalOp::Delete,
            txn_id,
            table_name: table.to_string(),
            key: key.to_string(),
            data: String::new(),
        });
    }

    /// COMMIT — fsync로 커밋 레코드를 디스크에 영속화 (innodb_flush_log_at_trx_commit=1 동등)
    pub fn log_commit(&self, txn_id: u64) {
        let record = WalRecord {
            op: WalOp::Commit, txn_id,
            table_name: String::new(), key: String::new(), data: String::new(),
        };
        let _g = self.io.wal_lock.lock().unwrap();
        self.write_encoded_locked(&Self::encode(&record), true);
    }

    /// COMMIT 레코드를 기록하되 fsync하지 않음 (Group Commit용).
    /// 호출자가 이후 GroupCommitCoordinator::sync_commit()으로 fsync를 보장해야 한다.
    pub fn log_commit_no_sync(&self, txn_id: u64) {
        let record = WalRecord {
            op: WalOp::Commit, txn_id,
            table_name: String::new(), key: String::new(), data: String::new(),
        };
        let _g = self.io.wal_lock.lock().unwrap();
        self.write_encoded_locked(&Self::encode(&record), false);
    }

    pub fn log_rollback(&self, txn_id: u64) {
        self.append(WalRecord {
            op: WalOp::Rollback, txn_id,
            table_name: String::new(), key: String::new(), data: String::new(),
        });
    }

    /// CHECKPOINT — 버퍼풀 플러시 완료 표시를 디스크에 영속화.
    /// 특정 트랜잭션에 속하지 않는 전역 마커이므로 txn_id는 0으로 고정한다.
    pub fn log_checkpoint(&self) {
        let record = WalRecord {
            op: WalOp::Checkpoint, txn_id: 0,
            table_name: String::new(), key: String::new(), data: String::new(),
        };
        let _g = self.io.wal_lock.lock().unwrap();
        self.write_encoded_locked(&Self::encode(&record), true);
    }

    /// WAL 전체 읽기 (복구용). 호출자가 io.wal_lock을 이미 보유하고 있어야 한다.
    fn read_all_locked(&self) -> Vec<WalRecord> {
        if !Path::new(&self.path).exists() {
            return vec![];
        }
        let mut file = File::open(&self.path).expect("WAL 읽기 실패");
        let mut buf = Vec::new();
        file.read_to_end(&mut buf).expect("WAL 읽기 실패");

        let mut records = Vec::new();
        let mut pos = 0;
        while let Some(record) = Self::decode(&buf, &mut pos) {
            records.push(record);
        }
        records
    }

    pub fn read_all(&self) -> Vec<WalRecord> {
        let _g = self.io.wal_lock.lock().unwrap();
        self.read_all_locked()
    }

    fn clear_locked(&self) {
        if Path::new(&self.path).exists() {
            std::fs::remove_file(&self.path).ok();
        }
    }

    /// WAL 파일 전체 삭제. 서버 부팅 시 1회 실행되는 크래시 복구(recover_from_wal) 완료 후에만
    /// 호출해야 안전하다 — 그 외에는 다른 세션의 진행 중인 트랜잭션 기록까지 함께 사라진다.
    pub fn clear(&self) {
        let _g = self.io.wal_lock.lock().unwrap();
        self.clear_locked();
    }

    /// 지정한 트랜잭션이 남긴 레코드만 제거하고 나머지는 보존한다.
    /// COMMIT/ROLLBACK 시 clear() 대신 사용 — 같은 파일을 공유하는 다른 세션의
    /// 진행 중인 트랜잭션 레코드를 파괴하지 않는다.
    pub fn remove_txn(&self, txn_id: u64) {
        let _g = self.io.wal_lock.lock().unwrap();
        let remaining: Vec<WalRecord> = self.read_all_locked()
            .into_iter()
            .filter(|r| r.txn_id != txn_id)
            .collect();
        if remaining.is_empty() {
            self.clear_locked();
            return;
        }
        let mut buf = Vec::new();
        for r in &remaining {
            buf.extend_from_slice(&Self::encode(r));
        }
        std::fs::write(&self.path, buf).ok();
    }

    /// WAL 파일 크기 (bytes)
    pub fn file_size(&self) -> u64 {
        std::fs::metadata(&self.path).map(|m| m.len()).unwrap_or(0)
    }

    /// 마지막 CHECKPOINT 이후 레코드만 남기고 WAL을 재작성한다.
    /// CHECKPOINT 레코드 자체는 포함시킨다 (복구 시작점 표시).
    pub fn truncate_to_last_checkpoint(&self) {
        let _g = self.io.wal_lock.lock().unwrap();
        let records = self.read_all_locked();
        if records.is_empty() { return; }

        // 마지막 CHECKPOINT 위치 찾기
        let last_cp = records.iter().rposition(|r| r.op == WalOp::Checkpoint);
        let Some(cp_idx) = last_cp else { return };

        // CHECKPOINT 이전 레코드는 이미 디스크에 반영됐으므로 제거
        let remaining = &records[cp_idx..];

        // CHECKPOINT 레코드 하나만 남은 경우 WAL 전체 초기화
        if remaining.len() <= 1 {
            self.clear_locked();
            return;
        }

        // 나머지 레코드로 WAL 재작성
        let mut buf = Vec::new();
        for r in remaining {
            buf.extend_from_slice(&Self::encode(r));
        }
        std::fs::write(&self.path, buf).ok();
    }

    /// WAL 자동 체크포인트가 필요한지 확인 (임계값 초과 여부)
    pub fn needs_auto_checkpoint(&self) -> bool {
        self.file_size() >= AUTO_CHECKPOINT_BYTES
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_dir(name: &str) -> String {
        let dir = format!("test_tmp_wal_{}_{}", name, std::process::id());
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn encode_decode_roundtrip_with_txn_id() {
        let record = WalRecord {
            op: WalOp::Insert,
            txn_id: 42,
            table_name: "t".to_string(),
            key: "k1".to_string(),
            data: "{\"a\":1}".to_string(),
        };
        let encoded = WalManager::encode(&record);
        let mut pos = 0;
        let decoded = WalManager::decode(&encoded, &mut pos).unwrap();
        assert_eq!(decoded.txn_id, 42);
        assert_eq!(decoded.table_name, "t");
        assert_eq!(decoded.key, "k1");
        assert_eq!(decoded.data, "{\"a\":1}");
        assert!(matches!(decoded.op, WalOp::Insert));
    }

    #[test]
    fn remove_txn_preserves_other_transactions() {
        let dir = test_dir("remove_preserve");
        let io = Arc::new(TxnIoShared::new());
        let wal = WalManager::new_with_dir(&dir, io);

        wal.log_insert(1, "t", "k1", "{}");
        wal.log_insert(2, "t", "k2", "{}");
        wal.log_insert(1, "t", "k3", "{}");

        wal.remove_txn(1);

        let remaining = wal.read_all();
        assert_eq!(remaining.len(), 1);
        assert_eq!(remaining[0].txn_id, 2);
        assert_eq!(remaining[0].key, "k2");

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn remove_txn_draining_last_txn_clears_file() {
        let dir = test_dir("remove_last");
        let io = Arc::new(TxnIoShared::new());
        let wal = WalManager::new_with_dir(&dir, io);

        wal.log_insert(1, "t", "k1", "{}");
        wal.remove_txn(1);

        assert!(wal.read_all().is_empty());
        assert_eq!(wal.file_size(), 0);

        std::fs::remove_dir_all(&dir).ok();
    }
}
