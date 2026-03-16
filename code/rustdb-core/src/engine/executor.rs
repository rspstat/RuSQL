// src/engine/executor.rs

use std::collections::HashMap;
use crate::transaction::txn_manager::{TransactionManager, UndoOp};
use crate::parser::ast::*;
use crate::catalog::schema::{Catalog, ColumnDef as SchemaCol};
use crate::storage::disk::DiskManager;
use crate::storage::btree::BPlusTree;

pub type Row = HashMap<String, String>;

pub struct Executor {
    pub catalog: Catalog,
    pub tables: HashMap<String, Vec<Row>>,
    pub indexes: HashMap<String, BPlusTree>,
    pub txn: TransactionManager,
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

        Executor { catalog, tables, indexes, txn: TransactionManager::new(), disk }
    }

    pub fn execute(&mut self, stmt: Statement) -> Result<String, String> {
        match stmt {
            Statement::Begin    => self.exec_begin(),
            Statement::Commit   => self.exec_commit(),
            Statement::Rollback => self.exec_rollback(),
            Statement::CreateTable { name, columns } => self.exec_create(name, columns),
            Statement::DropTable { name }            => self.exec_drop(name),
            Statement::Insert { table, values }      => self.exec_insert(table, values),
            Statement::Select { table, columns, condition, join, order_by, group_by, having, limit } => {
                self.exec_select(table, columns, condition, join, order_by, group_by, having, limit)
            }
            Statement::Update { table, assignments, condition } => {
                self.exec_update(table, assignments, condition)
            }
            Statement::Delete { table, condition }   => self.exec_delete(table, condition),
            Statement::AlterTable { table, action }  => self.exec_alter(table, action),
        }
    }

    fn exec_create(&mut self, name: String, columns: Vec<ColumnDef>) -> Result<String, String> {
        let col_names: Vec<String> = columns.iter().map(|c| c.name.clone()).collect();
        let schema_cols = columns.into_iter().map(|c| SchemaCol {
            name: c.name,
            data_type: c.data_type,
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

    fn exec_insert(&mut self, table: String, values: Vec<String>) -> Result<String, String> {
        let schema = self.catalog.get_table(&table)
            .ok_or(format!("Table '{}' not found", table))?;

        if values.len() != schema.columns.len() {
            return Err(format!(
                "Column count mismatch: expected {}, got {}",
                schema.columns.len(), values.len()
            ));
        }

        let col_names: Vec<String> = schema.columns.iter().map(|c| c.name.clone()).collect();
        let mut row = Row::new();
        for (col, val) in col_names.iter().zip(values.iter()) {
            row.insert(col.clone(), val.clone());
        }

        let key = values[0].clone();
        let val_json = serde_json::to_string(&row).unwrap();

        self.txn.log_insert(&table, &key, &val_json);

        if let Some(index) = self.indexes.get_mut(&table) {
            index.insert(key, val_json);
        }

        self.tables.get_mut(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .push(row);

        if !self.txn.is_active() {
            self.disk.save_table(&table, self.tables.get(&table).unwrap());
        }

        Ok("1 row inserted.".to_string())
    }

    fn matches_condition(row: &Row, condition: &Option<Condition>) -> bool {
        match condition {
            None => true,
            Some(cond) => {
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
                    ConditionValue::Subquery(_) => false, // 서브쿼리는 exec_select에서 처리
                    ConditionValue::Literal(lit) => {
                        match &cond.operator {
                            Operator::Eq  => &val == lit,
                            Operator::Ne  => &val != lit,
                            Operator::In  => false, // IN은 exec_select에서 처리
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

        // B+Tree 인덱스 검색 (첫 번째 컬럼 = 조건, JOIN/집계 없을 때만)
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

        let rows = self.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?.clone();

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

                    // 정수면 정수로 출력
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
                    format!(" {:width$} ", val, width = w)
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
        let rows = self.tables.get_mut(&table)
            .ok_or(format!("Table '{}' not found", table))?;
        let mut count = 0;
        for row in rows.iter_mut() {
            if Self::matches_condition(row, &condition) {
                for (col, val) in &assignments {
                    row.insert(col.clone(), val.clone());
                }
                count += 1;
            }
        }

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
        Ok(format!("{} row(s) updated.", count))
    }

    fn exec_delete(&mut self, table: String, condition: Option<Condition>) -> Result<String, String> {
        let rows = self.tables.get_mut(&table)
            .ok_or(format!("Table '{}' not found", table))?;
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
        let undo_ops = self.txn.abort()?;
        for op in undo_ops {
            match op {
                UndoOp::Insert { table, key } => {
                    if let Some(rows) = self.tables.get_mut(&table) {
                        rows.retain(|r| r.get("id").map(|v| v != &key).unwrap_or(true));
                    }
                    let rows_clone = self.tables.get(&table).unwrap().clone();
                    if let Some(index) = self.indexes.get_mut(&table) {
                        *index = BPlusTree::new();
                        for row in &rows_clone {
                            let k = row.values().next().cloned().unwrap_or_default();
                            let val_json = serde_json::to_string(row).unwrap();
                            index.insert(k, val_json);
                        }
                    }
                    self.disk.save_table(&table, self.tables.get(&table).unwrap());
                }
                UndoOp::Update { table, key: _, old_value } => {
                    if let Some(rows) = self.tables.get_mut(&table) {
                        for row in rows.iter_mut() {
                            if row.get("id") == old_value.get("id") {
                                *row = old_value.clone();
                            }
                        }
                    }
                    self.disk.save_table(&table, self.tables.get(&table).unwrap());
                }
                UndoOp::Delete { table, key: _, old_value } => {
                    if let Some(rows) = self.tables.get_mut(&table) {
                        rows.push(old_value);
                    }
                    self.disk.save_table(&table, self.tables.get(&table).unwrap());
                }
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
                match &cond.value.clone() {
                    ConditionValue::Literal(_) => Self::matches_condition(row, condition),
                    ConditionValue::Subquery(sub_stmt) => {
                        // 서브쿼리 실행해서 결과 값 목록 추출
                        let sub_result = self.execute(*sub_stmt.clone());
                        let val = match row.get(&cond.column) {
                            Some(v) => v.clone(),
                            None => return false,
                        };
                        // 서브쿼리 결과에서 첫 번째 컬럼 값들을 추출
                        if let Ok(_) = sub_result {
                            // 서브쿼리를 직접 테이블에서 실행
                            if let Statement::Select { table, columns, condition: sub_cond,
                                join: _, order_by: _, group_by: _, having: _, limit: _ } = *sub_stmt.clone() {
                                let rows = match self.tables.get(&table) {
                                    Some(r) => r.clone(),
                                    None => return false,
                                };
                                let sub_vals: Vec<String> = rows.into_iter()
                                    .filter(|r| Self::matches_condition(r, &sub_cond))
                                    .filter_map(|r| {
                                        match columns.first() {
                                            Some(SelectColumn::Column(col)) => r.get(col).cloned(),
                                            Some(SelectColumn::All) => r.values().next().cloned(),
                                            _ => None,
                                        }
                                    })
                                    .collect();
                                match cond.operator {
                                    Operator::In => sub_vals.contains(&val),
                                    Operator::Eq => sub_vals.first().map(|v| v == &val).unwrap_or(false),
                                    _ => false,
                                }
                            } else {
                                false
                            }
                        } else {
                            false
                        }
                    }
                }
            }
        }
    }
}