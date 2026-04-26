// src/engine/executor.rs

use std::collections::HashMap;
use chrono;
use crate::transaction::txn_manager::TransactionManager;
use crate::parser::ast::*;
use crate::catalog::schema::{Catalog, ColumnDef as SchemaCol};
use crate::storage::disk::{DiskManager, IndexMeta};
use crate::storage::btree::BPlusTree;
use crate::storage::buffer_pool::BufferPool;
use crate::storage::composite_index::CompositeIndex;
use crate::engine::lock_manager::{LockManager, LockResult};

pub type Row = HashMap<String, String>;
pub const NULL_VALUE: &str = "NULL";

pub struct Executor {
    pub catalog: Catalog,
    pub tables: HashMap<String, Vec<Row>>,
    pub indexes: HashMap<String, BPlusTree>,
    pub index_meta: HashMap<String, (String, String)>,
    /// 복합 인덱스: index_name → CompositeIndex
    pub composite_indexes: HashMap<String, CompositeIndex>,
    pub views: HashMap<String, Statement>,
    pub txn: TransactionManager,
    pub buffer_pool: BufferPool,
    disk: DiskManager,
    /// Row-level lock + wait-for graph (데드락 감지 포함)
    lock_mgr: LockManager,
}

impl Executor {
    pub fn new() -> Self {
        let disk = DiskManager::new();
        let mut catalog = Catalog::new();
        let mut tables = HashMap::new();
        let mut indexes = HashMap::new();

        for table_name in disk.list_tables() {
            if let Some(schema) = disk.load_schema(&table_name) {
                let first_col = schema.columns.first().map(|c| c.name.clone());
                let auto_inc_counters = schema.auto_increment_counters.clone();

                let _ = catalog.create_table_full(
                    table_name.clone(),
                    schema.columns.clone(),
                    schema.primary_key_columns.clone(),
                    schema.check_constraints.clone(),
                );

                // auto_increment 카운터 복원
                if let Some(ts) = catalog.get_table_mut(&table_name) {
                    ts.auto_increment_counters = auto_inc_counters;
                }

                let rows = disk.load_table(&table_name);

                let mut tree = BPlusTree::new();
                for row in &rows {
                    if let Some(ref col) = first_col {
                        if let Some(key) = row.get(col) {
                            let val_json = serde_json::to_string(row).unwrap();
                            tree.insert(key.clone(), val_json);
                        }
                    }
                }
                indexes.insert(table_name.clone(), tree);
                tables.insert(table_name, rows);
            }
        }

        // 저장된 뷰 로드
        let views = disk.load_views();

        // 저장된 인덱스 메타 로드 후 재빌드
        let index_meta_list = disk.load_index_meta();
        let mut index_meta: HashMap<String, (String, String)> = HashMap::new();
        let mut composite_indexes: HashMap<String, CompositeIndex> = HashMap::new();

        for meta in &index_meta_list {
            if meta.columns.len() == 1 {
                let column = &meta.columns[0];
                let mut tree = BPlusTree::new();
                if let Some(rows) = tables.get(&meta.table) {
                    for row in rows {
                        if let Some(val) = row.get(column) {
                            let json = serde_json::to_string(row).unwrap();
                            tree.insert(val.clone(), json);
                        }
                    }
                }
                let key = format!("{}_{}", meta.table, meta.name);
                indexes.insert(key, tree);
                index_meta.insert(meta.name.clone(), (meta.table.clone(), column.clone()));
            } else {
                let mut comp = CompositeIndex::new(meta.table.clone(), meta.columns.clone());
                if let Some(rows) = tables.get(&meta.table) {
                    comp.rebuild(rows);
                }
                composite_indexes.insert(meta.name.clone(), comp);
            }
        }

        let mut executor = Executor {
            catalog,
            tables,
            indexes,
            index_meta,
            composite_indexes,
            views,
            txn: TransactionManager::new(),
            buffer_pool: BufferPool::new(),
            disk,
            lock_mgr: LockManager::new(),
        };

        // WAL Crash Recovery
        executor.recover_from_wal();
        executor
    }

    pub fn execute(&mut self, stmt: Statement) -> Result<String, String> {
        match stmt {
            Statement::Begin    => self.exec_begin(),
            Statement::Commit   => self.exec_commit(),
            Statement::Rollback => self.exec_rollback(),
            Statement::CreateTable { name, columns, if_not_exists, primary_key_columns, check_constraints } => {
                self.exec_create(name, columns, if_not_exists, primary_key_columns, check_constraints)
            }
            Statement::DropTable { name, if_exists }  => self.exec_drop(name, if_exists),
            Statement::TruncateTable { name }        => self.exec_truncate(name),
            Statement::Insert { table, columns, values } => self.exec_insert(table, columns, values),
            Statement::InsertSelect { table, columns, query } => self.exec_insert_select(table, columns, *query),
            Statement::Select { table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update } => {
                self.exec_select(table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update)
            }
            Statement::Update { table, assignments, condition } => {
                self.exec_update(table, assignments, condition)
            }
            Statement::Delete { table, condition }   => self.exec_delete(table, condition),
            Statement::AlterTable { table, action }  => self.exec_alter(table, action),
            Statement::CreateIndex { index_name, table, columns } => {
                self.exec_create_index(index_name, table, columns)
            }
            Statement::DropIndex { index_name } => self.exec_drop_index(index_name),
            Statement::CreateView { name, query } => self.exec_create_view(name, *query),
            Statement::DropView { name } => self.exec_drop_view(name),
            Statement::ShowTables => self.exec_show_tables(),
            Statement::Describe { table } => self.exec_describe(table),
            Statement::ShowBufferPool => self.exec_show_buffer_pool(),
            Statement::ShowWal        => self.exec_show_wal(),
            Statement::Checkpoint     => self.exec_checkpoint(),
            Statement::SetIsolationLevel(level) => self.exec_set_isolation_level(level),
            Statement::ShowIsolationLevel       => self.exec_show_isolation_level(),
            Statement::Vacuum { table }         => self.exec_vacuum(table),
            Statement::ShowLocks                => self.exec_show_locks(),
            Statement::Savepoint { name }       => self.exec_savepoint(name),
            Statement::ReleaseSavepoint { name } => self.exec_release_savepoint(name),
            Statement::RollbackTo { name }      => self.exec_rollback_to(name),
            Statement::Explain(inner)           => self.exec_explain(*inner),
            Statement::Union { left, right, all, order_by, limit, offset } => self.exec_union(*left, *right, all, order_by, limit, offset),
            Statement::With { ctes, query }     => self.exec_with(ctes, *query),
        }
    }

    fn exec_union(
        &mut self,
        left: Statement,
        right: Statement,
        all: bool,
        order_by: Vec<OrderBy>,
        limit: Option<usize>,
        offset: Option<usize>,
    ) -> Result<String, String> {
        let left_out  = self.execute(left)?;
        let right_out = self.execute(right)?;

        let (left_cols,  mut left_rows)  = Self::parse_table_output(&left_out);
        let (right_cols, right_rows) = Self::parse_table_output(&right_out);

        if left_cols.is_empty() && right_cols.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        // Merge rows
        left_rows.extend(right_rows);
        let mut result = left_rows;

        // UNION (not ALL): deduplicate
        if !all {
            let mut seen: Vec<Vec<String>> = Vec::new();
            result.retain(|row| {
                let key: Vec<String> = left_cols.iter()
                    .map(|c| row.get(c).cloned().unwrap_or_default())
                    .collect();
                if seen.contains(&key) { false } else { seen.push(key); true }
            });
        }

        // Apply ORDER BY
        for ob in order_by.iter().rev() {
            let col = ob.column.clone();
            let asc = ob.ascending;
            result.sort_by(|a, b| {
                let va = a.get(&col).map(|s| s.as_str()).unwrap_or("");
                let vb = b.get(&col).map(|s| s.as_str()).unwrap_or("");
                let cmp = match (va.parse::<f64>(), vb.parse::<f64>()) {
                    (Ok(fa), Ok(fb)) => fa.partial_cmp(&fb).unwrap_or(std::cmp::Ordering::Equal),
                    _ => va.cmp(vb),
                };
                if asc { cmp } else { cmp.reverse() }
            });
        }

        // Apply OFFSET then LIMIT
        if let Some(n) = offset {
            let skip = n.min(result.len());
            result.drain(..skip);
        }
        if let Some(n) = limit {
            result.truncate(n);
        }

        if result.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        // Format using left query's column order
        let cols = if left_cols.is_empty() { right_cols } else { left_cols };
        let col_widths: Vec<usize> = cols.iter().map(|h| {
            let max_val = result.iter()
                .map(|row| row.get(h).map(|v| v.len()).unwrap_or(0))
                .max().unwrap_or(0);
            h.len().max(max_val)
        }).collect();

        let mut out = String::new();
        let sep = col_widths.iter().map(|w| "-".repeat(w + 2)).collect::<Vec<_>>().join("+");
        let sep = format!("+{}+", sep);

        out.push_str(&sep); out.push('\n');
        let hdr = cols.iter().zip(col_widths.iter())
            .map(|(h, w)| format!(" {:width$} ", h, width = w))
            .collect::<Vec<_>>().join("|");
        out.push_str(&format!("|{}|\n", hdr));
        out.push_str(&sep); out.push('\n');
        for row in &result {
            let line = cols.iter().zip(col_widths.iter())
                .map(|(c, w)| {
                    let v = row.get(c).map(|s| if s == NULL_VALUE { "NULL".to_string() } else { s.clone() }).unwrap_or_default();
                    format!(" {:width$} ", v, width = w)
                })
                .collect::<Vec<_>>().join("|");
            out.push_str(&format!("|{}|\n", line));
        }
        out.push_str(&sep);
        out.push_str(&format!("\n{} row(s) returned.", result.len()));
        Ok(out)
    }

    /// MVCC 가시성 판정: _xmax == "0" 또는 없으면 visible
    fn is_visible(row: &Row) -> bool {
        row.get("_xmax").map(|v| v == "0").unwrap_or(true)
    }

    /// "table.col" 또는 "col" 형식으로 row에서 값 조회.
    /// 전체 키가 없으면 테이블 prefix를 제거한 bare 컬럼명으로 fallback.
    fn get_col<'a>(row: &'a Row, col: &str) -> Option<&'a String> {
        row.get(col).or_else(|| {
            col.rfind('.').and_then(|i| row.get(&col[i + 1..]))
        })
    }

    fn eval_arith(row: &Row, expr: &ArithExpr) -> String {
        match expr {
            ArithExpr::Col(name) => Self::get_col(row, name).cloned().unwrap_or_else(|| NULL_VALUE.to_string()),
            ArithExpr::Num(n) => n.clone(),
            ArithExpr::Str(s) => s.clone(),
            ArithExpr::Add(l, r) => {
                let lv = Self::eval_arith(row, l);
                let rv = Self::eval_arith(row, r);
                match (lv.parse::<f64>(), rv.parse::<f64>()) {
                    (Ok(a), Ok(b)) => Self::format_arith_result(a + b),
                    _ => format!("{}{}", lv, rv),
                }
            }
            ArithExpr::Sub(l, r) => {
                let lv = Self::eval_arith(row, l);
                let rv = Self::eval_arith(row, r);
                match (lv.parse::<f64>(), rv.parse::<f64>()) {
                    (Ok(a), Ok(b)) => Self::format_arith_result(a - b),
                    _ => "0".to_string(),
                }
            }
            ArithExpr::Mul(l, r) => {
                let lv = Self::eval_arith(row, l);
                let rv = Self::eval_arith(row, r);
                match (lv.parse::<f64>(), rv.parse::<f64>()) {
                    (Ok(a), Ok(b)) => Self::format_arith_result(a * b),
                    _ => "0".to_string(),
                }
            }
            ArithExpr::Div(l, r) => {
                let lv = Self::eval_arith(row, l);
                let rv = Self::eval_arith(row, r);
                match (lv.parse::<f64>(), rv.parse::<f64>()) {
                    (Ok(a), Ok(b)) if b != 0.0 => Self::format_arith_result(a / b),
                    _ => "0".to_string(),
                }
            }
        }
    }

    fn format_arith_result(f: f64) -> String {
        if f.fract().abs() < 1e-9 && f.abs() < 1e15 {
            format!("{}", f as i64)
        } else {
            let s = format!("{:.6}", f);
            s.trim_end_matches('0').trim_end_matches('.').to_string()
        }
    }

    fn exec_create(
        &mut self,
        name: String,
        columns: Vec<ColumnDef>,
        if_not_exists: bool,
        primary_key_columns: Vec<String>,
        check_constraints: Vec<(Option<String>, String)>,
    ) -> Result<String, String> {
        // IF NOT EXISTS: 이미 존재하면 조용히 넘어감
        if if_not_exists && self.tables.contains_key(&name) {
            return Ok(format!("Table '{}' already exists, skipped.", name));
        }
        let schema_cols: Vec<SchemaCol> = columns.into_iter().map(|c| SchemaCol {
            name: c.name,
            data_type: c.data_type,
            primary_key: c.primary_key,
            not_null: c.not_null,
            unique: c.unique,
            unique_constraint_name: c.unique_constraint_name,
            auto_increment: c.auto_increment,
            default: c.default,
            foreign_key: c.foreign_key.map(|fk| crate::catalog::schema::ForeignKey {
                column: fk.column,
                ref_table: fk.ref_table,
                ref_column: fk.ref_column,
                on_delete: match fk.on_delete {
                    crate::parser::ast::FkAction::Restrict => crate::catalog::schema::FkAction::Restrict,
                    crate::parser::ast::FkAction::Cascade  => crate::catalog::schema::FkAction::Cascade,
                    crate::parser::ast::FkAction::SetNull  => crate::catalog::schema::FkAction::SetNull,
                },
                on_update: match fk.on_update {
                    crate::parser::ast::FkAction::Restrict => crate::catalog::schema::FkAction::Restrict,
                    crate::parser::ast::FkAction::Cascade  => crate::catalog::schema::FkAction::Cascade,
                    crate::parser::ast::FkAction::SetNull  => crate::catalog::schema::FkAction::SetNull,
                },
            }),
            check_expr: c.check_expr,
        }).collect();
        let schema_checks: Vec<crate::catalog::schema::CheckConstraint> = check_constraints.into_iter()
            .map(|(name, expr)| crate::catalog::schema::CheckConstraint { name, expression: expr })
            .collect();
        self.catalog.create_table_full(name.clone(), schema_cols, primary_key_columns, schema_checks)?;
        self.tables.insert(name.clone(), Vec::new());
        self.indexes.insert(name.clone(), BPlusTree::new());
        let full_schema = self.catalog.get_table(&name).unwrap();
        self.disk.save_schema(&name, full_schema);
        Ok(format!("Table '{}' created.", name))
    }

    fn exec_drop(&mut self, name: String, if_exists: bool) -> Result<String, String> {
        if if_exists && !self.tables.contains_key(&name) {
            return Ok(format!("Table '{}' does not exist, skipped.", name));
        }
        self.catalog.drop_table(&name)?;
        self.tables.remove(&name);
        self.indexes.remove(&name);
        self.disk.delete_table(&name);
        Ok(format!("Table '{}' dropped.", name))
    }

    fn exec_truncate(&mut self, name: String) -> Result<String, String> {
        self.tables.get_mut(&name)
            .ok_or(format!("Table '{}' not found", name))?
            .clear();
        if let Some(index) = self.indexes.get_mut(&name) {
            *index = BPlusTree::new();
        }
        // AUTO INCREMENT 카운터 리셋
        if let Some(schema) = self.catalog.get_table_mut(&name) {
            schema.auto_increment_counters.clear();
        }
        self.buffer_pool.invalidate(&name);
        self.disk.save_table(&name, &[]);
        Ok(format!("Table '{}' truncated.", name))
    }

    fn exec_with(
        &mut self,
        ctes: Vec<(String, Box<Statement>)>,
        query: Statement,
    ) -> Result<String, String> {
        // Materialise each CTE as a temporary in-memory table, then run the main query.
        let mut cte_names: Vec<String> = Vec::new();

        for (name, body) in ctes {
            // Conflict guard
            if self.tables.contains_key(&name) || self.views.contains_key(&name) {
                return Err(format!("CTE name '{}' conflicts with an existing table or view", name));
            }

            // Execute the CTE body and parse the result into virtual rows
            let output = self.execute(*body)?;
            let (col_names, rows) = Self::parse_table_output(&output);

            // Build a minimal schema for the virtual table
            let schema_cols: Vec<crate::catalog::schema::ColumnDef> = col_names.iter().map(|c| {
                crate::catalog::schema::ColumnDef {
                    name: c.clone(),
                    data_type: crate::parser::ast::DataType::Text,
                    primary_key: false,
                    not_null: false,
                    unique: false,
                    unique_constraint_name: None,
                    auto_increment: false,
                    default: None,
                    foreign_key: None,
                    check_expr: None,
                }
            }).collect();

            let _ = self.catalog.create_table(name.clone(), schema_cols);
            self.tables.insert(name.clone(), rows.clone());
            self.buffer_pool.write_page(&name, rows);
            self.indexes.insert(name.clone(), crate::storage::btree::BPlusTree::new());
            cte_names.push(name);
        }

        let result = self.execute(query);

        // Tear down temporary CTE tables
        for name in &cte_names {
            self.tables.remove(name);
            self.indexes.remove(name);
            self.buffer_pool.invalidate(name);
            let _ = self.catalog.drop_table(name);
        }

        result
    }

    fn exec_insert_select(
        &mut self,
        table: String,
        columns: Option<Vec<String>>,
        query: Statement,
    ) -> Result<String, String> {
        let output = self.execute(query)?;
        let (col_names, rows) = Self::parse_table_output(&output);
        if rows.is_empty() {
            return Ok("0 row(s) inserted.".to_string());
        }
        let all_values: Vec<Vec<String>> = rows.iter()
            .map(|row| col_names.iter().map(|c| row.get(c).cloned().unwrap_or_default()).collect())
            .collect();
        let insert_cols = columns.or(Some(col_names));
        self.exec_insert(table, insert_cols, all_values)
    }

    fn exec_insert(
        &mut self,
        table: String,
        col_list: Option<Vec<String>>,
        all_values: Vec<Vec<String>>,
    ) -> Result<String, String> {
        // 스키마 클론 (borrow 충돌 방지)
        let schema = self.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .clone();

        // 컬럼 목록이 있으면 모든 컬럼이 존재하는지 먼저 검증
        if let Some(ref cols) = col_list {
            for col in cols {
                if !schema.columns.iter().any(|c| &c.name == col) {
                    return Err(format!("Column '{}' not found in table '{}'", col, table));
                }
            }
        }

        let col_names: Vec<String> = schema.columns.iter().map(|c| c.name.clone()).collect();
        let constraints: Vec<(bool, bool, bool, bool)> = schema.columns.iter()
            .map(|c| (c.primary_key, c.not_null, c.unique, c.auto_increment))
            .collect();

        // auto_increment 카운터를 로컬에서 추적 (원자성 보장: 실패 시 schema에 반영 안 됨)
        let mut local_counters = schema.auto_increment_counters.clone();

        // ── 1단계: 전체 행 검증 (삽입 없음) ─────────────────────────────
        // 이미 검증 통과한 행들의 UNIQUE/PK 값을 추적 (같은 문장 내 중복 감지)
        let mut seen_unique: Vec<Vec<(usize, String)>> = Vec::new(); // 단일 PK/UNIQUE용
        let mut seen_composite_pk: Vec<Vec<String>> = Vec::new();    // 복합 PK 튜플용

        let mut prepared: Vec<Row> = Vec::new();

        for values in all_values {
            // 컬럼 목록 → 스키마 순서대로 값 매핑
            let positional: Vec<String> = match &col_list {
                None => {
                    if values.len() != schema.columns.len() {
                        return Err(format!(
                            "Column count mismatch: expected {}, got {}",
                            schema.columns.len(), values.len()
                        ));
                    }
                    values
                }
                Some(cols) => {
                    if cols.len() != values.len() {
                        return Err(format!(
                            "Column list length {} doesn't match value count {}",
                            cols.len(), values.len()
                        ));
                    }
                    let col_map: std::collections::HashMap<&str, String> = cols.iter()
                        .map(|s| s.as_str())
                        .zip(values.into_iter())
                        .collect();
                    schema.columns.iter()
                        .map(|c| col_map.get(c.name.as_str()).cloned().unwrap_or_default())
                        .collect()
                }
            };

            let mut final_values = positional;

            // DEFAULT 처리: 값이 비어있고 default가 있으면 default 적용
            for (i, col) in schema.columns.iter().enumerate() {
                if final_values[i].is_empty() {
                    if let Some(ref def) = col.default {
                        final_values[i] = if def == crate::parser::parser::NULL_DEFAULT {
                            NULL_VALUE.to_string()
                        } else if def.to_uppercase() == "NOW()" || def.to_uppercase() == "CURRENT_TIMESTAMP" {
                            // DATETIME/TIMESTAMP DEFAULT NOW()
                            chrono::Local::now().format("%Y-%m-%d %H:%M:%S").to_string()
                        } else {
                            def.clone()
                        };
                    } else {
                        // TIMESTAMP 컬럼에 값 없으면 현재 시각 자동 삽입
                        if matches!(col.data_type, DataType::Timestamp) {
                            final_values[i] = chrono::Local::now().format("%Y-%m-%d %H:%M:%S").to_string();
                        }
                    }
                }
            }

            // AUTO INCREMENT 처리 (로컬 카운터만 갱신)
            for (i, (_, _, _, auto_inc)) in constraints.iter().enumerate() {
                if *auto_inc && final_values[i].is_empty() {
                    let counter = local_counters.entry(col_names[i].clone()).or_insert(0);
                    *counter += 1;
                    final_values[i] = counter.to_string();
                }
            }

            // NOT NULL 검사
            for (i, (_, not_null, _, _)) in constraints.iter().enumerate() {
                if *not_null && (final_values[i].is_empty() || final_values[i] == NULL_VALUE) {
                    return Err(format!("Column '{}' cannot be NULL", col_names[i]));
                }
            }

            // UNIQUE / PRIMARY KEY 중복 검사 — 기존 행 대상
            {
                // 복합 PK 컬럼 목록
                let pk_cols: Vec<&str> = schema.primary_key_columns.iter().map(|s| s.as_str()).collect();
                let is_composite_pk = pk_cols.len() > 1;

                if let Some(rows) = self.tables.get(&table) {
                    if is_composite_pk {
                        // 복합 PK: (col1_val, col2_val, ...) 튜플이 중복인지 체크
                        let new_pk_tuple: Vec<String> = pk_cols.iter()
                            .map(|pk| {
                                col_names.iter().position(|c| c == pk)
                                    .map(|i| final_values[i].clone())
                                    .unwrap_or_default()
                            })
                            .collect();
                        for existing in rows.iter().filter(|r| Self::is_visible(r)) {
                            let existing_tuple: Vec<String> = pk_cols.iter()
                                .map(|pk| existing.get(*pk).cloned().unwrap_or_default())
                                .collect();
                            if existing_tuple == new_pk_tuple {
                                return Err(format!(
                                    "Duplicate composite primary key ({:?})", new_pk_tuple
                                ));
                            }
                        }
                    } else {
                        // 단일 PK / UNIQUE 컬럼 중복 체크
                        for (i, (pk, _, unique, _)) in constraints.iter().enumerate() {
                            if *pk || *unique {
                                let val = &final_values[i];
                                for existing in rows.iter().filter(|r| Self::is_visible(r)) {
                                    if existing.get(&col_names[i]) == Some(val) {
                                        return Err(format!(
                                            "Duplicate value '{}' for column '{}'", val, col_names[i]
                                        ));
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // UNIQUE / PRIMARY KEY 중복 검사 — 같은 INSERT 문 내 앞서 준비된 행 대상
            {
                let pk_cols_batch: Vec<&str> = schema.primary_key_columns.iter()
                    .map(|s| s.as_str()).collect();
                let is_composite_batch = pk_cols_batch.len() > 1;
                if is_composite_batch {
                    let new_pk_tuple: Vec<String> = pk_cols_batch.iter()
                        .map(|pk| col_names.iter().position(|c| c == pk)
                            .map(|i| final_values[i].clone())
                            .unwrap_or_default())
                        .collect();
                    for prev in &seen_composite_pk {
                        if *prev == new_pk_tuple {
                            return Err(format!(
                                "Duplicate composite primary key ({:?})", new_pk_tuple
                            ));
                        }
                    }
                    seen_composite_pk.push(new_pk_tuple);
                } else {
                    let this_row_unique: Vec<(usize, String)> = constraints.iter().enumerate()
                        .filter(|(_, (pk, _, unique, _))| *pk || *unique)
                        .map(|(i, _)| (i, final_values[i].clone()))
                        .collect();
                    for prev in &seen_unique {
                        for (i, val) in &this_row_unique {
                            if prev.iter().any(|(pi, pv)| pi == i && pv == val) {
                                return Err(format!(
                                    "Duplicate value '{}' for column '{}'", val, col_names[*i]
                                ));
                            }
                        }
                    }
                    seen_unique.push(this_row_unique);
                }
            }

            // Row 구성
            let mut row = Row::new();
            for (col, val) in col_names.iter().zip(final_values.iter()) {
                let stored_val = if val.is_empty() { NULL_VALUE.to_string() } else { val.clone() };
                row.insert(col.clone(), stored_val);
            }
            row.insert("_xmin".to_string(), self.txn.current_txn_id().to_string());
            row.insert("_xmax".to_string(), "0".to_string());

            // FOREIGN KEY 검사
            for col in &schema.columns {
                if let Some(fk) = &col.foreign_key {
                    let val = row.get(&col.name).cloned().unwrap_or_default();
                    if val.is_empty() || val == NULL_VALUE { continue; }
                    let ref_rows = self.tables.get(&fk.ref_table)
                        .ok_or(format!("Referenced table '{}' not found", fk.ref_table))?;
                    let exists = ref_rows.iter().any(|r| {
                        r.get(&fk.ref_column).map(|v| v == &val).unwrap_or(false)
                    });
                    if !exists {
                        return Err(format!(
                            "Foreign key violation: '{}' not found in '{}'.'{}'",
                            val, fk.ref_table, fk.ref_column
                        ));
                    }
                }
            }

            // CHECK 제약 검사 (컬럼 레벨)
            for col in &schema.columns {
                if let Some(ref expr) = col.check_expr {
                    if !Self::eval_check_expr(expr, &row) {
                        return Err(format!(
                            "CHECK constraint violated on column '{}': {}",
                            col.name, expr
                        ));
                    }
                }
            }
            // CHECK 제약 검사 (테이블 레벨)
            for check in &schema.check_constraints {
                if !Self::eval_check_expr(&check.expression, &row) {
                    let name = check.name.as_deref().unwrap_or(&check.expression);
                    return Err(format!("CHECK constraint '{}' violated", name));
                }
            }

            prepared.push(row);
        }

        // ── 2단계: 검증 통과 — 모든 행 삽입 ─────────────────────────────

        // auto_increment 카운터를 schema에 반영 후 저장
        if local_counters != schema.auto_increment_counters {
            let schema_mut = self.catalog.get_table_mut(&table).unwrap();
            schema_mut.auto_increment_counters = local_counters;
            let schema_saved = self.catalog.get_table(&table).unwrap();
            self.disk.save_schema(&table, schema_saved);
        }

        let inserted = prepared.len();

        for row in prepared {
            let pk_val = row.get(&col_names[0]).cloned().unwrap_or_default();
            let val_json = serde_json::to_string(&row).unwrap();

            self.txn.log_insert(&table, &pk_val, &val_json);

            if let Some(index) = self.indexes.get_mut(&table) {
                index.insert(pk_val, val_json);
            }

            // 복합 인덱스 갱신
            let comp_keys: Vec<String> = self.composite_indexes.iter()
                .filter(|(_, ci)| ci.table == table)
                .map(|(k, _)| k.clone())
                .collect();
            for k in comp_keys {
                if let Some(ci) = self.composite_indexes.get_mut(&k) {
                    ci.insert_row(&row);
                }
            }

            self.tables.get_mut(&table)
                .ok_or(format!("Table '{}' not found", table))?
                .push(row);
        }

        self.sort_by_pk(&table);

        // 버퍼 풀 갱신
        let rows = self.tables.get(&table).unwrap().clone();
        self.buffer_pool.write_page(&table, rows);

        // 모든 row 삽입 후 flush
        if !self.txn.is_active() {
            self.buffer_pool.flush_page(&table, &self.disk);
        }

        self.maybe_auto_checkpoint();
        Ok(format!("{} row(s) inserted.", inserted))
    }

    /// CHECK 제약 표현식 평가: "col > 0", "col IS NOT NULL", "col >= 1 AND col <= 100" 형식
    fn eval_check_expr(expr: &str, row: &Row) -> bool {
        use crate::parser::parser::Parser;
        let sql = format!("SELECT 1 FROM __check__ WHERE {}", expr);
        match Parser::new(&sql).parse() {
            Ok(crate::parser::ast::Statement::Select { condition: Some(expr), .. }) => {
                Self::eval_condexpr(row, &expr)
            }
            _ => true,
        }
    }

    /// Substitute "table.col" literals in a CondExpr with actual outer row values (correlated subqueries)
    fn substitute_correlated_condexpr(expr: &CondExpr, outer_row: &Row) -> CondExpr {
        match expr {
            CondExpr::And(l, r) => CondExpr::And(
                Box::new(Self::substitute_correlated_condexpr(l, outer_row)),
                Box::new(Self::substitute_correlated_condexpr(r, outer_row)),
            ),
            CondExpr::Or(l, r) => CondExpr::Or(
                Box::new(Self::substitute_correlated_condexpr(l, outer_row)),
                Box::new(Self::substitute_correlated_condexpr(r, outer_row)),
            ),
            CondExpr::Not(inner) => CondExpr::Not(Box::new(Self::substitute_correlated_condexpr(inner, outer_row))),
            CondExpr::Leaf(cond) => {
                let new_value = match &cond.value {
                    ConditionValue::Literal(s) if s.contains('.') => {
                        if let Some(v) = Self::get_col(outer_row, s) {
                            ConditionValue::Literal(v.clone())
                        } else {
                            cond.value.clone()
                        }
                    }
                    other => other.clone(),
                };
                CondExpr::Leaf(Condition {
                    left: cond.left.clone(),
                    operator: cond.operator.clone(),
                    value: new_value,
                })
            }
        }
    }

    fn matches_condexpr(row: &Row, condition: &Option<CondExpr>) -> bool {
        match condition {
            None => true,
            Some(expr) => Self::eval_condexpr(row, expr),
        }
    }

    fn eval_condexpr(row: &Row, expr: &CondExpr) -> bool {
        match expr {
            CondExpr::And(l, r)  => Self::eval_condexpr(row, l) && Self::eval_condexpr(row, r),
            CondExpr::Or(l, r)   => Self::eval_condexpr(row, l) || Self::eval_condexpr(row, r),
            CondExpr::Not(inner) => !Self::eval_condexpr(row, inner),
            CondExpr::Leaf(cond) => Self::eval_single(row, cond),
        }
    }

    fn eval_single(row: &Row, cond: &Condition) -> bool {
        let val = Self::eval_arith(row, &cond.left);

        let cmp_num = |a: &str, b: &str| -> Option<std::cmp::Ordering> {
            let a: f64 = a.parse().ok()?;
            let b: f64 = b.parse().ok()?;
            a.partial_cmp(&b)
        };

        match &cond.value {
            ConditionValue::Subquery(_) => false,
            ConditionValue::Between(start, end) => {
                // NULL in BETWEEN = false
                if val == NULL_VALUE { return false; }
                match (cmp_num(&val, start), cmp_num(&val, end)) {
                    (Some(s), Some(e)) =>
                        s != std::cmp::Ordering::Less && e != std::cmp::Ordering::Greater,
                    _ => val >= *start && val <= *end,
                }
            }
            ConditionValue::LiteralList(list) => {
                if val == NULL_VALUE { return false; }
                match &cond.operator {
                    Operator::In => list.iter().any(|item| {
                        match (val.parse::<f64>(), item.parse::<f64>()) {
                            (Ok(a), Ok(b)) => a == b,
                            _ => val == *item,
                        }
                    }),
                    Operator::NotIn => list.iter().all(|item| {
                        match (val.parse::<f64>(), item.parse::<f64>()) {
                            (Ok(a), Ok(b)) => a != b,
                            _ => val != *item,
                        }
                    }),
                    _ => false,
                }
            }
            ConditionValue::Literal(lit) => {
                // Resolve qualified column references (table.col) against the row.
                // Number literals like "3.14" start with a digit and are excluded.
                let resolved;
                let effective_lit: &str = if lit.contains('.')
                    && lit.chars().next().map(|c| c.is_alphabetic() || c == '_').unwrap_or(false)
                {
                    if let Some(v) = Self::get_col(row, lit) {
                        resolved = v.clone();
                        &resolved
                    } else { lit }
                } else { lit };

                match &cond.operator {
                    Operator::IsNull    => val == NULL_VALUE || val.is_empty(),
                    Operator::IsNotNull => val != NULL_VALUE && !val.is_empty(),
                    // NULL semantics: NULL compared with any non-IS operator = false
                    _ if val == NULL_VALUE => false,
                    _ if effective_lit == "__NULL__" => false,
                    Operator::Eq  => {
                        match (val.parse::<f64>(), effective_lit.parse::<f64>()) {
                            (Ok(a), Ok(b)) => a == b,
                            _ => val.as_str() == effective_lit,
                        }
                    }
                    Operator::Ne  => {
                        match (val.parse::<f64>(), effective_lit.parse::<f64>()) {
                            (Ok(a), Ok(b)) => a != b,
                            _ => val.as_str() != effective_lit,
                        }
                    }
                    Operator::In | Operator::NotIn | Operator::Exists | Operator::NotExists => false,
                    Operator::Like => {
                        let val_chars: Vec<char> = val.chars().collect();
                        let pat_chars: Vec<char> = effective_lit.chars().collect();
                        like_match(&val_chars, &pat_chars)
                    }
                    Operator::Between => false,
                    Operator::Gt  => cmp_num(&val, effective_lit)
                        .map(|o| o == std::cmp::Ordering::Greater).unwrap_or(false),
                    Operator::Lt  => cmp_num(&val, effective_lit)
                        .map(|o| o == std::cmp::Ordering::Less).unwrap_or(false),
                    Operator::Gte => cmp_num(&val, effective_lit)
                        .map(|o| o != std::cmp::Ordering::Less).unwrap_or(false),
                    Operator::Lte => cmp_num(&val, effective_lit)
                        .map(|o| o != std::cmp::Ordering::Greater).unwrap_or(false),
                }
            }
        }
    }

    fn exec_select(
        &mut self,
        table: String,
        subquery: Option<(Box<Statement>, String)>,
        distinct: bool,
        columns: Vec<SelectColumn>,
        condition: Option<CondExpr>,
        joins: Vec<Join>,
        order_by: Vec<OrderBy>,
        group_by: Option<Vec<String>>,
        having: Option<CondExpr>,
        limit: Option<usize>,
        offset: Option<usize>,
        for_update: bool,
    ) -> Result<String, String> {

        // FROM (SELECT ...) AS alias 처리
        if let Some((inner_stmt, alias)) = subquery {
            return self.exec_select_with_subquery(
                *inner_stmt, alias, distinct, columns, condition, joins,
                order_by, group_by, having, limit, offset, for_update,
            );
        }

        // 뷰 처리: 뷰를 FROM 서브쿼리처럼 실행하고 외부 쿼리 조건을 적용
        if let Some(view_stmt) = self.views.remove(&table) {
            let result = self.exec_select_with_subquery(
                view_stmt.clone(),
                table.clone(),
                distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update,
            );
            self.views.insert(table, view_stmt);
            return result;
        }

        // B+Tree 인덱스 검색 (FOR UPDATE / JOIN 있으면 풀 스캔 경로 사용)
        let has_agg = columns.iter().any(|c| matches!(c, SelectColumn::Agg { .. } | SelectColumn::AggAlias { .. }));
        if joins.is_empty() && !has_agg && !for_update {
            if let Some(expr) = &condition {
                // Only apply index optimization for single-leaf conditions (no AND/OR)
                if let CondExpr::Leaf(cond) = expr {
                let pk_col_opt = self.catalog.get_table(&table)
                    .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()));

                let cond_col_name = if let ArithExpr::Col(name) = &cond.left { name.as_str() } else { "" };
                let is_pk_cond = !cond_col_name.is_empty() && pk_col_opt.as_deref() == Some(cond_col_name);

                if is_pk_cond {
                    // ── PK = val → B+Tree 포인트 검색 ─────────────────────
                    if cond.operator == Operator::Eq {
                        if let ConditionValue::Literal(lit) = &cond.value {
                            if let Some(index) = self.indexes.get(&table) {
                                if let Some(val_json) = index.search(lit) {
                                    let row: Row = serde_json::from_str(&val_json).unwrap();
                                    if Self::is_visible(&row) {
                                        return self.format_result(vec![row], columns, table, vec![]);
                                    } else {
                                        return Ok("0 rows returned.".to_string());
                                    }
                                } else {
                                    return Ok("0 rows returned.".to_string());
                                }
                            }
                        }
                    }

                    // ── PK BETWEEN a AND b → B+Tree 범위 스캔 ─────────────
                    if cond.operator == Operator::Between {
                        if let ConditionValue::Between(start, end) = &cond.value {
                            if let Some(index) = self.indexes.get(&table) {
                                let json_vals = index.range_search(start, end);
                                let rows: Vec<Row> = json_vals.iter()
                                    .filter_map(|j| serde_json::from_str(j).ok())
                                    .filter(|r| Self::is_visible(r))
                                    .collect();
                                return self.format_result(rows, columns, table, vec![]);
                            }
                        }
                    }

                    // ── PK > val / PK >= val → scan_from ──────────────────
                    if matches!(cond.operator, Operator::Gt | Operator::Gte) {
                        if let ConditionValue::Literal(lit) = &cond.value {
                            let inclusive = cond.operator == Operator::Gte;
                            if let Some(index) = self.indexes.get(&table) {
                                let pairs = index.scan_from(lit, inclusive);
                                let rows: Vec<Row> = pairs.iter()
                                    .filter_map(|(_, j)| serde_json::from_str(j).ok())
                                    .filter(|r| Self::is_visible(r))
                                    .collect();
                                return self.format_result(rows, columns, table, vec![]);
                            }
                        }
                    }

                    // ── PK < val / PK <= val → scan_to ────────────────────
                    if matches!(cond.operator, Operator::Lt | Operator::Lte) {
                        if let ConditionValue::Literal(lit) = &cond.value {
                            let inclusive = cond.operator == Operator::Lte;
                            if let Some(index) = self.indexes.get(&table) {
                                let pairs = index.scan_to(lit, inclusive);
                                let rows: Vec<Row> = pairs.iter()
                                    .filter_map(|(_, j)| serde_json::from_str(j).ok())
                                    .filter(|r| Self::is_visible(r))
                                    .collect();
                                return self.format_result(rows, columns, table, vec![]);
                            }
                        }
                    }
                }
                } // end CondExpr::Leaf

                // ── 복합 인덱스 검색: WHERE col1 = v1 AND col2 = v2 ───────
                let eq_map = collect_eq_conditions_expr(expr);
                if !eq_map.is_empty() {
                    let matching_idx = self.composite_indexes.iter()
                        .find(|(_, ci)| ci.table == table && ci.matches_conditions(&eq_map))
                        .map(|(k, _)| k.clone());
                    if let Some(idx_key) = matching_idx {
                        let result = self.composite_indexes[&idx_key].search_from_eq_map(&eq_map);
                        if let Some(val_json) = result {
                            if let Ok(row) = serde_json::from_str::<Row>(&val_json) {
                                return self.format_result(vec![row], columns, table, vec![]);
                            }
                        }
                        return Ok("0 rows returned.".to_string());
                    }
                }
            }
        }

        if !self.tables.contains_key(&table) {
            return Err(format!("Table '{}' not found", table));
        }

        // REPEATABLE READ / SERIALIZABLE: 스냅샷에서 읽기
        let rows: Vec<Row> = if let Some(snap_rows) = self.txn.get_snapshot_table(&table) {
            snap_rows.clone()
        } else {
            self.buffer_pool.get_page(&table, &self.disk)
        };
        // MVCC: 논리 삭제된 행(_xmax != "0") 제외
        let rows: Vec<Row> = rows.into_iter().filter(|r| Self::is_visible(r)).collect();

        // JOIN 처리 (다중 JOIN 순차 적용)
        let result: Vec<Row> = if joins.is_empty() {
            rows.into_iter()
                .filter(|r| self.matches_condition_with_subquery(r, &condition))
                .collect()
        } else {
            let mut current = rows;
            for j in &joins {
                let right_rows_raw = if let Some(snap) = self.txn.get_snapshot_table(&j.table) {
                    snap.clone()
                } else {
                    self.tables.get(&j.table)
                        .ok_or(format!("Table '{}' not found", j.table))?.clone()
                };
                let right_rows: Vec<Row> = right_rows_raw.into_iter().filter(|r| Self::is_visible(r)).collect();

                let mut joined = Vec::new();
                // right 테이블 row를 merged row에 합칠 때:
                // 1) "table.col" 형식의 prefixed 키로 저장 (충돌 없음)
                // 2) bare 키는 left 테이블 값이 없을 때만 추가 (left 우선)
                let merge_right = |merged: &mut Row, right: &Row, tbl: &str| {
                    for (k, v) in right.iter() {
                        merged.insert(format!("{}.{}", tbl, k), v.clone());
                        merged.entry(k.clone()).or_insert_with(|| v.clone());
                    }
                };
                let null_right = |merged: &mut Row, right_cols: &[String], tbl: &str| {
                    for col in right_cols {
                        merged.insert(format!("{}.{}", tbl, col), NULL_VALUE.to_string());
                        merged.entry(col.clone()).or_insert_with(|| NULL_VALUE.to_string());
                    }
                };
                let right_schema_cols: Vec<String> = self.catalog.get_table(&j.table)
                    .map(|s| s.columns.iter().map(|c| c.name.clone()).collect())
                    .unwrap_or_default();

                match j.join_type {
                    JoinType::Inner => {
                        for left in &current {
                            for right in &right_rows {
                                let mut merged = left.clone();
                                merge_right(&mut merged, right, &j.table);
                                if Self::eval_condexpr(&merged, &j.on_expr) {
                                    joined.push(merged);
                                }
                            }
                        }
                    }
                    JoinType::Left => {
                        for left in &current {
                            let mut matched = false;
                            for right in &right_rows {
                                let mut merged = left.clone();
                                merge_right(&mut merged, right, &j.table);
                                if Self::eval_condexpr(&merged, &j.on_expr) {
                                    joined.push(merged);
                                    matched = true;
                                }
                            }
                            if !matched {
                                let mut merged = left.clone();
                                null_right(&mut merged, &right_schema_cols, &j.table);
                                joined.push(merged);
                            }
                        }
                    }
                    JoinType::Right => {
                        let left_cols: Vec<String> = current.first()
                            .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                            .unwrap_or_default();
                        for right in &right_rows {
                            let mut matched = false;
                            for left in &current {
                                let mut merged = left.clone();
                                merge_right(&mut merged, right, &j.table);
                                if Self::eval_condexpr(&merged, &j.on_expr) {
                                    joined.push(merged);
                                    matched = true;
                                }
                            }
                            if !matched {
                                let mut merged = Row::new();
                                for col in &left_cols {
                                    merged.insert(col.clone(), NULL_VALUE.to_string());
                                }
                                merge_right(&mut merged, right, &j.table);
                                joined.push(merged);
                            }
                        }
                    }
                }
                current = joined;
            }
            current.into_iter()
                .filter(|r| self.matches_condition_with_subquery(r, &condition))
                .collect()
        };

        // ORDER BY
        let mut result = result;
        if !order_by.is_empty() {
            result.sort_by(|a, b| {
                for ord in &order_by {
                    let av = Self::get_col(a, &ord.column).cloned().unwrap_or_default();
                    let bv = Self::get_col(b, &ord.column).cloned().unwrap_or_default();
                    let cmp = match (av.parse::<f64>(), bv.parse::<f64>()) {
                        (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(std::cmp::Ordering::Equal),
                        _ => av.cmp(&bv),
                    };
                    let cmp = if ord.ascending { cmp } else { cmp.reverse() };
                    if cmp != std::cmp::Ordering::Equal { return cmp; }
                }
                std::cmp::Ordering::Equal
            });
        }

        // GROUP BY + 집계 (통합)
        if let Some(ref group_cols) = group_by {
            // 삽입 순서 유지: order 벡터 + HashMap
            let mut group_order: Vec<Vec<String>> = Vec::new();
            let mut group_data: std::collections::HashMap<Vec<String>, Vec<Row>> =
                std::collections::HashMap::new();
            for row in &result {
                let key: Vec<String> = group_cols.iter()
                    .map(|c| Self::get_col(row, c).cloned().unwrap_or_default())
                    .collect();
                if !group_data.contains_key(&key) { group_order.push(key.clone()); }
                group_data.entry(key).or_default().push(row.clone());
            }

            let mut group_rows: Vec<Row> = group_order.iter().map(|key| {
                let grp = &group_data[key];
                let mut out = Row::new();
                for (col, val) in group_cols.iter().zip(key.iter()) {
                    out.insert(col.clone(), val.clone());
                }
                for col in &columns {
                    let (func, col_name, label) = match col {
                        SelectColumn::Agg { func, col: cn } =>
                            (func, cn.as_str(), Self::agg_label(func, cn)),
                        SelectColumn::AggAlias { func, col: cn, alias } =>
                            (func, cn.as_str(), alias.clone()),
                        _ => continue,
                    };
                    let vals: Vec<f64> = grp.iter()
                        .filter_map(|r| {
                            if col_name == "*" { Some(1.0) }
                            else { r.get(col_name)?.parse::<f64>().ok() }
                        })
                        .collect();
                    let agg_val = match func {
                        AggFunc::Count => grp.len() as f64,
                        AggFunc::Sum   => vals.iter().sum(),
                        AggFunc::Avg   => if vals.is_empty() { 0.0 } else {
                            vals.iter().sum::<f64>() / vals.len() as f64 },
                        AggFunc::Min   => vals.iter().cloned().fold(f64::INFINITY, f64::min),
                        AggFunc::Max   => vals.iter().cloned().fold(f64::NEG_INFINITY, f64::max),
                    };
                    let v = match func {
                        AggFunc::Avg => format!("{:.4}", agg_val),
                        _ => if agg_val.fract() == 0.0 { format!("{}", agg_val as i64) }
                             else { format!("{:.2}", agg_val) },
                    };
                    out.insert(label, v);
                }
                out
            }).collect();

            // HAVING 필터 (집계된 컬럼 기준)
            if let Some(ref hav) = having {
                group_rows.retain(|row| Self::matches_condexpr(row, &Some(hav.clone())));
            }
            if let Some(n) = offset { let skip = n.min(group_rows.len()); group_rows.drain(..skip); }
            if let Some(n) = limit { group_rows.truncate(n); }
            return self.format_result(group_rows, columns, table, joins.clone());
        }

        // HAVING (GROUP BY 없는 경우)
        if let Some(ref hav) = having {
            result.retain(|row| Self::matches_condexpr(row, &Some(hav.clone())));
        }

        // OFFSET then LIMIT
        if let Some(n) = offset { let skip = n.min(result.len()); result.drain(..skip); }
        if let Some(n) = limit { result.truncate(n); }

        // DISTINCT: 선택된 컬럼 기준 중복 제거
        if distinct {
            let mut seen: Vec<Vec<String>> = Vec::new();
            result.retain(|row| {
                let key: Vec<String> = columns.iter().map(|c| match c {
                    SelectColumn::All => row.values().cloned().collect::<Vec<_>>().join(","),
                    SelectColumn::Column(n) | SelectColumn::ColumnAlias(n, _) =>
                        row.get(n).cloned().unwrap_or_default(),
                    SelectColumn::Agg { col, .. } | SelectColumn::AggAlias { col, .. } =>
                        row.get(col).cloned().unwrap_or_default(),
                    SelectColumn::Func { name, args, .. } =>
                        Self::apply_scalar_func(name, args, row),
                    SelectColumn::CaseWhen { branches, else_val, .. } => {
                        let resolve = |s: &str| -> String {
                            Self::get_col(row, s).cloned().unwrap_or_else(|| s.to_string())
                        };
                        let mut v = else_val.as_deref().map(&resolve).unwrap_or_default();
                        for b in branches {
                            if Self::eval_condexpr(row, &b.condition) {
                                v = resolve(&b.result);
                                break;
                            }
                        }
                        v
                    }
                    SelectColumn::Expr { expr, .. } => Self::eval_arith(row, expr),
                }).collect();
                if seen.contains(&key) { false } else { seen.push(key); true }
            });
        }

        // 집계 함수 처리 (GROUP BY 없음)
        if has_agg {
            let mut agg_results: Vec<(String, String)> = Vec::new();
            for col in &columns {
                let (func, col_name, label) = match col {
                    SelectColumn::Agg { func, col: cn } =>
                        (func, cn, Self::agg_label(func, cn)),
                    SelectColumn::AggAlias { func, col: cn, alias } =>
                        (func, cn, alias.clone()),
                    _ => continue,
                };
                let vals: Vec<f64> = result.iter()
                    .filter_map(|r| {
                        if col_name == "*" { Some(1.0) }
                        else { r.get(col_name)?.parse::<f64>().ok() }
                    })
                    .collect();
                let agg_val = match func {
                    AggFunc::Count => result.len() as f64,
                    AggFunc::Sum   => vals.iter().sum(),
                    AggFunc::Avg   => if vals.is_empty() { 0.0 } else {
                        vals.iter().sum::<f64>() / vals.len() as f64
                    },
                    AggFunc::Min   => vals.iter().cloned().fold(f64::INFINITY, f64::min),
                    AggFunc::Max   => vals.iter().cloned().fold(f64::NEG_INFINITY, f64::max),
                };
                let val_str = if agg_val.fract() == 0.0 {
                    format!("{}", agg_val as i64)
                } else {
                    format!("{:.2}", agg_val)
                };
                agg_results.push((label, val_str));
            }

            let col_widths: Vec<usize> = agg_results.iter()
                .map(|(k, v)| k.len().max(v.len()))
                .collect();
            let separator = col_widths.iter()
                .map(|w| "-".repeat(w + 2))
                .collect::<Vec<_>>().join("+");
            let separator = format!("+{}+", separator);
            let mut output = String::new();
            output.push_str(&separator); output.push('\n');
            let header = agg_results.iter().zip(col_widths.iter())
                .map(|((k, _), w)| format!(" {:width$} ", k, width = w))
                .collect::<Vec<_>>().join("|");
            output.push_str(&format!("|{}|\n", header));
            output.push_str(&separator); output.push('\n');
            let row_line = agg_results.iter().zip(col_widths.iter())
                .map(|((_, v), w)| format!(" {:width$} ", v, width = w))
                .collect::<Vec<_>>().join("|");
            output.push_str(&format!("|{}|\n", row_line));
            output.push_str(&separator);
            return Ok(output);
        }

        // FOR UPDATE: 결과 행에 잠금 획득
        if for_update {
            if !self.txn.is_active() {
                return Err("SELECT FOR UPDATE requires an active transaction (BEGIN first).".to_string());
            }
            let txn_id = self.txn.current_txn_id();
            let pk_col = self.catalog.get_table(&table)
                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                .unwrap_or_else(|| "id".to_string());
            for row in &result {
                let pk_val = row.get(&pk_col).cloned().unwrap_or_default();
                match self.lock_mgr.acquire(&table, &pk_val, txn_id) {
                    LockResult::Granted => {}
                    LockResult::Conflict { holder } => {
                        return Err(format!(
                            "Row '{}' in '{}' is locked by transaction {}. Cannot SELECT FOR UPDATE.",
                            pk_val, table, holder
                        ));
                    }
                    LockResult::Deadlock { holder } => {
                        return Err(format!(
                            "Deadlock detected: transaction {} waits for transaction {} (SELECT FOR UPDATE). Transaction {} aborted.",
                            txn_id, holder, txn_id
                        ));
                    }
                }
            }
        }

        self.format_result(result, columns, table, joins)
    }

    // ─── FROM 서브쿼리 실행 ──────────────────────────────────────
    fn exec_select_with_subquery(
        &mut self,
        inner_stmt: Statement,
        alias: String,
        distinct: bool,
        columns: Vec<SelectColumn>,
        condition: Option<CondExpr>,
        joins: Vec<Join>,
        order_by: Vec<OrderBy>,
        group_by: Option<Vec<String>>,
        having: Option<CondExpr>,
        limit: Option<usize>,
        offset: Option<usize>,
        for_update: bool,
    ) -> Result<String, String> {
        if self.tables.contains_key(&alias) || self.views.contains_key(&alias) {
            return Err(format!("Alias '{}' conflicts with an existing table or view", alias));
        }

        let inner_output = self.execute(inner_stmt)?;
        let (col_names, virtual_rows) = Self::parse_table_output(&inner_output);
        if col_names.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        self.tables.insert(alias.clone(), virtual_rows.clone());
        self.buffer_pool.write_page(&alias, virtual_rows);
        let schema_cols: Vec<crate::catalog::schema::ColumnDef> = col_names.iter()
            .map(|name| crate::catalog::schema::ColumnDef {
                name: name.clone(),
                data_type: crate::parser::ast::DataType::Text,
                primary_key: false,
                not_null: false,
                unique: false,
                unique_constraint_name: None,
                auto_increment: false,
                default: None,
                foreign_key: None,
                check_expr: None,
            })
            .collect();
        let _ = self.catalog.create_table(alias.clone(), schema_cols);

        let result = self.exec_select(
            alias.clone(), None, distinct, columns, condition,
            joins, order_by, group_by, having, limit, offset, for_update,
        );

        self.tables.remove(&alias);
        self.buffer_pool.invalidate(&alias);
        let _ = self.catalog.drop_table(&alias);

        result
    }

    /// 포맷된 ASCII 테이블 출력 → (컬럼명 목록, Row 목록)
    /// 행에는 MVCC 필드(_xmin=1, _xmax=0)가 자동으로 추가됨
    fn parse_table_output(output: &str) -> (Vec<String>, Vec<Row>) {
        let lines: Vec<&str> = output.lines().collect();
        if lines.is_empty() || !lines.first().map(|l| l.starts_with('+')).unwrap_or(false) {
            return (vec![], vec![]);
        }

        let mut col_names: Vec<String> = vec![];
        let mut rows: Vec<Row> = vec![];
        let mut header_parsed = false;

        for line in &lines {
            if line.starts_with('+') {
                continue;
            }
            if line.starts_with('|') {
                let cells: Vec<String> = line
                    .split('|')
                    .filter(|s| !s.is_empty())
                    .map(|s| s.trim().to_string())
                    .collect();
                if !header_parsed {
                    col_names = cells;
                    header_parsed = true;
                } else {
                    let mut row = Row::new();
                    for (i, col) in col_names.iter().enumerate() {
                        let val = cells.get(i).cloned().unwrap_or_default();
                        row.insert(col.clone(), val);
                    }
                    // 가상 행은 항상 visible (MVCC 필드 설정)
                    row.insert("_xmin".to_string(), "1".to_string());
                    row.insert("_xmax".to_string(), "0".to_string());
                    rows.push(row);
                }
            }
        }

        (col_names, rows)
    }

    /// 스칼라 함수 평가: row에서 인수를 해석해 결과 문자열 반환
    fn apply_scalar_func(func_name: &str, args: &[String], row: &Row) -> String {
        // 인수를 row 컬럼값 또는 리터럴로 해석
        let resolve = |arg: &str, row: &Row| -> String {
            if arg.starts_with('\'') && arg.ends_with('\'') {
                arg[1..arg.len()-1].to_string()
            } else if let Some(v) = row.get(arg) {
                v.clone()
            } else {
                // table.col 형태
                if let Some(idx) = arg.rfind('.') {
                    row.get(&arg[idx+1..]).cloned().unwrap_or_else(|| arg.to_string())
                } else {
                    arg.to_string()
                }
            }
        };

        match func_name {
            "UPPER" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.to_uppercase()
            }
            "LOWER" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.to_lowercase()
            }
            "LENGTH" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.len().to_string()
            }
            "TRIM" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.trim().to_string()
            }
            "CONCAT" => {
                args.iter().map(|a| resolve(a, row)).collect::<Vec<_>>().join("")
            }
            "SUBSTR" | "SUBSTRING" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let start: usize = args.get(1).and_then(|a| resolve(a, row).parse::<i64>().ok())
                    .map(|n| if n > 0 { (n - 1) as usize } else { 0 })
                    .unwrap_or(0);
                let len_opt: Option<usize> = args.get(2).and_then(|a| resolve(a, row).parse::<usize>().ok());
                let chars: Vec<char> = v.chars().collect();
                let end = len_opt.map(|l| (start + l).min(chars.len())).unwrap_or(chars.len());
                chars[start.min(chars.len())..end].iter().collect()
            }
            "NOW" => {
                chrono::Local::now().format("%Y-%m-%d %H:%M:%S").to_string()
            }
            "CURDATE" => {
                chrono::Local::now().format("%Y-%m-%d").to_string()
            }
            "DATE_FORMAT" => {
                let date_val = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let fmt_arg = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                // 간단한 포맷 변환: %Y, %m, %d
                let parts: Vec<&str> = date_val.split('-').collect();
                fmt_arg
                    .replace("%Y", parts.first().copied().unwrap_or(""))
                    .replace("%m", parts.get(1).copied().unwrap_or(""))
                    .replace("%d", parts.get(2).copied().unwrap_or(""))
            }
            "COALESCE" => {
                for arg in args {
                    let v = resolve(arg, row);
                    if v != NULL_VALUE && !v.is_empty() { return v; }
                }
                NULL_VALUE.to_string()
            }
            "IFNULL" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                if v == NULL_VALUE || v.is_empty() {
                    args.get(1).map(|a| resolve(a, row)).unwrap_or_default()
                } else {
                    v
                }
            }
            "REPLACE" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let from = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                let to   = args.get(2).map(|a| resolve(a, row)).unwrap_or_default();
                v.replace(&from, &to)
            }
            "ROUND" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default()
                    .parse().unwrap_or(0.0);
                let decimals: i32 = args.get(1).map(|a| resolve(a, row))
                    .unwrap_or_default().parse().unwrap_or(0);
                let factor = 10f64.powi(decimals);
                format!("{}", (v * factor).round() / factor)
            }
            "ABS" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default()
                    .parse().unwrap_or(0.0);
                format!("{}", v.abs())
            }
            "CEIL" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default()
                    .parse().unwrap_or(0.0);
                format!("{}", v.ceil())
            }
            "FLOOR" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default()
                    .parse().unwrap_or(0.0);
                format!("{}", v.floor())
            }
            "MOD" => {
                let a: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default()
                    .parse().unwrap_or(0.0);
                let b: f64 = args.get(1).map(|a| resolve(a, row)).unwrap_or_default()
                    .parse().unwrap_or(1.0);
                if b == 0.0 { "NULL".to_string() } else { format!("{}", a % b) }
            }
            // IF(condition_col, true_val, false_val)
            // condition_col is evaluated: non-empty and non-zero = true
            "IF" => {
                let cond_val = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let true_val  = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                let false_val = args.get(2).map(|a| resolve(a, row)).unwrap_or_default();
                let is_true = !cond_val.is_empty()
                    && cond_val != "0"
                    && cond_val != "false"
                    && cond_val != NULL_VALUE;
                if is_true { true_val } else { false_val }
            }
            _ => format!("{}()", func_name),
        }
    }

    fn agg_label(func: &AggFunc, col: &str) -> String {
        match func {
            AggFunc::Count => format!("COUNT({})", col),
            AggFunc::Sum   => format!("SUM({})", col),
            AggFunc::Avg   => format!("AVG({})", col),
            AggFunc::Min   => format!("MIN({})", col),
            AggFunc::Max   => format!("MAX({})", col),
        }
    }

    fn format_result(
        &self,
        result: Vec<Row>,
        columns: Vec<SelectColumn>,
        table: String,
        joins: Vec<Join>,
    ) -> Result<String, String> {
        if result.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        // 열 정의: (헤더명, 값 추출 방법 — Key 또는 Func 평가)
        enum ColSource {
            Key(String),
            Func { name: String, args: Vec<String> },
            CaseWhen { branches: Vec<CaseWhenBranch>, else_val: Option<String> },
            Expr(ArithExpr),
        }
        let col_defs: Vec<(String, ColSource)> = if columns.iter().any(|c| c == &SelectColumn::All) {
            let mut pairs: Vec<(String, ColSource)> = self.catalog.get_table(&table)
                .map(|s| s.columns.iter().map(|c| (c.name.clone(), ColSource::Key(c.name.clone()))).collect())
                .unwrap_or_default();
            for j in &joins {
                if let Some(schema) = self.catalog.get_table(&j.table) {
                    for c in &schema.columns {
                        pairs.push((c.name.clone(), ColSource::Key(c.name.clone())));
                    }
                }
            }
            pairs
        } else {
            columns.iter().filter_map(|c| match c {
                SelectColumn::Column(name) => {
                    // 헤더는 bare 컬럼명 (table.col → col)
                    let header = name.rfind('.').map(|i| name[i+1..].to_string()).unwrap_or_else(|| name.clone());
                    Some((header, ColSource::Key(name.clone())))
                }
                SelectColumn::ColumnAlias(name, alias) => Some((alias.clone(), ColSource::Key(name.clone()))),
                SelectColumn::Agg { func, col } => {
                    let lbl = Self::agg_label(func, col);
                    Some((lbl.clone(), ColSource::Key(lbl)))
                }
                SelectColumn::AggAlias { func, col, alias } => {
                    let _lbl = Self::agg_label(func, col);
                    Some((alias.clone(), ColSource::Key(alias.clone())))
                }
                SelectColumn::Func { name, args, alias } => {
                    let header = alias.clone().unwrap_or_else(|| format!("{}()", name));
                    Some((header, ColSource::Func { name: name.clone(), args: args.clone() }))
                }
                SelectColumn::CaseWhen { branches, else_val, alias } => {
                    let header = alias.clone().unwrap_or_else(|| "CASE".to_string());
                    Some((header, ColSource::CaseWhen {
                        branches: branches.clone(),
                        else_val: else_val.clone(),
                    }))
                }
                SelectColumn::Expr { expr, alias } => {
                    let header = alias.clone().unwrap_or_else(|| arith_to_str(expr));
                    Some((header, ColSource::Expr(expr.clone())))
                }
                SelectColumn::All => None,
            }).collect()
        };

        // 모든 행의 값을 미리 계산해서 width 계산에 사용
        let resolved_rows: Vec<Vec<String>> = result.iter().map(|row| {
            col_defs.iter().map(|(_, src)| {
                let raw = match src {
                    ColSource::Key(key) => Self::get_col(row, key).cloned().unwrap_or_default(),
                    ColSource::Func { name, args } => Self::apply_scalar_func(name, args, row),
                    ColSource::Expr(expr) => Self::eval_arith(row, expr),
                    ColSource::CaseWhen { branches, else_val } => {
                        let resolve = |s: &str| -> String {
                            Self::get_col(row, s).cloned().unwrap_or_else(|| s.to_string())
                        };
                        let mut result_val = else_val.as_deref()
                            .map(&resolve)
                            .unwrap_or_else(|| NULL_VALUE.to_string());
                        for branch in branches {
                            if Self::eval_condexpr(row, &branch.condition) {
                                result_val = resolve(&branch.result);
                                break;
                            }
                        }
                        result_val
                    }
                };
                if raw == NULL_VALUE { "NULL".to_string() } else { raw }
            }).collect()
        }).collect();

        let col_widths: Vec<usize> = col_defs.iter().enumerate().map(|(i, (header, _))| {
            let max_val = resolved_rows.iter()
                .map(|row_vals| row_vals[i].len())
                .max().unwrap_or(0);
            header.len().max(max_val)
        }).collect();

        let mut output = String::new();
        let separator = col_widths.iter()
            .map(|w| "-".repeat(w + 2))
            .collect::<Vec<_>>().join("+");
        let separator = format!("+{}+", separator);

        output.push_str(&separator); output.push('\n');
        let header = col_defs.iter().zip(col_widths.iter())
            .map(|((h, _), w)| format!(" {:width$} ", h, width = w))
            .collect::<Vec<_>>().join("|");
        output.push_str(&format!("|{}|\n", header));
        output.push_str(&separator); output.push('\n');

        for row_vals in &resolved_rows {
            let line = row_vals.iter().zip(col_widths.iter())
                .map(|(val, w)| format!(" {:width$} ", val, width = w))
                .collect::<Vec<_>>().join("|");
            output.push_str(&format!("|{}|\n", line));
        }
        output.push_str(&separator);
        output.push_str(&format!("\n{} row(s) returned.", result.len()));
        Ok(output)
    }

    fn exec_update(
        &mut self,
        table: String,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
    ) -> Result<String, String> {
        // PK 컬럼명 먼저 추출 (borrow 분리)
        let pk_col = self.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .columns.iter()
            .find(|c| c.primary_key)
            .map(|c| c.name.clone())
            .unwrap_or_else(|| "id".to_string());

        // 서브쿼리 조건 지원: 먼저 매칭되는 PK 목록을 수집 (borrow 분리)
        let candidate_rows: Vec<Row> = self.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .iter()
            .filter(|r| Self::is_visible(r))
            .cloned()
            .collect();
        let matching_pks: Vec<String> = candidate_rows.iter()
            .filter(|r| self.matches_condition_with_subquery(r, &condition))
            .map(|r| r.get(&pk_col).cloned().unwrap_or_default())
            .collect();

        let rows = self.tables.get_mut(&table)
            .ok_or(format!("Table '{}' not found", table))?;

        let mut count = 0;
        let mut undo_entries: Vec<(String, String, String)> = Vec::new();
        let cur_txn = self.txn.current_txn_id();

        for row in rows.iter_mut() {
            if matching_pks.contains(&row.get(&pk_col).cloned().unwrap_or_default()) {
                let key = row.get(&pk_col).cloned().unwrap_or_default();

                // 잠금 충돌 / 데드락 체크 (활성 트랜잭션 안에서만)
                if cur_txn != 0 {
                    match self.lock_mgr.acquire(&table, &key, cur_txn) {
                        LockResult::Granted => {}
                        LockResult::Conflict { holder } => {
                            return Err(format!(
                                "Row '{}' in '{}' is locked by transaction {}. Cannot UPDATE.",
                                key, table, holder
                            ));
                        }
                        LockResult::Deadlock { holder } => {
                            return Err(format!(
                                "Deadlock detected: transaction {} waits for transaction {} (UPDATE '{}'. Transaction {} aborted.",
                                cur_txn, holder, table, cur_txn
                            ));
                        }
                    }
                }

                let old_json = serde_json::to_string(row).unwrap();
                // Evaluate all RHS before writing any LHS (preserves self-referential semantics)
                let new_vals: Vec<(String, String)> = assignments.iter()
                    .map(|(col, expr)| (col.clone(), Self::eval_arith(row, expr)))
                    .collect();
                for (col, val) in new_vals {
                    row.insert(col, val);
                }
                // CHECK 제약 검사 (수정 후 row 기준)
                if let Some(schema) = self.catalog.get_table(&table) {
                    for col in &schema.columns {
                        if let Some(ref expr) = col.check_expr {
                            if !Self::eval_check_expr(expr, row) {
                                return Err(format!(
                                    "CHECK constraint violated on column '{}': {}",
                                    col.name, expr
                                ));
                            }
                        }
                    }
                    for check in &schema.check_constraints {
                        if !Self::eval_check_expr(&check.expression, row) {
                            let cname = check.name.as_deref().unwrap_or(&check.expression);
                            return Err(format!("CHECK constraint '{}' violated", cname));
                        }
                    }
                }
                let new_json = serde_json::to_string(row).unwrap();
                undo_entries.push((key, old_json, new_json));
                count += 1;
            }
        }

        // WAL 로깅 (트랜잭션 활성 시만)
        for (key, old_json, new_json) in &undo_entries {
            self.txn.log_update(&table, key, old_json, new_json);
        }

        let rows_clone = self.tables.get(&table).unwrap().clone();
        if let Some(index) = self.indexes.get_mut(&table) {
            *index = BPlusTree::new();
            for row in &rows_clone {
                let k = row.get(&pk_col).cloned().unwrap_or_default();
                let val_json = serde_json::to_string(row).unwrap();
                index.insert(k, val_json);
            }
        }

        // 복합 인덱스 재빌드
        let comp_keys: Vec<String> = self.composite_indexes.iter()
            .filter(|(_, ci)| ci.table == table)
            .map(|(k, _)| k.clone())
            .collect();
        for k in comp_keys {
            if let Some(ci) = self.composite_indexes.get_mut(&k) {
                ci.rebuild(&rows_clone);
            }
        }

        // ON UPDATE FK 처리: assignments에 변경된 컬럼이 다른 테이블에서 FK로 참조되는지 확인
        let changed_cols: Vec<String> = assignments.iter().map(|(c, _)| c.clone()).collect();
        let other_tables: Vec<(String, Vec<crate::catalog::schema::ColumnDef>)> =
            self.catalog.tables.iter()
                .filter(|(name, _)| *name != &table)
                .map(|(name, schema)| (name.clone(), schema.columns.clone()))
                .collect();

        for (_, old_json, _) in &undo_entries {
            // 이전 PK 값: old_json에서 pk_col 추출
            let old_row: Row = serde_json::from_str(old_json).unwrap_or_default();
            for assign_col in &changed_cols {
                let old_val = old_row.get(assign_col).cloned().unwrap_or_default();
                let new_val = assignments.iter()
                    .find(|(c, _)| c == assign_col)
                    .map(|(_, expr)| Self::eval_arith(&old_row, expr))
                    .unwrap_or_default();
                if old_val == new_val { continue; }

                for (other_table, cols) in &other_tables {
                    for col in cols {
                        if let Some(fk) = &col.foreign_key {
                            if fk.ref_table == table && fk.ref_column == *assign_col {
                                match fk.on_update {
                                    crate::catalog::schema::FkAction::Restrict => {
                                        if let Some(other_rows) = self.tables.get(other_table) {
                                            let referenced = other_rows.iter()
                                                .filter(|r| Self::is_visible(r))
                                                .any(|r| r.get(&col.name).map(|v| v == &old_val).unwrap_or(false));
                                            if referenced {
                                                return Err(format!(
                                                    "Foreign key violation (ON UPDATE RESTRICT): '{}' is referenced by '{}'.'{}'",
                                                    assign_col, other_table, col.name
                                                ));
                                            }
                                        }
                                    }
                                    crate::catalog::schema::FkAction::Cascade => {
                                        if let Some(other_rows) = self.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &old_val).unwrap_or(false) {
                                                    row.insert(col.name.clone(), new_val.clone());
                                                }
                                            }
                                        }
                                        let rows_clone2 = self.tables.get(other_table).unwrap().clone();
                                        self.buffer_pool.write_page(other_table, rows_clone2.clone());
                                        self.buffer_pool.flush_page(other_table, &self.disk);
                                    }
                                    crate::catalog::schema::FkAction::SetNull => {
                                        if let Some(other_rows) = self.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &old_val).unwrap_or(false) {
                                                    row.insert(col.name.clone(), NULL_VALUE.to_string());
                                                }
                                            }
                                        }
                                        let rows_clone2 = self.tables.get(other_table).unwrap().clone();
                                        self.buffer_pool.write_page(other_table, rows_clone2.clone());
                                        self.buffer_pool.flush_page(other_table, &self.disk);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        let rows = self.tables.get(&table).unwrap().clone();
        self.buffer_pool.write_page(&table, rows);
        self.buffer_pool.flush_page(&table, &self.disk);
        self.maybe_auto_checkpoint();
        Ok(format!("{} row(s) updated.", count))
    }

    fn exec_delete(&mut self, table: String, condition: Option<CondExpr>) -> Result<String, String> {
        // 서브쿼리 조건 지원: 먼저 매칭 행을 수집 (borrow 분리)
        let candidates: Vec<Row> = self.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .iter()
            .filter(|r| Self::is_visible(r))
            .cloned()
            .collect();
        let rows_to_delete: Vec<Row> = candidates.into_iter()
            .filter(|r| self.matches_condition_with_subquery(r, &condition))
            .collect();

        // FK 처리 (CASCADE / RESTRICT / SET NULL)
        let other_tables: Vec<(String, Vec<crate::catalog::schema::ColumnDef>)> =
            self.catalog.tables.iter()
                .filter(|(name, _)| *name != &table)
                .map(|(name, schema)| (name.clone(), schema.columns.clone()))
                .collect();

        for del_row in &rows_to_delete {
            for (other_table, cols) in &other_tables {
                for col in cols {
                    if let Some(fk) = &col.foreign_key {
                        if fk.ref_table == table {
                            let del_val = del_row.get(&fk.ref_column)
                                .cloned().unwrap_or_default();

                            match fk.on_delete {
                                crate::catalog::schema::FkAction::Restrict => {
                                    if let Some(other_rows) = self.tables.get(other_table) {
                                        let referenced = other_rows.iter()
                                            .filter(|r| Self::is_visible(r))
                                            .any(|r| r.get(&col.name).map(|v| v == &del_val).unwrap_or(false));
                                        if referenced {
                                            return Err(format!(
                                                "Foreign key violation: row in '{}' is referenced by '{}'.'{}'",
                                                table, other_table, col.name
                                            ));
                                        }
                                    }
                                }
                                crate::catalog::schema::FkAction::Cascade => {
                                    if self.txn.is_active() {
                                        // 트랜잭션 안: MVCC 논리 삭제
                                        let txn_id = self.txn.current_txn_id().to_string();
                                        if let Some(other_rows) = self.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &del_val).unwrap_or(false) {
                                                    row.insert("_xmax".to_string(), txn_id.clone());
                                                }
                                            }
                                        }
                                    } else {
                                        // 트랜잭션 밖: 물리 삭제
                                        if let Some(other_rows) = self.tables.get_mut(other_table) {
                                            other_rows.retain(|r| {
                                                !(Self::is_visible(r) && r.get(&col.name).map(|v| v == &del_val).unwrap_or(false))
                                            });
                                        }
                                    }
                                    let rows_clone = self.tables.get(other_table).unwrap().clone();
                                    self.buffer_pool.write_page(other_table, rows_clone.clone());
                                    self.buffer_pool.flush_page(other_table, &self.disk);
                                }
                                crate::catalog::schema::FkAction::SetNull => {
                                    if let Some(other_rows) = self.tables.get_mut(other_table) {
                                        for row in other_rows.iter_mut() {
                                            if Self::is_visible(row) && row.get(&col.name).map(|v| v == &del_val).unwrap_or(false) {
                                                row.insert(col.name.clone(), NULL_VALUE.to_string());
                                            }
                                        }
                                    }
                                    let rows_clone = self.tables.get(other_table).unwrap().clone();
                                    self.buffer_pool.write_page(other_table, rows_clone.clone());
                                    self.buffer_pool.flush_page(other_table, &self.disk);
                                }
                            }
                        }
                    }
                }
            }
        }

        let pk_col = self.catalog.get_table(&table)
            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
            .unwrap_or_else(|| "id".to_string());
        let mut deleted = 0usize;

        if self.txn.is_active() {
            // ── 트랜잭션 안: MVCC 논리 삭제 (_xmax = txn_id) ──
            let txn_id = self.txn.current_txn_id();
            let txn_id_str = txn_id.to_string();
            let rows = self.tables.get_mut(&table).unwrap();
            for row in rows.iter_mut() {
                if Self::is_visible(row) && Self::matches_condexpr(row, &condition) {
                    let key = row.get(&pk_col).cloned().unwrap_or_default();

                    // 잠금 충돌 / 데드락 체크
                    match self.lock_mgr.acquire(&table, &key, txn_id) {
                        LockResult::Granted => {}
                        LockResult::Conflict { holder } => {
                            return Err(format!(
                                "Row '{}' in '{}' is locked by transaction {}. Cannot DELETE.",
                                key, table, holder
                            ));
                        }
                        LockResult::Deadlock { holder } => {
                            return Err(format!(
                                "Deadlock detected: transaction {} waits for transaction {} (DELETE '{}'. Transaction {} aborted.",
                                txn_id, holder, table, txn_id
                            ));
                        }
                    }

                    let old_json = serde_json::to_string(row).unwrap();
                    self.txn.log_delete(&table, &key, &old_json);
                    row.insert("_xmax".to_string(), txn_id_str.clone());
                    deleted += 1;
                }
            }
        } else {
            // ── 트랜잭션 밖: 물리 삭제 ──
            let rows = self.tables.get_mut(&table).unwrap();
            let before = rows.len();
            rows.retain(|r| !(Self::is_visible(r) && Self::matches_condexpr(r, &condition)));
            deleted = before - rows.len();
        }

        let rows_clone = self.tables.get(&table).unwrap().clone();

        if !self.txn.is_active() {
            // 물리 삭제 후: 인덱스 재빌드 + 버퍼 풀 즉시 flush
            if let Some(index) = self.indexes.get_mut(&table) {
                *index = BPlusTree::new();
                for row in &rows_clone {
                    let key = row.values().next().cloned().unwrap_or_default();
                    let val_json = serde_json::to_string(row).unwrap();
                    index.insert(key, val_json);
                }
            }
            let comp_keys: Vec<String> = self.composite_indexes.iter()
                .filter(|(_, ci)| ci.table == table)
                .map(|(k, _)| k.clone())
                .collect();
            for k in comp_keys {
                if let Some(ci) = self.composite_indexes.get_mut(&k) {
                    ci.rebuild(&rows_clone);
                }
            }
            self.buffer_pool.write_page(&table, rows_clone.clone());
            self.buffer_pool.flush_page(&table, &self.disk);
        } else {
            // 논리 삭제 후: 버퍼 풀 갱신 (SELECT가 최신 _xmax 반영)
            self.buffer_pool.write_page(&table, rows_clone);
        }

        self.maybe_auto_checkpoint();
        Ok(format!("{} row(s) deleted.", deleted))
    }

    fn exec_begin(&mut self) -> Result<String, String> {
        let txn_id = self.txn.begin_with_snapshot(&self.tables)?;
        let level = format!("{:?}", self.txn.isolation_level);
        Ok(format!("Transaction {} started. (isolation: {})", txn_id, level))
    }

    fn exec_commit(&mut self) -> Result<String, String> {
        // SERIALIZABLE: 커밋 전 팬텀 읽기 검증
        if let Err(e) = self.txn.validate_serializable(&self.tables) {
            // 검증 실패 → 자동 롤백 후 오류 반환
            self.apply_rollback();
            return Err(format!("{} (auto-rolled back)", e));
        }

        // 트랜잭션 중 수정된 테이블을 버퍼 풀 + 디스크에 반영
        let dirty = self.txn.dirty_tables();
        for table in &dirty {
            if let Some(rows) = self.tables.get(table) {
                let rows_clone = rows.clone();
                self.buffer_pool.write_page(table, rows_clone);
                self.buffer_pool.flush_page(table, &self.disk);
            }
        }

        let txn_id = self.txn.current_txn_id();
        self.txn.commit()?;
        // 이 트랜잭션이 보유한 모든 잠금 해제
        self.lock_mgr.release(txn_id);
        Ok("Transaction committed.".to_string())
    }

    /// 롤백 공통 헬퍼: exec_rollback과 SERIALIZABLE 자동 롤백에서 공유
    fn apply_rollback(&mut self) {
        let txn_id = self.txn.current_txn_id();
        let undo_entries = match self.txn.abort() {
            Ok(entries) => entries,
            Err(_) => return,
        };
        self.lock_mgr.release(txn_id);
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    let pk_col = self.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = self.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.get(&pk_col).cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    self.disk.save_table(&entry.table, &rows_clone);
                }
                "UPDATE" => {
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            let pk_col = self.catalog.get_table(&entry.table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());
                            if let Some(rows) = self.tables.get_mut(&entry.table) {
                                for row in rows.iter_mut() {
                                    if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                        *row = old_row.clone();
                                        break;
                                    }
                                }
                            }
                            let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                            self.disk.save_table(&entry.table, &rows_clone);
                        }
                    }
                }
                "DELETE" => {
                    // MVCC: 논리 삭제 취소 → _xmax = "0" 복원
                    let pk_col = self.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        for row in rows.iter_mut() {
                            if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                row.insert("_xmax".to_string(), "0".to_string());
                            }
                        }
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    self.disk.save_table(&entry.table, &rows_clone);
                }
                _ => {}
            }
        }
    }

    fn exec_rollback(&mut self) -> Result<String, String> {
        let txn_id = self.txn.current_txn_id();
        let undo_entries = self.txn.abort()?;
        // 잠금 해제
        self.lock_mgr.release(txn_id);
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    let pk_col = self.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = self.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.get(&pk_col).cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    self.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    self.buffer_pool.flush_page(&entry.table, &self.disk);
                }
                "UPDATE" => {
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            // PK 컬럼명 추출
                            let pk_col = self.catalog.get_table(&entry.table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());

                            if let Some(rows) = self.tables.get_mut(&entry.table) {
                                for row in rows.iter_mut() {
                                    if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                        *row = old_row.clone();
                                        break;
                                    }
                                }
                            }
                            let rows_clone = self.tables.get(&entry.table).unwrap().clone();
                            // B+Tree 인덱스 재빌드 (rollback 후 stale 데이터 방지)
                            if let Some(index) = self.indexes.get_mut(&entry.table) {
                                *index = BPlusTree::new();
                                for row in &rows_clone {
                                    let k = row.get(&pk_col).cloned().unwrap_or_default();
                                    let v = serde_json::to_string(row).unwrap();
                                    index.insert(k, v);
                                }
                            }
                            self.buffer_pool.write_page(&entry.table, rows_clone.clone());
                            self.buffer_pool.flush_page(&entry.table, &self.disk);
                        }
                    }
                }
                "DELETE" => {
                    // MVCC: 논리 삭제 취소 → _xmax = "0" 복원
                    let pk_col = self.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        for row in rows.iter_mut() {
                            if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                row.insert("_xmax".to_string(), "0".to_string());
                            }
                        }
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    // B+Tree 인덱스 재빌드
                    if let Some(index) = self.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            if Self::is_visible(row) {
                                let k = row.get(&pk_col).cloned().unwrap_or_default();
                                let v = serde_json::to_string(row).unwrap();
                                index.insert(k, v);
                            }
                        }
                    }
                    self.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    self.buffer_pool.flush_page(&entry.table, &self.disk);
                }
                _ => {}
            }
        }
        Ok("Transaction rolled back.".to_string())
    }

    fn exec_savepoint(&mut self, name: String) -> Result<String, String> {
        self.txn.create_savepoint(&name)?;
        Ok(format!("Savepoint '{}' created.", name))
    }

    fn exec_release_savepoint(&mut self, name: String) -> Result<String, String> {
        self.txn.release_savepoint(&name)?;
        Ok(format!("Savepoint '{}' released.", name))
    }

    fn exec_rollback_to(&mut self, name: String) -> Result<String, String> {
        let undo_entries = self.txn.rollback_to_savepoint(&name)?;
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    let pk_col = self.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = self.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.get(&pk_col).cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    self.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    self.buffer_pool.flush_page(&entry.table, &self.disk);
                }
                "UPDATE" => {
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            let pk_col = self.catalog.get_table(&entry.table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());
                            if let Some(rows) = self.tables.get_mut(&entry.table) {
                                for row in rows.iter_mut() {
                                    if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                        *row = old_row.clone();
                                        break;
                                    }
                                }
                            }
                            let rows_clone = self.tables.get(&entry.table).unwrap().clone();
                            self.buffer_pool.write_page(&entry.table, rows_clone.clone());
                            self.buffer_pool.flush_page(&entry.table, &self.disk);
                        }
                    }
                }
                "DELETE" => {
                    let pk_col = self.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        for row in rows.iter_mut() {
                            if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                row.insert("_xmax".to_string(), "0".to_string());
                            }
                        }
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    self.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    self.buffer_pool.flush_page(&entry.table, &self.disk);
                }
                _ => {}
            }
        }
        Ok(format!("Rolled back to savepoint '{}'.", name))
    }

    fn exec_alter(&mut self, table: String, action: AlterAction) -> Result<String, String> {
        match action {
            AlterAction::AddColumn(col) => {
                let schema = self.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                schema.columns.push(SchemaCol {
                    name: col.name.clone(),
                    data_type: col.data_type,
                    primary_key: false,
                    not_null: col.not_null,
                    unique: col.unique,
                    unique_constraint_name: col.unique_constraint_name,
                    auto_increment: false,
                    default: col.default.clone(),
                    foreign_key: None,
                    check_expr: col.check_expr,
                });
                // 기존 행에 default 값(없으면 NULL) 채우기
                let fill_val = match &col.default {
                    Some(d) if d == crate::parser::parser::NULL_DEFAULT => NULL_VALUE.to_string(),
                    Some(d) => d.clone(),
                    None    => NULL_VALUE.to_string(),
                };
                if let Some(rows) = self.tables.get_mut(&table) {
                    for row in rows.iter_mut() {
                        row.insert(col.name.clone(), fill_val.clone());
                    }
                }
                let full_schema = self.catalog.get_table(&table).unwrap();
                self.disk.save_schema(&table, full_schema);
                self.disk.save_table(&table, self.tables.get(&table).unwrap());
                Ok(format!("Column '{}' added to '{}'.", col.name, table))
            }
            AlterAction::DropColumn(col_name) => {
                let schema = self.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                schema.columns.retain(|c| c.name != col_name);
                if let Some(rows) = self.tables.get_mut(&table) {
                    for row in rows.iter_mut() {
                        row.remove(&col_name);
                    }
                }
                let full_schema = self.catalog.get_table(&table).unwrap();
                self.disk.save_schema(&table, full_schema);
                self.disk.save_table(&table, self.tables.get(&table).unwrap());
                Ok(format!("Column '{}' dropped from '{}'.", col_name, table))
            }
            AlterAction::RenameColumn { from, to } => {
                let schema = self.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                for col in schema.columns.iter_mut() {
                    if col.name == from { col.name = to.clone(); }
                }
                if let Some(rows) = self.tables.get_mut(&table) {
                    for row in rows.iter_mut() {
                        if let Some(val) = row.remove(&from) {
                            row.insert(to.clone(), val);
                        }
                    }
                }
                let full_schema = self.catalog.get_table(&table).unwrap();
                self.disk.save_schema(&table, full_schema);
                self.disk.save_table(&table, self.tables.get(&table).unwrap());
                Ok(format!("Column '{}' renamed to '{}' in '{}'.", from, to, table))
            }
            AlterAction::ModifyColumn(col) => {
                // 컬럼 존재 확인
                let exists = self.catalog.tables.get(&table)
                    .ok_or(format!("Table '{}' not found", table))?
                    .columns.iter().any(|c| c.name == col.name);
                if !exists {
                    return Err(format!("Column '{}' not found in '{}'", col.name, table));
                }
                // 기존 데이터 타입 변환 검증: 기존 행의 값이 새 타입으로 캐스팅 가능한지 확인
                if let Some(rows) = self.tables.get(&table) {
                    for row in rows.iter().filter(|r| Self::is_visible(r)) {
                        if let Some(val) = row.get(&col.name) {
                            if val == NULL_VALUE || val.is_empty() { continue; }
                            let ok = match &col.data_type {
                                DataType::Int   => val.parse::<i64>().is_ok(),
                                DataType::Float => val.parse::<f64>().is_ok(),
                                DataType::Boolean => matches!(val.to_lowercase().as_str(), "true" | "false" | "1" | "0"),
                                DataType::Text | DataType::Varchar(_) | DataType::Date
                                | DataType::DateTime | DataType::Timestamp => true,
                                DataType::Decimal(_, _) => val.parse::<f64>().is_ok(),
                                DataType::Unknown => true,
                            };
                            if !ok {
                                return Err(format!(
                                    "Cannot convert value '{}' in column '{}' to {:?}",
                                    val, col.name, col.data_type
                                ));
                            }
                        }
                    }
                }
                // 스키마 업데이트
                let schema = self.catalog.tables.get_mut(&table).unwrap();
                if let Some(c) = schema.columns.iter_mut().find(|c| c.name == col.name) {
                    c.data_type = col.data_type;
                    c.not_null   = col.not_null;
                    c.unique     = col.unique;
                    c.unique_constraint_name = col.unique_constraint_name;
                    c.auto_increment = col.auto_increment;
                    c.default    = col.default;
                    // primary_key는 MODIFY로 변경 불가 (무시)
                }
                let full_schema = self.catalog.get_table(&table).unwrap();
                self.disk.save_schema(&table, full_schema);
                Ok(format!("Column '{}' in '{}' modified.", col.name, table))
            }
        }
    }

    fn matches_condition_with_subquery(&mut self, row: &Row, condition: &Option<CondExpr>) -> bool {
        match condition {
            None => true,
            Some(expr) => self.eval_condexpr_with_subquery(row, expr),
        }
    }

    fn eval_condexpr_with_subquery(&mut self, row: &Row, expr: &CondExpr) -> bool {
        match expr {
            CondExpr::And(l, r) =>
                self.eval_condexpr_with_subquery(row, l) && self.eval_condexpr_with_subquery(row, r),
            CondExpr::Or(l, r) =>
                self.eval_condexpr_with_subquery(row, l) || self.eval_condexpr_with_subquery(row, r),
            CondExpr::Not(inner) => !self.eval_condexpr_with_subquery(row, inner),
            CondExpr::Leaf(cond) => self.eval_single_with_subquery(row, cond),
        }
    }

    fn eval_single_with_subquery(&mut self, row: &Row, cond: &Condition) -> bool {
        match &cond.value.clone() {
            ConditionValue::Literal(_) | ConditionValue::Between(_, _) | ConditionValue::LiteralList(_) => {
                Self::eval_single(row, cond)
            }
            ConditionValue::Subquery(sub_stmt) => {
                if matches!(cond.operator, Operator::Exists | Operator::NotExists) {
                    if let Statement::Select {
                        table, subquery, distinct, columns, condition: sub_cond,
                        joins, order_by, group_by, having, limit, offset, ..
                    } = *sub_stmt.clone() {
                        let sub_cond = sub_cond.map(|c| Self::substitute_correlated_condexpr(&c, row));
                        let result = self.exec_select(
                            table, subquery, distinct, columns, sub_cond,
                            joins, order_by, group_by, having, limit, offset, false
                        );
                        let has_rows = match result {
                            Ok(ref output) => !output.contains("0 rows returned"),
                            Err(_) => false,
                        };
                        return match cond.operator {
                            Operator::Exists    => has_rows,
                            Operator::NotExists => !has_rows,
                            _ => unreachable!(),
                        };
                    }
                    return false;
                }

                let val = Self::eval_arith(row, &cond.left);
                if val == NULL_VALUE { return false; }

                if let Statement::Select {
                    table, subquery, distinct, columns, condition: sub_cond,
                    joins, order_by, group_by, having, limit, offset, ..
                } = *sub_stmt.clone() {
                    let result = self.exec_select(
                        table, subquery, distinct, columns.clone(), sub_cond,
                        joins, order_by, group_by, having, limit, offset, false
                    );
                    match result {
                        Ok(output) => {
                            let sub_vals = self.extract_values_from_output(&output);
                            match cond.operator {
                                Operator::In    => sub_vals.contains(&val),
                                Operator::NotIn => !sub_vals.contains(&val),
                                Operator::Eq    => sub_vals.first()
                                    .map(|v| {
                                        match (val.parse::<f64>(), v.parse::<f64>()) {
                                            (Ok(a), Ok(b)) => a == b,
                                            _ => v == &val,
                                        }
                                    }).unwrap_or(false),
                                Operator::Gt | Operator::Lt |
                                Operator::Gte | Operator::Lte => {
                                    if let Some(sub_val) = sub_vals.first() {
                                        let a: f64 = val.parse().unwrap_or(0.0);
                                        let b: f64 = sub_val.parse().unwrap_or(0.0);
                                        match cond.operator {
                                            Operator::Gt  => a > b,
                                            Operator::Lt  => a < b,
                                            Operator::Gte => a >= b,
                                            Operator::Lte => a <= b,
                                            _ => false,
                                        }
                                    } else { false }
                                }
                                _ => false,
                            }
                        }
                        Err(_) => false,
                    }
                } else { false }
            }
        }
    }

    fn extract_values_from_output(&self, output: &str) -> Vec<String> {
        // 테이블 출력에서 첫 번째 컬럼 값들 추출
        // +----+-------+
        // | id | name  |
        // +----+-------+
        // | 1  | Alice |
        let mut vals = Vec::new();
        let mut header_passed = false;
        let mut separator_count = 0;

        for line in output.lines() {
            if line.starts_with('+') {
                separator_count += 1;
                if separator_count == 2 { header_passed = true; }
                continue;
            }
            if line.starts_with('|') && header_passed {
                // 첫 번째 셀 값 추출
                let first_val = line.split('|')
                    .filter(|s| !s.is_empty())
                    .next()
                    .map(|s| s.trim().to_string());
                if let Some(v) = first_val {
                    if !v.is_empty() {
                        vals.push(v);
                    }
                }
            }
        }
        vals
    }

    fn exec_create_index(&mut self, index_name: String, table: String, columns: Vec<String>) -> Result<String, String> {
        if !self.tables.contains_key(&table) {
            return Err(format!("Table '{}' not found", table));
        }

        if columns.len() == 1 {
            // 단일 컬럼 → 기존 BPlusTree 인덱스
            let column = &columns[0];
            let mut tree = BPlusTree::new();
            if let Some(rows) = self.tables.get(&table) {
                for row in rows {
                    if let Some(val) = row.get(column) {
                        let json = serde_json::to_string(row).unwrap();
                        tree.insert(val.clone(), json);
                    }
                }
            }
            let key = format!("{}_{}", table, index_name);
            self.indexes.insert(key, tree);
            self.index_meta.insert(index_name.clone(), (table.clone(), column.clone()));
            self.persist_index_meta();
            Ok(format!("Index '{}' created on '{}'.'{}'.", index_name, table, column))
        } else {
            // 복합 컬럼 → CompositeIndex
            let mut comp = CompositeIndex::new(table.clone(), columns.clone());
            if let Some(rows) = self.tables.get(&table) {
                comp.rebuild(rows);
            }
            self.composite_indexes.insert(index_name.clone(), comp);
            self.persist_index_meta();
            Ok(format!("Composite index '{}' created on '{}' ({}).", index_name, table, columns.join(", ")))
        }
    }

    fn exec_drop_index(&mut self, index_name: String) -> Result<String, String> {
        if let Some((table, _)) = self.index_meta.remove(&index_name) {
            let key = format!("{}_{}", table, index_name);
            self.indexes.remove(&key);
            self.persist_index_meta();
            Ok(format!("Index '{}' dropped.", index_name))
        } else if self.composite_indexes.remove(&index_name).is_some() {
            self.persist_index_meta();
            Ok(format!("Composite index '{}' dropped.", index_name))
        } else {
            Ok(format!("Index '{}' does not exist, skipped.", index_name))
        }
    }

    fn exec_create_view(&mut self, name: String, query: Statement) -> Result<String, String> {
        if let Statement::Select { ref table, .. } = query {
            if !self.tables.contains_key(table) {
                return Err(format!("Table '{}' not found", table));
            }
        }
        self.views.insert(name.clone(), query);
        self.disk.save_views(&self.views);
        Ok(format!("View '{}' created.", name))
    }

    fn exec_drop_view(&mut self, name: String) -> Result<String, String> {
        if self.views.remove(&name).is_some() {
            self.disk.save_views(&self.views);
            Ok(format!("View '{}' dropped.", name))
        } else {
            Ok(format!("View '{}' does not exist, skipped.", name))
        }
    }

    /// 현재 index_meta + composite_indexes를 disk에 저장
    fn persist_index_meta(&self) {
        let mut meta_list: Vec<IndexMeta> = Vec::new();
        for (name, (table, col)) in &self.index_meta {
            meta_list.push(IndexMeta {
                name: name.clone(),
                table: table.clone(),
                columns: vec![col.clone()],
            });
        }
        for (name, comp) in &self.composite_indexes {
            meta_list.push(IndexMeta {
                name: name.clone(),
                table: comp.table.clone(),
                columns: comp.columns.clone(),
            });
        }
        self.disk.save_index_meta(&meta_list);
    }

    fn exec_show_tables(&self) -> Result<String, String> {
        let tables: Vec<String> = self.catalog.tables.keys().cloned().collect();
        if tables.is_empty() {
            return Ok("No tables found.".to_string());
        }
        let mut output = String::new();
        let max_len = tables.iter().map(|t| t.len()).max().unwrap_or(5).max(5);
        let sep = format!("+{}+", "-".repeat(max_len + 2));
        output.push_str(&format!("{}\n", sep));
        output.push_str(&format!("| {:width$} |\n", "Tables", width = max_len));
        output.push_str(&format!("{}\n", sep));
        for t in &tables {
            output.push_str(&format!("| {:width$} |\n", t, width = max_len));
        }
        output.push_str(&sep);
        Ok(output)
    }

    fn exec_describe(&self, table: String) -> Result<String, String> {
        let schema = self.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?;
        let mut output = String::new();
        let sep = "+------------------+---------+-----+-----+----------------+-----------------+";
        output.push_str(&format!("{}\n", sep));
        output.push_str("| Field            | Type    | PK  | NN  | Auto Increment | Default         |\n");
        output.push_str(&format!("{}\n", sep));
        for col in &schema.columns {
            let type_str = match &col.data_type {
                crate::parser::ast::DataType::Int     => "INT".to_string(),
                crate::parser::ast::DataType::Text    => "TEXT".to_string(),
                crate::parser::ast::DataType::Float   => "FLOAT".to_string(),
                crate::parser::ast::DataType::Boolean => "BOOLEAN".to_string(),
                crate::parser::ast::DataType::Date      => "DATE".to_string(),
                crate::parser::ast::DataType::DateTime  => "DATETIME".to_string(),
                crate::parser::ast::DataType::Timestamp => "TIMESTAMP".to_string(),
                crate::parser::ast::DataType::Varchar(n) => format!("VARCHAR({})", n),
                crate::parser::ast::DataType::Decimal(p, s) => format!("DECIMAL({},{})", p, s),
                crate::parser::ast::DataType::Unknown => "UNKNOWN".to_string(),
            };
            let def_str = match &col.default {
                None    => "NULL".to_string(),
                Some(d) if d == crate::parser::parser::NULL_DEFAULT => "NULL".to_string(),
                Some(d) => d.clone(),
            };
            output.push_str(&format!(
                "| {:16} | {:7} | {:3} | {:3} | {:14} | {:15} |\n",
                col.name, type_str,
                if col.primary_key { "YES" } else { "NO" },
                if col.not_null { "YES" } else { "NO" },
                if col.auto_increment { "YES" } else { "NO" },
                def_str,
            ));
        }
        output.push_str(sep);
        Ok(output)
    }

    fn exec_show_buffer_pool(&self) -> Result<String, String> {
        let mut output = String::new();
        let sep = "+----------------------+---------+";
        output.push_str(&format!("{}\n", sep));
        output.push_str("| 항목                 | 값      |\n");
        output.push_str(&format!("{}\n", sep));
        output.push_str(&format!("| 캐시 사용량          | {:7} |\n", self.buffer_pool.usage()));
        output.push_str(&format!("| 최대 용량            | {:7} |\n", 64));
        output.push_str(&format!("| 캐시 히트            | {:7} |\n", self.buffer_pool.hit_count));
        output.push_str(&format!("| 캐시 미스            | {:7} |\n", self.buffer_pool.miss_count));
        output.push_str(&format!("| 적중률               | {:6.1}% |\n", self.buffer_pool.hit_rate()));
        output.push_str(sep);
        Ok(output)
    }

    fn exec_show_wal(&self) -> Result<String, String> {
        let records = self.txn.wal_records();
        let size = self.txn.wal_size();
        let mut out = String::new();
        let sep = "+------------+----------+----------+";
        out.push_str(&format!("WAL 파일 크기: {} bytes\n", size));
        out.push_str(&format!("{}\n", sep));
        out.push_str("| op         | table    | key      |\n");
        out.push_str(&format!("{}\n", sep));
        for r in &records {
            out.push_str(&format!("| {:<10} | {:<8} | {:<8} |\n",
                format!("{:?}", r.op),
                &r.table_name[..r.table_name.len().min(8)],
                &r.key[..r.key.len().min(8)],
            ));
        }
        out.push_str(sep);
        Ok(out)
    }

    fn exec_set_isolation_level(&mut self, level: IsolationLevel) -> Result<String, String> {
        let name = match &level {
            IsolationLevel::ReadUncommitted => "READ UNCOMMITTED",
            IsolationLevel::ReadCommitted   => "READ COMMITTED",
            IsolationLevel::RepeatableRead  => "REPEATABLE READ",
            IsolationLevel::Serializable    => "SERIALIZABLE",
        };
        self.txn.set_isolation_level(level);
        Ok(format!("Isolation level set to {}.", name))
    }

    fn exec_show_isolation_level(&self) -> Result<String, String> {
        let name = match self.txn.isolation_level {
            IsolationLevel::ReadUncommitted => "READ UNCOMMITTED",
            IsolationLevel::ReadCommitted   => "READ COMMITTED",
            IsolationLevel::RepeatableRead  => "REPEATABLE READ",
            IsolationLevel::Serializable    => "SERIALIZABLE",
        };
        Ok(format!("Current isolation level: {}", name))
    }

    /// VACUUM [table]: 논리 삭제된 행(_xmax != "0")을 물리적으로 제거
    fn exec_vacuum(&mut self, table: Option<String>) -> Result<String, String> {
        let targets: Vec<String> = match table {
            Some(t) => {
                if !self.tables.contains_key(&t) {
                    return Err(format!("Table '{}' not found", t));
                }
                vec![t]
            }
            None => self.tables.keys().cloned().collect(),
        };

        let mut total_removed = 0usize;
        for t in &targets {
            let rows = self.tables.get_mut(t).unwrap();
            let before = rows.len();
            rows.retain(|r| Self::is_visible(r));
            let removed = before - rows.len();
            total_removed += removed;

            if removed > 0 {
                // 인덱스 재빌드
                let rows_clone = self.tables.get(t).unwrap().clone();
                if let Some(index) = self.indexes.get_mut(t) {
                    *index = BPlusTree::new();
                    for row in &rows_clone {
                        let key = row.values().next().cloned().unwrap_or_default();
                        let val_json = serde_json::to_string(row).unwrap();
                        index.insert(key, val_json);
                    }
                }
                let comp_keys: Vec<String> = self.composite_indexes.iter()
                    .filter(|(_, ci)| ci.table == *t)
                    .map(|(k, _)| k.clone())
                    .collect();
                for k in comp_keys {
                    if let Some(ci) = self.composite_indexes.get_mut(&k) {
                        ci.rebuild(&rows_clone);
                    }
                }
                self.buffer_pool.write_page(t, rows_clone.clone());
                self.buffer_pool.flush_page(t, &self.disk);
            }
        }

        Ok(format!("VACUUM complete. {} dead row(s) removed.", total_removed))
    }

    /// EXPLAIN <SELECT> — 쿼리 실행 계획 출력 (실제 실행 안 함)
    fn exec_explain(&self, stmt: Statement) -> Result<String, String> {
        let (table, condition, joins) = match &stmt {
            Statement::Select { table, condition, joins, subquery, .. } => {
                if subquery.is_some() {
                    return Ok("EXPLAIN: Subquery-based SELECT → Strategy: SUBQUERY SCAN".to_string());
                }
                (table.clone(), condition.clone(), joins.clone())
            }
            other => return Ok(format!("EXPLAIN: {:?} → no index optimization available", other)),
        };

        // 테이블 통계
        let row_count = self.tables.get(&table).map(|r| r.len()).unwrap_or(0);
        let visible_count = self.tables.get(&table)
            .map(|rows| rows.iter().filter(|r| Self::is_visible(r)).count())
            .unwrap_or(0);

        let pk_col = self.catalog.get_table(&table)
            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()));

        // 접근 경로 결정
        let access_path = if !joins.is_empty() {
            format!("FULL SCAN + JOIN ({})", joins.iter().map(|j| j.table.as_str()).collect::<Vec<_>>().join(", "))
        } else if let Some(expr) = &condition {
            // Extract first leaf for index analysis
            let leaf = condexpr_first_leaf(expr);
            if let Some(cond) = leaf {
                let cond_col = if let ArithExpr::Col(name) = &cond.left { name.as_str() } else { "" };
                let is_pk = !cond_col.is_empty()
                    && pk_col.as_deref() == Some(cond_col)
                    && matches!(expr, CondExpr::Leaf(_));

                if is_pk {
                    match &cond.operator {
                        Operator::Eq => {
                            if let ConditionValue::Literal(v) = &cond.value {
                                format!("INDEX SCAN (pk={})  [B+Tree point lookup, cost≈O(log {})]", v, row_count)
                            } else { "INDEX SCAN (pk=subquery)".to_string() }
                        }
                        Operator::Between => {
                            if let ConditionValue::Between(a, b) = &cond.value {
                                format!("INDEX RANGE SCAN (pk BETWEEN {} AND {})  [B+Tree range, cost≈O(log {}+k)]", a, b, row_count)
                            } else { "INDEX RANGE SCAN".to_string() }
                        }
                        Operator::Gt => {
                            if let ConditionValue::Literal(v) = &cond.value {
                                format!("INDEX RANGE SCAN (pk > {})  [B+Tree scan_from, cost≈O(log {}+k)]", v, row_count)
                            } else { "INDEX RANGE SCAN".to_string() }
                        }
                        Operator::Gte => {
                            if let ConditionValue::Literal(v) = &cond.value {
                                format!("INDEX RANGE SCAN (pk >= {})  [B+Tree scan_from, cost≈O(log {}+k)]", v, row_count)
                            } else { "INDEX RANGE SCAN".to_string() }
                        }
                        Operator::Lt => {
                            if let ConditionValue::Literal(v) = &cond.value {
                                format!("INDEX RANGE SCAN (pk < {})  [B+Tree scan_to, cost≈O(log {}+k)]", v, row_count)
                            } else { "INDEX RANGE SCAN".to_string() }
                        }
                        Operator::Lte => {
                            if let ConditionValue::Literal(v) = &cond.value {
                                format!("INDEX RANGE SCAN (pk <= {})  [B+Tree scan_to, cost≈O(log {}+k)]", v, row_count)
                            } else { "INDEX RANGE SCAN".to_string() }
                        }
                        _ => format!("FULL SCAN  [no index for {:?}, cost≈O({})]", cond.operator, row_count),
                    }
                } else {
                    let single_idx = self.index_meta.iter()
                        .find(|(_, (t, c))| t == &table && c == cond_col)
                        .map(|(k, _)| k.clone());
                    if let Some(idx_name) = single_idx {
                        match &cond.operator {
                            Operator::Eq => format!("INDEX SCAN ({} on '{}')  [B+Tree point, cost≈O(log {})]", idx_name, cond_col, row_count),
                            Operator::Between | Operator::Gt | Operator::Gte | Operator::Lt | Operator::Lte => {
                                format!("INDEX RANGE SCAN ({} on '{}')  [B+Tree range, cost≈O(log {}+k)]", idx_name, cond_col, row_count)
                            }
                            _ => format!("INDEX SCAN ({} on '{}')  [filtered, cost≈O(log {})]", idx_name, cond_col, row_count),
                        }
                    } else {
                        let eq_map = collect_eq_conditions_expr(expr);
                        let comp_idx = self.composite_indexes.iter()
                            .find(|(_, ci)| ci.table == table && ci.matches_conditions(&eq_map))
                            .map(|(k, _)| k.clone());
                        if let Some(idx_name) = comp_idx {
                            format!("COMPOSITE INDEX SCAN ({})  [cost≈O(1)]", idx_name)
                        } else {
                            format!("FULL SCAN  [no index on '{}', cost≈O({})]", cond_col, row_count)
                        }
                    }
                }
            } else {
                format!("FULL SCAN  [complex WHERE, cost≈O({})]", row_count)
            }
        } else {
            format!("FULL SCAN  [no WHERE clause, cost≈O({})]", row_count)
        };

        let mut out = String::new();
        out.push_str("+--------------------------------------------------+\n");
        out.push_str("|                  QUERY PLAN                      |\n");
        out.push_str("+--------------------------------------------------+\n");
        out.push_str(&format!("| Table       : {:<35}|\n", table));
        out.push_str(&format!("| Total rows  : {:<35}|\n", row_count));
        out.push_str(&format!("| Visible rows: {:<35}|\n", visible_count));
        if let Some(pk) = &pk_col {
            out.push_str(&format!("| PK column   : {:<35}|\n", pk));
        }
        out.push_str("|                                                  |\n");
        // access_path가 길면 줄바꿈
        for (i, chunk) in access_path.as_bytes().chunks(48).enumerate() {
            let s = std::str::from_utf8(chunk).unwrap_or("?");
            if i == 0 {
                out.push_str(&format!("| Access path : {:<35}|\n", s));
            } else {
                out.push_str(&format!("|               {:<35}|\n", s));
            }
        }
        out.push_str("+--------------------------------------------------+");
        Ok(out)
    }

    /// SHOW LOCKS: 보유 잠금 + wait-for 그래프 + 데드락 이력 출력
    fn exec_show_locks(&self) -> Result<String, String> {
        let mut output = String::new();

        // ── 1. 현재 보유 잠금 ──────────────────────────────────────────
        let locks = self.lock_mgr.lock_rows();
        if locks.is_empty() {
            output.push_str("No active row locks.\n");
        } else {
            output.push_str("+------------------+-----+--------+\n");
            output.push_str("| table            | key | txn_id |\n");
            output.push_str("+------------------+-----+--------+\n");
            for (tbl, key, txn_id) in &locks {
                output.push_str(&format!("| {:16} | {:3} | {:6} |\n", tbl, key, txn_id));
            }
            output.push_str("+------------------+-----+--------+\n");
        }

        // ── 2. Wait-for 그래프 ────────────────────────────────────────
        let wait_for = self.lock_mgr.wait_for_rows();
        if !wait_for.is_empty() {
            output.push_str("\nWait-for graph:\n");
            for (waiter, blocker) in &wait_for {
                output.push_str(&format!("  txn {} waits for txn {}\n", waiter, blocker));
            }
        }

        // ── 3. 데드락 이력 ────────────────────────────────────────────
        let history = self.lock_mgr.deadlock_history();
        if !history.is_empty() {
            output.push_str("\nDeadlock history (this session):\n");
            for (victim, blocker) in history {
                output.push_str(&format!("  txn {} deadlocked with txn {} (victim: {})\n", victim, blocker, victim));
            }
        }

        if output.trim().is_empty() {
            output = "No active row locks.".to_string();
        }
        Ok(output.trim_end().to_string())
    }

    /// 테이블 행을 PK 기준으로 정렬 (클러스터드 인덱스: 물리적 저장 순서 = PK 순서)
    fn sort_by_pk(&mut self, table: &str) {
        let pk_col = self.catalog.get_table(table)
            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()));
        if let Some(pk) = pk_col {
            if let Some(rows) = self.tables.get_mut(table) {
                rows.sort_by(|a, b| {
                    let ka = a.get(&pk).cloned().unwrap_or_default();
                    let kb = b.get(&pk).cloned().unwrap_or_default();
                    match (ka.parse::<i64>(), kb.parse::<i64>()) {
                        (Ok(na), Ok(nb)) => na.cmp(&nb),
                        _ => ka.cmp(&kb),
                    }
                });
            }
        }
    }

    /// 수동 CHECKPOINT 명령 실행:
    /// 1) 버퍼풀의 모든 dirty 페이지를 디스크에 flush
    /// 2) WAL에 CHECKPOINT 레코드 기록
    /// 3) 이전 커밋된 레코드를 WAL에서 정리
    fn exec_checkpoint(&mut self) -> Result<String, String> {
        let dirty_before = self.buffer_pool.usage();
        self.buffer_pool.flush_all(&self.disk);
        self.txn.do_checkpoint();
        Ok(format!(
            "Checkpoint completed. {} dirty page(s) flushed.",
            dirty_before
        ))
    }

    /// 자동 체크포인트: WAL 크기가 임계값을 초과하면 체크포인트를 수행한다.
    /// 활성 트랜잭션 중에도 중간 체크포인트를 찍어 복구 범위를 줄인다.
    fn maybe_auto_checkpoint(&mut self) {
        if self.txn.needs_auto_checkpoint() {
            self.buffer_pool.flush_all(&self.disk);
            self.txn.do_checkpoint();
            eprintln!("[AutoCheckpoint] WAL 임계값 초과 → 체크포인트 실행");
        }
    }

    fn recover_from_wal(&mut self) {
        let records = self.txn.wal_records();
        if records.is_empty() { return; }

        // 마지막 CHECKPOINT 이후 레코드만 재생 (체크포인트 이전은 이미 디스크에 반영됨)
        let start_idx = records
            .iter()
            .rposition(|r| matches!(r.op, crate::transaction::wal::WalOp::Checkpoint))
            .map(|i| i + 1)
            .unwrap_or(0);

        let replay_records = &records[start_idx..];
        if replay_records.is_empty() {
            self.txn.wal_clear();
            return;
        }

        // COMMIT 레코드가 있는지 확인
        let has_commit = replay_records.iter().any(|r| {
            matches!(r.op, crate::transaction::wal::WalOp::Commit)
        });

        if !has_commit {
            // 미완료 트랜잭션 → WAL 삭제 (rollback 처리)
            self.txn.wal_clear();
            eprintln!("[Recovery] 미완료 트랜잭션 감지 → WAL 삭제");
            return;
        }

        // COMMIT된 트랜잭션 replay (체크포인트 이후 레코드만)
        eprintln!("[Recovery] WAL replay 시작 ({} 레코드, start_idx={})", replay_records.len(), start_idx);
        for record in replay_records {
            match record.op {
                crate::transaction::wal::WalOp::Insert => {
                    if let Ok(row) = serde_json::from_str::<Row>(&record.data) {
                        let table = &record.table_name;
                        if let Some(rows) = self.tables.get_mut(table) {
                            // 이미 존재하면 스킵
                            let pk_col = self.catalog.get_table(table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());
                            let key = row.get(&pk_col).cloned().unwrap_or_default();
                            let exists = rows.iter().any(|r| r.get(&pk_col).map(|v| v == &key).unwrap_or(false));
                            if !exists {
                                rows.push(row.clone());
                                let val_json = serde_json::to_string(&row).unwrap();
                                if let Some(index) = self.indexes.get_mut(table) {
                                    index.insert(key, val_json);
                                }
                                self.disk.save_table(table, self.tables.get(table).unwrap());
                                eprintln!("[Recovery] INSERT replay: {}", table);
                            }
                        }
                    }
                }
                crate::transaction::wal::WalOp::Update => {
                    if let Ok(new_row) = serde_json::from_str::<Row>(&record.data) {
                        let table = &record.table_name;
                        let pk_col = self.catalog.get_table(table)
                            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                            .unwrap_or_else(|| "id".to_string());
                        if let Some(rows) = self.tables.get_mut(table) {
                            for row in rows.iter_mut() {
                                if row.get(&pk_col) == new_row.get(&pk_col) {
                                    *row = new_row.clone();
                                    break;
                                }
                            }
                        }
                        self.disk.save_table(table, self.tables.get(table).unwrap());
                        eprintln!("[Recovery] UPDATE replay: {}", table);
                    }
                }
                crate::transaction::wal::WalOp::Delete => {
                    let table = &record.table_name;
                    let pk_col = self.catalog.get_table(table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = self.tables.get_mut(table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &record.key).unwrap_or(true));
                    }
                    self.disk.save_table(table, self.tables.get(table).unwrap());
                    eprintln!("[Recovery] DELETE replay: {}", table);
                }
                _ => {}
            }
        }

        // Replay 완료 후 WAL 삭제
        self.txn.wal_clear();
        eprintln!("[Recovery] WAL replay 완료 → WAL 삭제");
    }
}

/// CondExpr 트리에서 AND-연결된 `col = literal` 조건들을 수집 (복합 인덱스용)
fn collect_eq_conditions_expr(expr: &CondExpr) -> HashMap<String, String> {
    let mut map = HashMap::new();
    collect_eq_recursive(expr, &mut map);
    map
}

fn collect_eq_recursive(expr: &CondExpr, map: &mut HashMap<String, String>) {
    match expr {
        CondExpr::And(l, r) => {
            collect_eq_recursive(l, map);
            collect_eq_recursive(r, map);
        }
        CondExpr::Or(_, _) | CondExpr::Not(_) => {} // OR/NOT breaks composite index optimization
        CondExpr::Leaf(c) if c.operator == Operator::Eq => {
            if let (ArithExpr::Col(name), ConditionValue::Literal(lit)) = (&c.left, &c.value) {
                map.insert(name.clone(), lit.clone());
            }
        }
        CondExpr::Leaf(_) => {}
    }
}

/// Returns the first leaf Condition in a CondExpr (for index analysis)
fn condexpr_first_leaf(expr: &CondExpr) -> Option<&Condition> {
    match expr {
        CondExpr::Leaf(c) => Some(c),
        CondExpr::And(l, _) | CondExpr::Or(l, _) => condexpr_first_leaf(l),
        CondExpr::Not(inner) => condexpr_first_leaf(inner),
    }
}

fn arith_to_str(expr: &ArithExpr) -> String {
    match expr {
        ArithExpr::Col(name) => name.clone(),
        ArithExpr::Num(n) => n.clone(),
        ArithExpr::Str(s) => format!("'{}'", s),
        ArithExpr::Add(l, r) => format!("{}+{}", arith_to_str(l), arith_to_str(r)),
        ArithExpr::Sub(l, r) => format!("{}-{}", arith_to_str(l), arith_to_str(r)),
        ArithExpr::Mul(l, r) => format!("{}*{}", arith_to_str(l), arith_to_str(r)),
        ArithExpr::Div(l, r) => format!("{}/{}", arith_to_str(l), arith_to_str(r)),
    }
}

fn like_match(val: &[char], pat: &[char]) -> bool {
    match (val, pat) {
        (_, []) => val.is_empty(),
        ([], ['%', rest @ ..]) => like_match(&[], rest),
        ([], _) => false,
        ([_, v_rest @ ..], ['%', p_rest @ ..]) =>
            like_match(v_rest, pat) || like_match(val, p_rest),
        ([_, v_rest @ ..], ['_', p_rest @ ..]) => like_match(v_rest, p_rest),
        ([v, v_rest @ ..], [p, p_rest @ ..]) =>
            v == p && like_match(v_rest, p_rest),
    }
}