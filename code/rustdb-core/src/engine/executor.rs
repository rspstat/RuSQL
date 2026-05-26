// src/engine/executor.rs

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, RwLock};

thread_local! {
    static USER_FUNCTIONS: RefCell<HashMap<String, (Vec<String>, String)>> = RefCell::new(HashMap::new());
}
use sha2::{Sha256, Digest};
use chrono;
use serde::{Serialize, Deserialize};
use crate::transaction::txn_manager::TransactionManager;
use crate::transaction::group_commit::GroupCommitCoordinator;
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

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RoleRecord {
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RoleGrant {
    pub role: String,
    pub user: String,
    pub host: String,
    pub with_admin_option: bool,
}

pub type Row = HashMap<String, String>;
pub const NULL_VALUE: &str = "NULL";

#[derive(Debug, Clone, Default)]
pub struct ColumnStats {
    pub distinct_count: usize,
    pub null_count: usize,
    pub min_val: Option<String>,
    pub max_val: Option<String>,
}

#[derive(Debug, Clone, Default)]
pub struct TableStats {
    pub total_rows: usize,
    pub columns: HashMap<String, ColumnStats>,
}

pub struct SharedDatabase {
    pub catalog: Catalog,
    pub tables: HashMap<String, Vec<Row>>,
    pub indexes: HashMap<String, BPlusTree>,
    pub index_meta: HashMap<String, (String, String)>,
    pub composite_indexes: HashMap<String, CompositeIndex>,
    pub views: HashMap<String, Statement>,
    pub view_raw_sql: HashMap<String, String>,
    pub buffer_pool: BufferPool,
    pub disk: DiskManager,
    pub lock_mgr: LockManager,
    pub databases: HashSet<String>,
    pub users: Vec<UserRecord>,
    pub grants: Vec<GrantRecord>,
    pub roles: Vec<RoleRecord>,
    pub role_grants: Vec<RoleGrant>,
    pub synonyms: HashMap<String, String>,
    pub group_commit_coord: Arc<GroupCommitCoordinator>,
    pub data_dir: String,
    pub table_stats: HashMap<String, TableStats>,
    pub procedures: HashMap<String, (Vec<(String, String, String)>, Vec<Statement>)>,
    /// key = trigger name, value = (table, timing, event, body)
    pub triggers: HashMap<String, (String, String, String, Vec<Statement>)>,
    /// AUTO VACUUM: 커밋된 DML 누적 카운터 (임계값 초과 시 자동 VACUUM)
    pub dml_since_vacuum: usize,
    /// User-defined scalar functions: name → (params: Vec<name>, body_expr: String)
    pub user_functions: HashMap<String, (Vec<String>, String)>,
}

/// SHA-256 해시 (hex 문자열 반환)
fn hash_password(password: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(password.as_bytes());
    hasher.finalize().iter().map(|b| format!("{:02x}", b)).collect()
}

impl SharedDatabase {
    /// TCP 인증: users가 비어있으면 open 모드(항상 통과).
    /// 저장값이 SHA-256 hex(64자)이면 해시 비교, 아니면 레거시 평문 비교 후 자동 마이그레이션.
    pub fn validate_credentials(&self, user: &str, password: &str) -> bool {
        if self.users.is_empty() { return true; }
        self.users.iter().any(|u| {
            u.user == user
                && match &u.password_hash {
                    None       => password.is_empty(),
                    Some(hash) => {
                        let hashed = hash_password(password);
                        hash == &hashed || hash == password // 해시 또는 레거시 평문
                    }
                }
        })
    }

    /// users가 비어있으면 root/root 계정을 자동 생성하고 true 반환.
    pub fn ensure_default_user(&mut self) -> bool {
        if self.users.is_empty() {
            self.users.push(UserRecord {
                user: "root".into(),
                host: "%".into(),
                password_hash: Some(hash_password("root")),
            });
            self.disk.save_users(&self.users);
            true
        } else {
            false
        }
    }
}

/// 저장 프로시저 내 제어 흐름 신호 (LEAVE / ITERATE)
#[derive(Debug, Clone)]
enum ProcSignal {
    Leave(Option<String>),
    Iterate(Option<String>),
}

pub struct Executor {
    pub shared: Arc<RwLock<SharedDatabase>>,
    pub txn: TransactionManager,
    pub current_db: String,
    /// 트랜잭션 중 DML 변경분을 보관하는 세션 로컬 테이블 버퍼.
    /// COMMIT 시 s.tables에 적용되며, ROLLBACK 시 폐기된다.
    pub session_tables: HashMap<String, Vec<Row>>,
    /// 저장 프로시저 로컬 변수 (DECLARE / SET)
    pub proc_vars: HashMap<String, String>,
    /// 세션 사용자 변수 (@var)
    pub user_vars: HashMap<String, String>,
    /// PREPARE로 등록된 쿼리 문자열
    pub prepared_stmts: HashMap<String, String>,
    /// LEAVE / ITERATE 제어 흐름 신호
    proc_signal: Option<ProcSignal>,
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

    /// Like qualify_name but also resolves synonyms first
    fn qualify_name_with_synonyms(&self, s: &SharedDatabase, name: String) -> String {
        let resolved = s.synonyms.get(&name).cloned().unwrap_or(name);
        if resolved.contains('.') { resolved } else { format!("{}.{}", self.current_db, resolved) }
    }

    fn strip_db_prefix(name: &str) -> &str {
        name.split('.').last().unwrap_or(name)
    }

    fn merge_conditions(a: Option<CondExpr>, b: Option<CondExpr>) -> Option<CondExpr> {
        match (a, b) {
            (None, b) => b,
            (a, None) => a,
            (Some(a), Some(b)) => Some(CondExpr::And(Box::new(a), Box::new(b))),
        }
    }

    /// Strip current_db prefix for display
    fn display_name<'a>(&self, key: &'a str) -> &'a str {
        let prefix = format!("{}.", self.current_db);
        key.strip_prefix(prefix.as_str()).unwrap_or(key)
    }

    pub fn new() -> Self {
        Self::new_with_options("data", 64)
    }

    pub fn new_with_buffer_pool_size(capacity: usize) -> Self {
        Self::new_with_options("data", capacity)
    }

    pub fn new_with_dir(dir: &str) -> Self {
        Self::new_with_options(dir, 64)
    }

    pub fn new_with_options(dir: &str, buffer_pool_capacity: usize) -> Self {
        let disk = DiskManager::new_with_dir(dir);
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
        let mut view_raw_sql: HashMap<String, String> = HashMap::new();
        for db in &databases {
            let db_views = disk.load_views(db);
            for (k, v) in db_views {
                let qualified_k = if k.contains('.') { k } else { format!("{}.{}", db, k) };
                views.insert(qualified_k, v);
            }
            let db_view_sql = disk.load_view_raw_sql(db);
            for (k, v) in db_view_sql {
                let qualified_k = if k.contains('.') { k } else { format!("{}.{}", db, k) };
                view_raw_sql.insert(qualified_k, v);
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
        let roles: Vec<RoleRecord> = disk.load_roles();
        let role_grants: Vec<RoleGrant> = disk.load_role_grants();
        let synonyms: HashMap<String, String> = disk.load_synonyms();

        let current_db = databases.iter().min().cloned().unwrap_or_else(|| "rustdb".to_string());
        let mut executor = Executor {
            shared: Arc::new(RwLock::new(SharedDatabase {
                catalog,
                tables,
                indexes,
                index_meta,
                composite_indexes,
                views,
                view_raw_sql,
                buffer_pool: BufferPool::with_capacity(buffer_pool_capacity),
                disk,
                lock_mgr: LockManager::new(),
                databases,
                users,
                grants,
                roles,
                role_grants,
                synonyms,
                group_commit_coord: Arc::new(GroupCommitCoordinator::new_with_wal_path(
                    format!("{}/rustdb.wal", dir)
                )),
                data_dir: dir.to_string(),
                table_stats: HashMap::new(),
                procedures: HashMap::new(),
                triggers: HashMap::new(),
                dml_since_vacuum: 0,
                user_functions: HashMap::new(),
            })),
            txn: TransactionManager::new_with_dir(dir),
            current_db,
            session_tables: HashMap::new(),
            proc_vars: HashMap::new(),
            user_vars: HashMap::new(),
            prepared_stmts: HashMap::new(),
            proc_signal: None,
        };

        // WAL Crash Recovery
        executor.recover_from_wal();
        executor
    }

    pub fn new_session(shared: Arc<RwLock<SharedDatabase>>) -> Self {
        let (current_db, data_dir) = {
            let s = shared.read().unwrap();
            let db = s.databases.iter().min().cloned().unwrap_or_else(|| "rustdb".to_string());
            let dir = s.data_dir.clone();
            (db, dir)
        };
        Executor {
            shared,
            txn: TransactionManager::new_with_dir(&data_dir),
            current_db,
            session_tables: HashMap::new(),
            proc_vars: HashMap::new(),
            user_vars: HashMap::new(),
            prepared_stmts: HashMap::new(),
            proc_signal: None,
        }
    }

    pub fn get_shared(&self) -> Arc<RwLock<SharedDatabase>> {
        Arc::clone(&self.shared)
    }

    pub fn execute(&mut self, stmt: Statement) -> Result<String, String> {
        // COMMIT은 두 단계로 분리: Phase1(락 보유 중) → fsync(락 해제 후) → finalize
        if let Statement::Commit = stmt {
            return self.execute_commit_grouped();
        }
        let arc = Arc::clone(&self.shared);
        let mut s = arc.write().unwrap();
        self.execute_with_s(&mut s, stmt)
    }

    /// Group Commit 경로:
    /// 1) SharedDatabase 락 보유 중: 검증 + dirty page 플러시 + COMMIT 레코드 기록(fsync 없음)
    /// 2) 락 해제 후: GroupCommitCoordinator로 단일 fsync (여러 세션이 공유)
    /// 3) WAL 및 트랜잭션 상태 정리
    fn execute_commit_grouped(&mut self) -> Result<String, String> {
        // Phase 1 — SharedDatabase 락 보유
        let coord = {
            let arc = Arc::clone(&self.shared);
            let mut s = arc.write().unwrap();
            self.exec_commit_phase1(&mut s)?;
            Arc::clone(&s.group_commit_coord)
        }; // SharedDatabase 락 해제

        // Phase 2 — 락 없이 Group fsync
        coord.sync_commit();

        // Phase 3 — WAL 파일 삭제 + 트랜잭션 상태 초기화
        self.txn.commit_finalize();
        {
            let arc = Arc::clone(&self.shared);
            let mut s = arc.write().unwrap();
            Self::maybe_auto_vacuum(&mut s);
        }

        Ok("Transaction committed.".to_string())
    }

    /// COMMIT Phase 1: SERIALIZABLE 검증, session_tables → s.tables 반영, buffer_pool 갱신.
    fn exec_commit_phase1(&mut self, s: &mut SharedDatabase) -> Result<(), String> {
        if let Err(e) = self.txn.validate_serializable(&s.tables) {
            self.apply_rollback(s);
            return Err(format!("{} (auto-rolled back)", e));
        }

        // session_tables(트랜잭션 working copy)를 s.tables와 buffer_pool에 적용
        let session_data: Vec<(String, Vec<Row>)> = self.session_tables.drain().collect();
        for (table, rows) in session_data {
            s.tables.insert(table.clone(), rows.clone());
            s.buffer_pool.write_page(&table, rows);
            s.buffer_pool.flush_page(&table, &s.disk);
        }

        let txn_id = self.txn.current_txn_id();
        self.txn.commit_write_record()?;
        s.lock_mgr.release(txn_id);

        Ok(())
    }

    fn execute_with_s(&mut self, s: &mut SharedDatabase, stmt: Statement) -> Result<String, String> {
        // Sync user functions into thread_local for eval_arith access
        USER_FUNCTIONS.with(|uf| *uf.borrow_mut() = s.user_functions.clone());

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
        // ROLE 관리
        if let Statement::CreateRole { name } = stmt {
            return self.exec_create_role(s, name);
        }
        if let Statement::DropRole { name, if_exists } = stmt {
            return self.exec_drop_role(s, name, if_exists);
        }
        if let Statement::GrantRole { role, user, host, with_admin_option } = stmt {
            return self.exec_grant_role(s, role, user, host, with_admin_option);
        }
        if let Statement::RevokeRole { role, user, host } = stmt {
            return self.exec_revoke_role(s, role, user, host);
        }
        if let Statement::ShowRoles = stmt {
            return self.exec_show_roles(s);
        }
        // SYNONYM 관리
        if let Statement::CreateSynonym { name, target, or_replace } = stmt {
            return self.exec_create_synonym(s, name, target, or_replace);
        }
        if let Statement::DropSynonym { name, if_exists } = stmt {
            return self.exec_drop_synonym(s, name, if_exists);
        }
        if let Statement::ShowSynonyms = stmt {
            return self.exec_show_synonyms(s);
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
            Statement::Insert { table, columns, values, on_conflict, returning } => self.exec_insert(s, table, columns, values, on_conflict, returning),
            Statement::InsertSelect { table, columns, query, on_conflict, returning } => self.exec_insert_select(s, table, columns, *query, on_conflict, returning),
            Statement::Select { table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update, for_share } => {
                self.exec_select(s, table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update, for_share)
            }
            Statement::Update { table, assignments, condition, returning } => {
                self.exec_update(s, table, assignments, condition, returning)
            }
            Statement::Delete { table, condition, returning } => self.exec_delete(s, table, condition, returning),
            Statement::AlterTable { table, action }  => self.exec_alter(s, table, action),
            Statement::CreateIndex { index_name, table, columns } => {
                self.exec_create_index(s, index_name, table, columns)
            }
            Statement::DropIndex { index_name } => self.exec_drop_index(s, index_name),
            Statement::CreateView { name, query, raw_sql } => self.exec_create_view(s, name, *query, raw_sql),
            Statement::DropView { name } => self.exec_drop_view(s, name),
            Statement::ShowTables => self.exec_show_tables(s),
            Statement::Describe { table } => self.exec_describe(s, table),
            Statement::ShowBufferPool => self.exec_show_buffer_pool(s),
            Statement::ShowWal        => self.exec_show_wal(),
            Statement::Checkpoint     => self.exec_checkpoint(s),
            Statement::SetIsolationLevel(level) => self.exec_set_isolation_level(level),
            Statement::ShowIsolationLevel       => self.exec_show_isolation_level(),
            Statement::Vacuum { table }         => self.exec_vacuum(s, table),
            Statement::AnalyzeTable { table }   => self.exec_analyze_table(s, table),
            Statement::ShowLocks                => self.exec_show_locks(s),
            Statement::Savepoint { name }       => self.exec_savepoint(name),
            Statement::ReleaseSavepoint { name } => self.exec_release_savepoint(name),
            Statement::RollbackTo { name }      => self.exec_rollback_to(s, name),
            Statement::Explain(inner)           => self.exec_explain(s, *inner),
            Statement::ExplainAnalyze(inner)    => self.exec_explain_analyze(s, *inner),
            Statement::Union { left, right, all, order_by, limit, offset } => self.exec_union(s, *left, *right, all, order_by, limit, offset),
            Statement::Intersect { left, right, all, order_by, limit, offset } => self.exec_intersect(s, *left, *right, all, order_by, limit, offset),
            Statement::Except { left, right, all, order_by, limit, offset } => self.exec_except(s, *left, *right, all, order_by, limit, offset),
            Statement::ShowCreateTable { table } => self.exec_show_create_table(s, table),
            Statement::ShowCreateView { view } => self.exec_show_create_view(s, view),
            Statement::ShowIndex { table } => self.exec_show_index(s, table),
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
            Statement::Merge { target, target_alias, source, source_alias, on,
                               when_matched_update, when_matched_delete, when_matched_delete_cond,
                               when_not_matched_columns, when_not_matched_values } => {
                self.exec_merge(s, target, target_alias, source, source_alias, on,
                                when_matched_update, when_matched_delete, when_matched_delete_cond,
                                when_not_matched_columns, when_not_matched_values)
            }
            Statement::CreateProcedure { name, params, body } => {
                self.exec_create_procedure(s, name, params, body)
            }
            Statement::CallProcedure { name, args } => self.exec_call_procedure(s, name, args),
            Statement::CreateTrigger { name, timing, event, table, body } => {
                self.exec_create_trigger(s, name, timing, event, table, body)
            }
            Statement::DropTrigger { name, if_exists } => self.exec_drop_trigger(s, name, if_exists),
            Statement::DropProcedure { name, if_exists } => self.exec_drop_procedure(s, name, if_exists),
            Statement::Backup { database, output_file } => self.exec_backup(s, database, output_file),
            Statement::ShowProcessList => self.exec_show_processlist(s),
            Statement::CreateFunction { name, params, body } => self.exec_create_function(s, name, params, body),
            Statement::DropFunction { name, if_exists } => self.exec_drop_function(s, name, if_exists),
            // 저장 프로시저 제어문
            Statement::ProcDeclare { name, typ: _, default } => {
                let val = default.unwrap_or_else(|| "NULL".to_string());
                self.proc_vars.insert(name, val);
                Ok(String::new())
            }
            Statement::ProcSet { name, expr } => {
                let val = Self::eval_arith(&self.proc_vars.clone(), &expr);
                self.proc_vars.insert(name, val);
                Ok(String::new())
            }
            Statement::ProcIf { condition, then_body, elseif_branches, else_body } => {
                self.exec_proc_if(s, condition, then_body, elseif_branches, else_body)
            }
            Statement::ProcWhile { label, condition, body } => {
                self.exec_proc_while(s, label, condition, body)
            }
            Statement::ProcLoop { label, body } => {
                self.exec_proc_loop(s, label, body)
            }
            Statement::ProcRepeat { label, body, until } => {
                self.exec_proc_repeat(s, label, body, until)
            }
            Statement::ProcLeave { label } => {
                self.proc_signal = Some(ProcSignal::Leave(label));
                Ok(String::new())
            }
            Statement::ProcIterate { label } => {
                self.proc_signal = Some(ProcSignal::Iterate(label));
                Ok(String::new())
            }
            Statement::PrepareStmt { name, query } => {
                self.prepared_stmts.insert(name.to_uppercase(), query);
                Ok("Query OK".to_string())
            }
            Statement::ExecuteStmt { name, using_vars } => {
                self.exec_execute(s, &name.to_uppercase(), &using_vars)
            }
            Statement::DeallocatePrepare { name } => {
                if self.prepared_stmts.remove(&name.to_uppercase()).is_some() {
                    Ok("Query OK".to_string())
                } else {
                    Err(format!("Unknown prepared statement: {}", name))
                }
            }
            Statement::SetUserVar { name, expr } => {
                let val = {
                    let mut vars = self.proc_vars.clone();
                    for (k, v) in &self.user_vars {
                        vars.insert(format!("@{}", k), v.clone());
                    }
                    Self::eval_arith(&vars, &expr)
                };
                self.user_vars.insert(name, val);
                Ok(String::new())
            }
            // These are handled in early-return blocks above; unreachable after qualify_stmt
            Statement::CreateUser { .. } | Statement::DropUser { .. }
            | Statement::Grant { .. } | Statement::Revoke { .. }
            | Statement::ShowGrants { .. } | Statement::ShowDatabases
            | Statement::CreateRole { .. } | Statement::DropRole { .. }
            | Statement::GrantRole { .. } | Statement::RevokeRole { .. }
            | Statement::ShowRoles
            | Statement::CreateSynonym { .. } | Statement::DropSynonym { .. }
            | Statement::ShowSynonyms => {
                Err("Internal error: management statement reached qualify pass".to_string())
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

    fn exec_intersect(
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
        let (left_cols, left_rows)   = Self::parse_table_output(&left_out);
        let (right_cols, right_rows) = Self::parse_table_output(&right_out);
        let cols = if left_cols.is_empty() { right_cols } else { left_cols };

        let right_keys: Vec<Vec<String>> = right_rows.iter()
            .map(|r| cols.iter().map(|c| r.get(c).cloned().unwrap_or_default()).collect())
            .collect();

        let mut result: Vec<Row> = Vec::new();
        let mut matched_right: Vec<usize> = Vec::new(); // for INTERSECT ALL
        for row in &left_rows {
            let key: Vec<String> = cols.iter().map(|c| row.get(c).cloned().unwrap_or_default()).collect();
            if all {
                // INTERSECT ALL: match once per right occurrence
                if let Some(pos) = right_keys.iter().enumerate()
                    .find(|(i, k)| !matched_right.contains(i) && *k == &key)
                    .map(|(i, _)| i)
                {
                    matched_right.push(pos);
                    result.push(row.clone());
                }
            } else {
                // INTERSECT: appear in both, deduplicate result
                if right_keys.contains(&key) && !result.iter().any(|r| {
                    cols.iter().map(|c| r.get(c).cloned().unwrap_or_default()).collect::<Vec<_>>() == key
                }) {
                    result.push(row.clone());
                }
            }
        }
        Self::apply_set_postprocess(&mut result, &cols, order_by, limit, offset);
        Ok(Self::format_set_result(&cols, result))
    }

    fn exec_except(
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
        let (left_cols, left_rows)   = Self::parse_table_output(&left_out);
        let (right_cols, right_rows) = Self::parse_table_output(&right_out);
        let cols = if left_cols.is_empty() { right_cols } else { left_cols };

        let right_keys: Vec<Vec<String>> = right_rows.iter()
            .map(|r| cols.iter().map(|c| r.get(c).cloned().unwrap_or_default()).collect())
            .collect();

        let mut right_counts: std::collections::HashMap<Vec<String>, usize> = std::collections::HashMap::new();
        for k in &right_keys { *right_counts.entry(k.clone()).or_insert(0) += 1; }

        let mut result: Vec<Row> = Vec::new();
        for row in &left_rows {
            let key: Vec<String> = cols.iter().map(|c| row.get(c).cloned().unwrap_or_default()).collect();
            if all {
                // EXCEPT ALL: subtract one right occurrence per left row
                let cnt = right_counts.entry(key.clone()).or_insert(0);
                if *cnt > 0 { *cnt -= 1; } else { result.push(row.clone()); }
            } else {
                // EXCEPT: rows in left not in right, deduplicated
                if !right_keys.contains(&key) && !result.iter().any(|r| {
                    cols.iter().map(|c| r.get(c).cloned().unwrap_or_default()).collect::<Vec<_>>() == key
                }) {
                    result.push(row.clone());
                }
            }
        }
        Self::apply_set_postprocess(&mut result, &cols, order_by, limit, offset);
        Ok(Self::format_set_result(&cols, result))
    }

    fn apply_set_postprocess(result: &mut Vec<Row>, cols: &[String], order_by: Vec<OrderBy>, limit: Option<usize>, offset: Option<usize>) {
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
        if let Some(n) = offset { let skip = n.min(result.len()); result.drain(..skip); }
        if let Some(n) = limit  { result.truncate(n); }
        let _ = cols; // suppress unused warning
    }

    fn format_set_result(cols: &[String], result: Vec<Row>) -> String {
        if result.is_empty() { return "0 rows returned.".to_string(); }
        let col_widths: Vec<usize> = cols.iter().map(|h| {
            let max_val = result.iter().map(|row| row.get(h).map(|v| v.len()).unwrap_or(0)).max().unwrap_or(0);
            h.len().max(max_val)
        }).collect();
        let mut out = String::new();
        let sep = col_widths.iter().map(|w| "-".repeat(w + 2)).collect::<Vec<_>>().join("+");
        let sep = format!("+{}+", sep);
        out.push_str(&sep); out.push('\n');
        let hdr = cols.iter().zip(col_widths.iter()).map(|(h, w)| format!(" {:width$} ", h, width = w)).collect::<Vec<_>>().join("|");
        out.push_str(&format!("|{}|\n", hdr));
        out.push_str(&sep); out.push('\n');
        for row in &result {
            let line = cols.iter().zip(col_widths.iter()).map(|(c, w)| {
                let v = row.get(c).map(|s| if s == NULL_VALUE { "NULL".to_string() } else { s.clone() }).unwrap_or_default();
                format!(" {:width$} ", v, width = w)
            }).collect::<Vec<_>>().join("|");
            out.push_str(&format!("|{}|\n", line));
        }
        out.push_str(&sep);
        out.push_str(&format!("\n{} row(s) returned.", result.len()));
        out
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
            // 4. Bare column name without dot: search row keys for unambiguous suffix ".{col}"
            let suffix = format!(".{}", col);
            let mut it = row.iter().filter(|(k, _)| k.ends_with(suffix.as_str()));
            match (it.next(), it.next()) {
                (Some((_, v)), None) => Some(v), // unambiguous
                _ => None,
            }
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
            ArithExpr::Cmp(l, op, r) => {
                let lv = Self::eval_arith(row, l);
                let rv = Self::eval_arith(row, r);
                let result = match (lv.parse::<f64>(), rv.parse::<f64>()) {
                    (Ok(a), Ok(b)) => match op.as_str() {
                        ">"  => a > b,
                        "<"  => a < b,
                        ">=" => a >= b,
                        "<=" => a <= b,
                        "="  => (a - b).abs() < 1e-9,
                        _    => a != b,
                    },
                    _ => match op.as_str() {
                        "="  => lv == rv,
                        ">"  => lv > rv,
                        "<"  => lv < rv,
                        ">=" => lv >= rv,
                        "<=" => lv <= rv,
                        _    => lv != rv,
                    },
                };
                if result { "1".to_string() } else { "0".to_string() }
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

    fn md5_hash(input: &[u8]) -> [u8; 16] {
        const S: [u32; 64] = [
            7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
            5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
            4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
            6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
        ];
        const K: [u32; 64] = [
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
        ];
        let orig_bits = (input.len() as u64).wrapping_mul(8);
        let mut msg = input.to_vec();
        msg.push(0x80);
        while msg.len() % 64 != 56 { msg.push(0); }
        msg.extend_from_slice(&orig_bits.to_le_bytes());
        let (mut a0, mut b0, mut c0, mut d0): (u32,u32,u32,u32) = (0x67452301,0xefcdab89,0x98badcfe,0x10325476);
        for chunk in msg.chunks(64) {
            let mut m = [0u32; 16];
            for i in 0..16 { m[i] = u32::from_le_bytes([chunk[i*4],chunk[i*4+1],chunk[i*4+2],chunk[i*4+3]]); }
            let (mut a, mut b, mut c, mut d) = (a0, b0, c0, d0);
            for i in 0..64u32 {
                let (f, g) = if i < 16 { ((b&c)|(!b&d), i) }
                    else if i < 32 { ((d&b)|(!d&c), (5*i+1)%16) }
                    else if i < 48 { (b^c^d, (3*i+5)%16) }
                    else { (c^(b|!d), (7*i)%16) };
                let tmp = d; d = c; c = b;
                b = b.wrapping_add(a.wrapping_add(f).wrapping_add(K[i as usize]).wrapping_add(m[g as usize]).rotate_left(S[i as usize]));
                a = tmp;
            }
            a0=a0.wrapping_add(a); b0=b0.wrapping_add(b); c0=c0.wrapping_add(c); d0=d0.wrapping_add(d);
        }
        let mut r = [0u8; 16];
        r[0..4].copy_from_slice(&a0.to_le_bytes()); r[4..8].copy_from_slice(&b0.to_le_bytes());
        r[8..12].copy_from_slice(&c0.to_le_bytes()); r[12..16].copy_from_slice(&d0.to_le_bytes());
        r
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
        returning: Option<Vec<SelectColumn>>,
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
        self.exec_insert(s, table, insert_cols, all_values, on_conflict, returning)
    }

    /// Resolve a view name to its underlying base table (for updatable views).
    /// Returns (base_table, view_condition) if the view is simple and updatable.
    /// Returns Err if the view is not updatable.
    fn resolve_updatable_view(s: &SharedDatabase, name: &str) -> Option<(String, Option<CondExpr>)> {
        let view_stmt = s.views.get(name)?;
        if let Statement::Select { table: base_table, joins, distinct, group_by, condition, subquery, .. } = view_stmt {
            if joins.is_empty() && !distinct && group_by.as_ref().map_or(true, |g| g.is_empty()) && subquery.is_none() {
                return Some((base_table.clone(), condition.clone()));
            }
        }
        None
    }

    fn exec_insert(
        &mut self,
        s: &mut SharedDatabase,
        table: String,
        col_list: Option<Vec<String>>,
        all_values: Vec<Vec<String>>,
        on_conflict: InsertConflict,
        returning: Option<Vec<SelectColumn>>,
    ) -> Result<String, String> {
        // Updatable view: redirect INSERT to the base table
        if s.views.contains_key(&table) {
            match Self::resolve_updatable_view(s, &table) {
                Some((base_table, _)) => {
                    return self.exec_insert(s, base_table, col_list, all_values, on_conflict, returning);
                }
                None => return Err(format!("View '{}' is not updatable (has JOINs, DISTINCT, GROUP BY, or subquery)", Self::strip_db_prefix(&table))),
            }
        }
        self.fire_triggers(s, &table, "BEFORE", "INSERT");
        let committed = if self.txn.is_active() {
            Some(self.session_swap_in(s, &table))
        } else {
            None
        };
        let result = self.exec_insert_inner(s, table.clone(), col_list, all_values, on_conflict, returning);
        if let Some(c) = committed {
            self.session_swap_out(s, &table, c);
        }
        if result.is_ok() { self.fire_triggers(s, &table, "AFTER", "INSERT"); }
        result
    }

    fn exec_insert_inner(
        &mut self,
        s: &mut SharedDatabase,
        table: String,
        col_list: Option<Vec<String>>,
        all_values: Vec<Vec<String>>,
        on_conflict: InsertConflict,
        returning: Option<Vec<SelectColumn>>,
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
        let returning_rows: Vec<Row> = if returning.is_some() { prepared.clone() } else { vec![] };

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
        // 트랜잭션 중에는 버퍼 풀 갱신 생략 (COMMIT 시 일괄 처리)
        if !self.txn.is_active() {
            s.buffer_pool.write_page(&table, rows);
            s.buffer_pool.flush_page(&table, &s.disk);
            Self::maybe_auto_vacuum(s);
        }

        self.maybe_auto_checkpoint(s);
        if let Some(ret_cols) = returning {
            Ok(Self::format_returning_rows(&returning_rows, &ret_cols))
        } else {
            Ok(format!("{} row(s) inserted.", inserted))
        }
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
                // Resolve column references against the row.
                // Qualified (table.col) or bare identifiers starting with alpha/_ that are
                // not parseable as numbers are tried as column lookups first.
                let resolved;
                let is_ident_like = lit.chars().next()
                    .map(|c| c.is_alphabetic() || c == '_').unwrap_or(false)
                    && lit.parse::<f64>().is_err();
                let effective_lit: &str = if is_ident_like {
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
                    Operator::Regexp => {
                        regex::Regex::new(effective_lit)
                            .map(|re| re.is_match(&val))
                            .unwrap_or(false)
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
        for_share: bool,
    ) -> Result<String, String> {

        // FROM (SELECT ...) AS alias 처리
        if let Some((inner_stmt, alias)) = subquery {
            return self.exec_select_with_subquery(
                s, *inner_stmt, alias, distinct, columns, condition, joins,
                order_by, group_by, having, limit, offset, for_update, for_share,
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
                    SelectColumn::WinFunc { alias, func, .. } => alias.clone().unwrap_or_else(|| match func {
                        WindowFunc::RowNumber   => "row_number".to_string(),
                        WindowFunc::Rank        => "rank".to_string(),
                        WindowFunc::DenseRank   => "dense_rank".to_string(),
                        WindowFunc::Lag         => "lag".to_string(),
                        WindowFunc::Lead        => "lead".to_string(),
                        WindowFunc::FirstValue  => "first_value".to_string(),
                        WindowFunc::LastValue   => "last_value".to_string(),
                        WindowFunc::NthValue    => "nth_value".to_string(),
                        WindowFunc::Ntile       => "ntile".to_string(),
                        WindowFunc::PercentRank => "percent_rank".to_string(),
                        WindowFunc::CumeDist    => "cume_dist".to_string(),
                        WindowFunc::Sum         => "sum".to_string(),
                        WindowFunc::Avg         => "avg".to_string(),
                        WindowFunc::Count       => "count".to_string(),
                        WindowFunc::Min         => "min".to_string(),
                        WindowFunc::Max         => "max".to_string(),
                    }),
                    SelectColumn::Subquery { alias, .. } => alias.clone().unwrap_or_else(|| "(subquery)".to_string()),
                };
                (header, col.clone())
            }).collect();
            // proc_vars + user_vars(@key)를 row로 사용
            let mut eval_row = self.proc_vars.clone();
            for (k, v) in &self.user_vars {
                eval_row.insert(format!("@{}", k), v.clone());
            }
            let eval_col_val = |col: &SelectColumn| -> String {
                match col {
                    SelectColumn::Func { name, args, .. } => Self::apply_scalar_func(name, args, &eval_row),
                    SelectColumn::Expr { expr, .. } => Self::eval_arith(&eval_row, expr),
                    SelectColumn::Column(c) => eval_row.get(c.as_str()).cloned().unwrap_or_else(|| c.clone()),
                    SelectColumn::ColumnAlias(c, _) => eval_row.get(c.as_str()).cloned().unwrap_or_else(|| c.clone()),
                    _ => String::new(),
                }
            };
            let widths: Vec<usize> = col_defs.iter().map(|(h, col)| {
                let val = eval_col_val(col);
                h.len().max(val.len())
            }).collect();
            let sep: String = widths.iter().map(|w| "+".to_string() + &"-".repeat(w + 2)).collect::<String>() + "+";
            let hdr: String = col_defs.iter().zip(widths.iter()).map(|((h, _), w)| format!("| {:width$} ", h, width = w)).collect::<String>() + "|";
            let row_str: String = col_defs.iter().zip(widths.iter()).map(|((_, col), w)| {
                let val = eval_col_val(col);
                format!("| {:width$} ", val, width = w)
            }).collect::<String>() + "|";
            return Ok(format!("{}\n{}\n{}\n{}\n{}\n1 row(s) returned.", sep, hdr, sep, row_str, sep));
        }

        // INFORMATION_SCHEMA 가상 테이블
        if let Some(pos) = table.to_lowercase().find("information_schema.") {
            let which = table[pos + 19..].to_string();
            return self.exec_information_schema(s, &which, columns, condition, order_by, limit, offset);
        }

        // 뷰 처리: 뷰를 FROM 서브쿼리처럼 실행하고 외부 쿼리 조건을 적용
        if let Some(view_stmt) = s.views.remove(&table) {
            let result = self.exec_select_with_subquery(
                s, view_stmt.clone(),
                table.clone(),
                distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update, for_share,
            );
            s.views.insert(table, view_stmt);
            return result;
        }

        // ── JOIN 순서 최적화 (greedy, INNER-only) ─────────────────────────
        let joins = Self::reorder_joins_greedy(&table, joins, &s.tables);

        // ── Planner: 인덱스 / 조인 알고리즘 결정 ──────────────────────────
        let has_agg = columns.iter().any(|c| matches!(c, SelectColumn::Agg { .. } | SelectColumn::AggAlias { .. }));
        let has_win = columns.iter().any(|c| matches!(c, SelectColumn::WinFunc { .. }));
        let planner = Planner::new(&s.tables, &s.indexes, &s.index_meta, &s.composite_indexes, &s.catalog, &s.table_stats);
        let plan = planner.plan_covering(&table, &condition, &joins, &columns);

        // 인덱스 경로 실행 (집계 / FOR UPDATE / JOIN / LIMIT / ORDER BY 없을 때만)
        if joins.is_empty() && !has_agg && !has_win && !for_update && !for_share
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

        // 읽기 우선순위: 세션 쓰기 버퍼 > REPEATABLE READ 스냅샷 > 커밋 데이터
        let rows: Vec<Row> = if let Some(session_rows) = self.session_tables.get(&table) {
            session_rows.clone()
        } else if let Some(snap_rows) = self.txn.get_snapshot_table(&table) {
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
                let right_rows_raw = if let Some(session_rows) = self.session_tables.get(&j.table) {
                    session_rows.clone()
                } else if let Some(snap) = self.txn.get_snapshot_table(&j.table) {
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

                // Cross/Natural/FullOuter joins always use nested loop (no hash/sort-merge)
                let algo = if matches!(j.join_type, JoinType::Cross | JoinType::Natural | JoinType::FullOuter) {
                    None
                } else {
                    plan.joins.get(ji).map(|jp| &jp.algo)
                };

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
                            _ => unreachable!("Cross/Natural joins do not use Sort-Merge"),
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
                            _ => unreachable!("Cross/Natural joins do not use Hash Join"),
                        }
                        out
                    }
                    _ => {
                        // ── Nested Loop Join (default) ───────────────────
                        let mut out = Vec::new();
                        // JOIN ... USING(col, ...) — treat as inner equi-join on specified columns
                        if !j.using_cols.is_empty() {
                            let using_cols = j.using_cols.clone();
                            for left in &current {
                                for right in &right_rows {
                                    let matches = using_cols.iter().all(|col| {
                                        let lv = left.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                                        let rv = right.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                                        lv == rv && lv != NULL_VALUE
                                    });
                                    if matches {
                                        let mut merged = left.clone();
                                        merge_right(&mut merged, right, &j.table);
                                        out.push(merged);
                                    }
                                }
                            }
                            out
                        } else {
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
                            JoinType::Cross => {
                                // Cartesian product — no condition
                                for left in &current {
                                    for right in &right_rows {
                                        let mut merged = left.clone();
                                        merge_right(&mut merged, right, &j.table);
                                        out.push(merged);
                                    }
                                }
                            }
                            JoinType::Natural => {
                                // Equi-join on all columns with identical names in both tables
                                let common_cols: Vec<String> = right_schema_cols.iter()
                                    .filter(|rc| current.first()
                                        .map(|lr| lr.contains_key(*rc) || lr.keys().any(|k| k == *rc))
                                        .unwrap_or(false))
                                    .cloned()
                                    .collect();
                                for left in &current {
                                    for right in &right_rows {
                                        let mut merged = left.clone();
                                        merge_right(&mut merged, right, &j.table);
                                        let matches = common_cols.iter().all(|col| {
                                            let lv = left.get(col).map(String::as_str).unwrap_or("");
                                            let rv = right.get(col).map(String::as_str).unwrap_or("");
                                            lv == rv
                                        });
                                        if matches { out.push(merged); }
                                    }
                                }
                            }
                            JoinType::FullOuter => {
                                let left_cols: Vec<String> = current.first()
                                    .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                                    .unwrap_or_default();
                                let mut matched_right: HashSet<usize> = HashSet::new();
                                for left in &current {
                                    let mut any_match = false;
                                    for (ri, right) in right_rows.iter().enumerate() {
                                        let mut merged = left.clone();
                                        merge_right(&mut merged, right, &j.table);
                                        if Self::eval_condexpr(&merged, &j.on_expr) {
                                            out.push(merged);
                                            matched_right.insert(ri);
                                            any_match = true;
                                        }
                                    }
                                    if !any_match {
                                        let mut merged = left.clone();
                                        null_right(&mut merged, &right_schema_cols, &j.table);
                                        out.push(merged);
                                    }
                                }
                                for (ri, right) in right_rows.iter().enumerate() {
                                    if !matched_right.contains(&ri) {
                                        let mut merged: Row = left_cols.iter()
                                            .map(|c| (c.clone(), NULL_VALUE.to_string()))
                                            .collect();
                                        merge_right(&mut merged, right, &j.table);
                                        out.push(merged);
                                    }
                                }
                            }
                        }
                        out
                        } // end else (using_cols is empty)
                    }
                };
                current = joined;
            }
            current.into_iter()
                .filter(|r| self.matches_condition_with_subquery(s, r, &condition))
                .collect()
        };

        // WINDOW FUNCTIONS (WHERE 필터 후, ORDER BY 전)
        let mut result = result;
        if has_win {
            result = Self::compute_window_functions(result, &columns);
        }

        // ORDER BY
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
                    let distinct_vals = |rows: &[Row]| -> Vec<f64> {
                        let seen: HashSet<String> = rows.iter()
                            .filter_map(|r| r.get(col_name).filter(|v| v.as_str() != NULL_VALUE).cloned())
                            .collect();
                        seen.iter().filter_map(|v| v.parse::<f64>().ok()).collect()
                    };
                    let agg_val = match func {
                        AggFunc::Count => grp.len() as f64,
                        AggFunc::CountDistinct => {
                            let distinct: HashSet<String> = grp.iter()
                                .filter_map(|r| r.get(col_name).filter(|v| v.as_str() != NULL_VALUE).cloned())
                                .collect();
                            distinct.len() as f64
                        }
                        AggFunc::Sum          => vals.iter().sum(),
                        AggFunc::SumDistinct  => { let dv = distinct_vals(grp); dv.iter().sum() }
                        AggFunc::Avg          => if vals.is_empty() { 0.0 } else {
                            vals.iter().sum::<f64>() / vals.len() as f64 }
                        AggFunc::AvgDistinct  => { let dv = distinct_vals(grp);
                            if dv.is_empty() { 0.0 } else { dv.iter().sum::<f64>() / dv.len() as f64 } }
                        AggFunc::Min   => vals.iter().cloned().fold(f64::INFINITY, f64::min),
                        AggFunc::Max   => vals.iter().cloned().fold(f64::NEG_INFINITY, f64::max),
                        AggFunc::Stddev => {
                            if vals.is_empty() { 0.0 } else {
                                let mean = vals.iter().sum::<f64>() / vals.len() as f64;
                                (vals.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / vals.len() as f64).sqrt()
                            }
                        }
                        AggFunc::Variance => {
                            if vals.is_empty() { 0.0 } else {
                                let mean = vals.iter().sum::<f64>() / vals.len() as f64;
                                vals.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / vals.len() as f64
                            }
                        }
                        AggFunc::GroupConcat { .. } => unreachable!(),
                    };
                    let v = match func {
                        AggFunc::Avg | AggFunc::AvgDistinct |
                        AggFunc::Stddev | AggFunc::Variance => format!("{:.4}", agg_val),
                        _ => if agg_val.fract() == 0.0 { format!("{}", agg_val as i64) }
                             else { format!("{:.4}", agg_val) },
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
                    SelectColumn::WinFunc { alias, func, .. } => {
                        let key = alias.clone().unwrap_or_else(|| match func {
                            WindowFunc::RowNumber   => "row_number".to_string(),
                            WindowFunc::Rank        => "rank".to_string(),
                            WindowFunc::DenseRank   => "dense_rank".to_string(),
                            WindowFunc::Lag         => "lag".to_string(),
                            WindowFunc::Lead        => "lead".to_string(),
                            WindowFunc::FirstValue  => "first_value".to_string(),
                            WindowFunc::LastValue   => "last_value".to_string(),
                            WindowFunc::NthValue    => "nth_value".to_string(),
                            WindowFunc::Ntile       => "ntile".to_string(),
                            WindowFunc::PercentRank => "percent_rank".to_string(),
                            WindowFunc::CumeDist    => "cume_dist".to_string(),
                            WindowFunc::Sum         => "sum".to_string(),
                            WindowFunc::Avg         => "avg".to_string(),
                            WindowFunc::Count       => "count".to_string(),
                            WindowFunc::Min         => "min".to_string(),
                            WindowFunc::Max         => "max".to_string(),
                        });
                        row.get(&key).cloned().unwrap_or_default()
                    }
                    SelectColumn::Subquery { .. } => String::new(),
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
                let col_name_str = col_name.as_str();
                let distinct_vals_g = |rows: &[Row]| -> Vec<f64> {
                    let seen: HashSet<String> = rows.iter()
                        .filter_map(|r| r.get(col_name_str).filter(|v| v.as_str() != NULL_VALUE).cloned())
                        .collect();
                    seen.iter().filter_map(|v| v.parse::<f64>().ok()).collect()
                };
                let agg_val = match func {
                    AggFunc::Count => result.len() as f64,
                    AggFunc::CountDistinct => {
                        let distinct: HashSet<String> = result.iter()
                            .filter_map(|r| r.get(col_name_str).filter(|v| v.as_str() != NULL_VALUE).cloned())
                            .collect();
                        distinct.len() as f64
                    }
                    AggFunc::Sum         => vals.iter().sum(),
                    AggFunc::SumDistinct => { let dv = distinct_vals_g(&result); dv.iter().sum() }
                    AggFunc::Avg         => if vals.is_empty() { 0.0 } else {
                        vals.iter().sum::<f64>() / vals.len() as f64 }
                    AggFunc::AvgDistinct => { let dv = distinct_vals_g(&result);
                        if dv.is_empty() { 0.0 } else { dv.iter().sum::<f64>() / dv.len() as f64 } }
                    AggFunc::Min   => vals.iter().cloned().fold(f64::INFINITY, f64::min),
                    AggFunc::Max   => vals.iter().cloned().fold(f64::NEG_INFINITY, f64::max),
                    AggFunc::Stddev => {
                        if vals.is_empty() { 0.0 } else {
                            let mean = vals.iter().sum::<f64>() / vals.len() as f64;
                            (vals.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / vals.len() as f64).sqrt()
                        }
                    }
                    AggFunc::Variance => {
                        if vals.is_empty() { 0.0 } else {
                            let mean = vals.iter().sum::<f64>() / vals.len() as f64;
                            vals.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / vals.len() as f64
                        }
                    }
                    AggFunc::GroupConcat { .. } => unreachable!(),
                };
                let val_str = match func {
                    AggFunc::Avg | AggFunc::AvgDistinct |
                    AggFunc::Stddev | AggFunc::Variance => format!("{:.4}", agg_val),
                    _ => if agg_val.fract() == 0.0 { format!("{}", agg_val as i64) }
                         else { format!("{:.4}", agg_val) },
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

        // FOR UPDATE: 결과 행에 배타 잠금 획득
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

        // FOR SHARE: 결과 행에 공유 잠금 획득
        if for_share {
            if !self.txn.is_active() {
                return Err("SELECT FOR SHARE requires an active transaction (BEGIN first).".to_string());
            }
            let txn_id = self.txn.current_txn_id();
            let pk_col = s.catalog.get_table(&table)
                .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                .unwrap_or_else(|| "id".to_string());
            for row in &result {
                let pk_val = row.get(&pk_col).cloned().unwrap_or_default();
                match s.lock_mgr.acquire_shared(&table, &pk_val, txn_id) {
                    LockResult::Granted => {}
                    LockResult::Conflict { holder } => {
                        return Err(format!(
                            "Row '{}' in '{}' is locked exclusively by transaction {}. Cannot SELECT FOR SHARE.",
                            pk_val, table, holder
                        ));
                    }
                    LockResult::Deadlock { holder } => {
                        return Err(format!(
                            "Deadlock detected: transaction {} waits for transaction {} (SELECT FOR SHARE). Transaction {} aborted.",
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
        for_share: bool,
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
            joins, order_by, group_by, having, limit, offset, for_update, for_share,
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
        // Check user-defined functions first
        let uf_hit = USER_FUNCTIONS.with(|uf| {
            let map = uf.borrow();
            map.get(&func_name.to_lowercase()).cloned()
        });
        if let Some((params, body_json)) = uf_hit {
            if let Ok(expr) = serde_json::from_str::<ArithExpr>(&body_json) {
                // Build a synthetic row binding param names to arg values
                let mut bound_row: Row = row.clone();
                let resolve_arg = |arg: &str| -> String {
                    if arg.starts_with('\'') && arg.ends_with('\'') { return arg[1..arg.len()-1].to_string(); }
                    if let Some(v) = row.get(arg) { return v.clone(); }
                    arg.to_string()
                };
                for (i, param) in params.iter().enumerate() {
                    let val = args.get(i).map(|a| resolve_arg(a)).unwrap_or_default();
                    bound_row.insert(param.clone(), val);
                }
                return Self::eval_arith(&bound_row, &expr);
            }
        }

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
            "REGEXP_LIKE" | "REGEXP" => {
                let val = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let pat = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                regex::Regex::new(&pat)
                    .map(|re| if re.is_match(&val) { "1" } else { "0" })
                    .unwrap_or("0")
                    .to_string()
            }
            "REGEXP_REPLACE" => {
                let val = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let pat = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                let rep = args.get(2).map(|a| resolve(a, row)).unwrap_or_default();
                regex::Regex::new(&pat)
                    .map(|re| re.replace_all(&val, rep.as_str()).into_owned())
                    .unwrap_or(val)
            }
            "REGEXP_MATCH" | "REGEXP_SUBSTR" => {
                let val = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let pat = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                regex::Regex::new(&pat)
                    .ok()
                    .and_then(|re| re.find(&val).map(|m| m.as_str().to_string()))
                    .unwrap_or_else(|| NULL_VALUE.to_string())
            }
            // ── 수학 함수 ─────────────────────────────────────────────────────
            "SQRT" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                if v < 0.0 { NULL_VALUE.to_string() } else { format!("{:.6}", v.sqrt()) }
            }
            "POW" | "POWER" => {
                let base: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                let exp: f64  = args.get(1).map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                format!("{}", base.powf(exp))
            }
            "LOG" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                if args.len() >= 2 {
                    let base: f64 = args.get(1).map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(std::f64::consts::E);
                    if v <= 0.0 || base <= 0.0 || base == 1.0 { NULL_VALUE.to_string() } else { format!("{:.6}", v.log(base)) }
                } else {
                    if v <= 0.0 { NULL_VALUE.to_string() } else { format!("{:.6}", v.ln()) }
                }
            }
            "LOG2" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                if v <= 0.0 { NULL_VALUE.to_string() } else { format!("{:.6}", v.log2()) }
            }
            "LOG10" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                if v <= 0.0 { NULL_VALUE.to_string() } else { format!("{:.6}", v.log10()) }
            }
            "EXP" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                format!("{:.6}", v.exp())
            }
            "SIN" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                format!("{:.6}", v.sin())
            }
            "COS" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                format!("{:.6}", v.cos())
            }
            "TAN" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                format!("{:.6}", v.tan())
            }
            "PI" => std::f64::consts::PI.to_string(),
            "SIGN" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                if v > 0.0 { "1".to_string() } else if v < 0.0 { "-1".to_string() } else { "0".to_string() }
            }
            "TRUNCATE" => {
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                let d: i32 = args.get(1).map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0);
                let factor = 10f64.powi(d);
                format!("{}", (v * factor).trunc() / factor)
            }
            "RAND" => {
                // 간단한 의사 난수 (seed 무시)
                let n: f64 = (std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .subsec_nanos() as f64) / 1_000_000_000.0;
                format!("{:.6}", n)
            }
            // ── 조건부 ────────────────────────────────────────────────────────
            "GREATEST" => {
                let vals: Vec<String> = args.iter().map(|a| resolve(a, row)).collect();
                vals.into_iter().max_by(|a, b| {
                    match (a.parse::<f64>(), b.parse::<f64>()) {
                        (Ok(fa), Ok(fb)) => fa.partial_cmp(&fb).unwrap_or(std::cmp::Ordering::Equal),
                        _ => a.cmp(b),
                    }
                }).unwrap_or_default()
            }
            "LEAST" => {
                let vals: Vec<String> = args.iter().map(|a| resolve(a, row)).collect();
                vals.into_iter().min_by(|a, b| {
                    match (a.parse::<f64>(), b.parse::<f64>()) {
                        (Ok(fa), Ok(fb)) => fa.partial_cmp(&fb).unwrap_or(std::cmp::Ordering::Equal),
                        _ => a.cmp(b),
                    }
                }).unwrap_or_default()
            }
            // ── 문자열 함수 ───────────────────────────────────────────────────
            "CHAR_LENGTH" | "CHARACTER_LENGTH" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.chars().count().to_string()
            }
            "LEFT" => {
                let s = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let n: usize = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                s.chars().take(n).collect()
            }
            "RIGHT" => {
                let s = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let n: usize = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                let chars: Vec<char> = s.chars().collect();
                chars[chars.len().saturating_sub(n)..].iter().collect()
            }
            "REVERSE" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.chars().rev().collect()
            }
            "REPEAT" => {
                let s = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let n: usize = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                s.repeat(n)
            }
            "INSTR" => {
                let haystack = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let needle   = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                match haystack.find(&needle as &str) {
                    Some(pos) => (pos + 1).to_string(),
                    None      => "0".to_string(),
                }
            }
            "LOCATE" => {
                // LOCATE(substr, str [, pos])
                let needle   = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let haystack = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                let start: usize = args.get(2).and_then(|a| resolve(a, row).parse::<usize>().ok())
                    .map(|p| p.saturating_sub(1)).unwrap_or(0);
                let slice = if start < haystack.len() { &haystack[start..] } else { "" };
                match slice.find(&needle as &str) {
                    Some(pos) => (start + pos + 1).to_string(),
                    None      => "0".to_string(),
                }
            }
            "LTRIM" => {
                args.first().map(|a| resolve(a, row)).unwrap_or_default().trim_start().to_string()
            }
            "RTRIM" => {
                args.first().map(|a| resolve(a, row)).unwrap_or_default().trim_end().to_string()
            }
            "SPACE" => {
                let n: usize = args.first().and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                " ".repeat(n)
            }
            "ASCII" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.chars().next().map(|c| (c as u32).to_string()).unwrap_or_else(|| "0".to_string())
            }
            "CHAR" => {
                args.iter().filter_map(|a| {
                    resolve(a, row).parse::<u8>().ok().map(|b| b as char)
                }).collect()
            }
            "HEX" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                if let Ok(n) = v.parse::<u64>() {
                    format!("{:X}", n)
                } else {
                    v.bytes().map(|b| format!("{:02X}", b)).collect()
                }
            }
            "UNHEX" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                (0..v.len()).step_by(2)
                    .filter_map(|i| u8::from_str_radix(&v[i..i+2], 16).ok().map(|b| b as char))
                    .collect()
            }
            "FORMAT" => {
                // FORMAT(number, decimals)
                let v: f64 = args.first().map(|a| resolve(a, row)).unwrap_or_default().parse().unwrap_or(0.0);
                let d: usize = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                let s = format!("{:.prec$}", v, prec = d);
                // 천 단위 콤마 삽입
                let (int_part, dec_part) = if let Some(pos) = s.find('.') {
                    (&s[..pos], &s[pos..])
                } else {
                    (s.as_str(), "")
                };
                let is_neg = int_part.starts_with('-');
                let digits = if is_neg { &int_part[1..] } else { int_part };
                let with_commas: String = digits.chars().rev().enumerate()
                    .flat_map(|(i, c)| if i > 0 && i % 3 == 0 { vec![',', c] } else { vec![c] })
                    .collect::<String>().chars().rev().collect();
                format!("{}{}{}", if is_neg { "-" } else { "" }, with_commas, dec_part)
            }
            // ── 날짜/시간 함수 ────────────────────────────────────────────────
            "YEAR" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.split('-').next().unwrap_or("").to_string()
            }
            "MONTH" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.split('-').nth(1).unwrap_or("").to_string()
            }
            "DAY" | "DAYOFMONTH" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.split('-').nth(2).and_then(|s| s.split(' ').next()).unwrap_or("").to_string()
            }
            "HOUR" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.split(' ').nth(1).and_then(|t| t.split(':').next()).unwrap_or("0").to_string()
            }
            "MINUTE" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.split(' ').nth(1).and_then(|t| t.split(':').nth(1)).unwrap_or("0").to_string()
            }
            "SECOND" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                v.split(' ').nth(1).and_then(|t| t.split(':').nth(2)).unwrap_or("0").to_string()
            }
            "DAYOFWEEK" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                use chrono::Datelike;
                chrono::NaiveDate::parse_from_str(&v, "%Y-%m-%d")
                    .map(|d| (d.weekday().num_days_from_sunday() + 1).to_string())
                    .unwrap_or_else(|_| NULL_VALUE.to_string())
            }
            "DAYOFYEAR" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                use chrono::Datelike;
                chrono::NaiveDate::parse_from_str(&v, "%Y-%m-%d")
                    .map(|d| d.ordinal().to_string())
                    .unwrap_or_else(|_| NULL_VALUE.to_string())
            }
            "WEEKDAY" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                use chrono::Datelike;
                chrono::NaiveDate::parse_from_str(&v, "%Y-%m-%d")
                    .map(|d| d.weekday().num_days_from_monday().to_string())
                    .unwrap_or_else(|_| NULL_VALUE.to_string())
            }
            "LAST_DAY" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                use chrono::{Datelike, NaiveDate};
                NaiveDate::parse_from_str(&v, "%Y-%m-%d").ok().and_then(|d| {
                    let (y, m) = if d.month() == 12 { (d.year() + 1, 1) } else { (d.year(), d.month() + 1) };
                    NaiveDate::from_ymd_opt(y, m, 1).map(|next| {
                        (next - chrono::Duration::days(1)).format("%Y-%m-%d").to_string()
                    })
                }).unwrap_or_else(|| NULL_VALUE.to_string())
            }
            "TIMESTAMPDIFF" => {
                // TIMESTAMPDIFF(unit, dt1, dt2)
                let unit = args.first().map(|a| resolve(a, row)).unwrap_or_default().to_uppercase();
                let d1   = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                let d2   = args.get(2).map(|a| resolve(a, row)).unwrap_or_default();
                let parse = |s: &str| -> Option<chrono::NaiveDateTime> {
                    chrono::NaiveDateTime::parse_from_str(s, "%Y-%m-%d %H:%M:%S").ok()
                        .or_else(|| chrono::NaiveDate::parse_from_str(s, "%Y-%m-%d").ok()
                            .map(|d| d.and_hms_opt(0, 0, 0).unwrap()))
                };
                match (parse(&d1), parse(&d2)) {
                    (Some(dt1), Some(dt2)) => {
                        let diff = dt2.signed_duration_since(dt1);
                        match unit.as_str() {
                            "SECOND" => diff.num_seconds().to_string(),
                            "MINUTE" => diff.num_minutes().to_string(),
                            "HOUR"   => diff.num_hours().to_string(),
                            "DAY"    => diff.num_days().to_string(),
                            "WEEK"   => (diff.num_days() / 7).to_string(),
                            "MONTH"  => {
                                use chrono::Datelike;
                                let months = (dt2.year() - dt1.year()) * 12 + dt2.month() as i32 - dt1.month() as i32;
                                months.to_string()
                            }
                            "YEAR"   => {
                                use chrono::Datelike;
                                (dt2.year() - dt1.year()).to_string()
                            }
                            _ => NULL_VALUE.to_string(),
                        }
                    }
                    _ => NULL_VALUE.to_string(),
                }
            }
            "DATE_SUB" => {
                let date_str = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let amount: i64 = args.get(1).and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                let unit = args.get(2).map(|s| s.as_str()).unwrap_or("DAY");
                use chrono::{NaiveDate, Datelike};
                if let Ok(d) = NaiveDate::parse_from_str(&date_str, "%Y-%m-%d") {
                    let result = match unit {
                        "DAY"   => d - chrono::Duration::days(amount),
                        "MONTH" => {
                            let months = d.month() as i64 - amount;
                            let (year, month) = if months <= 0 {
                                (d.year() + ((months - 1) / 12) as i32 - 1, (months.rem_euclid(12) + 12) as u32 % 12 + 1)
                            } else {
                                (d.year() + ((months - 1) / 12) as i32, ((months - 1) % 12 + 1) as u32)
                            };
                            NaiveDate::from_ymd_opt(year, month, d.day()).unwrap_or(d)
                        }
                        "YEAR"  => NaiveDate::from_ymd_opt(d.year() - amount as i32, d.month(), d.day()).unwrap_or(d),
                        _ => d,
                    };
                    result.format("%Y-%m-%d").to_string()
                } else {
                    NULL_VALUE.to_string()
                }
            }
            "CURTIME" | "CURRENT_TIME" => {
                chrono::Local::now().format("%H:%M:%S").to_string()
            }
            "CURRENT_TIMESTAMP" | "LOCALTIME" | "LOCALTIMESTAMP" => {
                chrono::Local::now().format("%Y-%m-%d %H:%M:%S").to_string()
            }
            "UNIX_TIMESTAMP" => {
                if args.is_empty() {
                    std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_secs()
                        .to_string()
                } else {
                    let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                    chrono::NaiveDateTime::parse_from_str(&v, "%Y-%m-%d %H:%M:%S")
                        .ok()
                        .or_else(|| chrono::NaiveDate::parse_from_str(&v, "%Y-%m-%d").ok()
                            .map(|d| d.and_hms_opt(0,0,0).unwrap()))
                        .map(|dt| dt.and_utc().timestamp().to_string())
                        .unwrap_or_else(|| NULL_VALUE.to_string())
                }
            }
            "FROM_UNIXTIME" => {
                let ts: i64 = args.first().and_then(|a| resolve(a, row).parse().ok()).unwrap_or(0);
                use chrono::TimeZone;
                chrono::Local.timestamp_opt(ts, 0).single()
                    .map(|dt| dt.format("%Y-%m-%d %H:%M:%S").to_string())
                    .unwrap_or_else(|| NULL_VALUE.to_string())
            }
            // ── 타입/변환 ──────────────────────────────────────────────────────
            "ISNULL" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                if v == NULL_VALUE || v.is_empty() { "1".to_string() } else { "0".to_string() }
            }
            "CONVERT" => {
                // CONVERT(val, type) — CAST의 별칭
                let val      = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let type_str = args.get(1).map(|s| s.as_str()).unwrap_or("TEXT");
                match type_str {
                    "INT" | "INTEGER" | "SIGNED" => val.parse::<i64>().map(|n| n.to_string()).unwrap_or_else(|_| "0".to_string()),
                    "FLOAT" | "DOUBLE" | "DECIMAL" | "UNSIGNED" => val.parse::<f64>().map(|n| format!("{}", n)).unwrap_or_else(|_| "0".to_string()),
                    _ => val,
                }
            }
            "BIT_LENGTH" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                (v.len() * 8).to_string()
            }
            "MD5" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let hash = Self::md5_hash(v.as_bytes());
                hash.iter().map(|b| format!("{:02x}", b)).collect::<String>()
            }
            "UUID" => {
                let ts = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_nanos();
                format!("{:08x}-{:04x}-4{:03x}-{:04x}-{:012x}",
                    (ts >> 32) as u32, (ts >> 16) as u16 & 0xffff,
                    ts as u16 & 0x0fff, 0x8000u16 | (ts >> 48) as u16 & 0x3fff,
                    ts as u64 & 0xffffffffffff)
            }
            "JSON_EXTRACT" => {
                let json_str = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let path = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                Self::json_extract(&json_str, &path)
            }
            "JSON_UNQUOTE" => {
                let v = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                if v.starts_with('"') && v.ends_with('"') && v.len() >= 2 {
                    v[1..v.len()-1].replace("\\\"", "\"").replace("\\\\", "\\")
                } else {
                    v
                }
            }
            "JSON_VALUE" => {
                let json_str = args.first().map(|a| resolve(a, row)).unwrap_or_default();
                let path = args.get(1).map(|a| resolve(a, row)).unwrap_or_default();
                let extracted = Self::json_extract(&json_str, &path);
                if extracted.starts_with('"') && extracted.ends_with('"') && extracted.len() >= 2 {
                    extracted[1..extracted.len()-1].replace("\\\"", "\"").replace("\\\\", "\\")
                } else {
                    extracted
                }
            }
            _ => format!("{}()", func_name),
        }
    }

    fn json_extract(json_str: &str, path: &str) -> String {
        // path format: $.key  or $.key.sub  or $.arr[0]
        let path = path.trim_matches('"');
        if !path.starts_with("$.") && path != "$" {
            return NULL_VALUE.to_string();
        }
        let parts: Vec<&str> = if path == "$" {
            vec![]
        } else {
            path[2..].split('.').collect()
        };
        let mut current = json_str.trim();
        for part in parts {
            // array index: key[n]
            let (key, idx) = if let Some(br) = part.find('[') {
                let k = &part[..br];
                let i = part[br+1..part.len()-1].parse::<usize>().ok();
                (k, i)
            } else {
                (part, None)
            };
            // find key in object
            if current.starts_with('{') {
                let search = format!("\"{}\":", key);
                if let Some(pos) = current.find(&search) {
                    let val_start = pos + search.len();
                    current = current[val_start..].trim_start();
                    current = Self::json_take_value(current);
                } else {
                    return NULL_VALUE.to_string();
                }
            } else {
                return NULL_VALUE.to_string();
            }
            if let Some(i) = idx {
                // current should be an array
                if current.starts_with('[') {
                    let items = Self::json_array_items(&current[1..current.len()-1]);
                    if let Some(item) = items.get(i) {
                        current = item;
                    } else {
                        return NULL_VALUE.to_string();
                    }
                } else {
                    return NULL_VALUE.to_string();
                }
            }
        }
        current.to_string()
    }

    fn json_take_value(s: &str) -> &str {
        let s = s.trim_start();
        if s.starts_with('"') {
            let mut i = 1;
            let bytes = s.as_bytes();
            while i < bytes.len() {
                if bytes[i] == b'\\' { i += 2; continue; }
                if bytes[i] == b'"' { return &s[..i+1]; }
                i += 1;
            }
            s
        } else if s.starts_with('{') || s.starts_with('[') {
            let open = if s.starts_with('{') { (b'{', b'}') } else { (b'[', b']') };
            let mut depth = 0i32;
            let mut in_str = false;
            for (i, &b) in s.as_bytes().iter().enumerate() {
                if in_str {
                    if b == b'\\' { continue; }
                    if b == b'"' { in_str = false; }
                } else {
                    if b == b'"' { in_str = true; }
                    else if b == open.0 { depth += 1; }
                    else if b == open.1 { depth -= 1; if depth == 0 { return &s[..i+1]; } }
                }
            }
            s
        } else {
            // number, bool, null — take until comma, }, ]
            let end = s.find(|c: char| c == ',' || c == '}' || c == ']').unwrap_or(s.len());
            s[..end].trim_end()
        }
    }

    fn json_array_items(inner: &str) -> Vec<&str> {
        let mut items = Vec::new();
        let mut s = inner.trim();
        while !s.is_empty() {
            let item = Self::json_take_value(s);
            items.push(item.trim());
            s = s[item.len()..].trim_start();
            if s.starts_with(',') { s = s[1..].trim_start(); }
        }
        items
    }

    fn format_returning_rows(rows: &[Row], cols: &[SelectColumn]) -> String {
        if rows.is_empty() { return "(0 rows)".to_string(); }
        let headers: Vec<(String, String)> = if cols.iter().any(|c| matches!(c, SelectColumn::All)) {
            rows[0].keys().filter(|k| !k.starts_with('_')).map(|k| (k.clone(), k.clone())).collect()
        } else {
            cols.iter().filter_map(|c| match c {
                SelectColumn::Column(name)            => Some((name.clone(), name.clone())),
                SelectColumn::ColumnAlias(name, alias) => Some((alias.clone(), name.clone())),
                _ => None,
            }).collect()
        };
        let data: Vec<Vec<String>> = rows.iter().map(|row| {
            headers.iter().map(|(_, key)| row.get(key).cloned().unwrap_or_else(|| NULL_VALUE.to_string())).collect()
        }).collect();
        let widths: Vec<usize> = headers.iter().enumerate().map(|(i, (h, _))| {
            let mv = data.iter().map(|r| r.get(i).map(|s| s.len()).unwrap_or(0)).max().unwrap_or(0);
            h.len().max(mv)
        }).collect();
        let sep = format!("+{}+", widths.iter().map(|w| "-".repeat(w + 2)).collect::<Vec<_>>().join("+"));
        let mut out = String::new();
        out.push_str(&sep); out.push('\n');
        let header_line = headers.iter().zip(&widths).map(|((h, _), w)| format!(" {:width$} ", h, width = w)).collect::<Vec<_>>().join("|");
        out.push_str(&format!("|{}|\n", header_line));
        out.push_str(&sep); out.push('\n');
        for row_vals in &data {
            let row_line = row_vals.iter().zip(&widths).map(|(v, w)| format!(" {:width$} ", v, width = w)).collect::<Vec<_>>().join("|");
            out.push_str(&format!("|{}|\n", row_line));
        }
        out.push_str(&sep);
        out
    }

    fn agg_label(func: &AggFunc, col: &str) -> String {
        match func {
            AggFunc::Count        => format!("COUNT({})", col),
            AggFunc::CountDistinct=> format!("COUNT(DISTINCT {})", col),
            AggFunc::Sum          => format!("SUM({})", col),
            AggFunc::SumDistinct  => format!("SUM(DISTINCT {})", col),
            AggFunc::Avg          => format!("AVG({})", col),
            AggFunc::AvgDistinct  => format!("AVG(DISTINCT {})", col),
            AggFunc::Min          => format!("MIN({})", col),
            AggFunc::Max          => format!("MAX({})", col),
            AggFunc::Stddev       => format!("STDDEV({})", col),
            AggFunc::Variance     => format!("VARIANCE({})", col),
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
        &mut self,
        s: &mut SharedDatabase,
        mut result: Vec<Row>,
        columns: Vec<SelectColumn>,
        table: String,
        joins: Vec<Join>,
    ) -> Result<String, String> {
        if result.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        // Pre-compute scalar subqueries and inject as __sq_N__ keys into each row
        {
            let mut sq_idx = 0usize;
            let sq_queries: Vec<(usize, Statement)> = columns.iter().filter_map(|c| {
                if let SelectColumn::Subquery { query, .. } = c {
                    let idx = sq_idx;
                    sq_idx += 1;
                    Some((idx, *query.clone()))
                } else { None }
            }).collect();
            for row in result.iter_mut() {
                for (idx, query) in &sq_queries {
                    let val = if let Statement::Select {
                        table: st, subquery, distinct, columns: sub_cols,
                        condition: sub_cond, joins: sub_joins, order_by,
                        group_by, having, limit, offset, ..
                    } = query.clone() {
                        let sub_cond = sub_cond.map(|c| Self::substitute_correlated_condexpr(&c, row));
                        match self.exec_select(
                            s, st, subquery, distinct, sub_cols, sub_cond,
                            sub_joins, order_by, group_by, having, limit, offset, false, false
                        ) {
                            Ok(output) => {
                                let vals = self.extract_values_from_output(&output);
                                vals.into_iter().next().unwrap_or_else(|| NULL_VALUE.to_string())
                            }
                            Err(_) => NULL_VALUE.to_string(),
                        }
                    } else { NULL_VALUE.to_string() };
                    row.insert(format!("__sq_{}__", idx), val);
                }
            }
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
            let mut sq_idx_col = 0usize;
            columns.iter().filter_map(|c| {
                match c {
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
                SelectColumn::WinFunc { func, alias, .. } => {
                    let header = alias.clone().unwrap_or_else(|| match func {
                        WindowFunc::RowNumber   => "row_number".to_string(),
                        WindowFunc::Rank        => "rank".to_string(),
                        WindowFunc::DenseRank   => "dense_rank".to_string(),
                        WindowFunc::Lag         => "lag".to_string(),
                        WindowFunc::Lead        => "lead".to_string(),
                        WindowFunc::FirstValue  => "first_value".to_string(),
                        WindowFunc::LastValue   => "last_value".to_string(),
                        WindowFunc::NthValue    => "nth_value".to_string(),
                        WindowFunc::Ntile       => "ntile".to_string(),
                        WindowFunc::PercentRank => "percent_rank".to_string(),
                        WindowFunc::CumeDist    => "cume_dist".to_string(),
                        WindowFunc::Sum         => "sum".to_string(),
                        WindowFunc::Avg         => "avg".to_string(),
                        WindowFunc::Count       => "count".to_string(),
                        WindowFunc::Min         => "min".to_string(),
                        WindowFunc::Max         => "max".to_string(),
                    });
                    Some((header.clone(), ColSource::Key(header)))
                }
                SelectColumn::Subquery { alias, .. } => {
                    let key = format!("__sq_{}__", sq_idx_col);
                    sq_idx_col += 1;
                    let header = alias.clone().unwrap_or_else(|| "(subquery)".to_string());
                    Some((header, ColSource::Key(key)))
                }
                SelectColumn::All => None,
                }
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

    fn frame_bounds(pos: usize, len: usize, frame: &Option<crate::parser::ast::WindowFrame>, has_order: bool) -> (usize, usize) {
        use crate::parser::ast::FrameBound;
        if let Some(f) = frame {
            let resolve = |b: &FrameBound| -> usize {
                match b {
                    FrameBound::UnboundedPreceding => 0,
                    FrameBound::Preceding(n)       => pos.saturating_sub(*n),
                    FrameBound::CurrentRow         => pos,
                    FrameBound::Following(n)       => (pos + n).min(len.saturating_sub(1)),
                    FrameBound::UnboundedFollowing => len.saturating_sub(1),
                }
            };
            (resolve(&f.start), resolve(&f.end))
        } else if has_order {
            (0, pos)
        } else {
            (0, len.saturating_sub(1))
        }
    }

    /// 윈도우 함수 계산: 각 WinFunc SelectColumn에 대해 결과값을 행에 삽입한다.
    fn compute_window_functions(mut rows: Vec<Row>, columns: &[SelectColumn]) -> Vec<Row> {
        for col in columns {
            let (func, wf_col, wf_offset, partition_by, win_order_by, alias, frame) = match col {
                SelectColumn::WinFunc { func, col, offset, partition_by, order_by, alias, frame } =>
                    (func, col, *offset, partition_by, order_by, alias, frame),
                _ => continue,
            };
            let label = alias.clone().unwrap_or_else(|| match func {
                WindowFunc::RowNumber   => "row_number".to_string(),
                WindowFunc::Rank        => "rank".to_string(),
                WindowFunc::DenseRank   => "dense_rank".to_string(),
                WindowFunc::Lag         => "lag".to_string(),
                WindowFunc::Lead        => "lead".to_string(),
                WindowFunc::FirstValue  => "first_value".to_string(),
                WindowFunc::LastValue   => "last_value".to_string(),
                WindowFunc::NthValue    => "nth_value".to_string(),
                WindowFunc::Ntile       => "ntile".to_string(),
                WindowFunc::PercentRank => "percent_rank".to_string(),
                WindowFunc::CumeDist    => "cume_dist".to_string(),
                WindowFunc::Sum         => "sum".to_string(),
                WindowFunc::Avg         => "avg".to_string(),
                WindowFunc::Count       => "count".to_string(),
                WindowFunc::Min         => "min".to_string(),
                WindowFunc::Max         => "max".to_string(),
            });

            let n = rows.len();
            let mut values: Vec<String> = vec![String::new(); n];

            // 파티션 그룹핑 (삽입 순서 유지)
            let mut partition_order: Vec<Vec<String>> = Vec::new();
            let mut partition_map: std::collections::HashMap<Vec<String>, Vec<usize>> =
                std::collections::HashMap::new();
            for (i, row) in rows.iter().enumerate() {
                let pk: Vec<String> = partition_by.iter()
                    .map(|c| Self::get_col(row, c).cloned().unwrap_or_default())
                    .collect();
                if !partition_map.contains_key(&pk) { partition_order.push(pk.clone()); }
                partition_map.entry(pk).or_default().push(i);
            }

            for pk in &partition_order {
                let part_indices = &partition_map[pk];

                // 윈도우 ORDER BY 기준 정렬
                let mut sorted: Vec<usize> = part_indices.clone();
                if !win_order_by.is_empty() {
                    sorted.sort_by(|&a, &b| {
                        for ord in win_order_by {
                            let av = Self::get_col(&rows[a], &ord.column).cloned().unwrap_or_default();
                            let bv = Self::get_col(&rows[b], &ord.column).cloned().unwrap_or_default();
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

                match func {
                    WindowFunc::RowNumber => {
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            values[row_idx] = (pos + 1).to_string();
                        }
                    }
                    WindowFunc::Rank => {
                        let mut rank = 1usize;
                        let mut i = 0;
                        while i < sorted.len() {
                            let mut j = i;
                            while j + 1 < sorted.len()
                                && Self::win_order_eq(&rows[sorted[i]], &rows[sorted[j + 1]], win_order_by)
                            {
                                j += 1;
                            }
                            for k in i..=j { values[sorted[k]] = rank.to_string(); }
                            rank += j - i + 1;
                            i = j + 1;
                        }
                    }
                    WindowFunc::DenseRank => {
                        let mut rank = 1usize;
                        let mut i = 0;
                        while i < sorted.len() {
                            let mut j = i;
                            while j + 1 < sorted.len()
                                && Self::win_order_eq(&rows[sorted[i]], &rows[sorted[j + 1]], win_order_by)
                            {
                                j += 1;
                            }
                            for k in i..=j { values[sorted[k]] = rank.to_string(); }
                            rank += 1;
                            i = j + 1;
                        }
                    }
                    WindowFunc::Lag => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let off = wf_offset.unsigned_abs() as usize;
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            values[row_idx] = if pos >= off {
                                Self::get_col(&rows[sorted[pos - off]], col_name)
                                    .cloned().unwrap_or_else(|| "NULL".to_string())
                            } else {
                                "NULL".to_string()
                            };
                        }
                    }
                    WindowFunc::Lead => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let off = wf_offset.unsigned_abs() as usize;
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            values[row_idx] = if pos + off < sorted.len() {
                                Self::get_col(&rows[sorted[pos + off]], col_name)
                                    .cloned().unwrap_or_else(|| "NULL".to_string())
                            } else {
                                "NULL".to_string()
                            };
                        }
                    }
                    WindowFunc::FirstValue => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (start, _) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            values[row_idx] = Self::get_col(&rows[sorted[start]], col_name)
                                .cloned().unwrap_or_else(|| "NULL".to_string());
                        }
                    }
                    WindowFunc::LastValue => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (_, end) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            values[row_idx] = Self::get_col(&rows[sorted[end]], col_name)
                                .cloned().unwrap_or_else(|| "NULL".to_string());
                        }
                    }
                    WindowFunc::NthValue => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        let nth = (wf_offset.max(1) as usize) - 1;
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (start, end) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            values[row_idx] = sorted.get(start + nth)
                                .filter(|_| start + nth <= end)
                                .and_then(|&i| Self::get_col(&rows[i], col_name).cloned())
                                .unwrap_or_else(|| "NULL".to_string());
                        }
                    }
                    WindowFunc::Sum => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (start, end) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            let sum: f64 = sorted[start..=end].iter()
                                .filter_map(|&i| Self::get_col(&rows[i], col_name)?.parse::<f64>().ok())
                                .sum();
                            values[row_idx] = Self::format_arith_result(sum);
                        }
                    }
                    WindowFunc::Avg => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (start, end) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            let vals: Vec<f64> = sorted[start..=end].iter()
                                .filter_map(|&i| Self::get_col(&rows[i], col_name)?.parse::<f64>().ok())
                                .collect();
                            values[row_idx] = if vals.is_empty() { "NULL".to_string() }
                                else { Self::format_arith_result(vals.iter().sum::<f64>() / vals.len() as f64) };
                        }
                    }
                    WindowFunc::Count => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (start, end) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            let count = if col_name == "*" {
                                end - start + 1
                            } else {
                                sorted[start..=end].iter()
                                    .filter(|&&i| Self::get_col(&rows[i], col_name)
                                        .map(|v| v != "NULL" && !v.is_empty()).unwrap_or(false))
                                    .count()
                            };
                            values[row_idx] = count.to_string();
                        }
                    }
                    WindowFunc::Min => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (start, end) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            let min_val = sorted[start..=end].iter()
                                .filter_map(|&i| Self::get_col(&rows[i], col_name).cloned())
                                .filter(|v| v != "NULL" && !v.is_empty())
                                .min_by(|a, b| match (a.parse::<f64>(), b.parse::<f64>()) {
                                    (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(std::cmp::Ordering::Equal),
                                    _ => a.cmp(b),
                                });
                            values[row_idx] = min_val.unwrap_or_else(|| "NULL".to_string());
                        }
                    }
                    WindowFunc::Max => {
                        let col_name = wf_col.as_deref().unwrap_or("");
                        let has_order = !win_order_by.is_empty();
                        for (pos, &row_idx) in sorted.iter().enumerate() {
                            let (start, end) = Self::frame_bounds(pos, sorted.len(), frame, has_order);
                            let max_val = sorted[start..=end].iter()
                                .filter_map(|&i| Self::get_col(&rows[i], col_name).cloned())
                                .filter(|v| v != "NULL" && !v.is_empty())
                                .max_by(|a, b| match (a.parse::<f64>(), b.parse::<f64>()) {
                                    (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(std::cmp::Ordering::Equal),
                                    _ => a.cmp(b),
                                });
                            values[row_idx] = max_val.unwrap_or_else(|| "NULL".to_string());
                        }
                    }
                    WindowFunc::Ntile => {
                        let n_buckets = wf_offset.max(1) as usize;
                        let total = sorted.len();
                        if total > 0 {
                            let n_eff = n_buckets.min(total);
                            for (pos, &row_idx) in sorted.iter().enumerate() {
                                values[row_idx] = (pos * n_eff / total + 1).to_string();
                            }
                        }
                    }
                    WindowFunc::PercentRank => {
                        let total = sorted.len();
                        if total <= 1 {
                            for &row_idx in &sorted { values[row_idx] = "0.0000".to_string(); }
                        } else {
                            // RANK 계산 후 (rank-1)/(total-1)
                            let mut ranks = vec![1usize; total];
                            let mut i = 0;
                            while i < sorted.len() {
                                let rank = i + 1;
                                let mut j = i;
                                while j + 1 < sorted.len()
                                    && Self::win_order_eq(&rows[sorted[i]], &rows[sorted[j + 1]], win_order_by)
                                {
                                    j += 1;
                                }
                                for k in i..=j { ranks[k] = rank; }
                                i = j + 1;
                            }
                            for (pos, &row_idx) in sorted.iter().enumerate() {
                                let pr = (ranks[pos] - 1) as f64 / (total - 1) as f64;
                                values[row_idx] = format!("{:.4}", pr);
                            }
                        }
                    }
                    WindowFunc::CumeDist => {
                        let total = sorted.len();
                        if total > 0 {
                            let mut i = 0;
                            while i < sorted.len() {
                                let mut j = i;
                                while j + 1 < sorted.len()
                                    && Self::win_order_eq(&rows[sorted[i]], &rows[sorted[j + 1]], win_order_by)
                                {
                                    j += 1;
                                }
                                let cd = (j + 1) as f64 / total as f64;
                                for k in i..=j { values[sorted[k]] = format!("{:.4}", cd); }
                                i = j + 1;
                            }
                        }
                    }
                }
            }

            for (i, row) in rows.iter_mut().enumerate() {
                row.insert(label.clone(), values[i].clone());
            }
        }
        rows
    }

    /// 윈도우 ORDER BY 기준으로 두 행이 동일한 순서값인지 확인 (RANK/DENSE_RANK 동점 판별)
    fn win_order_eq(a: &Row, b: &Row, order_by: &[OrderBy]) -> bool {
        order_by.iter().all(|ord| {
            let av = Self::get_col(a, &ord.column).cloned().unwrap_or_default();
            let bv = Self::get_col(b, &ord.column).cloned().unwrap_or_default();
            av == bv
        })
    }

    fn exec_update(
        &mut self,
        s: &mut SharedDatabase,
        table: String,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
        returning: Option<Vec<SelectColumn>>,
    ) -> Result<String, String> {
        // Updatable view: merge view WHERE with query WHERE, redirect to base table
        if s.views.contains_key(&table) {
            match Self::resolve_updatable_view(s, &table) {
                Some((base_table, view_cond)) => {
                    let merged_cond = Self::merge_conditions(view_cond, condition);
                    return self.exec_update(s, base_table, assignments, merged_cond, returning);
                }
                None => return Err(format!("View '{}' is not updatable", Self::strip_db_prefix(&table))),
            }
        }
        let committed_backups: Option<Vec<(String, Vec<Row>)>> = if self.txn.is_active() {
            let main = self.session_swap_in(s, &table);
            let mut backups = vec![(table.clone(), main)];
            // FK CASCADE 대상 테이블도 세션 상태로 스왑
            let fk_tables: Vec<String> = s.catalog.tables.iter()
                .filter(|(tname, schema)| *tname != &table && schema.columns.iter().any(|c|
                    c.foreign_key.as_ref().map(|fk|
                        fk.ref_table == table &&
                        !matches!(fk.on_update, crate::catalog::schema::FkAction::Restrict)
                    ).unwrap_or(false)))
                .map(|(tname, _)| tname.clone())
                .collect();
            for t in fk_tables {
                let c = self.session_swap_in(s, &t);
                backups.push((t, c));
            }
            Some(backups)
        } else {
            None
        };
        self.fire_triggers(s, &table, "BEFORE", "UPDATE");
        let result = self.exec_update_inner(s, table.clone(), assignments, condition, returning);
        if let Some(backups) = committed_backups {
            for (tname, committed) in backups {
                self.session_swap_out(s, &tname, committed);
            }
        }
        if result.is_ok() { self.fire_triggers(s, &table, "AFTER", "UPDATE"); }
        result
    }

    fn exec_update_inner(
        &mut self,
        s: &mut SharedDatabase,
        table: String,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
        returning: Option<Vec<SelectColumn>>,
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
                                        if !self.txn.is_active() {
                                            let rows_clone2 = s.tables.get(other_table).unwrap().clone();
                                            s.buffer_pool.write_page(other_table, rows_clone2);
                                            s.buffer_pool.flush_page(other_table, &s.disk);
                                        }
                                    }
                                    crate::catalog::schema::FkAction::SetNull => {
                                        if let Some(other_rows) = s.tables.get_mut(other_table) {
                                            for row in other_rows.iter_mut() {
                                                if Self::is_visible(row) && row.get(&col.name).map(|v| v == &old_val).unwrap_or(false) {
                                                    row.insert(col.name.clone(), NULL_VALUE.to_string());
                                                }
                                            }
                                        }
                                        if !self.txn.is_active() {
                                            let rows_clone2 = s.tables.get(other_table).unwrap().clone();
                                            s.buffer_pool.write_page(other_table, rows_clone2);
                                            s.buffer_pool.flush_page(other_table, &s.disk);
                                        }
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
                                        if !self.txn.is_active() {
                                            let rows_clone2 = s.tables.get(other_table).unwrap().clone();
                                            s.buffer_pool.write_page(other_table, rows_clone2);
                                            s.buffer_pool.flush_page(other_table, &s.disk);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if !self.txn.is_active() {
            let rows = s.tables.get(&table).unwrap().clone();
            s.buffer_pool.write_page(&table, rows);
            s.buffer_pool.flush_page(&table, &s.disk);
            Self::maybe_auto_vacuum(s);
        }
        self.maybe_auto_checkpoint(s);
        if let Some(ret_cols) = returning {
            let updated_rows: Vec<Row> = s.tables.get(&table).unwrap().iter()
                .filter(|r| Self::is_visible(r) && matching_pks.contains(&r.get(&pk_col).cloned().unwrap_or_default()))
                .cloned().collect();
            Ok(Self::format_returning_rows(&updated_rows, &ret_cols))
        } else {
            Ok(format!("{} row(s) updated.", count))
        }
    }

    fn exec_delete(&mut self, s: &mut SharedDatabase, table: String, condition: Option<CondExpr>, returning: Option<Vec<SelectColumn>>) -> Result<String, String> {
        // Updatable view: merge view WHERE with query WHERE, redirect to base table
        if s.views.contains_key(&table) {
            match Self::resolve_updatable_view(s, &table) {
                Some((base_table, view_cond)) => {
                    let merged_cond = Self::merge_conditions(view_cond, condition);
                    return self.exec_delete(s, base_table, merged_cond, returning);
                }
                None => return Err(format!("View '{}' is not updatable", Self::strip_db_prefix(&table))),
            }
        }
        let committed_backups: Option<Vec<(String, Vec<Row>)>> = if self.txn.is_active() {
            let main = self.session_swap_in(s, &table);
            let mut backups = vec![(table.clone(), main)];
            let fk_tables: Vec<String> = s.catalog.tables.iter()
                .filter(|(tname, schema)| *tname != &table && schema.columns.iter().any(|c|
                    c.foreign_key.as_ref().map(|fk|
                        fk.ref_table == table &&
                        !matches!(fk.on_delete, crate::catalog::schema::FkAction::Restrict)
                    ).unwrap_or(false)))
                .map(|(tname, _)| tname.clone())
                .collect();
            for t in fk_tables {
                let c = self.session_swap_in(s, &t);
                backups.push((t, c));
            }
            Some(backups)
        } else {
            None
        };
        self.fire_triggers(s, &table, "BEFORE", "DELETE");
        let result = self.exec_delete_inner(s, table.clone(), condition, returning);
        if let Some(backups) = committed_backups {
            for (tname, committed) in backups {
                self.session_swap_out(s, &tname, committed);
            }
        }
        if result.is_ok() { self.fire_triggers(s, &table, "AFTER", "DELETE"); }
        result
    }

    fn exec_delete_inner(&mut self, s: &mut SharedDatabase, table: String, condition: Option<CondExpr>, returning: Option<Vec<SelectColumn>>) -> Result<String, String> {
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
                                    if !self.txn.is_active() {
                                        let rows_clone = s.tables.get(other_table).unwrap().clone();
                                        s.buffer_pool.write_page(other_table, rows_clone);
                                        s.buffer_pool.flush_page(other_table, &s.disk);
                                    }
                                }
                                crate::catalog::schema::FkAction::SetNull => {
                                    if let Some(other_rows) = s.tables.get_mut(other_table) {
                                        for row in other_rows.iter_mut() {
                                            if Self::is_visible(row) && row.get(&col.name).map(|v| v == &del_val).unwrap_or(false) {
                                                row.insert(col.name.clone(), NULL_VALUE.to_string());
                                            }
                                        }
                                    }
                                    if !self.txn.is_active() {
                                        let rows_clone = s.tables.get(other_table).unwrap().clone();
                                        s.buffer_pool.write_page(other_table, rows_clone);
                                        s.buffer_pool.flush_page(other_table, &s.disk);
                                    }
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
                                    if !self.txn.is_active() {
                                        let rows_clone = s.tables.get(other_table).unwrap().clone();
                                        s.buffer_pool.write_page(other_table, rows_clone);
                                        s.buffer_pool.flush_page(other_table, &s.disk);
                                    }
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
            Self::maybe_auto_vacuum(s);
        // 트랜잭션 중에는 버퍼 풀 갱신 생략 (COMMIT 시 일괄 처리)
        }

        self.maybe_auto_checkpoint(s);
        if let Some(ret_cols) = returning {
            Ok(Self::format_returning_rows(&rows_to_delete, &ret_cols))
        } else {
            Ok(format!("{} row(s) deleted.", deleted))
        }
    }

    /// 트랜잭션 시작 시 해당 테이블의 커밋 상태를 session_tables로 스왑.
    /// 반환값: s.tables에서 꺼낸 커밋 상태 (나중에 swap_out에 전달).
    fn session_swap_in(&mut self, s: &mut SharedDatabase, table: &str) -> Vec<Row> {
        let committed = s.tables.get(table).cloned().unwrap_or_default();
        let working = self.session_tables.remove(table).unwrap_or_else(|| committed.clone());
        s.tables.insert(table.to_string(), working);
        committed
    }

    /// DML 완료 후 수정된 세션 상태를 session_tables에 저장, s.tables를 커밋 상태로 복원.
    fn session_swap_out(&mut self, s: &mut SharedDatabase, table: &str, committed: Vec<Row>) {
        let modified = s.tables.remove(table).unwrap_or_default();
        self.session_tables.insert(table.to_string(), modified);
        s.tables.insert(table.to_string(), committed);
    }

    fn exec_begin(&mut self, s: &SharedDatabase) -> Result<String, String> {
        self.session_tables.clear();
        let txn_id = self.txn.begin_with_snapshot(&s.tables)?;
        let level = format!("{:?}", self.txn.isolation_level);
        Ok(format!("Transaction {} started. (isolation: {})", txn_id, level))
    }

    fn exec_commit(&mut self, s: &mut SharedDatabase) -> Result<String, String> {
        // SERIALIZABLE: 커밋 전 팬텀 읽기 검증
        if let Err(e) = self.txn.validate_serializable(&s.tables) {
            self.apply_rollback(s);
            return Err(format!("{} (auto-rolled back)", e));
        }

        // session_tables → s.tables 적용 + 버퍼 풀 갱신 + 디스크 flush
        let session_data: Vec<(String, Vec<Row>)> = self.session_tables.drain().collect();
        for (table, rows) in session_data {
            s.tables.insert(table.clone(), rows.clone());
            s.buffer_pool.write_page(&table, rows);
            s.buffer_pool.flush_page(&table, &s.disk);
        }

        let txn_id = self.txn.current_txn_id();
        self.txn.commit()?;
        s.lock_mgr.release(txn_id);
        Self::maybe_auto_vacuum(s);
        Ok("Transaction committed.".to_string())
    }

    /// 롤백 공통 헬퍼: session_tables를 폐기하고 인덱스를 커밋 상태로 복원한다.
    fn apply_rollback(&mut self, s: &mut SharedDatabase) {
        let txn_id = self.txn.current_txn_id();
        let _ = self.txn.abort().ok();
        s.lock_mgr.release(txn_id);

        // session_tables에 있는 모든 수정 테이블의 인덱스를 커밋 상태(s.tables)로 복원
        // (트랜잭션 중 exec_update_inner/delete_inner가 s.indexes를 갱신하므로 ROLLBACK 시 필요)
        let dirty_tables: Vec<String> = self.session_tables.keys().cloned().collect();
        for table in &dirty_tables {
            if let Some(rows) = s.tables.get(table) {
                let rows_clone = rows.clone();

                // PK 인덱스 복원
                let pk_col = s.catalog.get_table(table)
                    .and_then(|sc| sc.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                    .unwrap_or_else(|| "id".to_string());
                if let Some(index) = s.indexes.get_mut(table) {
                    *index = BPlusTree::new();
                    for row in &rows_clone {
                        let k = row.get(&pk_col).cloned().unwrap_or_default();
                        index.insert(k, serde_json::to_string(row).unwrap());
                    }
                }

                // 보조 인덱스 복원
                let sec: Vec<(String, String)> = s.index_meta.iter()
                    .filter(|(_, (tbl, _))| tbl == table)
                    .map(|(name, (_, col))| (name.clone(), col.clone()))
                    .collect();
                for (idx_name, col) in sec {
                    let mut bucket: HashMap<String, Vec<Row>> = HashMap::new();
                    for row in &rows_clone {
                        if let Some(val) = row.get(&col) {
                            bucket.entry(val.clone()).or_default().push(row.clone());
                        }
                    }
                    let mut tree = BPlusTree::new();
                    for (key, bucket_rows) in bucket {
                        tree.insert(key, serde_json::to_string(&bucket_rows).unwrap());
                    }
                    s.indexes.insert(format!("{}_{}", table, idx_name), tree);
                }

                // 복합 인덱스 복원
                let comp_keys: Vec<String> = s.composite_indexes.iter()
                    .filter(|(_, ci)| &ci.table == table)
                    .map(|(k, _)| k.clone())
                    .collect();
                for k in comp_keys {
                    if let Some(ci) = s.composite_indexes.get_mut(&k) {
                        ci.rebuild(&rows_clone);
                    }
                }
            }
        }

        self.session_tables.clear();
    }

    fn exec_rollback(&mut self, s: &mut SharedDatabase) -> Result<String, String> {
        self.apply_rollback(s);
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
            let pk_col = s.catalog.get_table(&entry.table)
                .and_then(|schema| schema.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
                .unwrap_or_else(|| "id".to_string());
            match entry.operation.as_str() {
                "INSERT" => {
                    if let Some(rows) = self.session_tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get(&pk_col).map(|v| v != &entry.key).unwrap_or(true));
                    }
                }
                "UPDATE" => {
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            if let Some(rows) = self.session_tables.get_mut(&entry.table) {
                                for row in rows.iter_mut() {
                                    if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                        *row = old_row.clone();
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                "DELETE" => {
                    // exec_delete_inner은 트랜잭션 내에서 MVCC 논리 삭제(_xmax 세팅)를 하므로
                    // 롤백은 session_tables 내 해당 행의 _xmax를 "0"으로 복원
                    if let Some(rows) = self.session_tables.get_mut(&entry.table) {
                        for row in rows.iter_mut() {
                            if row.get(&pk_col).map(|v| v == &entry.key).unwrap_or(false) {
                                row.insert("_xmax".to_string(), "0".to_string());
                            }
                        }
                    }
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
                                DataType::Int | DataType::SmallInt | DataType::TinyInt
                                    => val.parse::<i64>().is_ok(),
                                DataType::BigInt => val.parse::<i64>().is_ok(),
                                DataType::Float => val.parse::<f64>().is_ok(),
                                DataType::Boolean => matches!(val.to_lowercase().as_str(), "true" | "false" | "1" | "0"),
                                DataType::Text | DataType::Varchar(_) | DataType::Date
                                | DataType::DateTime | DataType::Timestamp => true,
                                DataType::Decimal(_, _) => val.parse::<f64>().is_ok(),
                                DataType::Double => val.parse::<f64>().is_ok(),
                                DataType::Time | DataType::Year | DataType::Blob => true,
                                DataType::Json => true,
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
            AlterAction::AddForeignKey { name, column, ref_table, ref_column, on_delete, on_update } => {
                let schema = s.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                let col = schema.columns.iter_mut()
                    .find(|c| c.name == column)
                    .ok_or(format!("Column '{}' not found in '{}'", column, table))?;
                col.foreign_key = Some(crate::catalog::schema::ForeignKey {
                    column: column.clone(),
                    ref_table: ref_table.clone(),
                    ref_column: ref_column.clone(),
                    on_delete: match on_delete {
                        FkAction::Cascade   => crate::catalog::schema::FkAction::Cascade,
                        FkAction::Restrict  => crate::catalog::schema::FkAction::Restrict,
                        FkAction::SetNull   => crate::catalog::schema::FkAction::SetNull,
                        FkAction::SetDefault => crate::catalog::schema::FkAction::SetDefault,
                    },
                    on_update: match on_update {
                        FkAction::Cascade   => crate::catalog::schema::FkAction::Cascade,
                        FkAction::Restrict  => crate::catalog::schema::FkAction::Restrict,
                        FkAction::SetNull   => crate::catalog::schema::FkAction::SetNull,
                        FkAction::SetDefault => crate::catalog::schema::FkAction::SetDefault,
                    },
                });
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                let constraint_label = name.unwrap_or_else(|| format!("fk_{}", column));
                Ok(format!("Foreign key '{}' added to '{}'.", constraint_label, table))
            }
            AlterAction::DropForeignKey(fk_name) => {
                let schema = s.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                let mut found = false;
                for col in schema.columns.iter_mut() {
                    if col.foreign_key.is_some() {
                        let matches = col.foreign_key.as_ref().map(|fk| {
                            fk.column == fk_name || format!("fk_{}", fk.column) == fk_name
                        }).unwrap_or(false);
                        if matches {
                            col.foreign_key = None;
                            found = true;
                            break;
                        }
                    }
                }
                if !found {
                    return Err(format!("Foreign key '{}' not found in '{}'", fk_name, table));
                }
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                Ok(format!("Foreign key '{}' dropped from '{}'.", fk_name, table))
            }
            AlterAction::AddUniqueConstraint { name, column } => {
                let schema = s.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                let col = schema.columns.iter_mut()
                    .find(|c| c.name == column)
                    .ok_or(format!("Column '{}' not found in '{}'", column, table))?;
                col.unique = true;
                col.unique_constraint_name = name.clone();
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                let constraint_label = name.unwrap_or_else(|| format!("uq_{}", column));
                Ok(format!("Unique constraint '{}' added to '{}'.'{}'.", constraint_label, table, column))
            }
            AlterAction::AddCheckConstraint { name, expr } => {
                let schema = s.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                schema.check_constraints.push(crate::catalog::schema::CheckConstraint {
                    name: name.clone(),
                    expression: expr.clone(),
                });
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                let constraint_label = name.unwrap_or_else(|| expr.clone());
                Ok(format!("Check constraint '{}' added to '{}'.", constraint_label, table))
            }
            AlterAction::DropConstraint(constraint_name) => {
                let schema = s.catalog.tables.get_mut(&table)
                    .ok_or(format!("Table '{}' not found", table))?;
                let before = schema.check_constraints.len();
                schema.check_constraints.retain(|c| {
                    c.name.as_deref() != Some(&constraint_name) && c.expression != constraint_name
                });
                let removed_check = schema.check_constraints.len() < before;
                if !removed_check {
                    // Try unique constraint
                    for col in schema.columns.iter_mut() {
                        if col.unique_constraint_name.as_deref() == Some(&constraint_name) {
                            col.unique = false;
                            col.unique_constraint_name = None;
                            let full_schema = s.catalog.get_table(&table).unwrap();
                            s.disk.save_schema(&table, full_schema);
                            return Ok(format!("Constraint '{}' dropped from '{}'.", constraint_name, table));
                        }
                    }
                    return Err(format!("Constraint '{}' not found in '{}'", constraint_name, table));
                }
                let full_schema = s.catalog.get_table(&table).unwrap();
                s.disk.save_schema(&table, full_schema);
                Ok(format!("Constraint '{}' dropped from '{}'.", constraint_name, table))
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
            if !j.using_cols.is_empty() {
                let using_cols = j.using_cols.clone();
                for left in &current {
                    for right in &right_rows {
                        let matches = using_cols.iter().all(|col| {
                            let lv = left.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                            let rv = right.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                            lv == rv && lv != NULL_VALUE
                        });
                        if matches {
                            let mut merged = left.clone();
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            out.push(merged);
                        }
                    }
                }
                current = out;
                continue;
            }
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
                JoinType::Cross | JoinType::Natural | JoinType::FullOuter => {
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
            if !j.using_cols.is_empty() {
                let using_cols = j.using_cols.clone();
                for left in &current {
                    for right in &right_rows {
                        let matches = using_cols.iter().all(|col| {
                            let lv = left.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                            let rv = right.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                            lv == rv && lv != NULL_VALUE
                        });
                        if matches {
                            let mut merged = left.clone();
                            for (k, v) in right.iter() {
                                merged.insert(format!("{}.{}", tbl, k), v.clone());
                                merged.entry(k.clone()).or_insert_with(|| v.clone());
                            }
                            out.push(merged);
                        }
                    }
                }
                current = out;
                continue;
            }
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
                JoinType::Cross | JoinType::Natural | JoinType::FullOuter => {
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
                            joins, order_by, group_by, having, limit, offset, false, false
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
                    let sub_cond = sub_cond.map(|c| Self::substitute_correlated_condexpr(&c, row));
                    let result = self.exec_select(
                        s, table, subquery, distinct, columns.clone(), sub_cond,
                        joins, order_by, group_by, having, limit, offset, false, false
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

    fn exec_create_view(&mut self, s: &mut SharedDatabase, name: String, query: Statement, raw_sql: String) -> Result<String, String> {
        if let Statement::Select { ref table, .. } = query {
            if !s.tables.contains_key(table) {
                return Err(format!("Table '{}' not found", table));
            }
        }
        s.views.insert(name.clone(), query);
        if !raw_sql.is_empty() {
            s.view_raw_sql.insert(name.clone(), raw_sql);
        }
        self.persist_views_for_db(s, &self.current_db.clone());
        Ok(format!("View '{}' created.", name))
    }

    fn exec_drop_view(&mut self, s: &mut SharedDatabase, name: String) -> Result<String, String> {
        if s.views.remove(&name).is_some() {
            s.view_raw_sql.remove(&name);
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
        let db_view_sql: HashMap<String, String> = s.view_raw_sql.iter()
            .filter(|(k, _v)| k.starts_with(&prefix))
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect();
        s.disk.save_view_raw_sql(db, &db_view_sql);
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
            Statement::Select { table, subquery, columns, distinct, condition, joins, order_by, group_by, having, limit, offset, for_update, for_share } =>
                Statement::Select {
                    table: self.qualify_name_with_synonyms(s, table),
                    subquery: subquery.map(|(q, alias)| (Box::new(self.qualify_stmt(s, *q)), alias)),
                    columns: columns.into_iter().map(|c| match c {
                        SelectColumn::Subquery { query, alias } => SelectColumn::Subquery {
                            query: Box::new(self.qualify_stmt(s, *query)),
                            alias,
                        },
                        other => other,
                    }).collect(),
                    distinct,
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                    joins: joins.into_iter().map(|j| Join {
                        table: self.qualify_name_with_synonyms(s, j.table),
                        on_expr: self.qualify_condexpr(s, j.on_expr),
                        join_type: j.join_type,
                        using_cols: j.using_cols,
                    }).collect(),
                    order_by, group_by,
                    having: having.map(|h| self.qualify_condexpr(s, h)),
                    limit, offset, for_update, for_share,
                },
            Statement::Insert { table, columns, values, on_conflict, returning } =>
                Statement::Insert { table: self.qualify_name_with_synonyms(s, table), columns, values, on_conflict, returning },
            Statement::InsertSelect { table, columns, query, on_conflict, returning } =>
                Statement::InsertSelect {
                    table: self.qualify_name_with_synonyms(s, table),
                    columns,
                    query: Box::new(self.qualify_stmt(s, *query)),
                    on_conflict,
                    returning,
                },
            Statement::Update { table, assignments, condition, returning } =>
                Statement::Update {
                    table: self.qualify_name_with_synonyms(s, table),
                    assignments,
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                    returning,
                },
            Statement::Delete { table, condition, returning } =>
                Statement::Delete {
                    table: self.qualify_name_with_synonyms(s, table),
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                    returning,
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
            Statement::CreateView { name, query, raw_sql } =>
                Statement::CreateView {
                    name: self.qualify_name(name),
                    query: Box::new(self.qualify_stmt(s, *query)),
                    raw_sql,
                },
            Statement::DropView { name } =>
                Statement::DropView { name: self.qualify_name(name) },
            Statement::Describe { table } =>
                Statement::Describe { table: self.qualify_name(table) },
            Statement::Vacuum { table } =>
                Statement::Vacuum { table: table.map(|t| self.qualify_name(t)) },
            Statement::AnalyzeTable { table } =>
                Statement::AnalyzeTable { table: self.qualify_name(table) },
            Statement::Union { left, right, all, order_by, limit, offset } =>
                Statement::Union {
                    left:  Box::new(self.qualify_stmt(s, *left)),
                    right: Box::new(self.qualify_stmt(s, *right)),
                    all, order_by, limit, offset,
                },
            Statement::Intersect { left, right, all, order_by, limit, offset } =>
                Statement::Intersect {
                    left:  Box::new(self.qualify_stmt(s, *left)),
                    right: Box::new(self.qualify_stmt(s, *right)),
                    all, order_by, limit, offset,
                },
            Statement::Except { left, right, all, order_by, limit, offset } =>
                Statement::Except {
                    left:  Box::new(self.qualify_stmt(s, *left)),
                    right: Box::new(self.qualify_stmt(s, *right)),
                    all, order_by, limit, offset,
                },
            Statement::ShowCreateTable { table } =>
                Statement::ShowCreateTable { table: self.qualify_name(table) },
            Statement::ShowCreateView { view } =>
                Statement::ShowCreateView { view: self.qualify_name(view) },
            Statement::ShowIndex { table } =>
                Statement::ShowIndex { table: self.qualify_name(table) },
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
            Statement::ExplainAnalyze(inner) =>
                Statement::ExplainAnalyze(Box::new(self.qualify_stmt(s, *inner))),
            Statement::MultiUpdate { tables, joins, assignments, condition } =>
                Statement::MultiUpdate {
                    tables: tables.into_iter().map(|t| self.qualify_name(t)).collect(),
                    joins: joins.into_iter().map(|j| Join {
                        table: self.qualify_name_with_synonyms(s, j.table),
                        on_expr: self.qualify_condexpr(s, j.on_expr),
                        join_type: j.join_type,
                        using_cols: j.using_cols,
                    }).collect(),
                    assignments,
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                },
            Statement::MultiDelete { delete_tables, from_table, joins, condition } =>
                Statement::MultiDelete {
                    delete_tables: delete_tables.into_iter().map(|t| self.qualify_name(t)).collect(),
                    from_table: self.qualify_name(from_table),
                    joins: joins.into_iter().map(|j| Join {
                        table: self.qualify_name_with_synonyms(s, j.table),
                        on_expr: self.qualify_condexpr(s, j.on_expr),
                        join_type: j.join_type,
                        using_cols: j.using_cols,
                    }).collect(),
                    condition: condition.map(|c| self.qualify_condexpr(s, c)),
                },
            Statement::Merge { target, target_alias, source, source_alias, on,
                               when_matched_update, when_matched_delete, when_matched_delete_cond,
                               when_not_matched_columns, when_not_matched_values } =>
                Statement::Merge {
                    target: self.qualify_name(target),
                    target_alias,
                    source: self.qualify_name(source),
                    source_alias,
                    on,
                    when_matched_update,
                    when_matched_delete,
                    when_matched_delete_cond,
                    when_not_matched_columns,
                    when_not_matched_values,
                },
            Statement::CreateTrigger { name, timing, event, table, body } =>
                Statement::CreateTrigger {
                    name,
                    timing,
                    event,
                    table: self.qualify_name(table),
                    body,
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
                crate::parser::ast::DataType::BigInt  => "BIGINT".to_string(),
                crate::parser::ast::DataType::SmallInt => "SMALLINT".to_string(),
                crate::parser::ast::DataType::TinyInt => "TINYINT".to_string(),
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
                crate::parser::ast::DataType::Blob => "BLOB".to_string(),
                crate::parser::ast::DataType::Json => "JSON".to_string(),
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

    fn exec_show_create_table(&self, s: &SharedDatabase, table: String) -> Result<String, String> {
        use crate::parser::ast::DataType;
        use crate::catalog::schema::FkAction;

        let schema = s.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?;

        let bare_name = table.split('.').last().unwrap_or(&table);

        let type_str = |dt: &DataType| -> String {
            match dt {
                DataType::Int       => "INT".to_string(),
                DataType::BigInt    => "BIGINT".to_string(),
                DataType::SmallInt  => "SMALLINT".to_string(),
                DataType::TinyInt   => "TINYINT".to_string(),
                DataType::Text      => "TEXT".to_string(),
                DataType::Float     => "FLOAT".to_string(),
                DataType::Boolean   => "BOOLEAN".to_string(),
                DataType::Date      => "DATE".to_string(),
                DataType::DateTime  => "DATETIME".to_string(),
                DataType::Timestamp => "TIMESTAMP".to_string(),
                DataType::Varchar(n) => format!("VARCHAR({})", n),
                DataType::Decimal(p, sc) => format!("DECIMAL({},{})", p, sc),
                DataType::Double    => "DOUBLE".to_string(),
                DataType::Time      => "TIME".to_string(),
                DataType::Year      => "YEAR".to_string(),
                DataType::Enum(vals) => format!("ENUM({})", vals.iter().map(|v| format!("'{}'", v)).collect::<Vec<_>>().join(", ")),
                DataType::Set(vals)  => format!("SET({})", vals.iter().map(|v| format!("'{}'", v)).collect::<Vec<_>>().join(", ")),
                DataType::Blob      => "BLOB".to_string(),
                DataType::Json      => "JSON".to_string(),
                DataType::Unknown   => "UNKNOWN".to_string(),
            }
        };

        let fk_action_str = |a: &FkAction| -> &str {
            match a {
                FkAction::Restrict   => "RESTRICT",
                FkAction::Cascade    => "CASCADE",
                FkAction::SetNull    => "SET NULL",
                FkAction::SetDefault => "SET DEFAULT",
            }
        };

        let mut lines: Vec<String> = Vec::new();
        let composite_pk = &schema.primary_key_columns;

        for col in &schema.columns {
            let mut parts = vec![format!("`{}`", col.name), type_str(&col.data_type)];
            if col.not_null || col.primary_key { parts.push("NOT NULL".to_string()); }
            if col.auto_increment { parts.push("AUTO_INCREMENT".to_string()); }
            if let Some(def) = &col.default {
                if def != crate::parser::parser::NULL_DEFAULT {
                    let needs_quotes = matches!(col.data_type,
                        DataType::Enum(_) | DataType::Set(_) | DataType::Varchar(_) | DataType::Text)
                        && !def.starts_with('\'') && !def.starts_with('"');
                    let display = if needs_quotes { format!("'{}'", def) } else { def.clone() };
                    parts.push(format!("DEFAULT {}", display));
                }
            }
            // Single-column PK inline (only when no composite PK)
            if col.primary_key && composite_pk.is_empty() {
                parts.push("PRIMARY KEY".to_string());
            }
            if col.unique && col.unique_constraint_name.is_none() {
                parts.push("UNIQUE".to_string());
            }
            lines.push(format!("  {}", parts.join(" ")));
        }

        // Composite PRIMARY KEY
        if !composite_pk.is_empty() {
            let cols_str = composite_pk.iter().map(|c| format!("`{}`", c)).collect::<Vec<_>>().join(", ");
            lines.push(format!("  PRIMARY KEY ({})", cols_str));
        }

        // UNIQUE constraints with names
        for col in &schema.columns {
            if let Some(ref uname) = col.unique_constraint_name {
                lines.push(format!("  UNIQUE KEY `{}` (`{}`)", uname, col.name));
            }
        }

        // FOREIGN KEY constraints
        for col in &schema.columns {
            if let Some(ref fk) = col.foreign_key {
                let ref_bare = fk.ref_table.split('.').last().unwrap_or(&fk.ref_table);
                lines.push(format!(
                    "  FOREIGN KEY (`{}`) REFERENCES `{}`(`{}`) ON DELETE {} ON UPDATE {}",
                    fk.column, ref_bare, fk.ref_column,
                    fk_action_str(&fk.on_delete), fk_action_str(&fk.on_update)
                ));
            }
        }

        // CHECK constraints
        for cc in &schema.check_constraints {
            if let Some(ref name) = cc.name {
                lines.push(format!("  CONSTRAINT `{}` CHECK ({})", name, cc.expression));
            } else {
                lines.push(format!("  CHECK ({})", cc.expression));
            }
        }

        let ddl = format!("CREATE TABLE `{}` (\n{}\n);", bare_name, lines.join(",\n"));
        Ok(format!("Table: {}\n{}", bare_name, ddl))
    }

    fn exec_show_create_view(&self, s: &SharedDatabase, view: String) -> Result<String, String> {
        let q_view = if view.contains('.') { view.clone() }
                     else { format!("{}.{}", self.current_db, view) };
        let bare = view.split('.').last().unwrap_or(&view);

        if s.views.contains_key(&q_view) {
            let select_sql = s.view_raw_sql.get(&q_view)
                .cloned()
                .unwrap_or_else(|| "<view definition not available>".to_string());
            let ddl = format!("CREATE VIEW `{}` AS {}", bare, select_sql);
            Ok(format!("View: {}\nCreate View: {}", bare, ddl))
        } else {
            Err(format!("View '{}' not found", bare))
        }
    }

    fn exec_show_index(&self, s: &SharedDatabase, table: String) -> Result<String, String> {
        let q_table = if table.contains('.') { table.clone() }
                      else { format!("{}.{}", self.current_db, table) };
        let bare = table.split('.').last().unwrap_or(&table);

        if !s.catalog.tables.contains_key(&q_table) {
            return Err(format!("Table '{}' not found", bare));
        }

        let mut rows: Vec<String> = Vec::new();
        rows.push("Table\tKey_name\tColumn_name\tIndex_type".to_string());

        for (idx_name, (t, col)) in &s.index_meta {
            if *t == q_table {
                rows.push(format!("{}\t{}\t{}\tBTREE", bare, idx_name, col));
            }
        }
        for (idx_name, comp) in &s.composite_indexes {
            if comp.table == q_table {
                let cols = comp.columns.join(", ");
                rows.push(format!("{}\t{}\t{}\tBTREE", bare, idx_name, cols));
            }
        }

        if rows.len() == 1 {
            Ok(format!("No indexes found on table '{}'", bare))
        } else {
            Ok(rows.join("\n"))
        }
    }

    fn exec_backup(&self, s: &SharedDatabase, database: Option<String>, output_file: Option<String>) -> Result<String, String> {
        let target_db = database.unwrap_or_else(|| self.current_db.clone()).to_lowercase();
        let mut out = String::new();

        out.push_str(&format!("-- RustDB backup of database `{}`\n", target_db));
        out.push_str(&format!("-- Generated: {}\n\n", {
            let secs = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs();
            format!("{:04}-{:02}-{:02}", 1970 + secs/31536000, ((secs%31536000)/2628000)+1, ((secs%2628000)/86400)+1)
        }));
        out.push_str(&format!("CREATE DATABASE IF NOT EXISTS `{}`;\nUSE `{}`;\n\n", target_db, target_db));

        // For each table in the target database, emit CREATE TABLE + INSERTs
        let table_keys: Vec<String> = s.tables.keys()
            .filter(|k| k.starts_with(&format!("{}.", target_db)))
            .cloned()
            .collect();

        let mut sorted_keys = table_keys;
        sorted_keys.sort();

        for qkey in &sorted_keys {
            let bare = qkey.split('.').last().unwrap_or(qkey);
            if s.catalog.get_table(qkey).is_some() {
                let ddl = self.build_create_table_ddl(s, qkey);
                out.push_str(&format!("DROP TABLE IF EXISTS `{}`;\n", bare));
                out.push_str(&ddl);
                out.push_str("\n\n");
            }
            if let Some(rows) = s.tables.get(qkey) {
                let visible: Vec<&Row> = rows.iter().filter(|r| Self::is_visible(r)).collect();
                if !visible.is_empty() {
                    let cols: Vec<String> = if let Some(first) = visible.first() {
                        first.keys().filter(|k| !k.starts_with('_')).cloned().collect()
                    } else { vec![] };
                    let col_list = cols.iter().map(|c| format!("`{}`", c)).collect::<Vec<_>>().join(", ");
                    for row in &visible {
                        let vals = cols.iter().map(|c| {
                            match row.get(c) {
                                Some(v) if v == "NULL" => "NULL".to_string(),
                                Some(v) => format!("'{}'", v.replace('\'', "''")),
                                None => "NULL".to_string(),
                            }
                        }).collect::<Vec<_>>().join(", ");
                        out.push_str(&format!("INSERT INTO `{}` ({}) VALUES ({});\n", bare, col_list, vals));
                    }
                    out.push('\n');
                }
            }
        }

        if let Some(ref path) = output_file {
            match std::fs::write(path, &out) {
                Ok(_) => Ok(format!("Backup of '{}' written to '{}' ({} bytes).", target_db, path, out.len())),
                Err(e) => Err(format!("Failed to write backup file '{}': {}", path, e)),
            }
        } else {
            Ok(out)
        }
    }

    fn build_create_table_ddl(&self, s: &SharedDatabase, qkey: &str) -> String {
        let bare = qkey.split('.').last().unwrap_or(qkey);
        if let Some(schema) = s.catalog.get_table(qkey) {
            let type_str = |dt: &crate::parser::ast::DataType| -> String {
                use crate::parser::ast::DataType;
                match dt {
                    DataType::Int        => "INT".to_string(),
                    DataType::BigInt     => "BIGINT".to_string(),
                    DataType::SmallInt   => "SMALLINT".to_string(),
                    DataType::TinyInt    => "TINYINT".to_string(),
                    DataType::Float      => "FLOAT".to_string(),
                    DataType::Varchar(n) => format!("VARCHAR({})", n),
                    DataType::Text       => "TEXT".to_string(),
                    DataType::Boolean    => "BOOLEAN".to_string(),
                    DataType::Date       => "DATE".to_string(),
                    DataType::Enum(vals) => format!("ENUM({})", vals.iter().map(|v| format!("'{}'", v)).collect::<Vec<_>>().join(",")),
                    DataType::Set(vals)  => format!("SET({})", vals.iter().map(|v| format!("'{}'", v)).collect::<Vec<_>>().join(",")),
                    DataType::Blob       => "BLOB".to_string(),
                    DataType::Json       => "JSON".to_string(),
                    DataType::DateTime   => "DATETIME".to_string(),
                    DataType::Timestamp  => "TIMESTAMP".to_string(),
                    DataType::Decimal(p, s) => format!("DECIMAL({},{})", p, s),
                    DataType::Double     => "DOUBLE".to_string(),
                    DataType::Time       => "TIME".to_string(),
                    DataType::Year       => "YEAR".to_string(),
                    DataType::Unknown    => "TEXT".to_string(),
                }
            };
            let mut lines: Vec<String> = Vec::new();
            for col in &schema.columns {
                let mut parts = vec![format!("`{}`", col.name), type_str(&col.data_type)];
                if col.not_null || col.primary_key { parts.push("NOT NULL".to_string()); }
                if col.auto_increment { parts.push("AUTO_INCREMENT".to_string()); }
                if let Some(def) = &col.default {
                    if def != crate::parser::parser::NULL_DEFAULT {
                        parts.push(format!("DEFAULT '{}'", def));
                    }
                }
                if col.primary_key && schema.primary_key_columns.is_empty() {
                    parts.push("PRIMARY KEY".to_string());
                }
                lines.push(format!("  {}", parts.join(" ")));
            }
            if !schema.primary_key_columns.is_empty() {
                let pkc = schema.primary_key_columns.iter().map(|c| format!("`{}`", c)).collect::<Vec<_>>().join(", ");
                lines.push(format!("  PRIMARY KEY ({})", pkc));
            }
            let fk_action_str = |a: &crate::catalog::schema::FkAction| -> &'static str {
                match a {
                    crate::catalog::schema::FkAction::Restrict   => "RESTRICT",
                    crate::catalog::schema::FkAction::Cascade    => "CASCADE",
                    crate::catalog::schema::FkAction::SetNull    => "SET NULL",
                    crate::catalog::schema::FkAction::SetDefault => "SET DEFAULT",
                }
            };
            for col in &schema.columns {
                if let Some(ref fk) = col.foreign_key {
                    let ref_bare = fk.ref_table.split('.').last().unwrap_or(&fk.ref_table);
                    lines.push(format!(
                        "  FOREIGN KEY (`{}`) REFERENCES `{}`(`{}`) ON DELETE {} ON UPDATE {}",
                        fk.column, ref_bare, fk.ref_column,
                        fk_action_str(&fk.on_delete),
                        fk_action_str(&fk.on_update),
                    ));
                }
            }
            format!("CREATE TABLE `{}` (\n{}\n);", bare, lines.join(",\n"))
        } else {
            format!("-- (schema not found for {})", bare)
        }
    }

    fn exec_show_processlist(&self, _s: &SharedDatabase) -> Result<String, String> {
        let sep = "+----+------+-----------+--------+-------+------+";
        let mut out = String::new();
        out.push_str(&format!("{}\n", sep));
        out.push_str("| Id | User | Host      | db     | State | Time |\n");
        out.push_str(&format!("{}\n", sep));
        out.push_str(&format!("| {:<2} | {:<4} | {:<9} | {:<6} | {:<5} | {:<4} |\n",
            1, "root", "localhost", &self.current_db, "Query", 0));
        out.push_str(sep);
        Ok(out)
    }

    fn exec_execute(&mut self, s: &mut SharedDatabase, name: &str, using_vars: &[String]) -> Result<String, String> {
        let mut query = self.prepared_stmts.get(name)
            .cloned()
            .ok_or_else(|| format!("Unknown prepared statement: {}", name))?;
        // Substitute '?' placeholders with user_var values positionally
        for var in using_vars {
            let val = self.user_vars.get(var.as_str()).cloned().unwrap_or_else(|| "NULL".to_string());
            query = query.replacen('?', &val, 1);
        }
        let stmt = crate::parser::parser::Parser::new(&query).parse()
            .map_err(|e| format!("EXECUTE parse error: {}", e))?;
        self.execute_with_s(s, stmt)
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

    /// Extract table prefixes from dotted column references in a condition tree.
    fn collect_table_refs_from_expr(expr: &CondExpr, refs: &mut HashSet<String>) {
        match expr {
            CondExpr::And(l, r) | CondExpr::Or(l, r) => {
                Self::collect_table_refs_from_expr(l, refs);
                Self::collect_table_refs_from_expr(r, refs);
            }
            CondExpr::Not(inner) => Self::collect_table_refs_from_expr(inner, refs),
            CondExpr::Leaf(cond) => {
                if let ArithExpr::Col(c) = &cond.left {
                    if let Some(pos) = c.rfind('.') {
                        refs.insert(c[..pos].to_string());
                    }
                }
                if let ConditionValue::Literal(v) = &cond.value {
                    if let Some(pos) = v.rfind('.') {
                        let prefix = &v[..pos];
                        if prefix.chars().next().map(|c| c.is_alphabetic() || c == '_').unwrap_or(false) {
                            refs.insert(prefix.to_string());
                        }
                    }
                }
            }
        }
    }

    /// Greedy JOIN reorder: smallest table first, dependency-aware, INNER joins only.
    /// If any non-INNER join is present, returns original order unchanged.
    fn reorder_joins_greedy(base_table: &str, joins: Vec<Join>, tables: &HashMap<String, Vec<Row>>) -> Vec<Join> {
        if joins.len() <= 1 { return joins; }
        if joins.iter().any(|j| !matches!(j.join_type, JoinType::Inner | JoinType::Natural)) { return joins; }

        let mut available: HashSet<String> = HashSet::new();
        available.insert(base_table.to_string());
        if let Some(pos) = base_table.rfind('.') {
            available.insert(base_table[pos + 1..].to_string());
        }

        let mut remaining = joins;
        let mut reordered = Vec::new();

        while !remaining.is_empty() {
            let candidates: Vec<usize> = remaining.iter().enumerate()
                .filter_map(|(i, j)| {
                    let mut refs = HashSet::new();
                    Self::collect_table_refs_from_expr(&j.on_expr, &mut refs);
                    let join_bare = j.table.split('.').last().unwrap_or(&j.table);
                    let ok = refs.iter().all(|r| {
                        let r_bare = r.split('.').last().unwrap_or(r);
                        available.contains(r) || available.contains(r_bare)
                            || r == &j.table || r_bare == join_bare
                    });
                    if ok { Some(i) } else { None }
                })
                .collect();

            let best_idx = if candidates.is_empty() {
                0
            } else {
                *candidates.iter().min_by_key(|&&i| {
                    tables.get(&remaining[i].table).map(|r| r.len()).unwrap_or(usize::MAX)
                }).unwrap_or(&candidates[0])
            };

            let j = remaining.remove(best_idx);
            available.insert(j.table.clone());
            if let Some(pos) = j.table.rfind('.') {
                available.insert(j.table[pos + 1..].to_string());
            }
            reordered.push(j);
        }
        reordered
    }

    /// ANALYZE TABLE t — 컬럼별 통계 수집 후 TableStats 저장
    fn exec_analyze_table(&self, s: &mut SharedDatabase, table: String) -> Result<String, String> {
        let rows = s.tables.get(&table)
            .ok_or_else(|| format!("Table '{}' not found", self.display_name(&table)))?
            .iter()
            .filter(|r| Self::is_visible(r))
            .cloned()
            .collect::<Vec<_>>();

        let total = rows.len();
        let mut distinct: HashMap<String, std::collections::HashSet<String>> = HashMap::new();
        let mut null_cnt: HashMap<String, usize> = HashMap::new();
        let mut min_map: HashMap<String, String> = HashMap::new();
        let mut max_map: HashMap<String, String> = HashMap::new();

        for row in &rows {
            for (key, val) in row.iter() {
                if key.starts_with('_') { continue; } // skip _xmin/_xmax
                let col = key.rsplit('.').next().unwrap_or(key).to_string();
                if val == NULL_VALUE || val.is_empty() {
                    *null_cnt.entry(col).or_insert(0) += 1;
                } else {
                    distinct.entry(col.clone()).or_default().insert(val.clone());
                    // numeric-aware min/max
                    let new_lt = match (min_map.get(&col), val.parse::<f64>()) {
                        (None, _) => true,
                        (Some(cur), Ok(v)) => cur.parse::<f64>().map_or(true, |c| v < c),
                        (Some(cur), Err(_)) => val.as_str() < cur.as_str(),
                    };
                    if new_lt { min_map.insert(col.clone(), val.clone()); }
                    let new_gt = match (max_map.get(&col), val.parse::<f64>()) {
                        (None, _) => true,
                        (Some(cur), Ok(v)) => cur.parse::<f64>().map_or(true, |c| v > c),
                        (Some(cur), Err(_)) => val.as_str() > cur.as_str(),
                    };
                    if new_gt { max_map.insert(col.clone(), val.clone()); }
                }
            }
        }

        let mut col_stats: HashMap<String, ColumnStats> = HashMap::new();
        for (col, set) in &distinct {
            col_stats.insert(col.clone(), ColumnStats {
                distinct_count: set.len(),
                null_count: *null_cnt.get(col).unwrap_or(&0),
                min_val: min_map.get(col).cloned(),
                max_val: max_map.get(col).cloned(),
            });
        }
        // columns that are all-NULL have no distinct entry
        for (col, cnt) in &null_cnt {
            col_stats.entry(col.clone()).or_insert_with(|| ColumnStats {
                distinct_count: 0,
                null_count: *cnt,
                min_val: None,
                max_val: None,
            });
        }

        let table_display = self.display_name(&table).to_string();
        s.table_stats.insert(table.clone(), TableStats { total_rows: total, columns: col_stats.clone() });

        // Build output table
        let schema = s.catalog.get_table(&table);
        let col_order: Vec<String> = schema
            .map(|sc| sc.columns.iter().map(|c| c.name.clone()).collect())
            .unwrap_or_else(|| { let mut v: Vec<String> = col_stats.keys().cloned().collect(); v.sort(); v });

        let width = 50usize;
        let bar = format!("+{}+", "-".repeat(width));
        let header = format!("| {:<width$}|", format!("ANALYZE: {} ({} rows)", table_display, total), width = width - 1);
        let col_header = format!("| {:<12} | {:>8} | {:>8} | {:<10} | {:<10} |",
            "column", "distinct", "nulls", "min", "max");
        let col_bar = "+".to_string() + &"-".repeat(14) + "+" + &"-".repeat(10) + "+" + &"-".repeat(10) + "+" + &"-".repeat(12) + "+" + &"-".repeat(12) + "+";

        let mut lines = vec![bar.clone(), header, bar.clone(), col_header, col_bar.clone()];
        for col in &col_order {
            if let Some(cs) = col_stats.get(col) {
                let min_s = cs.min_val.as_deref().unwrap_or("NULL");
                let max_s = cs.max_val.as_deref().unwrap_or("NULL");
                lines.push(format!("| {:<12} | {:>8} | {:>8} | {:<10} | {:<10} |",
                    if col.len() > 12 { &col[..12] } else { col },
                    cs.distinct_count,
                    cs.null_count,
                    if min_s.len() > 10 { &min_s[..10] } else { min_s },
                    if max_s.len() > 10 { &max_s[..10] } else { max_s },
                ));
            }
        }
        lines.push(col_bar);
        Ok(lines.join("\n"))
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
        let planner = Planner::new(&s.tables, &s.indexes, &s.index_meta, &s.composite_indexes, &s.catalog, &s.table_stats);
        let plan = planner.plan_covering(&table, &condition, &joins, &columns);
        Ok(planner.explain(&plan))
    }

    /// EXPLAIN ANALYZE <SELECT> — 실행 계획 + 실제 실행 결과(행 수·소요 시간) 출력
    fn exec_explain_analyze(&mut self, s: &mut SharedDatabase, stmt: Statement) -> Result<String, String> {
        // 실행 계획 문자열 생성 (실행 전, 헤더만 교체)
        let plan_body = match &stmt {
            Statement::Select { table, condition, joins, subquery, columns, .. } => {
                if subquery.is_some() {
                    "| Access: SUBQUERY SCAN                            |\n".to_string()
                } else {
                    let planner = Planner::new(&s.tables, &s.indexes, &s.index_meta, &s.composite_indexes, &s.catalog, &s.table_stats);
                    let plan = planner.plan_covering(table, condition, joins, columns);
                    // explain()에서 헤더·구분선을 제외한 중간 행만 추출
                    planner.explain(&plan)
                        .lines()
                        .skip(3)  // +---+ | QUERY PLAN | +---+
                        .take_while(|l| !l.starts_with('+'))
                        .map(|l| format!("{}\n", l))
                        .collect()
                }
            }
            _ => String::new(),
        };

        // 실제 실행 + 시간 측정
        let start = std::time::Instant::now();
        let result = self.execute_with_s(s, stmt)?;
        let elapsed = start.elapsed();

        // 실제 반환 행 수 추출
        let actual_rows = result.lines()
            .find(|l| l.contains("row(s) returned"))
            .and_then(|l| l.trim().split_whitespace().next())
            .and_then(|n| n.parse::<usize>().ok())
            .unwrap_or(0);

        let sep = "+--------------------------------------------------+";
        let fmt_row = |label: &str, val: &str| -> String {
            format!("| {:<48} |\n", format!("{}: {}", label, val))
        };

        let mut out = String::new();
        out.push_str(sep); out.push('\n');
        out.push_str("|              QUERY PLAN (ANALYZE)                |\n");
        out.push_str(sep); out.push('\n');
        out.push_str(&plan_body);
        out.push_str("|                                                  |\n");
        out.push_str(&fmt_row("Actual rows", &actual_rows.to_string()));
        out.push_str(&fmt_row("Actual time", &format!("{:.3} sec", elapsed.as_secs_f64())));
        out.push_str(sep);
        Ok(out)
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

    /// AUTO VACUUM: 커밋된 DML이 누적 임계값(200회)을 초과하면 전체 테이블의 dead row를 제거한다.
    fn maybe_auto_vacuum(s: &mut SharedDatabase) {
        const AUTO_VACUUM_THRESHOLD: usize = 200;
        s.dml_since_vacuum += 1;
        if s.dml_since_vacuum < AUTO_VACUUM_THRESHOLD {
            return;
        }
        s.dml_since_vacuum = 0;
        let tables: Vec<String> = s.tables.keys().cloned().collect();
        let mut total_removed = 0usize;
        for t in &tables {
            let rows = s.tables.get_mut(t).unwrap();
            let before = rows.len();
            rows.retain(|r| Self::is_visible(r));
            let removed = before - rows.len();
            total_removed += removed;
            if removed > 0 {
                let rows_clone = rows.clone();
                if let Some(index) = s.indexes.get_mut(t) {
                    *index = BPlusTree::new();
                    for row in &rows_clone {
                        let key = row.values().next().cloned().unwrap_or_default();
                        index.insert(key, serde_json::to_string(row).unwrap());
                    }
                }
                s.buffer_pool.write_page(t, rows_clone.clone());
                s.buffer_pool.flush_page(t, &s.disk);
            }
        }
        if total_removed > 0 {
            eprintln!("[AutoVacuum] dead row {} 행 제거 완료", total_removed);
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
            password_hash: password.map(|p| hash_password(&p)),
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

    // ── ROLE 관리 ──────────────────────────────────────────────────────────────

    fn exec_create_role(&mut self, s: &mut SharedDatabase, name: String) -> Result<String, String> {
        if s.roles.iter().any(|r| r.name == name) {
            return Err(format!("Role '{}' already exists", name));
        }
        s.roles.push(RoleRecord { name });
        s.disk.save_roles(&s.roles);
        Ok("Query OK".to_string())
    }

    fn exec_drop_role(&mut self, s: &mut SharedDatabase, name: String, if_exists: bool) -> Result<String, String> {
        let before = s.roles.len();
        s.roles.retain(|r| r.name != name);
        if s.roles.len() == before && !if_exists {
            return Err(format!("Role '{}' does not exist", name));
        }
        s.role_grants.retain(|rg| rg.role != name);
        s.disk.save_roles(&s.roles);
        s.disk.save_role_grants(&s.role_grants);
        Ok("Query OK".to_string())
    }

    fn exec_grant_role(&mut self, s: &mut SharedDatabase, role: String, user: String, host: String, with_admin_option: bool) -> Result<String, String> {
        if !s.roles.iter().any(|r| r.name == role) {
            return Err(format!("Role '{}' does not exist", role));
        }
        s.role_grants.retain(|rg| !(rg.role == role && rg.user == user && rg.host == host));
        s.role_grants.push(RoleGrant { role, user, host, with_admin_option });
        s.disk.save_role_grants(&s.role_grants);
        Ok("Query OK".to_string())
    }

    fn exec_revoke_role(&mut self, s: &mut SharedDatabase, role: String, user: String, host: String) -> Result<String, String> {
        let before = s.role_grants.len();
        s.role_grants.retain(|rg| !(rg.role == role && rg.user == user && rg.host == host));
        if s.role_grants.len() == before {
            return Err(format!("Role '{}' not granted to '{}'@'{}'", role, user, host));
        }
        s.disk.save_role_grants(&s.role_grants);
        Ok("Query OK".to_string())
    }

    fn exec_show_roles(&self, s: &SharedDatabase) -> Result<String, String> {
        if s.roles.is_empty() {
            return Ok("No roles defined.".to_string());
        }
        let max_len = s.roles.iter().map(|r| r.name.len()).max().unwrap_or(4).max(4);
        let sep = format!("+{}+", "-".repeat(max_len + 2));
        let header = format!("| {:<width$} |", "Role", width = max_len);
        let mut out = format!("{}\n{}\n{}\n", sep, header, sep);
        for r in &s.roles {
            out.push_str(&format!("| {:<width$} |\n", r.name, width = max_len));
        }
        out.push_str(&sep);
        Ok(out)
    }

    // ── SYNONYM 관리 ──────────────────────────────────────────────────────────

    fn exec_create_synonym(&mut self, s: &mut SharedDatabase, name: String, target: String, or_replace: bool) -> Result<String, String> {
        if s.synonyms.contains_key(&name) && !or_replace {
            return Err(format!("Synonym '{}' already exists", name));
        }
        s.synonyms.insert(name, target);
        s.disk.save_synonyms(&s.synonyms);
        Ok("Query OK".to_string())
    }

    fn exec_drop_synonym(&mut self, s: &mut SharedDatabase, name: String, if_exists: bool) -> Result<String, String> {
        if s.synonyms.remove(&name).is_none() && !if_exists {
            return Err(format!("Synonym '{}' does not exist", name));
        }
        s.disk.save_synonyms(&s.synonyms);
        Ok("Query OK".to_string())
    }

    fn exec_show_synonyms(&self, s: &SharedDatabase) -> Result<String, String> {
        if s.synonyms.is_empty() {
            return Ok("No synonyms defined.".to_string());
        }
        let max_name = s.synonyms.keys().map(|k| k.len()).max().unwrap_or(7).max(7);
        let max_target = s.synonyms.values().map(|v| v.len()).max().unwrap_or(6).max(6);
        let sep = format!("+{}+{}+", "-".repeat(max_name + 2), "-".repeat(max_target + 2));
        let header = format!("| {:<w1$} | {:<w2$} |", "Synonym", "Target", w1 = max_name, w2 = max_target);
        let mut out = format!("{}\n{}\n{}\n", sep, header, sep);
        let mut pairs: Vec<(&String, &String)> = s.synonyms.iter().collect();
        pairs.sort_by_key(|(k, _)| *k);
        for (name, target) in pairs {
            out.push_str(&format!("| {:<w1$} | {:<w2$} |\n", name, target, w1 = max_name, w2 = max_target));
        }
        out.push_str(&sep);
        Ok(out)
    }

    // ── INFORMATION_SCHEMA ──────────────────────────────────────────────────

    fn info_schema_rows(s: &SharedDatabase, which: &str) -> Vec<Row> {
        fn split_name(name: &str) -> (&str, &str) {
            match name.find('.') {
                Some(pos) => (&name[..pos], &name[pos + 1..]),
                None => ("", name),
            }
        }
        fn dt_base(dt: &DataType) -> &'static str {
            match dt {
                DataType::Int => "int", DataType::BigInt => "bigint",
                DataType::SmallInt => "smallint", DataType::TinyInt => "tinyint",
                DataType::Text => "text",
                DataType::Varchar(_) => "varchar", DataType::Float => "float",
                DataType::Double => "double", DataType::Decimal(_, _) => "decimal",
                DataType::Boolean => "tinyint", DataType::Date => "date",
                DataType::DateTime => "datetime", DataType::Timestamp => "timestamp",
                DataType::Time => "time", DataType::Year => "year",
                DataType::Blob => "blob", DataType::Enum(_) => "enum",
                DataType::Set(_) => "set", DataType::Json => "json", _ => "varchar",
            }
        }
        fn dt_full(dt: &DataType) -> String {
            match dt {
                DataType::Varchar(n) => format!("varchar({})", n),
                DataType::Decimal(p, s) => format!("decimal({},{})", p, s),
                DataType::Boolean => "tinyint(1)".into(),
                other => dt_base(other).into(),
            }
        }

        match which.to_lowercase().as_str() {
            "schemata" | "schemas" => s.databases.iter().map(|db| {
                let mut r = Row::new();
                r.insert("CATALOG_NAME".into(), "def".into());
                r.insert("SCHEMA_NAME".into(), db.clone());
                r.insert("DEFAULT_CHARACTER_SET_NAME".into(), "utf8mb4".into());
                r.insert("DEFAULT_COLLATION_NAME".into(), "utf8mb4_0900_ai_ci".into());
                r.insert("SQL_PATH".into(), NULL_VALUE.into());
                r
            }).collect(),

            "tables" => {
                let mut rows: Vec<Row> = Vec::new();
                for (name, data) in &s.tables {
                    let (db, tbl) = split_name(name);
                    let mut r = Row::new();
                    r.insert("TABLE_CATALOG".into(), "def".into());
                    r.insert("TABLE_SCHEMA".into(), db.into());
                    r.insert("TABLE_NAME".into(), tbl.into());
                    r.insert("TABLE_TYPE".into(), "BASE TABLE".into());
                    r.insert("ENGINE".into(), "RustDB".into());
                    r.insert("VERSION".into(), "10".into());
                    r.insert("ROW_FORMAT".into(), "Dynamic".into());
                    r.insert("TABLE_ROWS".into(), data.len().to_string());
                    r.insert("AVG_ROW_LENGTH".into(), "0".into());
                    r.insert("DATA_LENGTH".into(), "0".into());
                    r.insert("MAX_DATA_LENGTH".into(), "0".into());
                    r.insert("INDEX_LENGTH".into(), "0".into());
                    r.insert("DATA_FREE".into(), "0".into());
                    r.insert("AUTO_INCREMENT".into(), NULL_VALUE.into());
                    r.insert("CREATE_TIME".into(), NULL_VALUE.into());
                    r.insert("UPDATE_TIME".into(), NULL_VALUE.into());
                    r.insert("CHECK_TIME".into(), NULL_VALUE.into());
                    r.insert("TABLE_COLLATION".into(), "utf8mb4_0900_ai_ci".into());
                    r.insert("CHECKSUM".into(), NULL_VALUE.into());
                    r.insert("CREATE_OPTIONS".into(), "".into());
                    r.insert("TABLE_COMMENT".into(), "".into());
                    rows.push(r);
                }
                for (name, _) in &s.views {
                    let (db, tbl) = split_name(name);
                    let mut r = Row::new();
                    r.insert("TABLE_CATALOG".into(), "def".into());
                    r.insert("TABLE_SCHEMA".into(), db.into());
                    r.insert("TABLE_NAME".into(), tbl.into());
                    r.insert("TABLE_TYPE".into(), "VIEW".into());
                    r.insert("ENGINE".into(), NULL_VALUE.into());
                    r.insert("VERSION".into(), NULL_VALUE.into());
                    r.insert("ROW_FORMAT".into(), NULL_VALUE.into());
                    r.insert("TABLE_ROWS".into(), NULL_VALUE.into());
                    r.insert("TABLE_COMMENT".into(), "VIEW".into());
                    rows.push(r);
                }
                rows
            }

            "columns" => {
                let mut rows: Vec<Row> = Vec::new();
                for schema in s.catalog.tables.values() {
                    let (db, tbl) = split_name(&schema.name);
                    for (i, col) in schema.columns.iter().enumerate() {
                        let char_max = match &col.data_type {
                            DataType::Varchar(n) => n.to_string(),
                            DataType::Text | DataType::Blob | DataType::Json => "65535".into(),
                            _ => NULL_VALUE.into(),
                        };
                        let num_prec = match &col.data_type {
                            DataType::Int | DataType::SmallInt | DataType::TinyInt => "10".into(),
                            DataType::BigInt => "19".into(),
                            DataType::Float => "12".into(),
                            DataType::Double => "22".into(),
                            DataType::Decimal(p, _) => p.to_string(),
                            _ => NULL_VALUE.into(),
                        };
                        let num_scale = match &col.data_type {
                            DataType::Decimal(_, s) => s.to_string(), _ => NULL_VALUE.into(),
                        };
                        let has_charset = matches!(&col.data_type,
                            DataType::Varchar(_) | DataType::Text | DataType::Enum(_) | DataType::Set(_) | DataType::Json);
                        let mut r = Row::new();
                        r.insert("TABLE_CATALOG".into(), "def".into());
                        r.insert("TABLE_SCHEMA".into(), db.into());
                        r.insert("TABLE_NAME".into(), tbl.into());
                        r.insert("COLUMN_NAME".into(), col.name.clone());
                        r.insert("ORDINAL_POSITION".into(), (i + 1).to_string());
                        r.insert("COLUMN_DEFAULT".into(), col.default.clone().unwrap_or(NULL_VALUE.into()));
                        r.insert("IS_NULLABLE".into(), if col.not_null { "NO".into() } else { "YES".into() });
                        r.insert("DATA_TYPE".into(), dt_base(&col.data_type).into());
                        r.insert("CHARACTER_MAXIMUM_LENGTH".into(), char_max);
                        r.insert("CHARACTER_OCTET_LENGTH".into(), NULL_VALUE.into());
                        r.insert("NUMERIC_PRECISION".into(), num_prec);
                        r.insert("NUMERIC_SCALE".into(), num_scale);
                        r.insert("DATETIME_PRECISION".into(), NULL_VALUE.into());
                        r.insert("CHARACTER_SET_NAME".into(), if has_charset { "utf8mb4".into() } else { NULL_VALUE.into() });
                        r.insert("COLLATION_NAME".into(), if has_charset { "utf8mb4_0900_ai_ci".into() } else { NULL_VALUE.into() });
                        r.insert("COLUMN_TYPE".into(), dt_full(&col.data_type));
                        r.insert("COLUMN_KEY".into(), if col.primary_key { "PRI".into() } else if col.unique { "UNI".into() } else { "".into() });
                        r.insert("EXTRA".into(), if col.auto_increment { "auto_increment".into() } else { "".into() });
                        r.insert("PRIVILEGES".into(), "select,insert,update,references".into());
                        r.insert("COLUMN_COMMENT".into(), "".into());
                        r.insert("GENERATION_EXPRESSION".into(), "".into());
                        rows.push(r);
                    }
                }
                rows
            }

            "key_column_usage" => {
                let mut rows: Vec<Row> = Vec::new();
                for schema in s.catalog.tables.values() {
                    let (db, tbl) = split_name(&schema.name);
                    let pk_cols: Vec<String> = if !schema.primary_key_columns.is_empty() {
                        schema.primary_key_columns.clone()
                    } else {
                        schema.columns.iter().filter(|c| c.primary_key).map(|c| c.name.clone()).collect()
                    };
                    for (pos, pk_col) in pk_cols.iter().enumerate() {
                        let mut r = Row::new();
                        r.insert("CONSTRAINT_CATALOG".into(), "def".into());
                        r.insert("CONSTRAINT_SCHEMA".into(), db.into());
                        r.insert("CONSTRAINT_NAME".into(), "PRIMARY".into());
                        r.insert("TABLE_CATALOG".into(), "def".into());
                        r.insert("TABLE_SCHEMA".into(), db.into());
                        r.insert("TABLE_NAME".into(), tbl.into());
                        r.insert("COLUMN_NAME".into(), pk_col.clone());
                        r.insert("ORDINAL_POSITION".into(), (pos + 1).to_string());
                        r.insert("POSITION_IN_UNIQUE_CONSTRAINT".into(), NULL_VALUE.into());
                        r.insert("REFERENCED_TABLE_SCHEMA".into(), NULL_VALUE.into());
                        r.insert("REFERENCED_TABLE_NAME".into(), NULL_VALUE.into());
                        r.insert("REFERENCED_COLUMN_NAME".into(), NULL_VALUE.into());
                        rows.push(r);
                    }
                    for (i, col) in schema.columns.iter().enumerate() {
                        if let Some(fk) = &col.foreign_key {
                            let (ref_db, ref_tbl) = split_name(&fk.ref_table);
                            let ref_db = if ref_db.is_empty() { db } else { ref_db };
                            let mut r = Row::new();
                            r.insert("CONSTRAINT_CATALOG".into(), "def".into());
                            r.insert("CONSTRAINT_SCHEMA".into(), db.into());
                            r.insert("CONSTRAINT_NAME".into(), format!("{}_ibfk_{}", tbl, i + 1));
                            r.insert("TABLE_CATALOG".into(), "def".into());
                            r.insert("TABLE_SCHEMA".into(), db.into());
                            r.insert("TABLE_NAME".into(), tbl.into());
                            r.insert("COLUMN_NAME".into(), col.name.clone());
                            r.insert("ORDINAL_POSITION".into(), "1".into());
                            r.insert("POSITION_IN_UNIQUE_CONSTRAINT".into(), "1".into());
                            r.insert("REFERENCED_TABLE_SCHEMA".into(), ref_db.into());
                            r.insert("REFERENCED_TABLE_NAME".into(), ref_tbl.into());
                            r.insert("REFERENCED_COLUMN_NAME".into(), fk.ref_column.clone());
                            rows.push(r);
                        }
                    }
                }
                rows
            }

            "table_constraints" => {
                let mut rows: Vec<Row> = Vec::new();
                for schema in s.catalog.tables.values() {
                    let (db, tbl) = split_name(&schema.name);
                    let has_pk = schema.columns.iter().any(|c| c.primary_key)
                        || !schema.primary_key_columns.is_empty();
                    if has_pk {
                        let mut r = Row::new();
                        r.insert("CONSTRAINT_CATALOG".into(), "def".into());
                        r.insert("CONSTRAINT_SCHEMA".into(), db.into());
                        r.insert("CONSTRAINT_NAME".into(), "PRIMARY".into());
                        r.insert("TABLE_SCHEMA".into(), db.into());
                        r.insert("TABLE_NAME".into(), tbl.into());
                        r.insert("CONSTRAINT_TYPE".into(), "PRIMARY KEY".into());
                        r.insert("ENFORCED".into(), "YES".into());
                        rows.push(r);
                    }
                    for col in &schema.columns {
                        if col.unique && !col.primary_key {
                            let cname = col.unique_constraint_name.clone().unwrap_or_else(|| col.name.clone());
                            let mut r = Row::new();
                            r.insert("CONSTRAINT_CATALOG".into(), "def".into());
                            r.insert("CONSTRAINT_SCHEMA".into(), db.into());
                            r.insert("CONSTRAINT_NAME".into(), cname);
                            r.insert("TABLE_SCHEMA".into(), db.into());
                            r.insert("TABLE_NAME".into(), tbl.into());
                            r.insert("CONSTRAINT_TYPE".into(), "UNIQUE".into());
                            r.insert("ENFORCED".into(), "YES".into());
                            rows.push(r);
                        }
                        if col.foreign_key.is_some() {
                            let mut r = Row::new();
                            r.insert("CONSTRAINT_CATALOG".into(), "def".into());
                            r.insert("CONSTRAINT_SCHEMA".into(), db.into());
                            r.insert("CONSTRAINT_NAME".into(), format!("{}_ibfk_{}", tbl, col.name));
                            r.insert("TABLE_SCHEMA".into(), db.into());
                            r.insert("TABLE_NAME".into(), tbl.into());
                            r.insert("CONSTRAINT_TYPE".into(), "FOREIGN KEY".into());
                            r.insert("ENFORCED".into(), "YES".into());
                            rows.push(r);
                        }
                    }
                    for cc in &schema.check_constraints {
                        let mut r = Row::new();
                        r.insert("CONSTRAINT_CATALOG".into(), "def".into());
                        r.insert("CONSTRAINT_SCHEMA".into(), db.into());
                        r.insert("CONSTRAINT_NAME".into(), cc.name.clone().unwrap_or_else(|| "chk".into()));
                        r.insert("TABLE_SCHEMA".into(), db.into());
                        r.insert("TABLE_NAME".into(), tbl.into());
                        r.insert("CONSTRAINT_TYPE".into(), "CHECK".into());
                        r.insert("ENFORCED".into(), "YES".into());
                        rows.push(r);
                    }
                }
                rows
            }

            "statistics" => {
                s.index_meta.iter().map(|(idx_name, (tbl_name, col_name))| {
                    let (db, tbl) = split_name(tbl_name);
                    let mut r = Row::new();
                    r.insert("TABLE_CATALOG".into(), "def".into());
                    r.insert("TABLE_SCHEMA".into(), db.into());
                    r.insert("TABLE_NAME".into(), tbl.into());
                    r.insert("NON_UNIQUE".into(), "1".into());
                    r.insert("INDEX_SCHEMA".into(), db.into());
                    r.insert("INDEX_NAME".into(), idx_name.clone());
                    r.insert("SEQ_IN_INDEX".into(), "1".into());
                    r.insert("COLUMN_NAME".into(), col_name.clone());
                    r.insert("COLLATION".into(), "A".into());
                    r.insert("CARDINALITY".into(), NULL_VALUE.into());
                    r.insert("SUB_PART".into(), NULL_VALUE.into());
                    r.insert("PACKED".into(), NULL_VALUE.into());
                    r.insert("NULLABLE".into(), "YES".into());
                    r.insert("INDEX_TYPE".into(), "BTREE".into());
                    r.insert("COMMENT".into(), "".into());
                    r.insert("INDEX_COMMENT".into(), "".into());
                    r.insert("IS_VISIBLE".into(), "YES".into());
                    r
                }).collect()
            }

            "views" => {
                s.views.iter().map(|(name, _)| {
                    let (db, vw) = split_name(name);
                    let mut r = Row::new();
                    r.insert("TABLE_CATALOG".into(), "def".into());
                    r.insert("TABLE_SCHEMA".into(), db.into());
                    r.insert("TABLE_NAME".into(), vw.into());
                    r.insert("VIEW_DEFINITION".into(), "".into());
                    r.insert("CHECK_OPTION".into(), "NONE".into());
                    r.insert("IS_UPDATABLE".into(), "NO".into());
                    r.insert("DEFINER".into(), "root@%".into());
                    r.insert("SECURITY_TYPE".into(), "DEFINER".into());
                    r.insert("CHARACTER_SET_CLIENT".into(), "utf8mb4".into());
                    r.insert("COLLATION_CONNECTION".into(), "utf8mb4_0900_ai_ci".into());
                    r
                }).collect()
            }

            "character_sets" | "collations" | "engines" => {
                let mut r = Row::new();
                match which.to_lowercase().as_str() {
                    "character_sets" => {
                        r.insert("CHARACTER_SET_NAME".into(), "utf8mb4".into());
                        r.insert("DEFAULT_COLLATE_NAME".into(), "utf8mb4_0900_ai_ci".into());
                        r.insert("DESCRIPTION".into(), "UTF-8 Unicode".into());
                        r.insert("MAXLEN".into(), "4".into());
                    }
                    "collations" => {
                        r.insert("COLLATION_NAME".into(), "utf8mb4_0900_ai_ci".into());
                        r.insert("CHARACTER_SET_NAME".into(), "utf8mb4".into());
                        r.insert("ID".into(), "255".into());
                        r.insert("IS_DEFAULT".into(), "Yes".into());
                        r.insert("IS_COMPILED".into(), "Yes".into());
                        r.insert("SORTLEN".into(), "0".into());
                        r.insert("PAD_ATTRIBUTE".into(), "NO PAD".into());
                    }
                    _ => { // engines
                        r.insert("ENGINE".into(), "RustDB".into());
                        r.insert("SUPPORT".into(), "DEFAULT".into());
                        r.insert("COMMENT".into(), "Custom Rust RDBMS".into());
                        r.insert("TRANSACTIONS".into(), "YES".into());
                        r.insert("XA".into(), "NO".into());
                        r.insert("SAVEPOINTS".into(), "YES".into());
                    }
                }
                vec![r]
            }

            _ => vec![], // 알 수 없는 IS 테이블 → 빈 결과
        }
    }

    fn exec_information_schema(
        &mut self,
        s: &mut SharedDatabase,
        which: &str,
        columns: Vec<SelectColumn>,
        condition: Option<CondExpr>,
        order_by: Vec<OrderBy>,
        limit: Option<usize>,
        offset: Option<usize>,
    ) -> Result<String, String> {
        // Normalize keys to lowercase so WHERE table_schema='db1' matches "TABLE_SCHEMA" stored key
        let mut rows: Vec<Row> = Self::info_schema_rows(s, which)
            .into_iter()
            .map(|r| r.into_iter().map(|(k, v)| (k.to_lowercase(), v)).collect())
            .collect();

        // WHERE 필터 (서브쿼리 없는 단순 조건만 지원)
        rows.retain(|r| Self::matches_condexpr(r, &condition));

        // ORDER BY
        if !order_by.is_empty() {
            rows.sort_by(|a, b| {
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

        // OFFSET / LIMIT
        let off = offset.unwrap_or(0);
        let rows: Vec<Row> = rows.into_iter().skip(off)
            .take(limit.unwrap_or(usize::MAX)).collect();

        if rows.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        // 컬럼 헤더 결정
        let col_names: Vec<String> = if columns.iter().any(|c| c == &SelectColumn::All) {
            // SELECT * → 첫 행 키를 알파벳순으로
            let mut keys: Vec<String> = rows[0].keys().cloned().collect();
            keys.sort();
            keys
        } else {
            columns.iter().filter_map(|c| match c {
                SelectColumn::Column(n) => Some(
                    n.rfind('.').map(|i| n[i + 1..].to_string()).unwrap_or(n.clone())
                ),
                SelectColumn::ColumnAlias(_, alias) => Some(alias.clone()),
                _ => None,
            }).collect()
        };

        if col_names.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        // 박스 그리기 포맷
        let col_widths: Vec<usize> = col_names.iter().map(|h| {
            rows.iter()
                .map(|r| Self::get_col(r, h).map(|v| v.len()).unwrap_or(0))
                .max().unwrap_or(0)
                .max(h.len())
        }).collect();

        let sep: String = col_widths.iter()
            .map(|w| format!("+{}", "-".repeat(w + 2)))
            .collect::<String>() + "+";
        let hdr: String = col_names.iter().zip(&col_widths)
            .map(|(h, w)| format!("| {:width$} ", h, width = w))
            .collect::<String>() + "|";

        let mut out = format!("{}\n{}\n{}\n", sep, hdr, sep);
        for row in &rows {
            let line: String = col_names.iter().zip(&col_widths)
                .map(|(col, w)| {
                    let v = Self::get_col(row, col).cloned().unwrap_or_default();
                    format!("| {:width$} ", v, width = w)
                })
                .collect::<String>() + "|";
            out.push_str(&line);
            out.push('\n');
        }
        out.push_str(&sep);
        out.push_str(&format!("\n{} row(s) returned.", rows.len()));
        Ok(out)
    }

    // ── MERGE INTO ───────────────────────────────────────────────────────────
    #[allow(clippy::too_many_arguments)]
    fn exec_merge(
        &mut self,
        s: &mut SharedDatabase,
        target: String,
        target_alias: Option<String>,
        source: String,
        source_alias: Option<String>,
        on: CondExpr,
        when_matched_update: Option<Vec<(String, ArithExpr)>>,
        when_matched_delete: bool,
        when_matched_delete_cond: Option<CondExpr>,
        when_not_matched_columns: Option<Vec<String>>,
        when_not_matched_values: Vec<String>,
    ) -> Result<String, String> {
        let source_rows: Vec<Row> = s.tables.get(&source)
            .ok_or(format!("Table '{}' not found", source))?
            .iter().filter(|r| Self::is_visible(r)).cloned().collect();

        let target_rows: Vec<Row> = s.tables.get(&target)
            .ok_or(format!("Table '{}' not found", target))?
            .iter().filter(|r| Self::is_visible(r)).cloned().collect();

        let pk_col = s.catalog.get_table(&target)
            .ok_or(format!("Table '{}' not found", target))?
            .columns.iter().find(|c| c.primary_key)
            .map(|c| c.name.clone())
            .unwrap_or_else(|| "id".to_string());

        let target_col_names: Vec<String> = s.catalog.get_table(&target)
            .unwrap().columns.iter().map(|c| c.name.clone()).collect();

        // Derive unqualified base names (e.g., "db1.dept" → "dept")
        let target_base = target.split('.').last().unwrap_or(&target).to_string();
        let source_base = source.split('.').last().unwrap_or(&source).to_string();

        // Resolved updates: (pk, Vec<(col, resolved_value_string)>)
        let mut update_rows: Vec<(String, Vec<(String, String)>)> = Vec::new();
        let mut delete_pks: Vec<String> = Vec::new();
        let mut insert_rows: Vec<Row> = Vec::new();

        for src_row in source_rows.iter() {
            let mut found = false;
            for tgt_row in &target_rows {
                // Build merged lookup row with prefixed keys for both tables
                let mut merged: Row = tgt_row.clone();
                for (k, v) in tgt_row.iter() {
                    merged.insert(format!("{}.{}", target_base, k), v.clone());
                }
                if let Some(ref alias) = target_alias {
                    for (k, v) in tgt_row.iter() {
                        merged.insert(format!("{}.{}", alias, k), v.clone());
                    }
                }
                for (k, v) in src_row.iter() {
                    merged.entry(k.clone()).or_insert_with(|| v.clone());
                    merged.insert(format!("{}.{}", source_base, k), v.clone());
                }
                if let Some(ref alias) = source_alias {
                    for (k, v) in src_row.iter() {
                        merged.insert(format!("{}.{}", alias, k), v.clone());
                    }
                }

                if Self::eval_condexpr(&merged, &on) {
                    let pk = tgt_row.get(&pk_col).cloned().unwrap_or_default();
                    found = true;
                    let delete_cond_ok = when_matched_delete_cond.as_ref()
                        .map(|c| Self::eval_condexpr(&merged, c))
                        .unwrap_or(true);
                    if when_matched_delete && delete_cond_ok {
                        delete_pks.push(pk);
                    } else if let Some(ref assigns) = when_matched_update {
                        // Evaluate assignment RHS using the merged row now
                        let resolved: Vec<(String, String)> = assigns.iter()
                            .map(|(col, expr)| (col.clone(), Self::eval_arith(&merged, expr)))
                            .collect();
                        update_rows.push((pk, resolved));
                    }
                    break;
                }
            }
            if !found && !when_not_matched_values.is_empty() {
                let cols = when_not_matched_columns.as_ref()
                    .map(|c| c.clone())
                    .unwrap_or_else(|| target_col_names.clone());
                let mut row: Row = HashMap::new();
                for (i, col) in cols.iter().enumerate() {
                    let raw = when_not_matched_values.get(i).cloned().unwrap_or_default();
                    // Resolve column reference from source row if not a quoted literal
                    let value = if raw.starts_with('\'') && raw.ends_with('\'') {
                        raw.trim_matches('\'').to_string()
                    } else if let Some(v) = src_row.get(&raw) {
                        v.clone()
                    } else if let Some(dot) = raw.find('.') {
                        let col_part = &raw[dot + 1..];
                        src_row.get(col_part).cloned().unwrap_or_else(|| raw.trim_matches('\'').to_string())
                    } else {
                        raw.trim_matches('\'').to_string()
                    };
                    row.insert(col.clone(), value);
                }
                insert_rows.push(row);
            }
        }

        let update_count = update_rows.len();
        let delete_count = delete_pks.len();
        let insert_count = insert_rows.len();

        // Apply updates — only update the visible row (is_visible check prevents hitting dead rows)
        if let Some(rows) = s.tables.get_mut(&target) {
            for (pk, resolved) in update_rows {
                if let Some(row) = rows.iter_mut().find(|r| {
                    r.get(&pk_col).map(|v| v == &pk).unwrap_or(false) && Self::is_visible(r)
                }) {
                    for (col, val) in resolved {
                        row.insert(col, val);
                    }
                }
            }
            rows.retain(|r| !delete_pks.contains(&r.get(&pk_col).cloned().unwrap_or_default()));
        }

        // Apply inserts — assign auto-increment id and MVCC fields
        {
            let ai_cols: Vec<(String, bool)> = s.catalog.get_table(&target)
                .map(|sc| sc.columns.iter().map(|c| (c.name.clone(), c.auto_increment)).collect())
                .unwrap_or_default();
            let mut local_counters: HashMap<String, i64> = s.catalog.get_table(&target)
                .map(|sc| sc.auto_increment_counters.clone())
                .unwrap_or_default();
            // Seed counter from visible rows if not already set
            for (col_name, is_ai) in &ai_cols {
                if *is_ai && !local_counters.contains_key(col_name) {
                    let max_id = s.tables.get(&target).map(|rows| {
                        rows.iter().filter(|r| Self::is_visible(r))
                            .filter_map(|r| r.get(col_name).and_then(|v| v.parse::<i64>().ok()))
                            .max().unwrap_or(0)
                    }).unwrap_or(0);
                    local_counters.insert(col_name.clone(), max_id);
                }
            }
            let txn_id = self.txn.current_txn_id().to_string();
            if let Some(rows) = s.tables.get_mut(&target) {
                for mut row in insert_rows {
                    for (col_name, is_ai) in &ai_cols {
                        if *is_ai && (!row.contains_key(col_name) || row[col_name].is_empty()) {
                            let counter = local_counters.entry(col_name.clone()).or_insert(0);
                            *counter += 1;
                            row.insert(col_name.clone(), counter.to_string());
                        }
                    }
                    row.entry("_xmin".to_string()).or_insert_with(|| txn_id.clone());
                    row.entry("_xmax".to_string()).or_insert_with(|| "0".to_string());
                    rows.push(row);
                }
            }
            if let Some(ts) = s.catalog.get_table_mut(&target) {
                ts.auto_increment_counters = local_counters;
            }
        }

        // Sync buffer pool so subsequent SELECTs see the MERGE changes
        if !self.txn.is_active() {
            if let Some(rows) = s.tables.get(&target) {
                let rows_clone = rows.clone();
                s.buffer_pool.write_page(&target, rows_clone);
                s.buffer_pool.flush_page(&target, &s.disk);
            }
        }

        Ok(format!("MERGE: {} updated, {} deleted, {} inserted.", update_count, delete_count, insert_count))
    }

    // ── 저장 프로시저 ────────────────────────────────────────────────────────
    fn exec_create_procedure(
        &mut self,
        s: &mut SharedDatabase,
        name: String,
        params: Vec<(String, String, String)>,
        body: Vec<Statement>,
    ) -> Result<String, String> {
        s.procedures.insert(name.clone(), (params, body));
        Ok(format!("Procedure '{}' created.", name))
    }

    /// Run a list of statements in procedure context; stops early on proc_signal
    fn exec_proc_stmts(&mut self, s: &mut SharedDatabase, stmts: Vec<Statement>) -> Result<String, String> {
        let mut last = String::new();
        for stmt in stmts {
            if self.proc_signal.is_some() { break; }
            last = self.execute_with_s(s, stmt)?;
        }
        Ok(last)
    }

    fn exec_proc_if(
        &mut self,
        s: &mut SharedDatabase,
        condition: CondExpr,
        then_body: Vec<Statement>,
        elseif_branches: Vec<(CondExpr, Vec<Statement>)>,
        else_body: Option<Vec<Statement>>,
    ) -> Result<String, String> {
        if Self::eval_condexpr(&self.proc_vars.clone(), &condition) {
            return self.exec_proc_stmts(s, then_body);
        }
        for (cond, body) in elseif_branches {
            if Self::eval_condexpr(&self.proc_vars.clone(), &cond) {
                return self.exec_proc_stmts(s, body);
            }
        }
        if let Some(body) = else_body {
            return self.exec_proc_stmts(s, body);
        }
        Ok(String::new())
    }

    fn exec_proc_while(
        &mut self,
        s: &mut SharedDatabase,
        label: Option<String>,
        condition: CondExpr,
        body: Vec<Statement>,
    ) -> Result<String, String> {
        let mut last = String::new();
        loop {
            if !Self::eval_condexpr(&self.proc_vars.clone(), &condition) { break; }
            last = self.exec_proc_stmts(s, body.clone())?;
            match &self.proc_signal {
                Some(ProcSignal::Leave(lbl)) if lbl.as_deref() == label.as_deref() || lbl.is_none() => {
                    self.proc_signal = None; break;
                }
                Some(ProcSignal::Leave(_)) => break, // propagate to outer loop
                Some(ProcSignal::Iterate(lbl)) if lbl.as_deref() == label.as_deref() || lbl.is_none() => {
                    self.proc_signal = None; // continue
                }
                Some(ProcSignal::Iterate(_)) => break, // propagate
                None => {}
            }
        }
        Ok(last)
    }

    fn exec_proc_loop(
        &mut self,
        s: &mut SharedDatabase,
        label: Option<String>,
        body: Vec<Statement>,
    ) -> Result<String, String> {
        let mut last;
        loop {
            last = self.exec_proc_stmts(s, body.clone())?;
            match &self.proc_signal {
                Some(ProcSignal::Leave(lbl)) if lbl.as_deref() == label.as_deref() || lbl.is_none() => {
                    self.proc_signal = None; break;
                }
                Some(ProcSignal::Leave(_)) => break,
                Some(ProcSignal::Iterate(lbl)) if lbl.as_deref() == label.as_deref() || lbl.is_none() => {
                    self.proc_signal = None; // continue loop
                }
                Some(ProcSignal::Iterate(_)) => break,
                None => {}
            }
        }
        Ok(last)
    }

    fn exec_proc_repeat(
        &mut self,
        s: &mut SharedDatabase,
        label: Option<String>,
        body: Vec<Statement>,
        until: CondExpr,
    ) -> Result<String, String> {
        let mut last;
        loop {
            last = self.exec_proc_stmts(s, body.clone())?;
            match &self.proc_signal {
                Some(ProcSignal::Leave(lbl)) if lbl.as_deref() == label.as_deref() || lbl.is_none() => {
                    self.proc_signal = None; break;
                }
                Some(ProcSignal::Leave(_)) => break,
                Some(ProcSignal::Iterate(lbl)) if lbl.as_deref() == label.as_deref() || lbl.is_none() => {
                    self.proc_signal = None;
                    // fall through to UNTIL check
                }
                Some(ProcSignal::Iterate(_)) => break,
                None => {}
            }
            if Self::eval_condexpr(&self.proc_vars.clone(), &until) { break; }
        }
        Ok(last)
    }

    fn exec_call_procedure(
        &mut self,
        s: &mut SharedDatabase,
        name: String,
        args: Vec<String>,
    ) -> Result<String, String> {
        let (params, body) = s.procedures.get(&name)
            .ok_or(format!("Procedure '{}' not found", name))?.clone();

        // save outer proc_vars, bind IN params
        let saved_vars = std::mem::take(&mut self.proc_vars);
        for (i, (dir, pname, _ptype)) in params.iter().enumerate() {
            if dir == "IN" || dir == "INOUT" {
                let val = args.get(i).cloned().unwrap_or_default();
                self.proc_vars.insert(pname.clone(), val);
            }
        }

        let last = self.exec_proc_stmts(s, body)?;
        self.proc_vars = saved_vars;
        self.proc_signal = None; // clear any signal that escaped the body

        Ok(if last.is_empty() { format!("Procedure '{}' executed.", name) } else { last })
    }

    fn exec_drop_procedure(
        &mut self,
        s: &mut SharedDatabase,
        name: String,
        if_exists: bool,
    ) -> Result<String, String> {
        if s.procedures.remove(&name).is_none() && !if_exists {
            return Err(format!("Procedure '{}' does not exist", name));
        }
        Ok(format!("Procedure '{}' dropped.", name))
    }

    // ── 사용자 정의 함수 ──────────────────────────────────────────────────────
    fn exec_create_function(
        &mut self,
        s: &mut SharedDatabase,
        name: String,
        params: Vec<String>,
        body: String,
    ) -> Result<String, String> {
        s.user_functions.insert(name.clone(), (params, body));
        Ok(format!("Function '{}' created.", name))
    }

    fn exec_drop_function(
        &mut self,
        s: &mut SharedDatabase,
        name: String,
        if_exists: bool,
    ) -> Result<String, String> {
        if s.user_functions.remove(&name).is_none() && !if_exists {
            return Err(format!("Function '{}' does not exist", name));
        }
        Ok(format!("Function '{}' dropped.", name))
    }

    // ── 트리거 ────────────────────────────────────────────────────────────────
    fn exec_create_trigger(
        &mut self,
        s: &mut SharedDatabase,
        name: String,
        timing: TriggerTiming,
        event: TriggerEvent,
        table: String,
        body: Vec<Statement>,
    ) -> Result<String, String> {
        let timing_str = match timing { TriggerTiming::Before => "BEFORE", TriggerTiming::After => "AFTER" };
        let event_str  = match event  { TriggerEvent::Insert  => "INSERT", TriggerEvent::Update => "UPDATE", TriggerEvent::Delete => "DELETE" };
        s.triggers.insert(name.clone(), (table, timing_str.to_string(), event_str.to_string(), body));
        Ok(format!("Trigger '{}' created.", name))
    }

    fn exec_drop_trigger(
        &mut self,
        s: &mut SharedDatabase,
        name: String,
        if_exists: bool,
    ) -> Result<String, String> {
        if s.triggers.remove(&name).is_none() && !if_exists {
            return Err(format!("Trigger '{}' does not exist", name));
        }
        Ok(format!("Trigger '{}' dropped.", name))
    }

    fn fire_triggers(&mut self, s: &mut SharedDatabase, table: &str, timing: &str, event: &str) {
        let bodies: Vec<Vec<Statement>> = s.triggers.values()
            .filter(|(t, ti, ev, _)| t == table && ti == timing && ev == event)
            .map(|(_, _, _, body)| body.clone())
            .collect();
        for body in bodies {
            for stmt in body {
                let _ = self.execute_with_s(s, stmt);
            }
        }
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
        ArithExpr::Cmp(l, op, r) => format!("{}{}{}", arith_to_str(l), op, arith_to_str(r)),
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
