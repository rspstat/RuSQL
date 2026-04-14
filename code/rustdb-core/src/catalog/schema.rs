pub use crate::parser::ast::DataType;
use std::collections::HashMap;
use serde::{Serialize, Deserialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum FkAction {
    Restrict,
    Cascade,
    SetNull,
}

impl Default for FkAction {
    fn default() -> Self { FkAction::Restrict }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ForeignKey {
    pub column: String,
    pub ref_table: String,
    pub ref_column: String,
    pub on_delete: FkAction,
    #[serde(default)]
    pub on_update: FkAction,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CheckConstraint {
    pub name: Option<String>,
    pub expression: String,   // raw SQL string, e.g. "age > 0"
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ColumnDef {
    pub name: String,
    pub data_type: DataType,
    pub primary_key: bool,
    pub not_null: bool,
    pub unique: bool,
    pub unique_constraint_name: Option<String>,
    pub auto_increment: bool,
    pub default: Option<String>,
    pub foreign_key: Option<ForeignKey>,
    #[serde(default)]
    pub check_expr: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TableSchema {
    pub name: String,
    pub columns: Vec<ColumnDef>,
    pub auto_increment_counters: HashMap<String, i64>,
    /// 복합 PK 컬럼 순서 (비어있으면 단일 PK 또는 PK 없음)
    #[serde(default)]
    pub primary_key_columns: Vec<String>,
    /// 테이블 레벨 CHECK 제약
    #[serde(default)]
    pub check_constraints: Vec<CheckConstraint>,
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
        self.create_table_full(name, columns, vec![], vec![])
    }

    pub fn create_table_full(
        &mut self,
        name: String,
        columns: Vec<ColumnDef>,
        primary_key_columns: Vec<String>,
        check_constraints: Vec<CheckConstraint>,
    ) -> Result<(), String> {
        if self.tables.contains_key(&name) {
            return Err(format!("Table '{}' already exists", name));
        }
        self.tables.insert(name.clone(), TableSchema {
            name,
            columns,
            auto_increment_counters: HashMap::new(),
            primary_key_columns,
            check_constraints,
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