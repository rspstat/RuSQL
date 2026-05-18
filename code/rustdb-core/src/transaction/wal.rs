use std::fs::{File, OpenOptions};
use std::io::{Write, Read};
use std::path::Path;

pub const WAL_PATH: &str = "rustdb.wal";

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
    pub table_name: String,
    pub key:        String,
    pub data:       String, // JSON
}

pub struct WalManager {
    path: String,
}

impl WalManager {
    pub fn new() -> Self {
        WalManager { path: WAL_PATH.to_string() }
    }

    pub fn new_with_dir(dir: &str) -> Self {
        WalManager { path: format!("{}/rustdb.wal", dir) }
    }

    /// 레코드 바이너리 인코딩
    /// [ op(1) | table_len(4) | table(n) | key_len(4) | key(n) | data_len(4) | data(n) ]
    fn encode(record: &WalRecord) -> Vec<u8> {
        let mut buf = Vec::new();
        let table_bytes = record.table_name.as_bytes();
        let key_bytes   = record.key.as_bytes();
        let data_bytes  = record.data.as_bytes();

        buf.push(record.op.clone() as u8);
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

        let table_name = Self::read_string(buf, pos)?;
        let key        = Self::read_string(buf, pos)?;
        let data       = Self::read_string(buf, pos)?;

        Some(WalRecord { op, table_name, key, data })
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
    fn write_encoded(&self, encoded: &[u8], sync: bool) {
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
        self.write_encoded(&Self::encode(&record), false);
    }

    pub fn log_insert(&self, table: &str, key: &str, data: &str) {
        self.append(WalRecord {
            op: WalOp::Insert,
            table_name: table.to_string(),
            key: key.to_string(),
            data: data.to_string(),
        });
    }

    pub fn log_update(&self, table: &str, key: &str, data: &str) {
        self.append(WalRecord {
            op: WalOp::Update,
            table_name: table.to_string(),
            key: key.to_string(),
            data: data.to_string(),
        });
    }

    pub fn log_delete(&self, table: &str, key: &str) {
        self.append(WalRecord {
            op: WalOp::Delete,
            table_name: table.to_string(),
            key: key.to_string(),
            data: String::new(),
        });
    }

    /// COMMIT — fsync로 커밋 레코드를 디스크에 영속화 (innodb_flush_log_at_trx_commit=1 동등)
    pub fn log_commit(&self) {
        let record = WalRecord {
            op: WalOp::Commit,
            table_name: String::new(),
            key: String::new(),
            data: String::new(),
        };
        self.write_encoded(&Self::encode(&record), true);
    }

    /// COMMIT 레코드를 기록하되 fsync하지 않음 (Group Commit용).
    /// 호출자가 이후 GroupCommitCoordinator::sync_commit()으로 fsync를 보장해야 한다.
    pub fn log_commit_no_sync(&self) {
        let record = WalRecord {
            op: WalOp::Commit,
            table_name: String::new(),
            key: String::new(),
            data: String::new(),
        };
        self.write_encoded(&Self::encode(&record), false);
    }

    pub fn log_rollback(&self) {
        self.append(WalRecord {
            op: WalOp::Rollback,
            table_name: String::new(),
            key: String::new(),
            data: String::new(),
        });
    }

    /// CHECKPOINT — 버퍼풀 플러시 완료 표시를 디스크에 영속화
    pub fn log_checkpoint(&self) {
        let record = WalRecord {
            op: WalOp::Checkpoint,
            table_name: String::new(),
            key: String::new(),
            data: String::new(),
        };
        self.write_encoded(&Self::encode(&record), true);
    }

    /// WAL 전체 읽기 (복구용)
    pub fn read_all(&self) -> Vec<WalRecord> {
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

    /// WAL 파일 삭제 (체크포인트 후)
    pub fn clear(&self) {
        if Path::new(&self.path).exists() {
            std::fs::remove_file(&self.path).ok();
        }
    }

    /// WAL 파일 크기 (bytes)
    pub fn file_size(&self) -> u64 {
        std::fs::metadata(&self.path).map(|m| m.len()).unwrap_or(0)
    }

    /// 마지막 CHECKPOINT 이후 레코드만 남기고 WAL을 재작성한다.
    /// CHECKPOINT 레코드 자체는 포함시킨다 (복구 시작점 표시).
    pub fn truncate_to_last_checkpoint(&self) {
        let records = self.read_all();
        if records.is_empty() { return; }

        // 마지막 CHECKPOINT 위치 찾기
        let last_cp = records.iter().rposition(|r| r.op == WalOp::Checkpoint);
        let Some(cp_idx) = last_cp else { return };

        // CHECKPOINT 이전 레코드는 이미 디스크에 반영됐으므로 제거
        let remaining = &records[cp_idx..];

        // CHECKPOINT 레코드 하나만 남은 경우 WAL 전체 초기화
        if remaining.len() <= 1 {
            self.clear();
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