pub const PAGE_SIZE: usize = 16384;
pub const MAGIC: u32 = 0x52444200; // "RDB\0"
pub const VERSION: u16 = 1;

pub const FLAG_COMPRESSED: u8 = 0x01; // LZ4 compression

#[derive(Debug, Clone)]
pub struct PageHeader {
    pub magic: u32,
    pub version: u16,
    pub flags: u8,
    pub row_count: u32,
    pub page_count: u32,
}

impl PageHeader {
    pub fn new() -> Self {
        PageHeader {
            magic: MAGIC,
            version: VERSION,
            flags: 0,
            row_count: 0,
            page_count: 0,
        }
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        let mut buf = Vec::new();
        buf.extend_from_slice(&self.magic.to_le_bytes());   // 0..4
        buf.extend_from_slice(&self.version.to_le_bytes()); // 4..6
        buf.push(self.flags);                               // 6
        buf.push(0u8);                                      // 7  (padding)
        buf.extend_from_slice(&self.row_count.to_le_bytes());  // 8..12
        buf.extend_from_slice(&self.page_count.to_le_bytes()); // 12..16
        buf.extend_from_slice(&[0u8; 16]);                     // 16..32 reserved
        buf // 32 bytes
    }

    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < 32 { return None; }
        let magic = u32::from_le_bytes(bytes[0..4].try_into().ok()?);
        if magic != MAGIC { return None; }
        let version   = u16::from_le_bytes(bytes[4..6].try_into().ok()?);
        let flags     = bytes[6];
        let row_count  = u32::from_le_bytes(bytes[8..12].try_into().ok()?);
        let page_count = u32::from_le_bytes(bytes[12..16].try_into().ok()?);
        Some(PageHeader { magic, version, flags, row_count, page_count })
    }

    pub fn is_compressed(&self) -> bool {
        self.flags & FLAG_COMPRESSED != 0
    }
}
