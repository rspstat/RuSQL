use std::fs::{File, OpenOptions};
use std::io::{Write, Read, BufWriter, Seek, SeekFrom};
use std::path::Path;

pub const WAL_PATH: &str = "rustdb.wal";

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

    /// WAL에 레코드 기록
    pub fn append(&self, record: WalRecord) {
        let mut file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            .expect("WAL 파일 열기 실패");
        let encoded = Self::encode(&record);
        file.write_all(&encoded).expect("WAL 기록 실패");
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

    pub fn log_commit(&self) {
        self.append(WalRecord {
            op: WalOp::Commit,
            table_name: String::new(),
            key: String::new(),
            data: String::new(),
        });
    }

    pub fn log_rollback(&self) {
        self.append(WalRecord {
            op: WalOp::Rollback,
            table_name: String::new(),
            key: String::new(),
            data: String::new(),
        });
    }

    pub fn log_checkpoint(&self) {
        self.append(WalRecord {
            op: WalOp::Checkpoint,
            table_name: String::new(),
            key: String::new(),
            data: String::new(),
        });
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
}