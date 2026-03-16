 // src/catalog/schema.rs

use std::collections::HashMap;
use crate::parser::ast::DataType;

#[derive(Debug, Clone)]
pub struct ColumnDef {
    pub name: String,
    pub data_type: DataType,
}

#[derive(Debug, Clone)]
pub struct TableSchema {
    pub name: String,
    pub columns: Vec<ColumnDef>,
}

pub struct Catalog {
    pub tables: HashMap<String, TableSchema>,
}

impl Catalog {
    pub fn new() -> Self {
        Catalog {
            tables: HashMap::new(),
        }
    }

    pub fn create_table(&mut self, name: String, columns: Vec<ColumnDef>) -> Result<(), String> {
        if self.tables.contains_key(&name) {
            return Err(format!("Table '{}' already exists", name));
        }
        self.tables.insert(name.clone(), TableSchema { name, columns });
        Ok(())
    }

    pub fn drop_table(&mut self, name: &str) -> Result<(), String> {
        if self.tables.remove(name).is_none() {
            return Err(format!("Table '{}' not found", name));
        }
        Ok(())
    }

    pub fn get_table(&self, name: &str) -> Option<&TableSchema> {
        self.tables.get(name)
    }
}
