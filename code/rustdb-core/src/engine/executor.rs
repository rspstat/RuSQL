// src/engine/executor.rs

use std::collections::HashMap;
use crate::transaction::txn_manager::TransactionManager;
use crate::parser::ast::*;
use crate::catalog::schema::{Catalog, ColumnDef as SchemaCol};
use crate::storage::disk::DiskManager;
use crate::storage::btree::BPlusTree;
use crate::storage::buffer_pool::BufferPool;
use crate::storage::composite_index::CompositeIndex;

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
    /// Row-level lock: (table, pk_val) → txn_id
    row_locks: HashMap<(String, String), u64>,
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

                let _ = catalog.create_table(table_name.clone(), schema.columns.clone());

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

        let mut executor = Executor {
            catalog,
            tables,
            indexes,
            index_meta: HashMap::new(),
            composite_indexes: HashMap::new(),
            views: HashMap::new(),
            txn: TransactionManager::new(),
            buffer_pool: BufferPool::new(),
            disk,
            row_locks: HashMap::new(),
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
            Statement::CreateTable { name, columns } => self.exec_create(name, columns),
            Statement::DropTable { name, if_exists }  => self.exec_drop(name, if_exists),
            Statement::TruncateTable { name }        => self.exec_truncate(name),
            Statement::Insert { table, values }      => self.exec_insert(table, values),
            Statement::Select { table, columns, condition, join, order_by, group_by, having, limit, for_update } => {
                self.exec_select(table, columns, condition, join, order_by, group_by, having, limit, for_update)
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
        }
    }

    /// MVCC 가시성 판정: _xmax == "0" 또는 없으면 visible
    fn is_visible(row: &Row) -> bool {
        row.get("_xmax").map(|v| v == "0").unwrap_or(true)
    }

    fn exec_create(&mut self, name: String, columns: Vec<ColumnDef>) -> Result<String, String> {
        let _col_names: Vec<String> = columns.iter().map(|c| c.name.clone()).collect();
        let schema_cols = columns.into_iter().map(|c| SchemaCol {
            name: c.name,
            data_type: c.data_type,
            primary_key: c.primary_key,
            not_null: c.not_null,
            unique: c.unique,
            auto_increment: c.auto_increment,
            foreign_key: c.foreign_key.map(|fk| crate::catalog::schema::ForeignKey {
                column: fk.column,
                ref_table: fk.ref_table,
                ref_column: fk.ref_column,
                on_delete: match fk.on_delete {
                    crate::parser::ast::FkAction::Restrict => crate::catalog::schema::FkAction::Restrict,
                    crate::parser::ast::FkAction::Cascade  => crate::catalog::schema::FkAction::Cascade,
                    crate::parser::ast::FkAction::SetNull  => crate::catalog::schema::FkAction::SetNull,
                },
            }),
        }).collect();
        self.catalog.create_table(name.clone(), schema_cols)?;
        self.tables.insert(name.clone(), Vec::new());
        self.indexes.insert(name.clone(), BPlusTree::new());
        // 전체 스키마(타입, PK, auto_increment 등) 저장
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

    fn exec_insert(&mut self, table: String, values: Vec<String>) -> Result<String, String> {
        // schema를 클론해서 borrow 충돌 방지
        let schema = self.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .clone();

        if values.len() != schema.columns.len() {
            return Err(format!(
                "Column count mismatch: expected {}, got {}",
                schema.columns.len(), values.len()
            ));
        }

        let col_names: Vec<String> = schema.columns.iter().map(|c| c.name.clone()).collect();
        let constraints: Vec<(bool, bool, bool, bool)> = schema.columns.iter()
            .map(|c| (c.primary_key, c.not_null, c.unique, c.auto_increment))
            .collect();

        let mut final_values = values.clone();

        // AUTO INCREMENT 처리
        let mut auto_inc_changed = false;
        for (i, (_, _, _, auto_inc)) in constraints.iter().enumerate() {
            if *auto_inc && final_values[i].is_empty() {
                let schema_mut = self.catalog.get_table_mut(&table).unwrap();
                let counter = schema_mut.auto_increment_counters
                    .entry(col_names[i].clone()).or_insert(0);
                *counter += 1;
                final_values[i] = counter.to_string();
                auto_inc_changed = true;
            }
        }
        // AUTO INCREMENT 카운터를 스키마 파일에 즉시 반영
        if auto_inc_changed {
            let schema = self.catalog.get_table(&table).unwrap();
            self.disk.save_schema(&table, schema);
        }

        // NOT NULL 검사
        for (i, (_, not_null, _, _)) in constraints.iter().enumerate() {
            if *not_null && (final_values[i].is_empty() || final_values[i] == NULL_VALUE) {
                return Err(format!("Column '{}' cannot be NULL", col_names[i]));
            }
        }

        // UNIQUE / PRIMARY KEY 중복 검사
        if let Some(rows) = self.tables.get(&table) {
            for (i, (pk, _, unique, _)) in constraints.iter().enumerate() {
                if *pk || *unique {
                    let val = &final_values[i];
                    for existing in rows {
                        if existing.get(&col_names[i]) == Some(val) {
                            return Err(format!(
                                "Duplicate value '{}' for column '{}'", val, col_names[i]
                            ));
                        }
                    }
                }
            }
        }

        let mut row = Row::new();
        for (col, val) in col_names.iter().zip(final_values.iter()) {
            let stored_val = if val.is_empty() { NULL_VALUE.to_string() } else { val.clone() };
            row.insert(col.clone(), stored_val);
        }
        // MVCC 버전 스탬프
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

        let key = final_values[0].clone();
        let val_json = serde_json::to_string(&row).unwrap();

        self.txn.log_insert(&table, &key, &val_json);

        if let Some(index) = self.indexes.get_mut(&table) {
            index.insert(key, val_json);
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

        // 클러스터드 인덱스: PK 기준으로 물리적 정렬 유지
        self.sort_by_pk(&table);

        // 버퍼 풀 항상 갱신 (SELECT가 최신 데이터를 읽도록)
        let rows = self.tables.get(&table).unwrap().clone();
        self.buffer_pool.write_page(&table, rows);
        if !self.txn.is_active() {
            self.buffer_pool.flush_page(&table, &self.disk);
        }

        self.maybe_auto_checkpoint();
        Ok("1 row inserted.".to_string())
    }

    fn matches_condition(row: &Row, condition: &Option<Condition>) -> bool {
        match condition {
            None => true,
            Some(cond) => Self::eval_condition(row, cond),
        }
    }

    fn eval_condition(row: &Row, cond: &Condition) -> bool {
        let base = Self::eval_single(row, cond);
        if let Some(and_cond) = &cond.and {
            base && Self::eval_condition(row, and_cond)
        } else if let Some(or_cond) = &cond.or {
            base || Self::eval_condition(row, or_cond)
        } else {
            base
        }
    }

    fn eval_single(row: &Row, cond: &Condition) -> bool {
        let val = match row.get(&cond.column) {
            Some(v) => v.clone(),
            None => return false,
        };

        let cmp_num = |a: &str, b: &str| -> Option<std::cmp::Ordering> {
            let a: f64 = a.parse().ok()?;
            let b: f64 = b.parse().ok()?;
            a.partial_cmp(&b)
        };

        match &cond.value {
            ConditionValue::Subquery(_) => false,
            ConditionValue::Between(start, end) => {
                match (cmp_num(&val, start), cmp_num(&val, end)) {
                    (Some(s), Some(e)) =>
                        s != std::cmp::Ordering::Less && e != std::cmp::Ordering::Greater,
                    _ => val >= *start && val <= *end,
                }
            }
            ConditionValue::Literal(lit) => {
                match &cond.operator {
                    Operator::Eq  => &val == lit,
                    Operator::IsNull    => val == NULL_VALUE || val.is_empty(),
                    Operator::IsNotNull => val != NULL_VALUE && !val.is_empty(),
                    Operator::Ne  => &val != lit,
                    Operator::In  => false,
                    Operator::Like => {
                        let val_chars: Vec<char> = val.chars().collect();
                        let pat_chars: Vec<char> = lit.chars().collect();
                        like_match(&val_chars, &pat_chars)
                    }
                    Operator::Between => false,
                    Operator::Gt  => cmp_num(&val, lit)
                        .map(|o| o == std::cmp::Ordering::Greater).unwrap_or(false),
                    Operator::Lt  => cmp_num(&val, lit)
                        .map(|o| o == std::cmp::Ordering::Less).unwrap_or(false),
                    Operator::Gte => cmp_num(&val, lit)
                        .map(|o| o != std::cmp::Ordering::Less).unwrap_or(false),
                    Operator::Lte => cmp_num(&val, lit)
                        .map(|o| o != std::cmp::Ordering::Greater).unwrap_or(false),
                }
            }
        }
    }

    fn exec_select(
        &mut self,
        table: String,
        columns: Vec<SelectColumn>,
        condition: Option<Condition>,
        join: Option<Join>,
        order_by: Option<OrderBy>,
        group_by: Option<Vec<String>>,
        having: Option<Condition>,
        limit: Option<usize>,
        for_update: bool,
    ) -> Result<String, String> {

        // 뷰 처리
        if let Some(view_stmt) = self.views.get(&table).cloned() {
            if let Statement::Select { table: vt, columns: vc, condition: vcond,
                join: vj, order_by: vo, group_by: vg, having: vh, limit: vl, .. } = view_stmt {
                return self.exec_select(vt, vc, vcond, vj, vo, vg, vh, vl, false);
            }
        }

        // B+Tree 인덱스 검색 (FOR UPDATE는 잠금 처리를 위해 풀 스캔 경로 사용)
        let has_agg = columns.iter().any(|c| matches!(c, SelectColumn::Agg { .. }));
        if join.is_none() && !has_agg && !for_update {
            if let Some(cond) = &condition {
                // 단일 컬럼 PK 인덱스 검색
                if cond.operator == Operator::Eq {
                    if let ConditionValue::Literal(lit) = &cond.value {
                        let schema = self.catalog.get_table(&table)
                            .ok_or(format!("Table '{}' not found", table))?;
                        let first_col = schema.columns.first().map(|c| c.name.clone());
                        if first_col.as_deref() == Some(cond.column.as_str()) {
                            if let Some(index) = self.indexes.get(&table) {
                                if let Some(val_json) = index.search(lit) {
                                    let row: Row = serde_json::from_str(&val_json).unwrap();
                                    if Self::is_visible(&row) {
                                        return self.format_result(vec![row], columns, table, None);
                                    } else {
                                        return Ok("0 rows returned.".to_string());
                                    }
                                } else {
                                    return Ok("0 rows returned.".to_string());
                                }
                            }
                        }
                    }
                }

                // 클러스터드 인덱스 범위 스캔: WHERE pk BETWEEN a AND b
                if cond.operator == Operator::Between
                    && cond.and.is_none()
                    && cond.or.is_none()
                {
                    if let ConditionValue::Between(start, end) = &cond.value {
                        if let Some(schema) = self.catalog.get_table(&table) {
                            let pk_col = schema.columns.iter()
                                .find(|c| c.primary_key)
                                .map(|c| c.name.clone());
                            if pk_col.as_deref() == Some(cond.column.as_str()) {
                                if let Some(index) = self.indexes.get(&table) {
                                    let json_vals = index.range_search(start, end);
                                    let rows: Vec<Row> = json_vals.iter()
                                        .filter_map(|j| serde_json::from_str(j).ok())
                                        .collect();
                                    return self.format_result(rows, columns, table, None);
                                }
                            }
                        }
                    }
                }

                // 복합 인덱스 검색: WHERE col1 = v1 AND col2 = v2 ...
                let eq_map = collect_eq_conditions(cond);
                if !eq_map.is_empty() {
                    let matching_idx = self.composite_indexes.iter()
                        .find(|(_, ci)| ci.table == table && ci.matches_conditions(&eq_map))
                        .map(|(k, _)| k.clone());
                    if let Some(idx_key) = matching_idx {
                        let result = self.composite_indexes[&idx_key].search_from_eq_map(&eq_map);
                        if let Some(val_json) = result {
                            if let Ok(row) = serde_json::from_str::<Row>(&val_json) {
                                return self.format_result(vec![row], columns, table, None);
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

        let result: Vec<Row> = if let Some(ref j) = join {
            let right_rows = if let Some(snap) = self.txn.get_snapshot_table(&j.table) {
                snap.clone()
            } else {
                self.tables.get(&j.table)
                    .ok_or(format!("Table '{}' not found", j.table))?.clone()
            };
            let mut joined = Vec::new();
            for left in &rows {
                for right in &right_rows {
                    if left.get(&j.left_col) == right.get(&j.right_col) {
                        let mut merged = left.clone();
                        merged.extend(right.clone());
                        joined.push(merged);
                    }
                }
            }
            joined.into_iter()
                .filter(|r| self.matches_condition_with_subquery(r, &condition))
                .collect()
        } else {
            rows.into_iter()
                .filter(|r| self.matches_condition_with_subquery(r, &condition))
                .collect()
        };

        // ORDER BY
        let mut result = result;
        if let Some(ref ord) = order_by {
            result.sort_by(|a, b| {
                let av = a.get(&ord.column).cloned().unwrap_or_default();
                let bv = b.get(&ord.column).cloned().unwrap_or_default();
                let cmp = match (av.parse::<f64>(), bv.parse::<f64>()) {
                    (Ok(a), Ok(b)) => a.partial_cmp(&b).unwrap_or(std::cmp::Ordering::Equal),
                    _ => av.cmp(&bv),
                };
                if ord.ascending { cmp } else { cmp.reverse() }
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
                    .map(|c| row.get(c).cloned().unwrap_or_default())
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
                    if let SelectColumn::Agg { func, col: col_name } = col {
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
                        let label = Self::agg_label(func, col_name);
                        let v = if agg_val.fract() == 0.0 { format!("{}", agg_val as i64) }
                                else { format!("{:.2}", agg_val) };
                        out.insert(label, v);
                    }
                }
                out
            }).collect();

            // HAVING 필터 (집계된 컬럼 기준)
            if let Some(ref hav) = having {
                group_rows.retain(|row| Self::matches_condition(row, &Some(hav.clone())));
            }
            if let Some(n) = limit { group_rows.truncate(n); }
            return self.format_result(group_rows, columns, table, join);
        }

        // HAVING (GROUP BY 없는 경우)
        if let Some(ref hav) = having {
            result.retain(|row| Self::matches_condition(row, &Some(hav.clone())));
        }

        // LIMIT
        if let Some(n) = limit {
            result.truncate(n);
        }

        // 집계 함수 처리 (GROUP BY 없음)
        if has_agg {
            let mut agg_results: Vec<(String, String)> = Vec::new();
            for col in &columns {
                if let SelectColumn::Agg { func, col: col_name } = col {
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

                    let label = match func {
                        AggFunc::Count => format!("COUNT({})", col_name),
                        AggFunc::Sum   => format!("SUM({})", col_name),
                        AggFunc::Avg   => format!("AVG({})", col_name),
                        AggFunc::Min   => format!("MIN({})", col_name),
                        AggFunc::Max   => format!("MAX({})", col_name),
                    };

                    let val_str = if agg_val.fract() == 0.0 {
                        format!("{}", agg_val as i64)
                    } else {
                        format!("{:.2}", agg_val)
                    };
                    agg_results.push((label, val_str));
                }
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
                let lock_key = (table.clone(), pk_val.clone());
                match self.row_locks.get(&lock_key) {
                    Some(&holder) if holder != txn_id => {
                        return Err(format!(
                            "Row '{}' in '{}' is locked by transaction {}.",
                            pk_val, table, holder
                        ));
                    }
                    _ => {
                        self.row_locks.insert(lock_key, txn_id);
                    }
                }
            }
        }

        self.format_result(result, columns, table, join)
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
        join: Option<Join>,
    ) -> Result<String, String> {
        if result.is_empty() {
            return Ok("0 rows returned.".to_string());
        }

        let col_names: Vec<String> = if columns.iter().any(|c| c == &SelectColumn::All) {
            if let Some(ref j) = join {
                let left_cols = self.catalog.get_table(&table).unwrap()
                    .columns.iter().map(|c| c.name.clone());
                let right_cols = self.catalog.get_table(&j.table).unwrap()
                    .columns.iter().map(|c| c.name.clone());
                left_cols.chain(right_cols).collect()
            } else {
                self.catalog.get_table(&table).unwrap()
                    .columns.iter().map(|c| c.name.clone()).collect()
            }
        } else {
            columns.iter().filter_map(|c| match c {
                SelectColumn::Column(name) => Some(name.clone()),
                SelectColumn::Agg { func, col } => Some(Self::agg_label(func, col)),
                SelectColumn::All => None,
            }).collect()
        };

        let col_widths: Vec<usize> = col_names.iter().map(|col| {
            let max_val = result.iter()
                .map(|r| r.get(col).map(|v| v.len()).unwrap_or(0))
                .max().unwrap_or(0);
            col.len().max(max_val)
        }).collect();

        let mut output = String::new();
        let separator = col_widths.iter()
            .map(|w| "-".repeat(w + 2))
            .collect::<Vec<_>>().join("+");
        let separator = format!("+{}+", separator);

        output.push_str(&separator); output.push('\n');
        let header = col_names.iter().zip(col_widths.iter())
            .map(|(col, w)| format!(" {:width$} ", col, width = w))
            .collect::<Vec<_>>().join("|");
        output.push_str(&format!("|{}|\n", header));
        output.push_str(&separator); output.push('\n');

        for row in &result {
            let line = col_names.iter().zip(col_widths.iter())
                .map(|(col, w)| {
                    let val = row.get(col).cloned().unwrap_or_default();
                    let display = if val == NULL_VALUE { "NULL".to_string() } else { val };
                    format!(" {:width$} ", display, width = w)
                }).collect::<Vec<_>>().join("|");
            output.push_str(&format!("|{}|\n", line));
        }
        output.push_str(&separator);
        output.push_str(&format!("\n{} row(s) returned.", result.len()));
        Ok(output)
    }

    fn exec_update(
        &mut self,
        table: String,
        assignments: Vec<(String, String)>,
        condition: Option<Condition>,
    ) -> Result<String, String> {
        // PK 컬럼명 먼저 추출 (borrow 분리)
        let pk_col = self.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .columns.iter()
            .find(|c| c.primary_key)
            .map(|c| c.name.clone())
            .unwrap_or_else(|| "id".to_string());

        let rows = self.tables.get_mut(&table)
            .ok_or(format!("Table '{}' not found", table))?;

        let mut count = 0;
        let mut undo_entries: Vec<(String, String, String)> = Vec::new();
        let cur_txn = self.txn.current_txn_id();

        for row in rows.iter_mut() {
            if Self::matches_condition(row, &condition) {
                let key = row.get(&pk_col).cloned().unwrap_or_default();

                // 잠금 충돌 체크
                if let Some(&holder) = self.row_locks.get(&(table.clone(), key.clone())) {
                    if holder != cur_txn {
                        return Err(format!(
                            "Row '{}' in '{}' is locked by transaction {}. Cannot UPDATE.",
                            key, table, holder
                        ));
                    }
                }

                let old_json = serde_json::to_string(row).unwrap();
                for (col, val) in &assignments {
                    row.insert(col.clone(), val.clone());
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

        let rows = self.tables.get(&table).unwrap().clone();
        self.buffer_pool.write_page(&table, rows);
        self.buffer_pool.flush_page(&table, &self.disk);
        self.maybe_auto_checkpoint();
        Ok(format!("{} row(s) updated.", count))
    }

    fn exec_delete(&mut self, table: String, condition: Option<Condition>) -> Result<String, String> {
        let rows_to_delete: Vec<Row> = self.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .iter()
            .filter(|r| Self::is_visible(r) && Self::matches_condition(r, &condition))
            .cloned()
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
                                                row.insert(col.name.clone(), String::new());
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
                if Self::is_visible(row) && Self::matches_condition(row, &condition) {
                    let key = row.get(&pk_col).cloned().unwrap_or_default();

                    // 잠금 충돌 체크
                    if let Some(&holder) = self.row_locks.get(&(table.clone(), key.clone())) {
                        if holder != txn_id {
                            return Err(format!(
                                "Row '{}' in '{}' is locked by transaction {}. Cannot DELETE.",
                                key, table, holder
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
            rows.retain(|r| !(Self::is_visible(r) && Self::matches_condition(r, &condition)));
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
        self.row_locks.retain(|_, holder| *holder != txn_id);
        Ok("Transaction committed.".to_string())
    }

    /// 롤백 공통 헬퍼: exec_rollback과 SERIALIZABLE 자동 롤백에서 공유
    fn apply_rollback(&mut self) {
        let txn_id = self.txn.current_txn_id();
        let undo_entries = match self.txn.abort() {
            Ok(entries) => entries,
            Err(_) => return,
        };
        self.row_locks.retain(|_, holder| *holder != txn_id);
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get("id").map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = self.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.values().next().cloned().unwrap_or_default();
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
        self.row_locks.retain(|_, holder| *holder != txn_id);
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get("id").map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = self.tables.get(&entry.table).cloned().unwrap_or_default();
                    if let Some(index) = self.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.values().next().cloned().unwrap_or_default();
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
                    self.buffer_pool.write_page(&entry.table, rows_clone.clone());
                    self.buffer_pool.flush_page(&entry.table, &self.disk);
                }
                _ => {}
            }
        }
        Ok("Transaction rolled back.".to_string())
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
                not_null: false,
                unique: false,
                auto_increment: false,
                foreign_key: None,
            });
                if let Some(rows) = self.tables.get_mut(&table) {
                    for row in rows.iter_mut() {
                        row.insert(col.name.clone(), String::new());
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
        }
    }

    fn matches_condition_with_subquery(&mut self, row: &Row, condition: &Option<Condition>) -> bool {
        match condition {
            None => true,
            Some(cond) => {
                // AND/OR 체인 처리
                let base = self.eval_condition_with_subquery(row, cond);
                base
            }
        }
    }

        fn eval_condition_with_subquery(&mut self, row: &Row, cond: &Condition) -> bool {
            let base = self.eval_single_with_subquery(row, cond);
            if let Some(and_cond) = &cond.and.clone() {
                base && self.eval_condition_with_subquery(row, &and_cond)
            } else if let Some(or_cond) = &cond.or.clone() {
                base || self.eval_condition_with_subquery(row, &or_cond)
            } else {
                base
            }
        }

        fn eval_single_with_subquery(&mut self, row: &Row, cond: &Condition) -> bool {
        match &cond.value.clone() {
            ConditionValue::Literal(_) | ConditionValue::Between(_, _) => {
                Self::eval_single(row, cond)
            }
            ConditionValue::Subquery(sub_stmt) => {
                let val = match row.get(&cond.column) {
                    Some(v) => v.clone(),
                    None => return false,
                };

                if let Statement::Select {
                    table, columns, condition: sub_cond,
                    join, order_by, group_by, having, limit, ..
                } = *sub_stmt.clone() {
                    let result = self.exec_select(
                        table, columns.clone(), sub_cond,
                        join, order_by, group_by, having, limit, false
                    );

                    match result {
                        Ok(output) => {
                            let sub_vals = self.extract_values_from_output(&output);
                            match cond.operator {
                                Operator::In  => sub_vals.contains(&val),
                                Operator::Eq  => sub_vals.first()
                                    .map(|v| v == &val).unwrap_or(false),
                                // 숫자 비교 연산자 추가
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
                                    } else {
                                        false
                                    }
                                }
                                _ => false,
                            }
                        }
                        Err(_) => false,
                    }
                } else {
                    false
                }
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
            Ok(format!("Index '{}' created on '{}'.'{}'.", index_name, table, column))
        } else {
            // 복합 컬럼 → CompositeIndex
            let mut comp = CompositeIndex::new(table.clone(), columns.clone());
            if let Some(rows) = self.tables.get(&table) {
                comp.rebuild(rows);
            }
            self.composite_indexes.insert(index_name.clone(), comp);
            Ok(format!("Composite index '{}' created on '{}' ({}).", index_name, table, columns.join(", ")))
        }
    }

    fn exec_drop_index(&mut self, index_name: String) -> Result<String, String> {
        if let Some((table, _)) = self.index_meta.remove(&index_name) {
            let key = format!("{}_{}", table, index_name);
            self.indexes.remove(&key);
            Ok(format!("Index '{}' dropped.", index_name))
        } else if self.composite_indexes.remove(&index_name).is_some() {
            Ok(format!("Composite index '{}' dropped.", index_name))
        } else {
            Err(format!("Index '{}' not found", index_name))
        }
    }

    fn exec_create_view(&mut self, name: String, query: Statement) -> Result<String, String> {
        if let Statement::Select { ref table, .. } = query {
            if !self.tables.contains_key(table) {
                return Err(format!("Table '{}' not found", table));
            }
        }
        self.views.insert(name.clone(), query);
        Ok(format!("View '{}' created.", name))
    }

    fn exec_drop_view(&mut self, name: String) -> Result<String, String> {
        if self.views.remove(&name).is_some() {
            Ok(format!("View '{}' dropped.", name))
        } else {
            Err(format!("View '{}' not found", name))
        }
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
        let sep = "+------------------+---------+-----+-----+----------------+";
        output.push_str(&format!("{}\n", sep));
        output.push_str("| Field            | Type    | PK  | NN  | Auto Increment |\n");
        output.push_str(&format!("{}\n", sep));
        for col in &schema.columns {
            let type_str = match col.data_type {
                crate::parser::ast::DataType::Int     => "INT",
                crate::parser::ast::DataType::Text    => "TEXT",
                crate::parser::ast::DataType::Float   => "FLOAT",
                crate::parser::ast::DataType::Boolean => "BOOLEAN",
            };
            output.push_str(&format!(
                "| {:16} | {:7} | {:3} | {:3} | {:14} |\n",
                col.name, type_str,
                if col.primary_key { "YES" } else { "NO" },
                if col.not_null { "YES" } else { "NO" },
                if col.auto_increment { "YES" } else { "NO" },
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

    /// SHOW LOCKS: 현재 보유 중인 행 잠금 목록 출력
    fn exec_show_locks(&self) -> Result<String, String> {
        if self.row_locks.is_empty() {
            return Ok("No active row locks.".to_string());
        }

        let mut entries: Vec<(&(String, String), &u64)> = self.row_locks.iter().collect();
        entries.sort_by(|a, b| a.0.cmp(b.0));

        let mut output = String::from("+----------------+-----+--------+\n");
        output.push_str("| table          | key | txn_id |\n");
        output.push_str("+----------------+-----+--------+\n");
        for ((tbl, key), txn_id) in &entries {
            output.push_str(&format!(
                "| {:14} | {:3} | {:6} |\n",
                tbl, key, txn_id
            ));
        }
        output.push_str("+----------------+-----+--------+");
        Ok(output)
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

/// WHERE 조건 체인에서 `col = literal` 조건들을 수집
fn collect_eq_conditions(cond: &Condition) -> HashMap<String, String> {
    let mut map = HashMap::new();
    let mut cur = Some(cond);
    while let Some(c) = cur {
        if c.operator == Operator::Eq {
            if let ConditionValue::Literal(lit) = &c.value {
                map.insert(c.column.clone(), lit.clone());
            }
        }
        // AND 체인만 따라감 (OR는 복합 인덱스 적용 불가)
        cur = c.and.as_deref();
    }
    map
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