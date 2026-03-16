// src/storage/page.rs

pub const PAGE_SIZE: usize = 4096;

#[derive(Debug, Clone)]
pub struct Page {
    pub id: u32,
    pub data: Vec<u8>,
    pub is_dirty: bool,
}

impl Page {
    pub fn new(id: u32) -> Self {
        Page {
            id,
            data: vec![0u8; PAGE_SIZE],
            is_dirty: false,
        }
    }

    pub fn write(&mut self, offset: usize, bytes: &[u8]) {
        self.data[offset..offset + bytes.len()].copy_from_slice(bytes);
        self.is_dirty = true;
    }

    pub fn read(&self, offset: usize, len: usize) -> &[u8] {
        &self.data[offset..offset + len]
    }
}