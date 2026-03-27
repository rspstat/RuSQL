pub const PAGE_SIZE: usize = 16384;
pub const MAGIC: u32 = 0x52444200; // "RDB\0"
pub const VERSION: u16 = 1;

#[derive(Debug, Clone)]
pub struct PageHeader {
    pub magic: u32,
    pub version: u16,
    pub row_count: u32,
    pub page_count: u32,
}

impl PageHeader {
    pub fn new() -> Self {
        PageHeader {
            magic: MAGIC,
            version: VERSION,
            row_count: 0,
            page_count: 0,
        }
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        let mut buf = Vec::new();
        buf.extend_from_slice(&self.magic.to_le_bytes());
        buf.extend_from_slice(&self.version.to_le_bytes());
        buf.extend_from_slice(&[0u8; 2]); // padding
        buf.extend_from_slice(&self.row_count.to_le_bytes());
        buf.extend_from_slice(&self.page_count.to_le_bytes());
        buf.extend_from_slice(&[0u8; 16]); // reserved
        buf // 32 bytes
    }

    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < 32 { return None; }
        let magic = u32::from_le_bytes(bytes[0..4].try_into().ok()?);
        if magic != MAGIC { return None; }
        let version = u16::from_le_bytes(bytes[4..6].try_into().ok()?);
        let row_count = u32::from_le_bytes(bytes[8..12].try_into().ok()?);
        let page_count = u32::from_le_bytes(bytes[12..16].try_into().ok()?);
        Some(PageHeader { magic, version, row_count, page_count })
    }
}