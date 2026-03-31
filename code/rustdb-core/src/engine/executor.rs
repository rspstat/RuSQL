// src/engine/executor.rs

use std::collections::HashMap;
use crate::transaction::txn_manager::TransactionManager;
use crate::parser::ast::*;
use crate::catalog::schema::{Catalog, ColumnDef as SchemaCol};
use crate::storage::disk::DiskManager;
use crate::storage::btree::BPlusTree;
use crate::storage::buffer_pool::BufferPool;

pub type Row = HashMap<String, String>;
pub const NULL_VALUE: &str = "NULL";

pub struct Executor {
    pub catalog: Catalog,
    pub tables: HashMap<String, Vec<Row>>,
    pub indexes: HashMap<String, BPlusTree>,
    pub index_meta: HashMap<String, (String, String)>,
    pub views: HashMap<String, Statement>,
    pub txn: TransactionManager,
    pub buffer_pool: BufferPool,  // ← 추가
    disk: DiskManager,
}

impl Executor {
    pub fn new() -> Self {
        let disk = DiskManager::new();
        let mut catalog = Catalog::new();
        let mut tables = HashMap::new();
        let mut indexes = HashMap::new();

        for table_name in disk.list_tables() {
            if let Some(columns) = disk.load_schema(&table_name) {
                let schema_cols = columns.iter().map(|c| SchemaCol {
                    name: c.clone(),
                    data_type: crate::parser::ast::DataType::Text,
                    primary_key: false,
                    not_null: false,
                    unique: false,
                    auto_increment: false,
                    foreign_key: None,
                }).collect();
                let _ = catalog.create_table(table_name.clone(), schema_cols);
                let rows = disk.load_table(&table_name);

                let mut tree = BPlusTree::new();
                for row in &rows {
                    if let Some(first_col) = columns.first() {
                        if let Some(key) = row.get(first_col) {
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
            views: HashMap::new(),
            txn: TransactionManager::new(),
            buffer_pool: BufferPool::new(),
            disk,
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
            Statement::DropTable { name }            => self.exec_drop(name),
            Statement::TruncateTable { name }        => self.exec_truncate(name),
            Statement::Insert { table, values }      => self.exec_insert(table, values),
            Statement::Select { table, columns, condition, join, order_by, group_by, having, limit } => {
                self.exec_select(table, columns, condition, join, order_by, group_by, having, limit)
            }
            Statement::Update { table, assignments, condition } => {
                self.exec_update(table, assignments, condition)
            }
            Statement::Delete { table, condition }   => self.exec_delete(table, condition),
            Statement::AlterTable { table, action }  => self.exec_alter(table, action),
            Statement::CreateIndex { index_name, table, column } => {
                self.exec_create_index(index_name, table, column)
            }
            Statement::DropIndex { index_name } => self.exec_drop_index(index_name),
            Statement::CreateView { name, query } => self.exec_create_view(name, *query),
            Statement::DropView { name } => self.exec_drop_view(name),
            Statement::ShowTables => self.exec_show_tables(),
            Statement::Describe { table } => self.exec_describe(table),
            Statement::ShowBufferPool => self.exec_show_buffer_pool(),
            Statement::ShowWal => self.exec_show_wal(),
        }
    }

    fn exec_create(&mut self, name: String, columns: Vec<ColumnDef>) -> Result<String, String> {
        let col_names: Vec<String> = columns.iter().map(|c| c.name.clone()).collect();
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
        self.disk.save_schema(&name, &col_names);
        Ok(format!("Table '{}' created.", name))
    }

    fn exec_drop(&mut self, name: String) -> Result<String, String> {
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
        for (i, (_, _, _, auto_inc)) in constraints.iter().enumerate() {
            if *auto_inc && final_values[i].is_empty() {
                let schema_mut = self.catalog.get_table_mut(&table).unwrap();
                let counter = schema_mut.auto_increment_counters
                    .entry(col_names[i].clone()).or_insert(0);
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

        self.tables.get_mut(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .push(row);

        if !self.txn.is_active() {
            let rows = self.tables.get(&table).unwrap().clone();
            self.buffer_pool.write_page(&table, rows);
            self.buffer_pool.flush_page(&table, &self.disk);
        }

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
    ) -> Result<String, String> {

        // 뷰 처리
        if let Some(view_stmt) = self.views.get(&table).cloned() {
            if let Statement::Select { table: vt, columns: vc, condition: vcond,
                join: vj, order_by: vo, group_by: vg, having: vh, limit: vl } = view_stmt {
                return self.exec_select(vt, vc, vcond, vj, vo, vg, vh, vl);
            }
        }

        // B+Tree 인덱스 검색
        let has_agg = columns.iter().any(|c| matches!(c, SelectColumn::Agg { .. }));
        if join.is_none() && !has_agg {
            if let Some(cond) = &condition {
                if cond.operator == Operator::Eq {
                    if let ConditionValue::Literal(lit) = &cond.value {
                        let schema = self.catalog.get_table(&table)
                            .ok_or(format!("Table '{}' not found", table))?;
                        let first_col = schema.columns.first().map(|c| c.name.clone());
                        if first_col.as_deref() == Some(cond.column.as_str()) {
                            if let Some(index) = self.indexes.get(&table) {
                                if let Some(val_json) = index.search(lit) {
                                    let row: Row = serde_json::from_str(&val_json).unwrap();
                                    return self.format_result(vec![row], columns, table, None);
                                } else {
                                    return Ok("0 rows returned.".to_string());
                                }
                            }
                        }
                    }
                }
            }
        }

        if !self.tables.contains_key(&table) {
            return Err(format!("Table '{}' not found", table));
        }
        let rows = self.buffer_pool.get_page(&table, &self.disk);

        let result: Vec<Row> = if let Some(ref j) = join {
            let right_rows = self.tables.get(&j.table)
                .ok_or(format!("Table '{}' not found", j.table))?.clone();
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

        // GROUP BY
        if let Some(ref group_cols) = group_by {
            let mut seen = std::collections::HashSet::new();
            result.retain(|row| {
                let key: Vec<String> = group_cols.iter()
                    .map(|c| row.get(c).cloned().unwrap_or_default())
                    .collect();
                seen.insert(key)
            });
        }

        // HAVING
        if let Some(ref hav) = having {
            result.retain(|row| Self::matches_condition(row, &Some(hav.clone())));
        }

        // LIMIT
        if let Some(n) = limit {
            result.truncate(n);
        }

        // 집계 함수 처리
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

        self.format_result(result, columns, table, join)
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
                _ => None,
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

        for row in rows.iter_mut() {
            if Self::matches_condition(row, &condition) {
                let key = row.get(&pk_col).cloned().unwrap_or_default();
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

        let rows = self.tables.get(&table).unwrap().clone();
        self.buffer_pool.write_page(&table, rows);
        self.buffer_pool.flush_page(&table, &self.disk);
        Ok(format!("{} row(s) updated.", count))
    }

    fn exec_delete(&mut self, table: String, condition: Option<Condition>) -> Result<String, String> {
        let rows_to_delete: Vec<Row> = self.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .iter()
            .filter(|r| Self::matches_condition(r, &condition))
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
                                        let referenced = other_rows.iter().any(|r| {
                                            r.get(&col.name).map(|v| v == &del_val).unwrap_or(false)
                                        });
                                        if referenced {
                                            return Err(format!(
                                                "Foreign key violation: row in '{}' is referenced by '{}'.'{}'",
                                                table, other_table, col.name
                                            ));
                                        }
                                    }
                                }
                                crate::catalog::schema::FkAction::Cascade => {
                                    if let Some(other_rows) = self.tables.get_mut(other_table) {
                                        other_rows.retain(|r| {
                                            r.get(&col.name).map(|v| v != &del_val).unwrap_or(true)
                                        });
                                    }
                                    let rows_clone = self.tables.get(other_table).unwrap().clone();
                                    self.buffer_pool.write_page(other_table, rows_clone.clone());
                                    self.buffer_pool.flush_page(other_table, &self.disk);
                                }
                                crate::catalog::schema::FkAction::SetNull => {
                                    if let Some(other_rows) = self.tables.get_mut(other_table) {
                                        for row in other_rows.iter_mut() {
                                            if row.get(&col.name).map(|v| v == &del_val).unwrap_or(false) {
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

        // 실제 삭제
        let rows = self.tables.get_mut(&table).unwrap();
        let before = rows.len();
        rows.retain(|r| !Self::matches_condition(r, &condition));
        let deleted = before - rows.len();

        let rows_clone = self.tables.get(&table).unwrap().clone();
        if let Some(index) = self.indexes.get_mut(&table) {
            *index = BPlusTree::new();
            for row in &rows_clone {
                let key = row.values().next().cloned().unwrap_or_default();
                let val_json = serde_json::to_string(row).unwrap();
                index.insert(key, val_json);
            }
        }

        self.disk.save_table(&table, self.tables.get(&table).unwrap());
        Ok(format!("{} row(s) deleted.", deleted))
    }

    fn exec_begin(&mut self) -> Result<String, String> {
        let txn_id = self.txn.begin()?;
        Ok(format!("Transaction {} started.", txn_id))
    }

    fn exec_commit(&mut self) -> Result<String, String> {
        self.txn.commit()?;
        Ok("Transaction committed.".to_string())
    }

    fn exec_rollback(&mut self) -> Result<String, String> {
        let undo_entries = self.txn.abort()?;
        for entry in undo_entries {
            match entry.operation.as_str() {
                "INSERT" => {
                    if let Some(rows) = self.tables.get_mut(&entry.table) {
                        rows.retain(|r| r.get("id").map(|v| v != &entry.key).unwrap_or(true));
                    }
                    let rows_clone = self.tables.get(&entry.table).unwrap().clone();
                    if let Some(index) = self.indexes.get_mut(&entry.table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.values().next().cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    self.disk.save_table(&entry.table, self.tables.get(&entry.table).unwrap());
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
                    if let Some(old_json) = &entry.old_data {
                        if let Ok(old_row) = serde_json::from_str::<Row>(old_json) {
                            if let Some(rows) = self.tables.get_mut(&entry.table) {
                                rows.push(old_row);
                            }
                            self.disk.save_table(&entry.table, self.tables.get(&entry.table).unwrap());
                        }
                    }
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
                let col_names: Vec<String> = self.catalog.tables.get(&table)
                    .unwrap().columns.iter().map(|c| c.name.clone()).collect();
                self.disk.save_schema(&table, &col_names);
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
                let col_names: Vec<String> = self.catalog.tables.get(&table)
                    .unwrap().columns.iter().map(|c| c.name.clone()).collect();
                self.disk.save_schema(&table, &col_names);
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
                let col_names: Vec<String> = self.catalog.tables.get(&table)
                    .unwrap().columns.iter().map(|c| c.name.clone()).collect();
                self.disk.save_schema(&table, &col_names);
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
                    join, order_by, group_by, having, limit
                } = *sub_stmt.clone() {
                    let result = self.exec_select(
                        table, columns.clone(), sub_cond,
                        join, order_by, group_by, having, limit
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

    fn exec_create_index(&mut self, index_name: String, table: String, column: String) -> Result<String, String> {
        if !self.tables.contains_key(&table) {
            return Err(format!("Table '{}' not found", table));
        }
        let mut tree = BPlusTree::new();
        if let Some(rows) = self.tables.get(&table) {
            for row in rows {
                if let Some(val) = row.get(&column) {
                    let json = serde_json::to_string(row).unwrap();
                    tree.insert(val.clone(), json);
                }
            }
        }
        let key = format!("{}_{}", table, index_name);
        self.indexes.insert(key.clone(), tree);
        self.index_meta.insert(index_name.clone(), (table.clone(), column.clone()));
        Ok(format!("Index '{}' created on '{}'.'{}'.", index_name, table, column))
    }

    fn exec_drop_index(&mut self, index_name: String) -> Result<String, String> {
        if let Some((table, _)) = self.index_meta.remove(&index_name) {
            let key = format!("{}_{}", table, index_name);
            self.indexes.remove(&key);
            Ok(format!("Index '{}' dropped.", index_name))
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

    fn recover_from_wal(&mut self) {
        let records = self.txn.wal_records();
        if records.is_empty() { return; }

        // COMMIT 레코드가 있는지 확인
        let has_commit = records.iter().any(|r| {
            matches!(r.op, crate::transaction::wal::WalOp::Commit)
        });

        if !has_commit {
            // 미완료 트랜잭션 → WAL 삭제 (rollback 처리)
            self.txn.wal_clear();
            eprintln!("[Recovery] 미완료 트랜잭션 감지 → WAL 삭제");
            return;
        }

        // COMMIT된 트랜잭션 replay
        eprintln!("[Recovery] WAL replay 시작 ({} 레코드)", records.len());
        for record in &records {
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