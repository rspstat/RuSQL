// src/engine/executor.rs

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, RwLock};
use chrono;
use serde::{Serialize, Deserialize};
use crate::transaction::txn_manager::TransactionManager;
use crate::parser::ast::*;
use crate::catalog::schema::{Catalog, ColumnDef as SchemaCol};
use crate::storage::disk::{DiskManager, IndexMeta};
use crate::storage::btree::BPlusTree;
use crate::storage::buffer_pool::BufferPool;
use crate::storage::composite_index::CompositeIndex;
use crate::engine::lock_manager::{LockManager, LockResult};
use crate::engine::planner::{Planner, AccessPath, JoinAlgo};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UserRecord {
    pub user: String,
    pub host: String,
    pub password_hash: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GrantRecord {
    pub user: String,
    pub host: String,
    pub object_type: String,
    pub object: String,
    pub privileges: Vec<String>,
    pub with_grant_option: bool,
}

pub type Row = HashMap<String, String>;
pub const NULL_VALUE: &str = "NULL";

pub struct SharedDatabase {
    pub catalog: Catalog,
    pub tables: HashMap<String, Vec<Row>>,
    pub indexes: HashMap<String, BPlusTree>,
    pub index_meta: HashMap<String, (String, String)>,
    pub composite_indexes: HashMap<String, CompositeIndex>,
    pub views: HashMap<String, Statement>,
    pub buffer_pool: BufferPool,
    pub disk: DiskManager,
    pub lock_mgr: LockManager,
    pub databases: HashSet<String>,
    pub users: Vec<UserRecord>,
    pub grants: Vec<GrantRecord>,
}

pub struct Executor {
    pub shared: Arc<RwLock<SharedDatabase>>,
    pub txn: TransactionManager,
    pub current_db: String,
}

impl Executor {
    /// "db.table" → ("db", "table")
    fn split_key(key: &str) -> (&str, &str) {
        if let Some(pos) = key.find('.') { (&key[..pos], &key[pos+1..]) }
        else { ("rustdb", key) }
    }

    /// Build a qualified key: if name has no dot, prefix with current_db
    fn qualify_name(&self, name: String) -> String {
        if name.contains('.') { name } else { format!("{}.{}", self.current_db, name) }
    }

    /// Strip current_db prefix for display
    fn display_name<'a>(&self, key: &'a str) -> &'a str {
        let prefix = format!("{}.", self.current_db);
        key.strip_prefix(prefix.as_str()).unwrap_or(key)
    }

    pub fn new() -> Self {
        let disk = DiskManager::new();
        let mut catalog = Catalog::new();
        let mut tables = HashMap::new();
        let mut indexes = HashMap::new();

        // Collect databases from disk directories (no hardcoded default)
        let mut databases: HashSet<String> = HashSet::new();
        for db in disk.list_databases() {
            databases.insert(db.to_lowercase());
        }

        // Load all tables from all databases (qualified keys: "db.table")
        for qualified_key in disk.list_tables() {
            if let Some(mut schema) = disk.load_schema(&qualified_key) {
                let (db, _tbl) = Self::split_key(&qualified_key);
                databases.insert(db.to_lowercase());

                // Qualify FK ref_table fields (migration from unqualified old data)
                for col in schema.columns.iter_mut() {
                    if let Some(ref mut fk) = col.foreign_key {
                        if !fk.ref_table.contains('.') {
                            fk.ref_table = format!("{}.{}", db, fk.ref_table);
                        }
                    }
                }

                let first_col = schema.columns.first().map(|c| c.name.clone());
                let auto_inc_counters = schema.auto_increment_counters.clone();

                let _ = catalog.create_table_full(
                    qualified_key.clone(),
                    schema.columns.clone(),
                    schema.primary_key_columns.clone(),
                    schema.check_constraints.clone(),
                );
                if let Some(ts) = catalog.get_table_mut(&qualified_key) {
                    ts.auto_increment_counters = auto_inc_counters;
                }

                let rows = disk.load_table(&qualified_key);
                let mut tree = BPlusTree::new();
                for row in &rows {
                    if let Some(ref col) = first_col {
                        if let Some(key) = row.get(col) {
                            let val_json = serde_json::to_string(row).unwrap();
                            tree.insert(key.clone(), val_json);
                        }
                    }
                }
                indexes.insert(qualified_key.clone(), tree);
                tables.insert(qualified_key, rows);
            }
        }

        // 모든 DB의 뷰 로드 (qualified view names: "db.view")
        let mut views: HashMap<String, Statement> = HashMap::new();
        for db in &databases {
            let db_views = disk.load_views(db);
            for (k, v) in db_views {
                let qualified_k = if k.contains('.') { k } else { format!("{}.{}", db, k) };
                views.insert(qualified_k, v);
            }
        }

        // 모든 DB의 인덱스 메타 로드
        let mut index_meta: HashMap<String, (String, String)> = HashMap::new();
        let mut composite_indexes: HashMap<String, CompositeIndex> = HashMap::new();
        for db in &databases {
            let meta_list = disk.load_index_meta(db);
            for meta in &meta_list {
                // Qualify table name in index meta
                let q_table = if meta.table.contains('.') {
                    meta.table.clone()
                } else {
                    format!("{}.{}", db, meta.table)
                };
                if meta.columns.len() == 1 {
                    let column = &meta.columns[0];
                    let mut tree = BPlusTree::new();
                    if let Some(rows) = tables.get(&q_table) {
                        for row in rows {
                            if let Some(val) = row.get(column) {
                                let json = serde_json::to_string(row).unwrap();
                                tree.insert(val.clone(), json);
                            }
                        }
                    }
                    let key = format!("{}_{}", q_table, meta.name);
                    indexes.insert(key, tree);
                    index_meta.insert(meta.name.clone(), (q_table, column.clone()));
                } else {
                    let mut comp = CompositeIndex::new(q_table.clone(), meta.columns.clone());
                    if let Some(rows) = tables.get(&q_table) {
                        comp.rebuild(rows);
                    }
                    composite_indexes.insert(meta.name.clone(), comp);
                }
            }
        }

        let users: Vec<UserRecord> = disk.load_users();
        let grants: Vec<GrantRecord> = disk.load_grants();

        let current_db = databases.iter().min().cloned().unwrap_or_else(|| "rustdb".to_string());
        let mut executor = Executor {
            shared: Arc::new(RwLock::new(SharedDatabase {
                catalog,
                tables,
                indexes,
                index_meta,
                composite_indexes,
                views,
                buffer_pool: BufferPool::new(),
                disk,
                lock_mgr: LockManager::new(),
                databases,
                users,
                grants,
            })),
            txn: TransactionManager::new(),
            current_db,
        };

        // WAL Crash Recovery
        executor.recover_from_wal();
        executor
    }

    pub fn new_session(shared: Arc<RwLock<SharedDatabase>>) -> Self {
        let current_db = {
            let s = shared.read().unwrap();
            s.databases.iter().min().cloned().unwrap_or_else(|| "rustdb".to_string())
        };
        Executor {
            shared,
            txn: TransactionManager::new(),
            current_db,
        }
    }

    pub fn get_shared(&self) -> Arc<RwLock<SharedDatabase>> {
        Arc::clone(&self.shared)
    }

    pub fn execute(&mut self, stmt: Statement) -> Result<String, String> {
        let arc = Arc::clone(&self.shared);
        let mut s = arc.write().unwrap();
        self.execute_with_s(&mut s, stmt)
    }

    fn execute_with_s(&mut self, s: &mut SharedDatabase, stmt: Statement) -> Result<String, String> {
        // USE은 qualification 전에 처리
        if let Statement::Use { database } = stmt {
            return self.exec_use(s, database);
        }
        // CreateDatabase/DropDatabase도 qualification 불필요
        if let Statement::CreateDatabase { name, if_not_exists } = stmt {
            return self.exec_create_database(s, name, if_not_exists);
        }
        if let Statement::DropDatabase { name, if_exists } = stmt {
            return self.exec_drop_database(s, name, if_exists);
        }
        // 사용자 관리 / 권한 — qualification 불필요
        if let Statement::CreateUser { user, host, password, if_not_exists } = stmt {
            return self.exec_create_user(s, user, host, password, if_not_exists);
        }
        if let Statement::DropUser { user, host, if_exists } = stmt {
            return self.exec_drop_user(s, user, host, if_exists);
        }
        if let Statement::Grant { privileges, object_type, object, user, host, with_grant_option } = stmt {
            return self.exec_grant(s, privileges, object_type, object, user, host, with_grant_option);
        }
        if let Statement::Revoke { privileges, object_type, object, user, host } = stmt {
            return self.exec_revoke(s, privileges, object_type, object, user, host);
        }
        if let Statement::ShowGrants { user, host } = stmt {
            return self.exec_show_grants(s, user, host);
        }
        if let Statement::ShowDatabases = stmt {
            return self.exec_show_databases(s);
        }
        // 모든 다른 statement: 테이블명을 "{current_db}.{table}" 형식으로 qualify
        let stmt = self.qualify_stmt(s, stmt);
        match stmt {
            Statement::Begin    => self.exec_begin(s),
            Statement::Commit   => self.exec_commit(s),
            Statement::Rollback => self.exec_rollback(s),
            Statement::CreateTable { name, columns, if_not_exists, primary_key_columns, check_constraints } => {
                self.exec_create(s, name, columns, if_not_exists, primary_key_columns, check_constraints)
            }
            Statement::DropTable { name, if_exists }  => self.exec_drop(s, name, if_exists),
            Statement::TruncateTable { name }        => self.exec_truncate(s, name),
            Statement::Insert { table, columns, values, on_conflict } => self.exec_insert(s, table, columns, values, on_conflict),
            Statement::InsertSelect { table, columns, query, on_conflict } => self.exec_insert_select(s, table, columns, *query, on_conflict),
            Statement::Select { table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update } => {
                self.exec_select(s, table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update)
            }
            Statement::Update { table, assignments, condition } => {
                self.exec_update(s, table, assignments, condition)
            }
            Statement::Delete { table, condition }   => self.exec_delete(s, table, condition),
            Statement::AlterTable { table, action }  => self.exec_alter(s, table, action),
            Statement::CreateIndex { index_name, table, columns } => {
                self.exec_create_index(s, index_name, table, columns)
            }
            Statement::DropIndex { index_name } => self.exec_drop_index(s, index_name),
            Statement::CreateView { name, query } => self.exec_create_view(s, name, *query),
            Statement::DropView { name } => self.exec_drop_view(s, name),
            Statement::ShowTables => self.exec_show_tables(s),
            Statement::Describe { table } => self.exec_describe(s, table),
            Statement::ShowBufferPool => self.exec_show_buffer_pool(s),
            Statement::ShowWal        => self.exec_show_wal(),
            Statement::Checkpoint     => self.exec_checkpoint(s),
            Statement::SetIsolationLevel(level) => self.exec_set_isolation_level(level),
            Statement::ShowIsolationLevel       => self.exec_show_isolation_level(),
            Statement::Vacuum { table }         => self.exec_vacuum(s, table),
            Statement::ShowLocks                => self.exec_show_locks(s),
            Statement::Savepoint { name }       => self.exec_savepoint(name),
            Statement::ReleaseSavepoint { name } => self.exec_release_savepoint(name),
            Statement::RollbackTo { name }      => self.exec_rollback_to(s, name),
            Statement::Explain(inner)           => self.exec_explain(s, *inner),
            Statement::Union { left, right, all, order_by, limit, offset } => self.exec_union(s, *left, *right, all, order_by, limit, offset),
            Statement::With { ctes, query, recursive } => self.exec_with(s, ctes, *query, recursive),
            Statement::CreateDatabase { name, if_not_exists } => self.exec_create_database(s, name, if_not_exists),
            Statement::DropDatabase { name, if_exists }       => self.exec_drop_database(s, name, if_exists),
            Statement::MultiUpdate { tables, joins, assignments, condition } => {
                self.exec_multi_update(s, tables, joins, assignments, condition)
            }
            Statement::MultiDelete { delete_tables, from_table, joins, condition } => {
                self.exec_multi_delete(s, delete_tables, from_table, joins, condition)
            }
            Statement::Use { database } => self.exec_use(s, database),
            // These are handled in early-return blocks above; unreachable after qualify_stmt
            Statement::CreateUser { .. } | Statement::DropUser { .. }
            | Statement::Grant { .. } | Statement::Revoke { .. }
            | Statement::ShowGrants { .. } | Statement::ShowDatabases => {
                Err("Internal error: user-management statement reached qualify pass".to_string())
            }
        }
    }

    fn exec_union(
        &mut self,
        s: &mut SharedDatabase,
        left: Statement,
        right: Statement,
        all: bool,
        order_by: Vec<OrderBy>,
        limit: Option<usize>,
        offset: Option<usize>,
    ) -> Result<String, String> {
        let left_out  = self.execute_with_s(s, left)?;
        let right_out = self.execute_with_s(s, right)?;

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
        // 1. Exact match
        if let Some(v) = row.get(col) { return Some(v); }

        if let Some(dot) = col.rfind('.') {
            let table_part = &col[..dot];   // e.g. "dept" or "rustdb.dept"
            let col_part   = &col[dot + 1..]; // e.g. "name"

            // 2. Look for any key ending with ".{table}.{col}" — handles qualified keys
            //    e.g. "rustdb.dept.name" when caller asks for "dept.name"
            let suffix = format!(".{}.{}", table_part, col_part);
            if let Some((_, v)) = row.iter().find(|(k, _)| k.ends_with(suffix.as_str())) {
                return Some(v);
            }

            // 3. Bare column name — only for unambiguous single-table lookups
            row.get(col_part)
        } else {
            None
        }
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
            ArithExpr::Func(name, args) => {
                let str_args: Vec<String> = args.iter().map(|a| match a {
                    ArithExpr::Col(c) => c.clone(),
                    ArithExpr::Str(s) => format!("'{}'", s),
                    ArithExpr::Num(n) => n.clone(),
                    other => {
                        let v = Self::eval_arith(row, other);
                        format!("'{}'", v)
                    }
                }).collect();
                Self::apply_scalar_func(name, &str_args, row)
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
        s: &mut SharedDatabase,
        name: String,
        columns: Vec<ColumnDef>,
        if_not_exists: bool,
        primary_key_columns: Vec<String>,
        check_constraints: Vec<(Option<String>, String)>,
    ) -> Result<String, String> {
        // IF NOT EXISTS: 이미 존재하면 조용히 넘어감
        if if_not_exists && s.tables.contains_key(&name) {
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
                    crate::parser::ast::FkAction::Restrict   => crate::catalog::schema::FkAction::Restrict,
                    crate::parser::ast::FkAction::Cascade    => crate::catalog::schema::FkAction::Cascade,
                    crate::parser::ast::FkAction::SetNull    => crate::catalog::schema::FkAction::SetNull,
                    crate::parser::ast::FkAction::SetDefault => crate::catalog::schema::FkAction::SetDefault,
                },
                on_update: match fk.on_update {
                    crate::parser::ast::FkAction::Restrict   => crate::catalog::schema::FkAction::Restrict,
                    crate::parser::ast::FkAction::Cascade    => crate::catalog::schema::FkAction::Cascade,
                    crate::parser::ast::FkAction::SetNull    => crate::catalog::schema::FkAction::SetNull,
                    crate::parser::ast::FkAction::SetDefault => crate::catalog::schema::FkAction::SetDefault,
                },
            }),
            check_expr: c.check_expr,
        }).collect();
        let schema_checks: Vec<crate::catalog::schema::CheckConstraint> = check_constraints.into_iter()
            .map(|(name, expr)| crate::catalog::schema::CheckConstraint { name, expression: expr })
            .collect();
        s.catalog.create_table_full(name.clone(), schema_cols, primary_key_columns, schema_checks)?;
        s.tables.insert(name.clone(), Vec::new());
        s.indexes.insert(name.clone(), BPlusTree::new());
        let full_schema = s.catalog.get_table(&name).unwrap();
        s.disk.save_schema(&name, full_schema);
        Ok(format!("Table '{}' created.", name))
    }

    fn exec_drop(&mut self, s: &mut SharedDatabase, name: String, if_exists: bool) -> Result<String, String> {
        if if_exists && !s.tables.contains_key(&name) {
            return Ok(format!("Table '{}' does not exist, skipped.", name));
        }
        s.catalog.drop_table(&name)?;
        s.tables.remove(&name);
        s.indexes.remove(&name);
        s.buffer_pool.invalidate(&name);
        s.disk.delete_table(&name);
        Ok(format!("Table '{}' dropped.", name))
    }

    fn exec_truncate(&mut self, s: &mut SharedDatabase, name: String) -> Result<String, String> {
        s.tables.get_mut(&name)
            .ok_or(format!("Table '{}' not found", name))?
            .clear();
        if let Some(index) = s.indexes.get_mut(&name) {
            *index = BPlusTree::new();
        }
        // AUTO INCREMENT 카운터 리셋
        if let Some(schema) = s.catalog.get_table_mut(&name) {
            schema.auto_increment_counters.clear();
        }
        s.buffer_pool.invalidate(&name);
        s.disk.save_table(&name, &[]);
        Ok(format!("Table '{}' truncated.", name))
    }

    fn exec_with(
        &mut self,
        s: &mut SharedDatabase,
        ctes: Vec<(String, Box<Statement>)>,
        query: Statement,
        recursive: bool,
    ) -> Result<String, String> {
        // Materialise each CTE as a temporary in-memory table, then run the main query.
        let mut cte_names: Vec<String> = Vec::new();

        for (name, body) in ctes {
            // Conflict guard
            if s.tables.contains_key(&name) || s.views.contains_key(&name) {
                return Err(format!("CTE name '{}' conflicts with an existing table or view", name));
            }

            // 재귀 CTE: RECURSIVE 키워드 + Union 구조일 때 base + 반복 실행
            let (col_names, rows) = if recursive && matches!(*body, Statement::Union { .. }) {
                let Statement::Union { left, right, .. } = *body else { unreachable!() };

                // 1단계: base case 실행
                let base_out = self.execute_with_s(s, *left)?;
                let (cols, mut accumulated) = Self::parse_table_output(&base_out);

                // CTE 테이블 초기화 (재귀 쿼리가 자신을 참조할 수 있도록)
                let schema_cols: Vec<crate::catalog::schema::ColumnDef> = cols.iter().map(|c| {
                    crate::catalog::schema::ColumnDef {
                        name: c.clone(),
                        data_type: crate::parser::ast::DataType::Text,
                        primary_key: false, not_null: false, unique: false,
                        unique_constraint_name: None, auto_increment: false,
                        default: None, foreign_key: None, check_expr: None,
                    }
                }).collect();
                let _ = s.catalog.create_table(name.clone(), schema_cols);
                s.tables.insert(name.clone(), accumulated.clone());
                s.buffer_pool.write_page(&name, accumulated.clone());
                s.indexes.insert(name.clone(), crate::storage::btree::BPlusTree::new());

                // 2단계: 재귀 반복 (새 행이 없을 때까지, 최대 1000회)
                for _ in 0..1000 {
                    let rec_out = self.execute_with_s(s, *right.clone())?;
                    let (rec_cols, new_rows) = Self::parse_table_output(&rec_out);
                    // CTE 컬럼명은 base case 기준 (positional 매핑)
                    let fresh: Vec<Row> = new_rows.into_iter()
                        .map(|rec_row| {
                            let mut mapped = Row::new();
                            for (i, base_col) in cols.iter().enumerate() {
                                let val = rec_cols.get(i)
                                    .and_then(|rc| rec_row.get(rc))
                                    .cloned()
                                    .unwrap_or_default();
                                mapped.insert(base_col.clone(), val);
                            }
                            mapped.insert("_xmin".to_string(), "1".to_string());
                            mapped.insert("_xmax".to_string(), "0".to_string());
                            mapped
                        })
                        .filter(|r| !accumulated.contains(r))
                        .collect();
                    if fresh.is_empty() { break; }
                    accumulated.extend(fresh);
                    s.tables.insert(name.clone(), accumulated.clone());
                    s.buffer_pool.write_page(&name, accumulated.clone());
                }

                cte_names.push(name.clone());
                let result = self.execute_with_s(s, query);
                for n in &cte_names {
                    s.tables.remove(n);
                    s.indexes.remove(n);
                    s.buffer_pool.invalidate(n);
                    let _ = s.catalog.drop_table(n);
                }
                return result;
            } else {
                // 일반 CTE (비재귀) — CTE body 실행 후 가상 테이블로 적재
                let output = self.execute_with_s(s, *body)?;
                Self::parse_table_output(&output)
            };

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

            let _ = s.catalog.create_table(name.clone(), schema_cols);
            s.tables.insert(name.clone(), rows.clone());
            s.buffer_pool.write_page(&name, rows);
            s.indexes.insert(name.clone(), crate::storage::btree::BPlusTree::new());
            cte_names.push(name);
        }

        let result = self.execute_with_s(s, query);

        // Tear down temporary CTE tables
        for name in &cte_names {
            s.tables.remove(name);
            s.indexes.remove(name);
            s.buffer_pool.invalidate(name);
            let _ = s.catalog.drop_table(name);
        }

        result
    }

    fn exec_insert_select(
        &mut self,
        s: &mut SharedDatabase,
        table: String,
        columns: Option<Vec<String>>,
        query: Statement,
        on_conflict: InsertConflict,
    ) -> Result<String, String> {
        let output = self.execute_with_s(s, query)?;
        let (col_names, rows) = Self::parse_table_output(&output);
        if rows.is_empty() {
            return Ok("0 row(s) inserted.".to_string());
        }
        let all_values: Vec<Vec<String>> = rows.iter()
            .map(|row| col_names.iter().map(|c| row.get(c).cloned().unwrap_or_default()).collect())
            .collect();
        let insert_cols = columns.or(Some(col_names));
        self.exec_insert(s, table, insert_cols, all_values, on_conflict)
    }

    fn exec_insert(
        &mut self,
        s: &mut SharedDatabase,
        table: String,
        col_list: Option<Vec<String>>,
        all_values: Vec<Vec<String>>,
        on_conflict: InsertConflict,
    ) -> Result<String, String> {
        // 스키마 클론 (borrow 충돌 방지)
        let schema = s.catalog.get_table(&table)
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
        // ON DUPLICATE KEY UPDATE: (conflicting_pk_val, assignments)
        let mut pending_updates: Vec<(String, Vec<(String, ArithExpr)>)> = Vec::new();

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

            // ENUM / SET 값 유효성 검사
            for (i, col) in schema.columns.iter().enumerate() {
                let val = &final_values[i];
                if val.is_empty() || val == NULL_VALUE { continue; }
                match &col.data_type {
                    DataType::Enum(allowed) => {
                        if !allowed.iter().any(|a| a == val) {
                            return Err(format!(
                                "Invalid ENUM value '{}' for column '{}'. Allowed: {}",
                                val, col.name,
                                allowed.iter().map(|s| format!("'{}'", s)).collect::<Vec<_>>().join(", ")
                            ));
                        }
                    }
                    DataType::Set(allowed) => {
                        for part in val.split(',') {
                            let part = part.trim();
                            if !part.is_empty() && !allowed.iter().any(|a| a == part) {
                                return Err(format!(
                                    "Invalid SET value '{}' for column '{}'. Allowed: {}",
                                    part, col.name,
                                    allowed.iter().map(|s| format!("'{}'", s)).collect::<Vec<_>>().join(", ")
                                ));
                            }
                        }
                    }
                    _ => {}
                }
            }

            // UNIQUE / PRIMARY KEY 중복 검사 — 기존 행 대상
            {
                // 복합 PK 컬럼 목록
                let pk_cols: Vec<&str> = schema.primary_key_columns.iter().map(|s| s.as_str()).collect();
                let is_composite_pk = pk_cols.len() > 1;

                if let Some(rows) = s.tables.get(&table) {
                    if is_composite_pk {
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
                                match &on_conflict {
                                    InsertConflict::Abort => return Err(format!(
                                        "Duplicate composite primary key ({:?})", new_pk_tuple
                                    )),
                                    InsertConflict::Ignore => { continue; }
                                    InsertConflict::Update(assignments) => {
                                        let pk_val = existing.get(&col_names[0]).cloned().unwrap_or_default();
                                        pending_updates.push((pk_val, assignments.clone()));
                                        continue;
                                    }
                                }
                            }
                        }
                    } else {
                        let mut dup_found = false;
                        'outer: for (i, (pk, _, unique, _)) in constraints.iter().enumerate() {
                            if *pk || *unique {
                                let val = &final_values[i];
                                for existing in rows.iter().filter(|r| Self::is_visible(r)) {
                                    if existing.get(&col_names[i]) == Some(val) {
                                        match &on_conflict {
                                            InsertConflict::Abort => return Err(format!(
                                                "Duplicate value '{}' for column '{}'", val, col_names[i]
                                            )),
                                            InsertConflict::Ignore => {
                                                dup_found = true;
                                                break 'outer;
                                            }
                                            InsertConflict::Update(assignments) => {
                                                let pk_val = existing.get(&col_names[0]).cloned().unwrap_or_default();
                                                pending_updates.push((pk_val, assignments.clone()));
                                                dup_found = true;
                                                break 'outer;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if dup_found { continue; }
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
                    let ref_rows = s.tables.get(&fk.ref_table)
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

        // ── ON DUPLICATE KEY UPDATE: 충돌 행 업데이트 ──────────────────────
        let had_updates = !pending_updates.is_empty();
        for (pk_val, assignments) in pending_updates {
            if let Some(rows) = s.tables.get_mut(&table) {
                for row in rows.iter_mut() {
                    if row.get(&col_names[0]) == Some(&pk_val) && Self::is_visible(row) {
                        for (col, expr) in &assignments {
                            let val = Self::eval_arith(row, expr);
                            row.insert(col.clone(), val);
                        }
                        break;
                    }
                }
            }
        }
        // 인덱스가 s.tables와 동기화되도록 재빌드 (PK 포인트 룩업이 인덱스를 사용하므로)
        if had_updates {
            let pk_col_name = schema.columns.iter()
                .find(|c| c.primary_key)
                .map(|c| c.name.clone())
                .unwrap_or_else(|| col_names[0].clone());
            let rows_snap = s.tables.get(&table).cloned().unwrap_or_default();
            if let Some(index) = s.indexes.get_mut(&table) {
                *index = BPlusTree::new();
                for row in &rows_snap {
                    let k = row.get(&pk_col_name).cloned().unwrap_or_default();
                    let v = serde_json::to_string(row).unwrap();
                    index.insert(k, v);
                }
            }
        }

        // ── 2단계: 검증 통과 — 모든 행 삽입 ─────────────────────────────

        // auto_increment 카운터를 schema에 반영 후 저장
        if local_counters != schema.auto_increment_counters {
            let schema_mut = s.catalog.get_table_mut(&table).unwrap();
            schema_mut.auto_increment_counters = local_counters;
            let schema_saved = s.catalog.get_table(&table).unwrap();
            s.disk.save_schema(&table, schema_saved);
        }

        let inserted = prepared.len();

        for row in prepared {
            let pk_val = row.get(&col_names[0]).cloned().unwrap_or_default();
            let val_json = serde_json::to_string(&row).unwrap();

            self.txn.log_insert(&table, &pk_val, &val_json);

            if let Some(index) = s.indexes.get_mut(&table) {
                index.insert(pk_val, val_json);
            }

            // 복합 인덱스 갱신
            let comp_keys: Vec<String> = s.composite_indexes.iter()
                .filter(|(_, ci)| ci.table == table)
                .map(|(k, _)| k.clone())
                .collect();
            for k in comp_keys {
                if let Some(ci) = s.composite_indexes.get_mut(&k) {
                    ci.insert_row(&row);
                }
            }

            s.tables.get_mut(&table)
                .ok_or(format!("Table '{}' not found", table))?
                .push(row);
        }

        self.sort_by_pk(s, &table);

        let rows = s.tables.get(&table).unwrap().clone();
        // 단일 컬럼 보조 인덱스 재빌드 (INSERT 후 stale 방지)
        self.rebuild_secondary_indexes(s, &table, &rows);
        s.buffer_pool.write_page(&table, rows);

        // 모든 row 삽입 후 flush
        if !self.txn.is_active() {
            s.buffer_pool.flush_page(&table, &s.disk);
        }

        self.maybe_auto_checkpoint(s);
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
        s: &mut SharedDatabase,
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
                s, *inner_stmt, alias, distinct, columns, condition, joins,
                order_by, group_by, having, limit, offset, for_update,
            );
        }

        // FROM 없는 스칼라 SELECT: 빈 행 하나로 표현식만 계산
        if table == "_dual_" || table.ends_with("._dual_") {
            let _empty_row = Row::new();
            // 컬럼 헤더 및 값 계산
            let col_defs: Vec<(String, SelectColumn)> = columns.iter().map(|col| {
                let header = match col {
                    SelectColumn::ColumnAlias(_, alias) => alias.clone(),
                    SelectColumn::Func { name, args: _, alias } => alias.clone().unwrap_or_else(|| name.clone()),
                    SelectColumn::Expr { expr, alias } => alias.clone().unwrap_or_else(|| arith_to_str(expr)),
                    SelectColumn::Agg { func, col } => format!("{:?}({})", func, col),
                    SelectColumn::AggAlias { alias, .. } => alias.clone(),
                    SelectColumn::Column(c) => c.clone(),
                    SelectColumn::All => "*".to_string(),
                    SelectColumn::CaseWhen { alias, .. } => alias.clone().unwrap_or_else(|| "case".to_string()),
                };
                (header, col.clone())
            }).collect();
            let widths: Vec<usize> = col_defs.iter().map(|(h, col)| {
                let val = match col {
                    SelectColumn::Func { name, args, .. } => Self::apply_scalar_func(name, args, &Row::new()),
                    SelectColumn::Expr { expr, .. } => Self::eval_arith(&Row::new(), expr),
                    SelectColumn::Column(c) => c.clone(),
                    SelectColumn::ColumnAlias(c, _) => c.clone(),
                    _ => String::new(),
                };
                h.len().max(val.len())
            }).collect();
            let sep: String = widths.iter().map(|w| "+".to_string() + &"-".repeat(w + 2)).collect::<String>() + "+";
            let hdr: String = col_defs.iter().zip(widths.iter()).map(|((h, _), w)| format!("| {:width$} ", h, width = w)).collect::<String>() + "|";
            let row_str: String = col_defs.iter().zip(widths.iter()).map(|((_, col), w)| {
                let val = match col {
                    SelectColumn::Func { name, args, .. } => Self::apply_scalar_func(name, args, &Row::new()),
                    SelectColumn::Expr { expr, .. } => Self::eval_arith(&Row::new(), expr),
                    SelectColumn::Column(c) => c.clone(),
                    SelectColumn::ColumnAlias(c, _) => c.clone(),
                    _ => String::new(),
                };
                format!("| {:width$} ", val, width = w)
            }).collect::<String>() + "|";
            return Ok(format!("{}\n{}\n{}\n{}\n{}\n1 row(s) returned.", sep, hdr, sep, row_str, sep));
        }

        // 뷰 처리: 뷰를 FROM 서브쿼리처럼 실행하고 외부 쿼리 조건을 적용
        if let Some(view_stmt) = s.views.remove(&table) {
            let result = self.exec_select_with_subquery(
                s, view_stmt.clone(),
                table.clone(),
                distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update,
            );
            s.views.insert(table, view_stmt);
            return result;
        }

        // ── Planner: 인덱스 / 조인 알고리즘 결정 ──────────────────────────
        let has_agg = columns.iter().any(|c| matches!(c, SelectColumn::Agg { .. } | SelectColumn::AggAlias { .. }));
        let planner = Planner::new(&s.tables, &s.indexes, &s.index_meta, &s.composite_indexes, &s.catalog);
        let plan = planner.plan_covering(&table, &condition, &joins, &columns);

        // 인덱스 경로 실행 (집계 / FOR UPDATE / JOIN / LIMIT / ORDER BY 없을 때만)
        if joins.is_empty() && !has_agg && !for_update
            && limit.is_none() && offset.is_none() && order_by.is_empty() && !distinct {
            match &plan.base.access {
                // ── PK 포인트 ──────────────────────────────────────────────
                AccessPath::PkPoint { key } => {
                    if let Some(index) = s.indexes.get(&table) {
                        if let Some(val_json) = index.search(key) {
                            let row: Row = serde_json::from_str(&val_json).unwrap_or_default();
                            if Self::is_visible(&row) {
                                return self.format_result(s, vec![row], columns, table, vec![]);
                            }
                        }
                        return Ok("0 rows returned.".to_string());
                    }
                }
                // ── PK BETWEEN ────────────────────────────────────────────
                AccessPath::PkBetween { start, end } => {
                    if let Some(index) = s.indexes.get(&table) {
                        let rows: Vec<Row> = index.range_search(start, end).iter()
                            .filter_map(|j| serde_json::from_str(j).ok())
                            .filter(|r| Self::is_visible(r)).collect();
                        return self.format_result(s, rows, columns, table, vec![]);
                    }
                }
                // ── PK 범위 스캔 ──────────────────────────────────────────
                AccessPath::PkRange { op, key } => {
                    if let Some(index) = s.indexes.get(&table) {
                        let inclusive = op.inclusive();
                        let rows: Vec<Row> = if op.is_lower_bound() {
                            index.scan_from(key, inclusive).iter()
                                .filter_map(|(_, j)| serde_json::from_str(j).ok())
                                .filter(|r| Self::is_visible(r)).collect()
                        } else {
                            index.scan_to(key, inclusive).iter()
                                .filter_map(|(_, j)| serde_json::from_str(j).ok())
                                .filter(|r| Self::is_visible(r)).collect()
                        };
                        return self.format_result(s, rows, columns, table, vec![]);
                    }
                }
                // ── 보조 인덱스 포인트 (중복 키 배열) ────────────────────
                AccessPath::SecondaryPoint { index_key, col, key, .. } => {
                    if let Some(index) = s.indexes.get(index_key) {
                        if let Some(json) = index.search(key) {
                            if plan.base.is_covering {
                                // 커버링 인덱스: 전체 Row 역직렬화 없이 JSON 배열 길이만 집계
                                let arr: Vec<serde_json::Value> = serde_json::from_str(&json).unwrap_or_default();
                                let count = arr.iter()
                                    .filter(|v| v.get("_xmax").and_then(|x| x.as_str()).map(|x| x == "0").unwrap_or(true))
                                    .count();
                                let synthetic: Vec<Row> = (0..count).map(|_| {
                                    let mut r = Row::new();
                                    r.insert(col.clone(), key.clone());
                                    r
                                }).collect();
                                return self.format_result(s, synthetic, columns, table, vec![]);
                            }
                            let rows: Vec<Row> = serde_json::from_str::<Vec<Row>>(&json)
                                .unwrap_or_default()
                                .into_iter()
                                .filter(|r| Self::is_visible(r))
                                .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
                                .collect();
                            return self.format_result(s, rows, columns, table, vec![]);
                        }
                        return Ok("0 rows returned.".to_string());
                    }
                }
                // ── 보조 인덱스 범위 스캔 ────────────────────────────────
                AccessPath::SecondaryRange { index_key, col, op, key, .. } => {
                    if let Some(index) = s.indexes.get(index_key) {
                        let inclusive = op.inclusive();
                        let pairs = if op.is_lower_bound() {
                            index.scan_from(key, inclusive)
                        } else {
                            index.scan_to(key, inclusive)
                        };
                        if plan.base.is_covering {
                            // 커버링 인덱스: 각 키의 JSON 배열 길이만 집계
                            let col_name = col.clone();
                            let synthetic: Vec<Row> = pairs.iter()
                                .flat_map(|(k, json)| {
                                    let arr: Vec<serde_json::Value> = serde_json::from_str(json).unwrap_or_default();
                                    let count = arr.iter()
                                        .filter(|v| v.get("_xmax").and_then(|x| x.as_str()).map(|x| x == "0").unwrap_or(true))
                                        .count();
                                    let col_name = col_name.clone();
                                    let k = k.clone();
                                    (0..count).map(move |_| {
                                        let mut r = Row::new();
                                        r.insert(col_name.clone(), k.clone());
                                        r
                                    }).collect::<Vec<_>>()
                                })
                                .collect();
                            return self.format_result(s, synthetic, columns, table, vec![]);
                        }
                        let rows: Vec<Row> = pairs.iter()
                            .filter_map(|(_, j)| serde_json::from_str::<Vec<Row>>(j).ok())
                            .flatten()
                            .filter(|r| Self::is_visible(r))
                            .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
                            .collect();
                        return self.format_result(s, rows, columns, table, vec![]);
                    }
                }
                // ── 복합 인덱스 ──────────────────────────────────────────
                AccessPath::CompositeIndex { index_name } => {
                    let eq_map = collect_eq_conditions_expr(&condition.clone().unwrap());
                    if let Some(val_json) = s.composite_indexes[index_name].search_from_eq_map(&eq_map) {
                        if let Ok(row) = serde_json::from_str::<Row>(&val_json) {
                            return self.format_result(s, vec![row], columns, table, vec![]);
                        }
                    }
                    return Ok("0 rows returned.".to_string());
                }
                AccessPath::SeqScan => {} // fall through
            }
        }

        if !s.tables.contains_key(&table) {
            return Err(format!("Table '{}' not found", table));
        }

        // REPEATABLE READ / SERIALIZABLE: 스냅샷에서 읽기
        let rows: Vec<Row> = if let Some(snap_rows) = self.txn.get_snapshot_table(&table) {
            snap_rows.clone()
        } else {
            s.buffer_pool.get_page(&table, &s.disk)
        };
        // MVCC: 논리 삭제된 행(_xmax != "0") 제외
        let rows: Vec<Row> = rows.into_iter().filter(|r| Self::is_visible(r)).collect();

        // ── JOIN 처리 (플래너가 선택한 알고리즘 사용) ──────────────────────
        let result: Vec<Row> = if joins.is_empty() {
            rows.into_iter()
                .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
                .collect()
        } else {
            let mut current = rows;
            for (ji, j) in joins.iter().enumerate() {
                let right_rows_raw = if let Some(snap) = self.txn.get_snapshot_table(&j.table) {
                    snap.clone()
                } else {
                    s.tables.get(&j.table)
                        .ok_or(format!("Table '{}' not found", j.table))?.clone()
                };
                let right_rows: Vec<Row> = right_rows_raw.into_iter().filter(|r| Self::is_visible(r)).collect();

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
                let right_schema_cols: Vec<String> = s.catalog.get_table(&j.table)
                    .map(|s| s.columns.iter().map(|c| c.name.clone()).collect())
                    .unwrap_or_default();

                // 플래너가 선택한 알고리즘 가져오기
                let algo = plan.joins.get(ji).map(|jp| &jp.algo);

                let joined = match algo {
                    Some(JoinAlgo::SortMerge { probe_col, build_col }) => {
                        // ── Sort-Merge Join ───────────────────────────────
                        // 양쪽 모두 조인 키 기준으로 정렬 후 투 포인터 병합.
                        // 시간 복잡도: O((N+M)log(N+M)) sort + O(N+M) merge.
                        let pc = probe_col.clone();
                        let bc = build_col.clone();
                        let tbl = j.table.clone();

                        let sort_cmp = |a: &str, b: &str| -> std::cmp::Ordering {
                            match (a.parse::<f64>(), b.parse::<f64>()) {
                                (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(std::cmp::Ordering::Equal),
                                _ => a.cmp(b),
                            }
                        };
                        let key_left = |row: &Row| -> String {
                            row.get(&pc)
                                .or_else(|| row.iter().find(|(k, _)| k.ends_with(&format!(".{}", pc))).map(|(_, v)| v))
                                .cloned()
                                .unwrap_or_default()
                        };
                        let key_right = |row: &Row| -> String {
                            row.get(&bc)
                                .or_else(|| row.get(&format!("{}.{}", tbl, bc)))
                                .cloned()
                                .unwrap_or_default()
                        };

                        let mut ls: Vec<Row> = current.clone();
                        ls.sort_by(|a, b| sort_cmp(&key_left(a), &key_left(b)));
                        let mut rs: Vec<Row> = right_rows.clone();
                        rs.sort_by(|a, b| sort_cmp(&key_right(a), &key_right(b)));

                        let mut out = Vec::new();
                        match j.join_type {
                            JoinType::Inner => {
                                let mut li = 0usize;
                                let mut ri = 0usize;
                                while li < ls.len() && ri < rs.len() {
                                    let lk = key_left(&ls[li]);
                                    let rk = key_right(&rs[ri]);
                                    match sort_cmp(&lk, &rk) {
                                        std::cmp::Ordering::Less    => { li += 1; }
                                        std::cmp::Ordering::Greater => { ri += 1; }
                                        std::cmp::Ordering::Equal   => {
                                            // 동일 키 그룹 수집
                                            let li0 = li;
                                            while li < ls.len() && key_left(&ls[li]) == lk { li += 1; }
                                            let ri0 = ri;
                                            while ri < rs.len() && key_right(&rs[ri]) == lk { ri += 1; }
                                            // 교차 곱
                                            for l in &ls[li0..li] {
                                                for r in &rs[ri0..ri] {
                                                    let mut merged = l.clone();
                                                    merge_right(&mut merged, r, &tbl);
                                                    out.push(merged);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            JoinType::Left => {
                                let mut ri = 0usize;
                                let mut li = 0usize;
                                while li < ls.len() {
                                    let lk = key_left(&ls[li]);
                                    // ri를 lk 이상의 첫 위치로 전진
                                    while ri < rs.len() && sort_cmp(&key_right(&rs[ri]), &lk) == std::cmp::Ordering::Less {
                                        ri += 1;
                                    }
                                    // 왼쪽 키 그룹 [li0, li)
                                    let li0 = li;
                                    while li < ls.len() && key_left(&ls[li]) == lk { li += 1; }
                                    // 오른쪽 매칭 그룹 [ri0, ri_end)
                                    let ri0 = ri;
                                    let mut ri_end = ri;
                                    while ri_end < rs.len() && key_right(&rs[ri_end]) == lk { ri_end += 1; }
                                    if ri_end > ri0 {
                                        for l in &ls[li0..li] {
                                            for r in &rs[ri0..ri_end] {
                                                let mut merged = l.clone();
                                                merge_right(&mut merged, r, &tbl);
                                                out.push(merged);
                                            }
                                        }
                                    } else {
                                        for l in &ls[li0..li] {
                                            let mut merged = l.clone();
                                            null_right(&mut merged, &right_schema_cols, &tbl);
                                            out.push(merged);
                                        }
                                    }
                                    ri = ri_end;
                                }
                            }
                            JoinType::Right => {
                                let left_cols: Vec<String> = current.first()
                                    .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                                    .unwrap_or_default();
                                let mut li = 0usize;
                                let mut ri = 0usize;
                                while ri < rs.len() {
                                    let rk = key_right(&rs[ri]);
                                    while li < ls.len() && sort_cmp(&key_left(&ls[li]), &rk) == std::cmp::Ordering::Less {
                                        li += 1;
                                    }
                                    let ri0 = ri;
                                    while ri < rs.len() && key_right(&rs[ri]) == rk { ri += 1; }
                                    let li0 = li;
                                    let mut li_end = li;
                                    while li_end < ls.len() && key_left(&ls[li_end]) == rk { li_end += 1; }
                                    if li_end > li0 {
                                        for l in &ls[li0..li_end] {
                                            for r in &rs[ri0..ri] {
                                                let mut merged = l.clone();
                                                merge_right(&mut merged, r, &tbl);
                                                out.push(merged);
                                            }
                                        }
                                    } else {
                                        for r in &rs[ri0..ri] {
                                            let mut merged = Row::new();
                                            for col in &left_cols { merged.insert(col.clone(), NULL_VALUE.to_string()); }
                                            merge_right(&mut merged, r, &tbl);
                                            out.push(merged);
                                        }
                                    }
                                    li = li0; // 같은 왼쪽 그룹이 다음 오른쪽 키에도 매칭될 수 있으므로 유지
                                }
                            }
                        }
                        out
                    }
                    Some(JoinAlgo::Hash { probe_col, build_col }) => {
                        // ── Hash Join ─────────────────────────────────────
                        // Build phase: right 테이블을 build_col 기준으로 해시화
                        let mut hash: HashMap<String, Vec<Row>> = HashMap::new();
                        let bc = build_col.clone();
                        let tbl = j.table.clone();
                        for right in &right_rows {
                            let key = right.get(&bc)
                                .or_else(|| right.get(&format!("{}.{}", tbl, bc)))
                                .cloned().unwrap_or_default();
                            hash.entry(key).or_default().push(right.clone());
                        }
                        // Probe phase: left 테이블로 해시 테이블 조회
                        let pc = probe_col.clone();
                        let mut out = Vec::new();
                        match j.join_type {
                            JoinType::Inner => {
                                for left in &current {
                                    let probe_key = left.get(&pc)
                                        .or_else(|| left.iter().find(|(k, _)| k.ends_with(&format!(".{}", pc))).map(|(_, v)| v))
                                        .cloned().unwrap_or_default();
                                    if let Some(matches) = hash.get(&probe_key) {
                                        for right in matches {
                                            let mut merged = left.clone();
                                            merge_right(&mut merged, right, &j.table);
                                            out.push(merged);
                                        }
                                    }
                                }
                            }
                            JoinType::Left => {
                                for left in &current {
                                    let probe_key = left.get(&pc)
                                        .or_else(|| left.iter().find(|(k, _)| k.ends_with(&format!(".{}", pc))).map(|(_, v)| v))
                                        .cloned().unwrap_or_default();
                                    if let Some(matches) = hash.get(&probe_key) {
                                        for right in matches {
                                            let mut merged = left.clone();
                                            merge_right(&mut merged, right, &j.table);
                                            out.push(merged);
                                        }
                                    } else {
                                        let mut merged = left.clone();
                                        null_right(&mut merged, &right_schema_cols, &j.table);
                                        out.push(merged);
                                    }
                                }
                            }
                            JoinType::Right => {
                                // Right join: build from left, probe with right
                                let mut left_hash: HashMap<String, Vec<Row>> = HashMap::new();
                                for left in &current {
                                    let key = left.get(&pc).cloned().unwrap_or_default();
                                    left_hash.entry(key).or_default().push(left.clone());
                                }
                                let left_cols: Vec<String> = current.first()
                                    .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                                    .unwrap_or_default();
                                for right in &right_rows {
                                    let key = right.get(&bc).cloned().unwrap_or_default();
                                    if let Some(lefts) = left_hash.get(&key) {
                                        for left in lefts {
                                            let mut merged = left.clone();
                                            merge_right(&mut merged, right, &j.table);
                                            out.push(merged);
                                        }
                                    } else {
                                        let mut merged = Row::new();
                                        for col in &left_cols { merged.insert(col.clone(), NULL_VALUE.to_string()); }
                                        merge_right(&mut merged, right, &j.table);
                                        out.push(merged);
                                    }
                                }
                            }
                        }
                        out
                    }
                    _ => {
                        // ── Nested Loop Join (default) ───────────────────
                        let mut out = Vec::new();
                        match j.join_type {
                            JoinType::Inner => {
                                for left in &current {
                                    for right in &right_rows {
                                        let mut merged = left.clone();
                                        merge_right(&mut merged, right, &j.table);
                                        if Self::eval_condexpr(&merged, &j.on_expr) { out.push(merged); }
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
                                            out.push(merged); matched = true;
                                        }
                                    }
                                    if !matched {
                                        let mut merged = left.clone();
                                        null_right(&mut merged, &right_schema_cols, &j.table);
                                        out.push(merged);
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
                                            out.push(merged); matched = true;
                                        }
                                    }
                                    if !matched {
                                        let mut merged = Row::new();
                                        for col in &left_cols { merged.insert(col.clone(), NULL_VALUE.to_string()); }
                                        merge_right(&mut merged, right, &j.table);
                                        out.push(merged);
                                    }
                                }
                            }
                        }
                        out
                    }
                };
                current = joined;
            }
            current.into_iter()
                .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
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
                    // GROUP_CONCAT: 문자열 수집 후 join
                    if let AggFunc::GroupConcat { separator } = func {
                        let strs: Vec<String> = grp.iter()
                            .filter_map(|r| {
                                let v = r.get(col_name)?;
                                if v == NULL_VALUE { None } else { Some(v.clone()) }
                            })
                            .collect();
                        out.insert(label, strs.join(separator));
                        continue;
                    }
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
                        AggFunc::GroupConcat { .. } => unreachable!(),
                    };
                    let v = match func {
                        AggFunc::Avg => format!("{:.4}", agg_val),
                        _ => if agg_val.fract() == 0.0 { format!("{}", agg_val as i64) }
                             else { format!("{:.2}", agg_val) },
                    };
                    out.insert(label, v);
                }
                // HAVING 절에서 참조되는 집계 함수 중 SELECT에 없는 것을 보충
                if let Some(ref hav) = having {
                    for agg_key in Self::extract_agg_refs_from_cond(hav) {
                        if !out.contains_key(&agg_key) {
                            out.insert(agg_key.clone(), Self::compute_agg_from_key(&agg_key, grp));
                        }
                    }
                }
                out
            }).collect();

            // HAVING 필터 (집계된 컬럼 기준)
            if let Some(ref hav) = having {
                group_rows.retain(|row| Self::matches_condexpr(row, &Some(hav.clone())));
            }
            // ORDER BY on aggregated results (handles aggregate aliases like avg_sal)
            if !order_by.is_empty() {
                group_rows.sort_by(|a, b| {
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
            if let Some(n) = offset { let skip = n.min(group_rows.len()); group_rows.drain(..skip); }
            if let Some(n) = limit { group_rows.truncate(n); }
            return self.format_result(s, group_rows, columns, table, joins.clone());
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
                // GROUP_CONCAT (전역)
                if let AggFunc::GroupConcat { separator } = func {
                    let strs: Vec<String> = result.iter()
                        .filter_map(|r| {
                            let v = r.get(col_name.as_str())?;
                            if v == NULL_VALUE { None } else { Some(v.clone()) }
                        })
                        .collect();
                    agg_results.push((label, strs.join(separator)));
                    continue;
                }
                let vals: Vec<f64> = result.iter()
                    .filter_map(|r| {
                        if col_name == "*" { Some(1.0) }
                        else { r.get(col_name.as_str())?.parse::<f64>().ok() }
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
                    AggFunc::GroupConcat { .. } => unreachable!(),
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
            let pk_col = s.catalog.get_table(&table)
                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                .unwrap_or_else(|| "id".to_string());
            for row in &result {
                let pk_val = row.get(&pk_col).cloned().unwrap_or_default();
                match s.lock_mgr.acquire(&table, &pk_val, txn_id) {
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

        self.format_result(s, result, columns, table, joins)
    }

    // ─── FROM 서브쿼리 실행 ──────────────────────────────────────
    fn exec_select_with_subquery(
        &mut self,
        s: &mut SharedDatabase,
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
        if s.tables.contains_key(&alias) || s.views.contains_key(&alias) {
            return Err(format!("Alias '{}' conflicts with an existing table or view", alias));
        }

        let inner_output = self.execute_with_s(s, inner_stmt)?;
        let (col_names, virtual_rows) = Self::parse_table_output(&inner_output);
        if col_names.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        s.tables.insert(alias.clone(), virtual_rows.clone());
        s.buffer_pool.write_page(&alias, virtual_rows);
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
        let _ = s.catalog.create_table(alias.clone(), schema_cols);

        let result = self.exec_select(
            s, alias.clone(), None, distinct, columns, condition,
            joins, order_by, group_by, having, limit, offset, for_update,
        );

        s.tables.remove(&alias);
        s.buffer_pool.invalidate(&alias);
        let _ = s.catalog.drop_table(&alias);

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
        // 인수를 row 컬럼값, 리터럴, 또는 산술식으로 해석
        let resolve = |arg: &str, row: &Row| -> String {
            if arg.starts_with('\'') && arg.ends_with('\'') {
                return arg[1..arg.len()-1].to_string();
            }
            if let Some(v) = Self::get_col(row, arg) {
                return v.clone();
            }
            // table.col 형태
            if let Some(idx) = arg.rfind('.') {
                if let Some(v) = row.get(&arg[idx+1..]) {
                    return v.clone();
                }
            }
            // 산술 표현식 폴백 (e.g. "salary / 1000000")
            let mut p = crate::parser::parser::Parser::new(arg);
            if let Ok(expr) = p.parse_arith_expr() {
                return Self::eval_arith(row, &expr);
            }
            arg.to_string()
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
            "NULLIF" => {
                let a = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let b = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                if a == b { NULL_VALUE.to_string() } else { a }
            }
            "LPAD" => {
                let s   = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let len: usize = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                let pad = args.get(2).map(|a| resolve(a, row)).unwrap_or_else(|| " ".to_string());
                if s.len() >= len { s[..len].to_string() }
                else {
                    let pad_needed = len - s.len();
                    let full_pad = pad.repeat((pad_needed / pad.len()) + 1);
                    format!("{}{}", &full_pad[..pad_needed], s)
                }
            }
            "RPAD" => {
                let s   = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let len: usize = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                let pad = args.get(2).map(|a| resolve(a, row)).unwrap_or_else(|| " ".to_string());
                if s.len() >= len { s[..len].to_string() }
                else {
                    let pad_needed = len - s.len();
                    let full_pad = pad.repeat((pad_needed / pad.len()) + 1);
                    format!("{}{}", s, &full_pad[..pad_needed])
                }
            }
            "CAST" => {
                let val      = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let type_str = args.get(1).map(|s| s.as_str()).unwrap_or("TEXT");
                match type_str {
                    "INT" | "INTEGER" => val.parse::<i64>().map(|n| n.to_string()).unwrap_or_else(|_| "0".to_string()),
                    "FLOAT" | "DOUBLE" | "DECIMAL" => val.parse::<f64>().map(|n| format!("{}", n)).unwrap_or_else(|_| "0".to_string()),
                    "BOOLEAN" => {
                        let b = !val.is_empty() && val != "0" && val != "false" && val.to_lowercase() != "false";
                        if b { "1".to_string() } else { "0".to_string() }
                    }
                    _ => val,
                }
            }
            "DATEDIFF" => {
                let d1 = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let d2 = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                fn parse_date(s: &str) -> Option<chrono::NaiveDate> {
                    chrono::NaiveDate::parse_from_str(s, "%Y-%m-%d").ok()
                }
                match (parse_date(&d1), parse_date(&d2)) {
                    (Some(a), Some(b)) => (a - b).num_days().to_string(),
                    _ => NULL_VALUE.to_string(),
                }
            }
            "DATE_ADD" => {
                let date_str = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let amount: i64 = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                let unit = args.get(2).map(|s| s.as_str()).unwrap_or("DAY");
                use chrono::{NaiveDate, Datelike};
                if let Ok(d) = NaiveDate::parse_from_str(&date_str, "%Y-%m-%d") {
                    let result = match unit {
                        "DAY"    => d + chrono::Duration::days(amount),
                        "MONTH"  => {
                            let months = d.month() as i64 + amount;
                            let year   = d.year() + ((months - 1) / 12) as i32;
                            let month  = ((months - 1).rem_euclid(12) + 1) as u32;
                            NaiveDate::from_ymd_opt(year, month, d.day()).unwrap_or(d)
                        }
                        "YEAR"   => {
                            NaiveDate::from_ymd_opt(d.year() + amount as i32, d.month(), d.day()).unwrap_or(d)
                        }
                        "HOUR" | "MINUTE" | "SECOND" => d, // DATE만 반환, 시간 무시
                        _ => d,
                    };
                    result.format("%Y-%m-%d").to_string()
                } else {
                    NULL_VALUE.to_string()
                }
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
            AggFunc::GroupConcat { .. } => format!("GROUP_CONCAT({})", col),
        }
    }

    // HAVING 절의 CondExpr에서 집계 함수 참조 문자열 수집
    fn extract_agg_refs_from_cond(expr: &CondExpr) -> Vec<String> {
        let mut refs = Vec::new();
        Self::collect_agg_refs_cond(expr, &mut refs);
        refs
    }

    fn collect_agg_refs_cond(expr: &CondExpr, out: &mut Vec<String>) {
        match expr {
            CondExpr::And(l, r) | CondExpr::Or(l, r) => {
                Self::collect_agg_refs_cond(l, out);
                Self::collect_agg_refs_cond(r, out);
            }
            CondExpr::Not(inner) => Self::collect_agg_refs_cond(inner, out),
            CondExpr::Leaf(cond) => Self::collect_agg_refs_arith(&cond.left, out),
        }
    }

    fn collect_agg_refs_arith(expr: &ArithExpr, out: &mut Vec<String>) {
        match expr {
            ArithExpr::Col(s) => {
                let u = s.to_uppercase();
                if (u.starts_with("COUNT(") || u.starts_with("SUM(") || u.starts_with("AVG(")
                    || u.starts_with("MIN(") || u.starts_with("MAX("))
                    && !out.contains(s)
                {
                    out.push(s.clone());
                }
            }
            ArithExpr::Add(l, r) | ArithExpr::Sub(l, r)
            | ArithExpr::Mul(l, r) | ArithExpr::Div(l, r) => {
                Self::collect_agg_refs_arith(l, out);
                Self::collect_agg_refs_arith(r, out);
            }
            _ => {}
        }
    }

    // "COUNT(*)", "SUM(col)" 등의 키 문자열로 그룹 집계값 계산
    fn compute_agg_from_key(key: &str, grp: &[Row]) -> String {
        let ku = key.to_uppercase();
        if ku.starts_with("COUNT(") {
            return format!("{}", grp.len());
        }
        let inner = match (key.find('('), key.rfind(')')) {
            (Some(s), Some(e)) => &key[s + 1..e],
            _ => return "0".to_string(),
        };
        let vals: Vec<f64> = grp.iter()
            .filter_map(|r| r.get(inner)?.parse::<f64>().ok())
            .collect();
        let v = if ku.starts_with("SUM(") {
            vals.iter().sum::<f64>()
        } else if ku.starts_with("AVG(") {
            if vals.is_empty() { 0.0 } else { vals.iter().sum::<f64>() / vals.len() as f64 }
        } else if ku.starts_with("MIN(") {
            vals.iter().cloned().fold(f64::INFINITY, f64::min)
        } else if ku.starts_with("MAX(") {
            vals.iter().cloned().fold(f64::NEG_INFINITY, f64::max)
        } else {
            0.0
        };
        if v.fract() == 0.0 { format!("{}", v as i64) } else { format!("{:.4}", v) }
    }

    fn format_result(
        &self,
        s: &SharedDatabase,
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
            let mut pairs: Vec<(String, ColSource)> = s.catalog.get_table(&table)
                .map(|s| s.columns.iter().map(|c| (c.name.clone(), ColSource::Key(c.name.clone()))).collect())
                .unwrap_or_default();
            for j in &joins {
                if let Some(schema) = s.catalog.get_table(&j.table) {
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
        s: &mut SharedDatabase,
        table: String,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
    ) -> Result<String, String> {
        // PK 컬럼명 먼저 추출 (borrow 분리)
        let pk_col = s.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .columns.iter()
            .find(|c| c.primary_key)
            .map(|c| c.name.clone())
            .unwrap_or_else(|| "id".to_string());

        // 서브쿼리 조건 지원: 먼저 매칭되는 PK 목록을 수집 (borrow 분리)
        let candidate_rows: Vec<Row> = s.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .iter()
            .filter(|r| Self::is_visible(r))
            .cloned()
            .collect();
        let matching_pks: Vec<String> = candidate_rows.iter()
            .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
            .map(|r| r.get(&pk_col).cloned().unwrap_or_default())
            .collect();

        let rows = s.tables.get_mut(&table)
            .ok_or(format!("Table '{}' not found", table))?;

        let mut count = 0;
        let mut undo_entries: Vec<(String, String, String)> = Vec::new();
        let cur_txn = self.txn.current_txn_id();

        for row in rows.iter_mut() {
            if matching_pks.contains(&row.get(&pk_col).cloned().unwrap_or_default()) {
                let key = row.get(&pk_col).cloned().unwrap_or_default();

                // 잠금 충돌 / 데드락 체크 (활성 트랜잭션 안에서만)
                if cur_txn != 0 {
                    match s.lock_mgr.acquire(&table, &key, cur_txn) {
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

                // ENUM / SET 값 유효성 검사 (row 반영 전)
                if let Some(schema) = s.catalog.get_table(&table) {
                    for (col_name, val) in &new_vals {
                        if val.is_empty() || val.as_str() == NULL_VALUE { continue; }
                        if let Some(col) = schema.columns.iter().find(|c| &c.name == col_name) {
                            match &col.data_type {
                                DataType::Enum(allowed) => {
                                    if !allowed.iter().any(|a| a == val) {
                                        return Err(format!(
                                            "Invalid ENUM value '{}' for column '{}'. Allowed: {}",
                                            val, col.name,
                                            allowed.iter().map(|s| format!("'{}'", s)).collect::<Vec<_>>().join(", ")
                                        ));
                                    }
                                }
                                DataType::Set(allowed) => {
                                    for part in val.split(',') {
                                        let part = part.trim();
                                        if !part.is_empty() && !allowed.iter().any(|a| a == part) {
                                            return Err(format!(
                                                "Invalid SET value '{}' for column '{}'. Allowed: {}",
                                                part, col.name,
                                                allowed.iter().map(|s| format!("'{}'", s)).collect::<Vec<_>>().join(", ")
                                            ));
                                        }
                                    }
                                }
                                _ => {}
                            }
                        }
                    }
                }

                for (col, val) in new_vals {
                    row.insert(col, val);
                }
                // CHECK 제약 검사 (수정 후 row 기준)
                if let Some(schema) = s.catalog.get_table(&table) {
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

        let rows_clone = s.tables.get(&table).unwrap().clone();
        if let Some(index) = s.indexes.get_mut(&table) {
            *index = BPlusTree::new();
            for row in &rows_clone {
                let k = row.get(&pk_col).cloned().unwrap_or_default();
                let val_json = serde_json::to_string(row).unwrap();
                index.insert(k, val_json);
            }
        }

        // 단일 컬럼 보조 인덱스 재빌드
        self.rebuild_secondary_indexes(s, &table, &rows_clone);

        // 복합 인덱스 재빌드
        let comp_keys: Vec<String> = s.composite_indexes.iter()
            .filter(|(_, ci)| ci.table == table)
            .map(|(k, _)| k.clone())
            .collect();
        for k in comp_keys {
            if let Some(ci) = s.composite_indexes.get_mut(&k) {
                ci.rebuild(&rows_clone);
            }
        }

        // ON UPDATE FK 처리: assignments에 변경된 컬럼이 다른 테이블에서 FK로 참조되는지 확인
        let changed_cols: Vec<String> = assignments.iter().map(|(c, _)| c.clone()).collect();
        let other_tables: Vec<(String, Vec<crate::catalog::schema::ColumnDef>)> =
            s.catalog.tables.iter()
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
                                        if let Some(other_rows) = s.tables.get(other_table) {
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
                                        if let Some(other_rows) = s.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &old_val).unwrap_or(false) {
                                                    row.insert(col.name.clone(), new_val.clone());
                                                }
                                            }
                                        }
                                        let rows_clone2 = s.tables.get(other_table).unwrap().clone();
                                        s.buffer_pool.write_page(other_table, rows_clone2.clone());
                                        s.buffer_pool.flush_page(other_table, &s.disk);
                                    }
                                    crate::catalog::schema::FkAction::SetNull => {
                                        if let Some(other_rows) = s.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &old_val).unwrap_or(false) {
                                                    row.insert(col.name.clone(), NULL_VALUE.to_string());
                                                }
                                            }
                                        }
                                        let rows_clone2 = s.tables.get(other_table).unwrap().clone();
                                        s.buffer_pool.write_page(other_table, rows_clone2.clone());
                                        s.buffer_pool.flush_page(other_table, &s.disk);
                                    }
                                    crate::catalog::schema::FkAction::SetDefault => {
                                        let default_val = s.catalog.get_table(other_table)
                                            .and_then(|s| s.columns.iter().find(|c| c.name == col.name))
                                            .and_then(|c| c.default.clone())
                                            .unwrap_or_else(|| NULL_VALUE.to_string());
                                        if let Some(other_rows) = s.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &old_val).unwrap_or(false) {
                                                    row.insert(col.name.clone(), default_val.clone());
                                                }
                                            }
                                        }
                                        let rows_clone2 = s.tables.get(other_table).unwrap().clone();
                                        s.buffer_pool.write_page(other_table, rows_clone2.clone());
                                        s.buffer_pool.flush_page(other_table, &s.disk);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        let rows = s.tables.get(&table).unwrap().clone();
        s.buffer_pool.write_page(&table, rows);
        s.buffer_pool.flush_page(&table, &s.disk);
        self.maybe_auto_checkpoint(s);
        Ok(format!("{} row(s) updated.", count))
    }

    fn exec_delete(&mut self, s: &mut SharedDatabase, table: String, condition: Option<CondExpr>) -> Result<String, String> {
        // 서브쿼리 조건 지원: 먼저 매칭 행을 수집 (borrow 분리)
        let candidates: Vec<Row> = s.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .iter()
            .filter(|r| Self::is_visible(r))
            .cloned()
            .collect();
        let rows_to_delete: Vec<Row> = candidates.into_iter()
            .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
            .collect();

        // FK 처리 (CASCADE / RESTRICT / SET NULL)
        let other_tables: Vec<(String, Vec<crate::catalog::schema::ColumnDef>)> =
            s.catalog.tables.iter()
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
                                    if let Some(other_rows) = s.tables.get(other_table) {
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
                                        if let Some(other_rows) = s.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &del_val).unwrap_or(false) {
                                                    row.insert("_xmax".to_string(), txn_id.clone());
                                                }
                                            }
                                        }
                                    } else {
                                        // 트랜잭션 밖: 물리 삭제
                                        if let Some(other_rows) = s.tables.get_mut(other_table) {
                                            other_rows.retain(|r| {
                                                !(Self::is_visible(r) && r.get(&col.name).map(|v| v == &del_val).unwrap_or(false))
                                            });
                                        }
                                    }
                                    let rows_clone = s.tables.get(other_table).unwrap().clone();
                                    s.buffer_pool.write_page(other_table, rows_clone.clone());
                                    s.buffer_pool.flush_page(other_table, &s.disk);
                                }
                                crate::catalog::schema::FkAction::SetNull => {
                                    if let Some(other_rows) = s.tables.get_mut(other_table) {
                                        for row in other_rows.iter_mut() {
                                            if Self::is_visible(row) && row.get(&col.name).map(|v| v == &del_val).unwrap_or(false) {
                                                row.insert(col.name.clone(), NULL_VALUE.to_string());
                                            }
                                        }
                                    }
                                    let rows_clone = s.tables.get(other_table).unwrap().clone();
                                    s.buffer_pool.write_page(other_table, rows_clone.clone());
                                    s.buffer_pool.flush_page(other_table, &s.disk);
                                }
                                crate::catalog::schema::FkAction::SetDefault => {
                                    let default_val = s.catalog.get_table(other_table)
                                        .and_then(|s| s.columns.iter().find(|c| c.name == col.name))
                                        .and_then(|c| c.default.clone())
                                        .unwrap_or_else(|| NULL_VALUE.to_string());
                                    if let Some(other_rows) = s.tables.get_mut(other_table) {
                                        for row in other_rows.iter_mut() {
                                            if Self::is_visible(row) && row.get(&col.name).map(|v| v == &del_val).unwrap_or(false) {
                                                row.insert(col.name.clone(), default_val.clone());
                                            }
                                        }
                                    }
                                    let rows_clone = s.tables.get(other_table).unwrap().clone();
                                    s.buffer_pool.write_page(other_table, rows_clone.clone());
                                    s.buffer_pool.flush_page(other_table, &s.disk);
                                }
                            }
                        }
                    }
                }
            }
        }

        let pk_col = s.catalog.get_table(&table)
            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
            .unwrap_or_else(|| "id".to_string());
        let mut deleted = 0usize;

        if self.txn.is_active() {
            // ── 트랜잭션 안: MVCC 논리 삭제 (_xmax = txn_id) ──
            let txn_id = self.txn.current_txn_id();
            let txn_id_str = txn_id.to_string();
            let rows = s.tables.get_mut(&table).unwrap();
            for row in rows.iter_mut() {
                if Self::is_visible(row) && Self::matches_condexpr(row, &condition) {
                    let key = row.get(&pk_col).cloned().unwrap_or_default();

                    // 잠금 충돌 / 데드락 체크
                    match s.lock_mgr.acquire(&table, &key, txn_id) {
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
            let rows = s.tables.get_mut(&table).unwrap();
            let before = rows.len();
            rows.retain(|r| !(Self::is_visible(r) && Self::matches_condexpr(r, &condition)));
            deleted = before - rows.len();
        }

        let rows_clone = s.tables.get(&table).unwrap().clone();

        if !self.txn.is_active() {
            // 물리 삭제 후: 인덱스 재빌드 + 버퍼 풀 즉시 flush
            if let Some(index) = s.indexes.get_mut(&table) {
                *index = BPlusTree::new();
                for row in &rows_clone {
                    let key = row.values().next().cloned().unwrap_or_default();
                    let val_json = serde_json::to_string(row).unwrap();
                    index.insert(key, val_json);
                }
            }
            let comp_keys: Vec<String> = s.composite_indexes.iter()
                .filter(|(_, ci)| ci.table == table)
                .map(|(k, _)| k.clone())
                .collect();
            for k in comp_keys {
                if let Some(ci) = s.composite_indexes.get_mut(&k) {
                    ci.rebuild(&rows_clone);
                }
            }
            s.buffer_pool.write_page(&table, rows_clone.clone());
            s.buffer_pool.flush_page(&table, &s.disk);
        } else {
            // 논리 삭제 후: 버퍼 풀 갱신 (SELECT가 최신 _xmax 반영)
            s.buffer_pool.write_page(&table, rows_clone);
        }

        self.maybe_auto_checkpoint(s);
        Ok(format!("{} row(s) deleted.", deleted))
    }

    fn exec_begin(&mut self, s: &SharedDatabase) -> Result<String, String> {
        let txn_id = self.txn.begin_with_snapshot(&s.tables)?;
        let level = format!("{:?}", self.txn.isolation_level);
        Ok(format!("Transaction {} started. (isolation: {})", txn_id, level))
    }

    fn exec_commit(&mut self, s: &mut SharedDatabase) -> Result<String, String> {
        // SERIALIZABLE: 커밋 전 팬텀 읽기 검증
        if let Err(e) = self.txn.validate_serializable(&s.tables) {
            // 검증 실패 → 자동 롤백 후 오류 반환
            self.apply_rollback(s);
            return Err(format!("{} (auto-rolled back)", e));
        }

        // 트랜잭션 중 수정된 테이블을 버퍼 풀 + 디스크에 반영
        let dirty = self.txn.dirty_tables();
        for table in &dirty {
            if let Some(rows) = s.tables.get(table) {
                let rows_clone = rows.clone();
                s.buffer_pool.write_page(table, rows_clone);
                s.buffer_pool.flush_page(table, &s.disk);
            }
        }

        let txn_id = self.txn.current_txn_id();
        self.txn.commit()?;
        // 이 트랜잭션이 보유한 모든 잠금 해제
        s.lock_mgr.release(txn_id);
        Ok("Transaction committed.".to_string())
    }

    /// 롤백 공통 헬퍼: exec_rollback과 SERIALIZABLE 자동 롤백에서 공유
    fn apply_rollback(&mut self, s: &mut SharedDatabase) {
        let txn_id = self.txn.current_txn_id();
        let undo_entries = match self.txn.abort() {
            Ok(entries) => entries,
            Err(_) => return,
        };
        s.lock_mgr.release(txn_id);
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    let pk_col = s.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = s.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = s.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.get(&pk_col).cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    s.disk.save_table(&entry.table, &rows_clone);
                }
                "UPDATE" => {
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            let pk_col = s.catalog.get_table(&entry.table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());
                            if let Some(rows) = s.tables.get_mut(&entry.table) {
                                for row in rows.iter_mut() {
                                    if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                        *row = old_row.clone();
                                        break;
                                    }
                                }
                            }
                            let rows_clone = s.tables.get(&entry.table).cloned().unwrap_or_default();
                            s.disk.save_table(&entry.table, &rows_clone);
                        }
                    }
                }
                "DELETE" => {
                    // MVCC: 논리 삭제 취소 → _xmax = "0" 복원
                    let pk_col = s.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                        for row in rows.iter_mut() {
                            if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                row.insert("_xmax".to_string(), "0".to_string());
                            }
                        }
                    }
                    let rows_clone = s.tables.get(&entry.table).cloned().unwrap_or_default();
                    s.disk.save_table(&entry.table, &rows_clone);
                }
                _ => {}
            }
        }
    }

    fn exec_rollback(&mut self, s: &mut SharedDatabase) -> Result<String, String> {
        let txn_id = self.txn.current_txn_id();
        let undo_entries = self.txn.abort()?;
        // 잠금 해제
        s.lock_mgr.release(txn_id);
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    let pk_col = s.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = s.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = s.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.get(&pk_col).cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    s.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    s.buffer_pool.flush_page(&entry.table, &s.disk);
                }
                "UPDATE" => {
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            // PK 컬럼명 추출
                            let pk_col = s.catalog.get_table(&entry.table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());

                            if let Some(rows) = s.tables.get_mut(&entry.table) {
                                for row in rows.iter_mut() {
                                    if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                        *row = old_row.clone();
                                        break;
                                    }
                                }
                            }
                            let rows_clone = s.tables.get(&entry.table).unwrap().clone();
                            // B+Tree 인덱스 재빌드 (rollback 후 stale 데이터 방지)
                            if let Some(index) = s.indexes.get_mut(&entry.table) {
                                *index = BPlusTree::new();
                                for row in &rows_clone {
                                    let k = row.get(&pk_col).cloned().unwrap_or_default();
                                    let v = serde_json::to_string(row).unwrap();
                                    index.insert(k, v);
                                }
                            }
                            s.buffer_pool.write_page(&entry.table, rows_clone.clone());
                            s.buffer_pool.flush_page(&entry.table, &s.disk);
                        }
                    }
                }
                "DELETE" => {
                    // MVCC: 논리 삭제 취소 → _xmax = "0" 복원
                    let pk_col = s.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                        for row in rows.iter_mut() {
                            if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                row.insert("_xmax".to_string(), "0".to_string());
                            }
                        }
                    }
                    let rows_clone = s.tables.get(&entry.table).cloned().unwrap_or_default();
                    // B+Tree 인덱스 재빌드
                    if let Some(index) = s.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            if Self::is_visible(row) {
                                let k = row.get(&pk_col).cloned().unwrap_or_default();
                                let v = serde_json::to_string(row).unwrap();
                                index.insert(k, v);
                            }
                        }
                    }
                    s.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    s.buffer_pool.flush_page(&entry.table, &s.disk);
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

    fn exec_rollback_to(&mut self, s: &mut SharedDatabase, name: String) -> Result<String, String> {
        let undo_entries = self.txn.rollback_to_savepoint(&name)?;
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    let pk_col = s.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = s.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = s.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.get(&pk_col).cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    s.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    s.buffer_pool.flush_page(&entry.table, &s.disk);
                }
                "UPDATE" => {
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            let pk_col = s.catalog.get_table(&entry.table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());
                            if let Some(rows) = s.tables.get_mut(&entry.table) {
                                for row in rows.iter_mut() {
                                    if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                        *row = old_row.clone();
                                        break;
                                    }
                                }
                            }
                            let rows_clone = s.tables.get(&entry.table).unwrap().clone();
                            s.buffer_pool.write_page(&entry.table, rows_clone.clone());
                            s.buffer_pool.flush_page(&entry.table, &s.disk);
                        }
                    }
                }
                "DELETE" => {
                    let pk_col = s.catalog.get_table(&entry.table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                        for row in rows.iter_mut() {
                            if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                row.insert("_xmax".to_string(), "0".to_string());
                            }
                        }
                    }
                    let rows_clone = s.tables.get(&entry.table).cloned().unwrap_or_default();
                    s.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    s.buffer_pool.flush_page(&entry.table, &s.disk);
                }
                _ => {}
            }
        }
        Ok(format!("Rolled back to savepoint '{}'.", name))
    }

    fn exec_alter(&mut self, s: &mut SharedDatabase, table: String, action: AlterAction) -> Result<String, String> {
        match action {
            AlterAction::AddColumn(col) => {
                let schema = s.catalog.tables.get_mut(&table)
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
                if let Some(rows) = s.tables.get_mut(&table) {
                    for row in rows.iter_mut() {
                        row.insert(col.name.clone(), fill_val.clone());
                    }
                }
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                s.disk.save_table(&table, s.tables.get(&table).unwrap());
                Ok(format!("Column '{}' added to '{}'.", col.name, table))
            }
            AlterAction::DropColumn(col_name) => {
                let schema = s.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                schema.columns.retain(|c| c.name != col_name);
                if let Some(rows) = s.tables.get_mut(&table) {
                    for row in rows.iter_mut() {
                        row.remove(&col_name);
                    }
                }
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                s.disk.save_table(&table, s.tables.get(&table).unwrap());
                Ok(format!("Column '{}' dropped from '{}'.", col_name, table))
            }
            AlterAction::RenameColumn { from, to } => {
                let schema = s.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                for col in schema.columns.iter_mut() {
                    if col.name == from { col.name = to.clone(); }
                }
                if let Some(rows) = s.tables.get_mut(&table) {
                    for row in rows.iter_mut() {
                        if let Some(val) = row.remove(&from) {
                            row.insert(to.clone(), val);
                        }
                    }
                }
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                s.disk.save_table(&table, s.tables.get(&table).unwrap());
                Ok(format!("Column '{}' renamed to '{}' in '{}'.", from, to, table))
            }
            AlterAction::ModifyColumn(col) => {
                // 컬럼 존재 확인
                let exists = s.catalog.tables.get(&table)
                    .ok_or(format!("Table '{}' not found", table))?
                    .columns.iter().any(|c| c.name == col.name);
                if !exists {
                    return Err(format!("Column '{}' not found in '{}'", col.name, table));
                }
                // 기존 데이터 타입 변환 검증: 기존 행의 값이 새 타입으로 캐스팅 가능한지 확인
                if let Some(rows) = s.tables.get(&table) {
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
                                DataType::Double => val.parse::<f64>().is_ok(),
                                DataType::Time | DataType::Year => true,
                                DataType::Enum(allowed) => allowed.iter().any(|a| a == val),
                                DataType::Set(allowed) => val.split(',').all(|p| {
                                    let p = p.trim();
                                    p.is_empty() || allowed.iter().any(|a| a == p)
                                }),
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
                let schema = s.catalog.tables.get_mut(&table).unwrap();
                if let Some(c) = schema.columns.iter_mut().find(|c| c.name == col.name) {
                    c.data_type = col.data_type;
                    c.not_null   = col.not_null;
                    c.unique     = col.unique;
                    c.unique_constraint_name = col.unique_constraint_name;
                    c.auto_increment = col.auto_increment;
                    c.default    = col.default;
                    // primary_key는 MODIFY로 변경 불가 (무시)
                }
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                Ok(format!("Column '{}' in '{}' modified.", col.name, table))
            }
            AlterAction::RenameTable { to } => {
                if !s.catalog.tables.contains_key(&table) {
                    return Err(format!("Table '{}' not found", table));
                }
                if s.catalog.tables.contains_key(&to) {
                    return Err(format!("Table '{}' already exists", to));
                }
                // Catalog rename
                let schema = s.catalog.tables.remove(&table).unwrap();
                s.catalog.tables.insert(to.clone(), schema);

                // In-memory data rename
                if let Some(rows) = s.tables.remove(&table) {
                    s.tables.insert(to.clone(), rows);
                }

                // B+Tree index rename
                if let Some(tree) = s.indexes.remove(&table) {
                    s.indexes.insert(to.clone(), tree);
                }

                // Secondary index meta rename
                for (_, (ref mut tbl, _)) in s.index_meta.iter_mut() {
                    if *tbl == table { *tbl = to.clone(); }
                }
                let sec_keys: Vec<String> = s.indexes.keys()
                    .filter(|k| k.starts_with(&format!("{}_", table)))
                    .cloned().collect();
                for old_key in sec_keys {
                    let suffix = &old_key[table.len()..];
                    let new_key = format!("{}{}", to, suffix);
                    if let Some(tree) = s.indexes.remove(&old_key) {
                        s.indexes.insert(new_key, tree);
                    }
                }

                // Composite index rename
                for (_, ci) in s.composite_indexes.iter_mut() {
                    if ci.table == table { ci.table = to.clone(); }
                }

                // Disk: save under new name, delete old files
                let full_schema = s.catalog.get_table(&to).unwrap();
                s.disk.save_schema(&to, full_schema);
                if let Some(rows) = s.tables.get(&to) {
                    s.disk.save_table(&to, rows);
                }
                s.disk.delete_table(&table);

                Ok(format!("Table '{}' renamed to '{}'.", table, to))
            }
        }
    }

    fn exec_create_database(&mut self, s: &mut SharedDatabase, name: String, if_not_exists: bool) -> Result<String, String> {
        let key = name.to_lowercase();
        if s.databases.contains(&key) {
            if if_not_exists {
                return Ok(format!("Database '{}' already exists (skipped).", name));
            }
            return Err(format!("Database '{}' already exists.", name));
        }
        s.disk.create_db_dir(&key);
        s.databases.insert(key.clone());
        Ok(format!("Database '{}' created.", key))
    }

    fn exec_drop_database(&mut self, s: &mut SharedDatabase, name: String, if_exists: bool) -> Result<String, String> {
        let key = name.to_lowercase();
        if !s.databases.contains(&key) {
            if if_exists {
                return Ok(format!("Database '{}' does not exist (skipped).", name));
            }
            return Err(format!("Database '{}' does not exist.", name));
        }
        // 해당 DB의 테이블들만 삭제
        let prefix = format!("{}.", key);
        let table_keys: Vec<String> = s.tables.keys()
            .filter(|k| k.starts_with(&prefix))
            .cloned().collect();
        for t in table_keys {
            s.catalog.tables.remove(&t);
            s.tables.remove(&t);
            s.indexes.remove(&t);
            s.buffer_pool.invalidate(&t);
            s.disk.delete_table(&t);
        }
        // 해당 DB의 secondary 인덱스 삭제
        let sec_keys: Vec<String> = s.indexes.keys()
            .filter(|k| k.starts_with(&prefix))
            .cloned().collect();
        for k in &sec_keys {
            s.buffer_pool.invalidate(k);
            s.indexes.remove(k);
        }
        s.index_meta.retain(|_, (tbl, _)| !tbl.starts_with(&prefix));
        s.composite_indexes.retain(|_, ci| !ci.table.starts_with(&prefix));

        // 해당 DB의 뷰 삭제
        s.views.retain(|k, _| !k.starts_with(&prefix));

        // DB 디렉토리 삭제
        s.disk.drop_db_dir(&key);
        s.databases.remove(&key);

        // 현재 DB가 삭제된 경우 다른 DB로 전환
        if self.current_db == key {
            if let Some(remaining) = s.databases.iter().next().cloned() {
                self.current_db = remaining;
            } else {
                self.current_db = String::new();
            }
        }

        Ok(format!("Database '{}' dropped.", key))
    }

    fn exec_multi_update(
        &mut self,
        s: &mut SharedDatabase,
        tables: Vec<String>,
        joins: Vec<Join>,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
    ) -> Result<String, String> {
        // Build joined rows from the first table + any explicit JOINs
        let first_table = tables.first()
            .ok_or("No tables specified for multi-table UPDATE")?
            .clone();

        let base_rows: Vec<Row> = s.tables.get(&first_table)
            .ok_or(format!("Table '{}' not found", first_table))?
            .iter()
            .filter(|r| Self::is_visible(r))
            .map(|r| {
                let mut prefixed = Row::new();
                for (k, v) in r.iter() {
                    prefixed.insert(format!("{}.{}", first_table, k), v.clone());
                    prefixed.entry(k.clone()).or_insert_with(|| v.clone());
                }
                prefixed
            })
            .collect();

        // Apply additional tables as cross-joins (comma-list style)
        let mut current = base_rows;
        for extra_tbl in tables.iter().skip(1) {
            let right_rows: Vec<Row> = s.tables.get(extra_tbl)
                .ok_or(format!("Table '{}' not found", extra_tbl))?
                .iter()
                .filter(|r| Self::is_visible(r))
                .cloned()
                .collect();
            let tbl = extra_tbl.clone();
            let mut out = Vec::new();
            for left in &current {
                for right in &right_rows {
                    let mut merged = left.clone();
                    for (k, v) in right.iter() {
                        merged.insert(format!("{}.{}", tbl, k), v.clone());
                        merged.entry(k.clone()).or_insert_with(|| v.clone());
                    }
                    out.push(merged);
                }
            }
            current = out;
        }

        // Apply explicit JOINs
        for j in &joins {
            let right_rows: Vec<Row> = s.tables.get(&j.table)
                .ok_or(format!("Table '{}' not found", j.table))?
                .iter()
                .filter(|r| Self::is_visible(r))
                .cloned()
                .collect();
            let tbl = j.table.clone();
            let mut out = Vec::new();
            match j.join_type {
                JoinType::Inner => {
                    for left in &current {
                        for right in &right_rows {
                            let mut merged = left.clone();
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            if Self::eval_condexpr(&merged, &j.on_expr) { out.push(merged); }
                        }
                    }
                }
                JoinType::Left => {
                    let right_schema_cols: Vec<String> = s.catalog.get_table(&j.table)
                        .map(|s| s.columns.iter().map(|c| c.name.clone()).collect())
                        .unwrap_or_default();
                    for left in &current {
                        let mut matched = false;
                        for right in &right_rows {
                            let mut merged = left.clone();
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            if Self::eval_condexpr(&merged, &j.on_expr) { out.push(merged); matched = true; }
                        }
                        if !matched {
                            let mut merged = left.clone();
                            for col in &right_schema_cols {
                                merged.insert(format!("{}.{}", tbl, col), NULL_VALUE.to_string());
                            }
                            out.push(merged);
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
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            if Self::eval_condexpr(&merged, &j.on_expr) { out.push(merged.clone()); matched = true; }
                        }
                        if !matched {
                            let mut merged = Row::new();
                            for col in &left_cols { merged.insert(col.clone(), NULL_VALUE.to_string()); }
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            out.push(merged);
                        }
                    }
                }
            }
            current = out;
        }

        // Apply WHERE filter
        let matched: Vec<Row> = current.into_iter()
            .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
            .collect();

        // Determine which tables are actually targeted by assignments
        let mut target_tables: Vec<String> = tables.clone();
        for j in &joins { target_tables.push(j.table.clone()); }

        // Resolve a bare/alias table name to the qualified name in target_tables
        let resolve_tbl = |name: &str| -> String {
            let suffix = format!(".{}", name);
            target_tables.iter()
                .find(|t| t.as_str() == name || t.ends_with(&suffix))
                .cloned()
                .unwrap_or_else(|| name.to_string())
        };

        // Build per-table, per-PK assignment map: table → { pk_val → { col → val } }
        // Use a HashSet to avoid applying the same (pk, col) update more than once
        // (a row may appear in multiple cross-join pairs, but its own values are evaluated correctly)
        let mut total_count = 0usize;

        // Collect unique target tables from assignments
        let mut assignment_tables: Vec<String> = Vec::new();
        for (col_expr, _) in &assignments {
            let tbl = if let Some(dot) = col_expr.find('.') {
                resolve_tbl(&col_expr[..dot])
            } else {
                first_table.clone()
            };
            if !assignment_tables.contains(&tbl) { assignment_tables.push(tbl); }
        }

        for tgt in &assignment_tables {
            let pk_col = s.catalog.get_table(tgt)
                .ok_or(format!("Table '{}' not found", tgt))?
                .columns.iter()
                .find(|c| c.primary_key)
                .map(|c| c.name.clone())
                .unwrap_or_else(|| "id".to_string());

            let pk_prefix = format!("{}.", tgt);

            // Build { pk → { col → val } } from matched rows — each pk deduplicated
            let mut pk_updates: HashMap<String, HashMap<String, String>> = HashMap::new();
            for merged_row in &matched {
                let pk_val = merged_row.get(&format!("{}{}", pk_prefix, pk_col))
                    .or_else(|| merged_row.get(&pk_col))
                    .cloned()
                    .unwrap_or_default();
                if pk_val.is_empty() { continue; }
                let entry = pk_updates.entry(pk_val).or_default();
                for (col_expr, rhs_expr) in &assignments {
                    let (tbl_name, bare_col) = if let Some(dot) = col_expr.find('.') {
                        (resolve_tbl(&col_expr[..dot]), col_expr[dot+1..].to_string())
                    } else {
                        (first_table.clone(), col_expr.clone())
                    };
                    if &tbl_name != tgt { continue; }
                    let new_val = Self::eval_arith(merged_row, rhs_expr);
                    entry.insert(bare_col, new_val);
                }
            }

            let rows = s.tables.get_mut(tgt)
                .ok_or(format!("Table '{}' not found", tgt))?;

            for row in rows.iter_mut() {
                let row_pk = row.get(&pk_col).cloned().unwrap_or_default();
                if let Some(col_vals) = pk_updates.get(&row_pk) {
                    for (col, val) in col_vals {
                        row.insert(col.clone(), val.clone());
                    }
                    total_count += 1;
                }
            }

            let rows_clone = s.tables.get(tgt).unwrap().clone();
            if let Some(index) = s.indexes.get_mut(tgt) {
                *index = BPlusTree::new();
                for row in &rows_clone {
                    let k = row.get(&pk_col).cloned().unwrap_or_default();
                    let val_json = serde_json::to_string(row).unwrap();
                    index.insert(k, val_json);
                }
            }
            self.rebuild_secondary_indexes(s, tgt, &rows_clone);
            let comp_keys: Vec<String> = s.composite_indexes.iter()
                .filter(|(_, ci)| ci.table == *tgt)
                .map(|(k, _)| k.clone())
                .collect();
            for k in comp_keys {
                if let Some(ci) = s.composite_indexes.get_mut(&k) {
                    ci.rebuild(&rows_clone);
                }
            }
            s.buffer_pool.write_page(tgt, rows_clone);
            s.buffer_pool.flush_page(tgt, &s.disk);
        }

        self.maybe_auto_checkpoint(s);
        Ok(format!("{} row(s) updated.", total_count))
    }

    fn exec_multi_delete(
        &mut self,
        s: &mut SharedDatabase,
        delete_tables: Vec<String>,
        from_table: String,
        joins: Vec<Join>,
        condition: Option<CondExpr>,
    ) -> Result<String, String> {
        // Build joined rows starting from from_table
        let base_rows: Vec<Row> = s.tables.get(&from_table)
            .ok_or(format!("Table '{}' not found", from_table))?
            .iter()
            .filter(|r| Self::is_visible(r))
            .map(|r| {
                let mut prefixed = Row::new();
                for (k, v) in r.iter() {
                    prefixed.insert(format!("{}.{}", from_table, k), v.clone());
                    prefixed.entry(k.clone()).or_insert_with(|| v.clone());
                }
                prefixed
            })
            .collect();

        let mut current = base_rows;
        for j in &joins {
            let right_rows: Vec<Row> = s.tables.get(&j.table)
                .ok_or(format!("Table '{}' not found", j.table))?
                .iter()
                .filter(|r| Self::is_visible(r))
                .cloned()
                .collect();
            let tbl = j.table.clone();
            let mut out = Vec::new();
            match j.join_type {
                JoinType::Inner => {
                    for left in &current {
                        for right in &right_rows {
                            let mut merged = left.clone();
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            if Self::eval_condexpr(&merged, &j.on_expr) { out.push(merged); }
                        }
                    }
                }
                JoinType::Left => {
                    let right_schema_cols: Vec<String> = s.catalog.get_table(&j.table)
                        .map(|s| s.columns.iter().map(|c| c.name.clone()).collect())
                        .unwrap_or_default();
                    for left in &current {
                        let mut matched = false;
                        for right in &right_rows {
                            let mut merged = left.clone();
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            if Self::eval_condexpr(&merged, &j.on_expr) { out.push(merged); matched = true; }
                        }
                        if !matched {
                            let mut merged = left.clone();
                            for col in &right_schema_cols {
                                merged.insert(format!("{}.{}", tbl, col), NULL_VALUE.to_string());
                            }
                            out.push(merged);
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
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            if Self::eval_condexpr(&merged, &j.on_expr) { out.push(merged); matched = true; }
                        }
                        if !matched {
                            let mut merged = Row::new();
                            for col in &left_cols { merged.insert(col.clone(), NULL_VALUE.to_string()); }
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            out.push(merged);
                        }
                    }
                }
            }
            current = out;
        }

        // Apply WHERE
        let matched: Vec<Row> = current.into_iter()
            .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
            .collect();

        let mut total_count = 0usize;

        for tgt in &delete_tables {
            let pk_col = s.catalog.get_table(tgt)
                .ok_or(format!("Table '{}' not found", tgt))?
                .columns.iter()
                .find(|c| c.primary_key)
                .map(|c| c.name.clone())
                .unwrap_or_else(|| "id".to_string());

            let pk_prefix = format!("{}.", tgt);
            let target_pks: std::collections::HashSet<String> = matched.iter()
                .filter_map(|r| r.get(&format!("{}{}", pk_prefix, pk_col))
                    .or_else(|| r.get(&pk_col)))
                .cloned()
                .collect();

            let rows = s.tables.get_mut(tgt)
                .ok_or(format!("Table '{}' not found", tgt))?;

            let before = rows.iter().filter(|r| Self::is_visible(r)).count();
            rows.retain(|r| !Self::is_visible(r) || !target_pks.contains(r.get(&pk_col).unwrap_or(&String::new())));
            let after = rows.iter().filter(|r| Self::is_visible(r)).count();
            total_count += before - after;

            let rows_clone = s.tables.get(tgt).unwrap().clone();
            if let Some(index) = s.indexes.get_mut(tgt) {
                *index = BPlusTree::new();
                for row in &rows_clone {
                    let k = row.get(&pk_col).cloned().unwrap_or_default();
                    let val_json = serde_json::to_string(row).unwrap();
                    index.insert(k, val_json);
                }
            }
            let comp_keys: Vec<String> = s.composite_indexes.iter()
                .filter(|(_, ci)| ci.table == *tgt)
                .map(|(k, _)| k.clone())
                .collect();
            for k in comp_keys {
                if let Some(ci) = s.composite_indexes.get_mut(&k) {
                    ci.rebuild(&rows_clone);
                }
            }
            s.buffer_pool.write_page(tgt, rows_clone);
            s.buffer_pool.flush_page(tgt, &s.disk);
        }

        self.maybe_auto_checkpoint(s);
        Ok(format!("{} row(s) deleted.", total_count))
    }

    fn matches_condition_with_subquery(&mut self, s: &mut SharedDatabase, row: &Row, condition: &Option<CondExpr>) -> bool {
        match condition {
            None => true,
            Some(expr) => self.eval_condexpr_with_subquery(s, row, expr),
        }
    }

    fn eval_condexpr_with_subquery(&mut self, s: &mut SharedDatabase, row: &Row, expr: &CondExpr) -> bool {
        match expr {
            CondExpr::And(l, r) =>
                self.eval_condexpr_with_subquery(s, row, l) && self.eval_condexpr_with_subquery(s, row, r),
            CondExpr::Or(l, r) =>
                self.eval_condexpr_with_subquery(s, row, l) || self.eval_condexpr_with_subquery(s, row, r),
            CondExpr::Not(inner) => !self.eval_condexpr_with_subquery(s, row, inner),
            CondExpr::Leaf(cond) => self.eval_single_with_subquery(s, row, cond),
        }
    }

    fn eval_single_with_subquery(&mut self, s: &mut SharedDatabase, row: &Row, cond: &Condition) -> bool {
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
                            s, table, subquery, distinct, columns, sub_cond,
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
                        s, table, subquery, distinct, columns.clone(), sub_cond,
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

    fn exec_create_index(&mut self, s: &mut SharedDatabase, index_name: String, table: String, columns: Vec<String>) -> Result<String, String> {
        if !s.tables.contains_key(&table) {
            return Err(format!("Table '{}' not found", table));
        }

        if columns.len() == 1 {
            // 단일 컬럼 → BPlusTree (key → JSON array of rows, supports duplicates)
            let column = &columns[0];
            let mut bucket: HashMap<String, Vec<Row>> = HashMap::new();
            if let Some(rows) = s.tables.get(&table) {
                for row in rows {
                    if let Some(val) = row.get(column) {
                        bucket.entry(val.clone()).or_default().push(row.clone());
                    }
                }
            }
            let mut tree = BPlusTree::new();
            for (key, rows) in bucket {
                tree.insert(key, serde_json::to_string(&rows).unwrap());
            }
            let key = format!("{}_{}", table, index_name);
            s.indexes.insert(key, tree);
            s.index_meta.insert(index_name.clone(), (table.clone(), column.clone()));
            self.persist_index_meta(s);
            Ok(format!("Index '{}' created on '{}'.'{}'.", index_name, table, column))
        } else {
            // 복합 컬럼 → CompositeIndex
            let mut comp = CompositeIndex::new(table.clone(), columns.clone());
            if let Some(rows) = s.tables.get(&table) {
                comp.rebuild(rows);
            }
            s.composite_indexes.insert(index_name.clone(), comp);
            self.persist_index_meta(s);
            Ok(format!("Composite index '{}' created on '{}' ({}).", index_name, table, columns.join(", ")))
        }
    }

    fn exec_drop_index(&mut self, s: &mut SharedDatabase, index_name: String) -> Result<String, String> {
        if let Some((table, _)) = s.index_meta.remove(&index_name) {
            let key = format!("{}_{}", table, index_name);
            s.indexes.remove(&key);
            self.persist_index_meta(s);
            Ok(format!("Index '{}' dropped.", index_name))
        } else if s.composite_indexes.remove(&index_name).is_some() {
            self.persist_index_meta(s);
            Ok(format!("Composite index '{}' dropped.", index_name))
        } else {
            Ok(format!("Index '{}' does not exist, skipped.", index_name))
        }
    }

    fn exec_create_view(&mut self, s: &mut SharedDatabase, name: String, query: Statement) -> Result<String, String> {
        if let Statement::Select { ref table, .. } = query {
            if !s.tables.contains_key(table) {
                return Err(format!("Table '{}' not found", table));
            }
        }
        s.views.insert(name.clone(), query);
        self.persist_views_for_db(s, &self.current_db.clone());
        Ok(format!("View '{}' created.", name))
    }

    fn exec_drop_view(&mut self, s: &mut SharedDatabase, name: String) -> Result<String, String> {
        if s.views.remove(&name).is_some() {
            self.persist_views_for_db(s, &self.current_db.clone());
            Ok(format!("View '{}' dropped.", name))
        } else {
            Ok(format!("View '{}' does not exist, skipped.", name))
        }
    }

    fn persist_views_for_db(&self, s: &SharedDatabase, db: &str) {
        let prefix = format!("{}.", db);
        let db_views: HashMap<String, Statement> = s.views.iter()
            .filter(|(k, _v)| k.starts_with(&prefix))
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect();
        s.disk.save_views(db, &db_views);
    }

    /// 현재 index_meta + composite_indexes를 disk에 저장
    /// 단일 컬럼 보조 인덱스를 rows 기준으로 재빌드한다 (UPDATE 후 stale 방지)
    fn rebuild_secondary_indexes(&mut self, s: &mut SharedDatabase, table: &str, rows: &[Row]) {
        let sec: Vec<(String, String)> = s.index_meta.iter()
            .filter(|(_, (tbl, _))| tbl == table)
            .map(|(name, (_, col))| (name.clone(), col.clone()))
            .collect();
        for (idx_name, col) in sec {
            let mut bucket: HashMap<String, Vec<Row>> = HashMap::new();
            for row in rows {
                if let Some(val) = row.get(&col) {
                    bucket.entry(val.clone()).or_default().push(row.clone());
                }
            }
            let mut tree = BPlusTree::new();
            for (key, bucket_rows) in bucket {
                tree.insert(key, serde_json::to_string(&bucket_rows).unwrap());
            }
            let key = format!("{}_{}", table, idx_name);
            s.indexes.insert(key, tree);
        }
    }

    fn persist_index_meta(&self, s: &SharedDatabase) {
        let mut meta_list: Vec<IndexMeta> = Vec::new();
        for (name, (table, col)) in &s.index_meta {
            meta_list.push(IndexMeta {
                name: name.clone(),
                table: table.clone(),
                columns: vec![col.clone()],
            });
        }
        for (name, comp) in &s.composite_indexes {
            meta_list.push(IndexMeta {
                name: name.clone(),
                table: comp.table.clone(),
                columns: comp.columns.clone(),
            });
        }
        // save_index_meta per-db
        let mut per_db: HashMap<String, Vec<IndexMeta>> = HashMap::new();
        for m in &meta_list {
            let (db, _) = Self::split_key(&m.table);
            per_db.entry(db.to_string()).or_default().push(m.clone());
        }
        for (db, mlist) in &per_db {
            s.disk.save_index_meta(db, mlist);
        }
        if per_db.is_empty() {
            s.disk.save_index_meta(&self.current_db, &[]);
        }
    }

    fn exec_use(&mut self, s: &mut SharedDatabase, database: String) -> Result<String, String> {
        let key = database.to_lowercase();
        if !s.databases.contains(&key) {
            return Err(format!("Unknown database '{}'.", database));
        }
        self.current_db = key.clone();
        Ok(format!("Database changed to '{}'.", key))
    }

    /// Qualify all table references in a statement with the current database.
    fn qualify_stmt(&self, s: &SharedDatabase, stmt: Statement) -> Statement {
        match stmt {
            Statement::Select { table, subquery, columns, distinct, condition, joins, order_by, group_by, having, limit, offset, for_update } =>
                Statement::Select {
                    table: self.qualify_name(table),
                    subquery: subquery.map(|(q, alias)| (Box::new(self.qualify_stmt(s, *q)), alias)),
                    columns,
                    distinct,
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                    joins: joins.into_iter().map(|j| Join {
                        table: self.qualify_name(j.table),
                        on_expr: self.qualify_condexpr(s, j.on_expr),
                        join_type: j.join_type,
                    }).collect(),
                    order_by, group_by,
                    having: having.map(|h| self.qualify_condexpr(s, h)),
                    limit, offset, for_update,
                },
            Statement::Insert { table, columns, values, on_conflict } =>
                Statement::Insert { table: self.qualify_name(table), columns, values, on_conflict },
            Statement::InsertSelect { table, columns, query, on_conflict } =>
                Statement::InsertSelect {
                    table: self.qualify_name(table),
                    columns,
                    query: Box::new(self.qualify_stmt(s, *query)),
                    on_conflict,
                },
            Statement::Update { table, assignments, condition } =>
                Statement::Update {
                    table: self.qualify_name(table),
                    assignments,
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                },
            Statement::Delete { table, condition } =>
                Statement::Delete {
                    table: self.qualify_name(table),
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                },
            Statement::CreateTable { name, columns, if_not_exists, primary_key_columns, check_constraints } => {
                let columns = columns.into_iter().map(|mut col| {
                    if let Some(ref mut fk) = col.foreign_key {
                        fk.ref_table = self.qualify_name(fk.ref_table.clone());
                    }
                    col
                }).collect();
                Statement::CreateTable { name: self.qualify_name(name), columns, if_not_exists, primary_key_columns, check_constraints }
            },
            Statement::DropTable { name, if_exists } =>
                Statement::DropTable { name: self.qualify_name(name), if_exists },
            Statement::TruncateTable { name } =>
                Statement::TruncateTable { name: self.qualify_name(name) },
            Statement::AlterTable { table, action } => {
                let action = match action {
                    AlterAction::RenameTable { to } =>
                        AlterAction::RenameTable { to: self.qualify_name(to) },
                    other => other,
                };
                Statement::AlterTable { table: self.qualify_name(table), action }
            }
            Statement::CreateIndex { index_name, table, columns } =>
                Statement::CreateIndex { index_name, table: self.qualify_name(table), columns },
            Statement::DropIndex { index_name } =>
                Statement::DropIndex { index_name },
            Statement::CreateView { name, query } =>
                Statement::CreateView {
                    name: self.qualify_name(name),
                    query: Box::new(self.qualify_stmt(s, *query)),
                },
            Statement::DropView { name } =>
                Statement::DropView { name: self.qualify_name(name) },
            Statement::Describe { table } =>
                Statement::Describe { table: self.qualify_name(table) },
            Statement::Vacuum { table } =>
                Statement::Vacuum { table: table.map(|t| self.qualify_name(t)) },
            Statement::Union { left, right, all, order_by, limit, offset } =>
                Statement::Union {
                    left:  Box::new(self.qualify_stmt(s, *left)),
                    right: Box::new(self.qualify_stmt(s, *right)),
                    all, order_by, limit, offset,
                },
            Statement::With { ctes, query, recursive } =>
                Statement::With {
                    ctes: ctes.into_iter().map(|(n, q)| (
                        self.qualify_name(n),
                        Box::new(self.qualify_stmt(s, *q))
                    )).collect(),
                    query: Box::new(self.qualify_stmt(s, *query)),
                    recursive,
                },
            Statement::Explain(inner) =>
                Statement::Explain(Box::new(self.qualify_stmt(s, *inner))),
            Statement::MultiUpdate { tables, joins, assignments, condition } =>
                Statement::MultiUpdate {
                    tables: tables.into_iter().map(|t| self.qualify_name(t)).collect(),
                    joins: joins.into_iter().map(|j| Join {
                        table: self.qualify_name(j.table),
                        on_expr: self.qualify_condexpr(s, j.on_expr),
                        join_type: j.join_type,
                    }).collect(),
                    assignments,
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                },
            Statement::MultiDelete { delete_tables, from_table, joins, condition } =>
                Statement::MultiDelete {
                    delete_tables: delete_tables.into_iter().map(|t| self.qualify_name(t)).collect(),
                    from_table: self.qualify_name(from_table),
                    joins: joins.into_iter().map(|j| Join {
                        table: self.qualify_name(j.table),
                        on_expr: self.qualify_condexpr(s, j.on_expr),
                        join_type: j.join_type,
                    }).collect(),
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                },
            // 나머지는 그대로
            other => other,
        }
    }

    fn qualify_condexpr(&self, s: &SharedDatabase, expr: CondExpr) -> CondExpr {
        match expr {
            CondExpr::And(l, r) => CondExpr::And(
                Box::new(self.qualify_condexpr(s, *l)),
                Box::new(self.qualify_condexpr(s, *r)),
            ),
            CondExpr::Or(l, r) => CondExpr::Or(
                Box::new(self.qualify_condexpr(s, *l)),
                Box::new(self.qualify_condexpr(s, *r)),
            ),
            CondExpr::Not(inner) => CondExpr::Not(Box::new(self.qualify_condexpr(s, *inner))),
            CondExpr::Leaf(cond) => CondExpr::Leaf(match cond.value {
                ConditionValue::Subquery(q) => Condition {
                    value: ConditionValue::Subquery(Box::new(self.qualify_stmt(s, *q))),
                    ..cond
                },
                _ => cond,
            }),
        }
    }

    fn exec_show_tables(&self, s: &SharedDatabase) -> Result<String, String> {
        // 현재 DB의 테이블만 표시, 접두사 제거
        let prefix = format!("{}.", self.current_db);
        let mut tables: Vec<String> = s.catalog.tables.keys()
            .filter(|k| k.starts_with(&prefix))
            .map(|k| k[prefix.len()..].to_string())
            .collect();
        if tables.is_empty() {
            return Ok(format!("No tables found in database '{}'.", self.current_db));
        }
        tables.sort();
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

    fn exec_describe(&self, s: &SharedDatabase, table: String) -> Result<String, String> {
        let schema = s.catalog.get_table(&table)
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
                crate::parser::ast::DataType::Double => "DOUBLE".to_string(),
                crate::parser::ast::DataType::Time => "TIME".to_string(),
                crate::parser::ast::DataType::Year => "YEAR".to_string(),
                crate::parser::ast::DataType::Enum(vals) => format!("ENUM({})", vals.iter().map(|v| format!("'{}'", v)).collect::<Vec<_>>().join(",")),
                crate::parser::ast::DataType::Set(vals) => format!("SET({})", vals.iter().map(|v| format!("'{}'", v)).collect::<Vec<_>>().join(",")),
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

    fn exec_show_buffer_pool(&self, s: &SharedDatabase) -> Result<String, String> {
        let mut output = String::new();
        let sep = "+----------------------+---------+";
        output.push_str(&format!("{}\n", sep));
        output.push_str("| 항목                 | 값      |\n");
        output.push_str(&format!("{}\n", sep));
        output.push_str(&format!("| 캐시 사용량          | {:7} |\n", s.buffer_pool.usage()));
        output.push_str(&format!("| 최대 용량            | {:7} |\n", 64));
        output.push_str(&format!("| 캐시 히트            | {:7} |\n", s.buffer_pool.hit_count));
        output.push_str(&format!("| 캐시 미스            | {:7} |\n", s.buffer_pool.miss_count));
        output.push_str(&format!("| 적중률               | {:6.1}% |\n", s.buffer_pool.hit_rate()));
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
    fn exec_vacuum(&mut self, s: &mut SharedDatabase, table: Option<String>) -> Result<String, String> {
        let targets: Vec<String> = match table {
            Some(t) => {
                if !s.tables.contains_key(&t) {
                    return Err(format!("Table '{}' not found", t));
                }
                vec![t]
            }
            None => s.tables.keys().cloned().collect(),
        };

        let mut total_removed = 0usize;
        for t in &targets {
            let rows = s.tables.get_mut(t).unwrap();
            let before = rows.len();
            rows.retain(|r| Self::is_visible(r));
            let removed = before - rows.len();
            total_removed += removed;

            if removed > 0 {
                // 인덱스 재빌드
                let rows_clone = s.tables.get(t).unwrap().clone();
                if let Some(index) = s.indexes.get_mut(t) {
                    *index = BPlusTree::new();
                    for row in &rows_clone {
                        let key = row.values().next().cloned().unwrap_or_default();
                        let val_json = serde_json::to_string(row).unwrap();
                        index.insert(key, val_json);
                    }
                }
                let comp_keys: Vec<String> = s.composite_indexes.iter()
                    .filter(|(_, ci)| ci.table == *t)
                    .map(|(k, _)| k.clone())
                    .collect();
                for k in comp_keys {
                    if let Some(ci) = s.composite_indexes.get_mut(&k) {
                        ci.rebuild(&rows_clone);
                    }
                }
                s.buffer_pool.write_page(t, rows_clone.clone());
                s.buffer_pool.flush_page(t, &s.disk);
            }
        }

        Ok(format!("VACUUM complete. {} dead row(s) removed.", total_removed))
    }

    /// EXPLAIN <SELECT> — 쿼리 실행 계획 출력 (실제 실행 안 함)
    fn exec_explain(&self, s: &SharedDatabase, stmt: Statement) -> Result<String, String> {
        let (table, condition, joins, columns) = match &stmt {
            Statement::Select { table, condition, joins, subquery, columns, .. } => {
                if subquery.is_some() {
                    return Ok("EXPLAIN: Subquery-based SELECT → SUBQUERY SCAN".to_string());
                }
                (table.clone(), condition.clone(), joins.clone(), columns.clone())
            }
            other => return Ok(format!("EXPLAIN: {:?} → not a SELECT", other)),
        };
        let planner = Planner::new(&s.tables, &s.indexes, &s.index_meta, &s.composite_indexes, &s.catalog);
        let plan = planner.plan_covering(&table, &condition, &joins, &columns);
        Ok(planner.explain(&plan))
    }

    /// SHOW LOCKS: 보유 잠금 + wait-for 그래프 + 데드락 이력 출력
    fn exec_show_locks(&self, s: &SharedDatabase) -> Result<String, String> {
        let mut output = String::new();

        // ── 1. 현재 보유 잠금 ──────────────────────────────────────────
        let locks = s.lock_mgr.lock_rows();
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
        let wait_for = s.lock_mgr.wait_for_rows();
        if !wait_for.is_empty() {
            output.push_str("\nWait-for graph:\n");
            for (waiter, blocker) in &wait_for {
                output.push_str(&format!("  txn {} waits for txn {}\n", waiter, blocker));
            }
        }

        // ── 3. 데드락 이력 ────────────────────────────────────────────
        let history = s.lock_mgr.deadlock_history();
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
    fn sort_by_pk(&mut self, s: &mut SharedDatabase, table: &str) {
        let pk_col = s.catalog.get_table(table)
            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()));
        if let Some(pk) = pk_col {
            if let Some(rows) = s.tables.get_mut(table) {
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
    fn exec_checkpoint(&mut self, s: &mut SharedDatabase) -> Result<String, String> {
        let dirty_before = s.buffer_pool.usage();
        s.buffer_pool.flush_all(&s.disk);
        self.txn.do_checkpoint();
        Ok(format!(
            "Checkpoint completed. {} dirty page(s) flushed.",
            dirty_before
        ))
    }

    /// 자동 체크포인트: WAL 크기가 임계값을 초과하면 체크포인트를 수행한다.
    /// 활성 트랜잭션 중에도 중간 체크포인트를 찍어 복구 범위를 줄인다.
    fn maybe_auto_checkpoint(&mut self, s: &mut SharedDatabase) {
        if self.txn.needs_auto_checkpoint() {
            s.buffer_pool.flush_all(&s.disk);
            self.txn.do_checkpoint();
            eprintln!("[AutoCheckpoint] WAL 임계값 초과 → 체크포인트 실행");
        }
    }

    fn recover_from_wal(&mut self) {
        let arc = Arc::clone(&self.shared);
        let mut s = arc.write().unwrap();
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
            // 미완료 트랜잭션 → Undo Log로 디스크 상태 복원 후 WAL 삭제
            if self.txn.has_undo_log_file() {
                let undo_entries = self.txn.read_undo_log_file();
                eprintln!("[Recovery] 미완료 트랜잭션 감지 → Undo Log {} 개 엔트리 적용", undo_entries.len());
                // 역순으로 적용 (마지막 변경 → 첫 번째 변경 순서로 복원)
                for entry in undo_entries.iter().rev() {
                    match entry.operation.as_str() {
                        "INSERT" => {
                            // INSERT 취소: 삽입된 행 삭제
                            let pk_col = s.catalog.get_table(&entry.table)
                                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                .unwrap_or_else(|| "id".to_string());
                            if let Some(rows) = s.tables.get_mut(&entry.table) {
                                rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                                let snap = rows.clone();
                                s.disk.save_table(&entry.table, &snap);
                                eprintln!("[Recovery] UNDO INSERT: {} key={}", entry.table, entry.key);
                            }
                        }
                        "UPDATE" => {
                            // UPDATE 취소: 이전 데이터로 복원
                            if let Some(old_json) = &entry.old_data {
                                if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                                    let pk_col = s.catalog.get_table(&entry.table)
                                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                                        .unwrap_or_else(|| "id".to_string());
                                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                                        for row in rows.iter_mut() {
                                            if row.get(&pk_col) == Some(&entry.key) {
                                                *row = old_row.clone();
                                                break;
                                            }
                                        }
                                        let snap = rows.clone();
                                        s.disk.save_table(&entry.table, &snap);
                                        eprintln!("[Recovery] UNDO UPDATE: {} key={}", entry.table, entry.key);
                                    }
                                }
                            }
                        }
                        "DELETE" => {
                            // DELETE 취소: 삭제된 행 재삽입
                            if let Some(old_json) = &entry.old_data {
                                if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                                    if let Some(rows) = s.tables.get_mut(&entry.table) {
                                        rows.push(old_row);
                                        let snap = rows.clone();
                                        s.disk.save_table(&entry.table, &snap);
                                        eprintln!("[Recovery] UNDO DELETE: {} key={}", entry.table, entry.key);
                                    }
                                }
                            }
                        }
                        _ => {}
                    }
                }
                self.txn.clear_undo_log_file();
            } else {
                eprintln!("[Recovery] 미완료 트랜잭션 감지 (Undo Log 없음) → WAL 삭제");
            }
            self.txn.wal_clear();
            return;
        }

        // COMMIT된 트랜잭션 replay (체크포인트 이후 레코드만)
        eprintln!("[Recovery] WAL replay 시작 ({} 레코드, start_idx={})", replay_records.len(), start_idx);
        for record in replay_records {
            match record.op {
                crate::transaction::wal::WalOp::Insert => {
                    if let Ok(row) = serde_json::from_str::<Row>(&record.data) {
                        let table = &record.table_name;
                        // catalog 조회를 get_mut 이전에 수행해 borrow 충돌 방지
                        let pk_col = s.catalog.get_table(table)
                            .and_then(|sch| sch.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                            .unwrap_or_else(|| "id".to_string());
                        if let Some(rows) = s.tables.get_mut(table) {
                            let key = row.get(&pk_col).cloned().unwrap_or_default();
                            let exists = rows.iter().any(|r| r.get(&pk_col).map(|v| v == &key).unwrap_or(false));
                            if !exists {
                                rows.push(row.clone());
                                let val_json = serde_json::to_string(&row).unwrap();
                                if let Some(index) = s.indexes.get_mut(table) {
                                    index.insert(key, val_json);
                                }
                                s.disk.save_table(table, s.tables.get(table).unwrap());
                                eprintln!("[Recovery] INSERT replay: {}", table);
                            }
                        }
                    }
                }
                crate::transaction::wal::WalOp::Update => {
                    if let Ok(new_row) = serde_json::from_str::<Row>(&record.data) {
                        let table = &record.table_name;
                        let pk_col = s.catalog.get_table(table)
                            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                            .unwrap_or_else(|| "id".to_string());
                        if let Some(rows) = s.tables.get_mut(table) {
                            for row in rows.iter_mut() {
                                if row.get(&pk_col) == new_row.get(&pk_col) {
                                    *row = new_row.clone();
                                    break;
                                }
                            }
                        }
                        s.disk.save_table(table, s.tables.get(table).unwrap());
                        eprintln!("[Recovery] UPDATE replay: {}", table);
                    }
                }
                crate::transaction::wal::WalOp::Delete => {
                    let table = &record.table_name;
                    let pk_col = s.catalog.get_table(table)
                        .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                        .unwrap_or_else(|| "id".to_string());
                    if let Some(rows) = s.tables.get_mut(table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &record.key).unwrap_or(true));
                    }
                    s.disk.save_table(table, s.tables.get(table).unwrap());
                    eprintln!("[Recovery] DELETE replay: {}", table);
                }
                _ => {}
            }
        }

        // Replay 완료 후 WAL 삭제
        self.txn.wal_clear();
        eprintln!("[Recovery] WAL replay 완료 → WAL 삭제");
    }

    // ── 사용자 관리 ──────────────────────────────────────────────────────────

    fn exec_create_user(
        &mut self,
        s: &mut SharedDatabase,
        user: String,
        host: String,
        password: Option<String>,
        if_not_exists: bool,
    ) -> Result<String, String> {
        let exists = s.users.iter().any(|u| u.user == user && u.host == host);
        if exists {
            if if_not_exists {
                return Ok(format!("User '{}@{}' already exists (IF NOT EXISTS — skipped).", user, host));
            }
            return Err(format!("User '{}@{}' already exists.", user, host));
        }
        s.users.push(UserRecord {
            user: user.clone(),
            host: host.clone(),
            password_hash: password,
        });
        s.disk.save_users(&s.users);
        Ok(format!("User '{}@{}' created.", user, host))
    }

    fn exec_drop_user(
        &mut self,
        s: &mut SharedDatabase,
        user: String,
        host: String,
        if_exists: bool,
    ) -> Result<String, String> {
        let before = s.users.len();
        s.users.retain(|u| !(u.user == user && u.host == host));
        if s.users.len() == before {
            if if_exists {
                return Ok(format!("User '{}@{}' does not exist (IF EXISTS — skipped).", user, host));
            }
            return Err(format!("User '{}@{}' does not exist.", user, host));
        }
        // Also remove their grants
        s.grants.retain(|g| !(g.user == user && g.host == host));
        s.disk.save_users(&s.users);
        s.disk.save_grants(&s.grants);
        Ok(format!("User '{}@{}' dropped.", user, host))
    }

    fn exec_grant(
        &mut self,
        s: &mut SharedDatabase,
        privileges: Vec<String>,
        object_type: String,
        object: String,
        user: String,
        host: String,
        with_grant_option: bool,
    ) -> Result<String, String> {
        // Find existing grant record for this user/object
        if let Some(existing) = s.grants.iter_mut().find(|g| {
            g.user == user && g.host == host && g.object == object && g.object_type == object_type
        }) {
            for priv_name in &privileges {
                if !existing.privileges.contains(priv_name) {
                    existing.privileges.push(priv_name.clone());
                }
            }
            if with_grant_option {
                existing.with_grant_option = true;
            }
        } else {
            s.grants.push(GrantRecord {
                user: user.clone(),
                host: host.clone(),
                object_type,
                object: object.clone(),
                privileges: privileges.clone(),
                with_grant_option,
            });
        }
        s.disk.save_grants(&s.grants);
        Ok(format!("Granted {} on {} to '{}@{}'.", privileges.join(", "), object, user, host))
    }

    fn exec_revoke(
        &mut self,
        s: &mut SharedDatabase,
        privileges: Vec<String>,
        object_type: String,
        object: String,
        user: String,
        host: String,
    ) -> Result<String, String> {
        let mut changed = false;
        for g in s.grants.iter_mut() {
            if g.user == user && g.host == host && g.object == object && g.object_type == object_type {
                let before = g.privileges.len();
                if privileges.contains(&"ALL PRIVILEGES".to_string()) {
                    g.privileges.clear();
                } else {
                    g.privileges.retain(|p| !privileges.contains(p));
                }
                if g.privileges.len() != before { changed = true; }
            }
        }
        // Remove empty grant records
        s.grants.retain(|g| !g.privileges.is_empty());
        s.disk.save_grants(&s.grants);
        if changed {
            Ok(format!("Revoked {} on {} from '{}@{}'.", privileges.join(", "), object, user, host))
        } else {
            Ok(format!("No matching grants found for '{}@{}'.", user, host))
        }
    }

    fn exec_show_grants(&self, s: &SharedDatabase, user: Option<String>, host: Option<String>) -> Result<String, String> {
        let filter_user = user.as_deref().unwrap_or("");
        let filter_host = host.as_deref().unwrap_or("");
        let show_all = user.is_none();

        let mut lines: Vec<String> = Vec::new();
        for g in &s.grants {
            if show_all || (g.user == filter_user && g.host == filter_host) {
                let priv_str = g.privileges.join(", ");
                let grant_opt = if g.with_grant_option { " WITH GRANT OPTION" } else { "" };
                lines.push(format!(
                    "GRANT {} ON {} TO '{}'@'{}'{};",
                    priv_str, g.object, g.user, g.host, grant_opt
                ));
            }
        }

        if lines.is_empty() {
            return Ok("No grants found.".to_string());
        }

        let max_len = lines.iter().map(|l| l.len()).max().unwrap_or(0);
        let sep = format!("+{}+", "-".repeat(max_len + 2));
        let header = format!("| {:<width$} |", "Grants", width = max_len);
        let mut out = format!("{}\n{}\n{}\n", sep, header, sep);
        for line in &lines {
            out.push_str(&format!("| {:<width$} |\n", line, width = max_len));
        }
        out.push_str(&sep);
        Ok(out)
    }

    fn exec_show_databases(&self, s: &SharedDatabase) -> Result<String, String> {
        let mut dbs: Vec<String> = s.databases.iter().cloned().collect();
        dbs.sort();
        if dbs.is_empty() {
            return Ok("No databases.".to_string());
        }
        let max_len = dbs.iter().map(|d| d.len()).max().unwrap_or(8).max(8);
        let sep = format!("+{}+", "-".repeat(max_len + 2));
        let header = format!("| {:<width$} |", "Database", width = max_len);
        let mut out = format!("{}\n{}\n{}\n", sep, header, sep);
        for db in &dbs {
            out.push_str(&format!("| {:<width$} |\n", db, width = max_len));
        }
        out.push_str(&sep);
        Ok(out)
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
        ArithExpr::Func(name, args) => {
            let a: Vec<String> = args.iter().map(arith_to_str).collect();
            format!("{}({})", name, a.join(","))
        }
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
