use crate::parser::ast::DataType;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub enum FkAction {
    Restrict,
    Cascade,
    SetNull,
}

#[derive(Debug, Clone)]
pub struct ForeignKey {
    pub column: String,
    pub ref_table: String,
    pub ref_column: String,
    pub on_delete: FkAction,
}

#[derive(Debug, Clone)]
pub struct ColumnDef {
    pub name: String,
    pub data_type: DataType,
    pub primary_key: bool,
    pub not_null: bool,
    pub unique: bool,
    pub auto_increment: bool,
    pub foreign_key: Option<ForeignKey>,
}

#[derive(Debug, Clone)]
pub struct TableSchema {
    pub name: String,
    pub columns: Vec<ColumnDef>,
    pub auto_increment_counters: HashMap<String, i64>,
}

#[derive(Debug, Clone)]
pub struct Catalog {
    pub tables: HashMap<String, TableSchema>,
}

impl Catalog {
    pub fn new() -> Self {
        Catalog { tables: HashMap::new() }
    }

    pub fn create_table(&mut self, name: String, columns: Vec<ColumnDef>) -> Result<(), String> {
        if self.tables.contains_key(&name) {
            return Err(format!("Table '{}' already exists", name));
        }
        self.tables.insert(name.clone(), TableSchema {
            name,
            columns,
            auto_increment_counters: HashMap::new(),
        });
        Ok(())
    }

    pub fn drop_table(&mut self, name: &str) -> Result<(), String> {
        self.tables.remove(name)
            .ok_or(format!("Table '{}' not found", name))?;
        Ok(())
    }

    pub fn get_table(&self, name: &str) -> Option<&TableSchema> {
        self.tables.get(name)
    }

    pub fn get_table_mut(&mut self, name: &str) -> Option<&mut TableSchema> {
        self.tables.get_mut(name)
    }
}