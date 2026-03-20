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
    pub index_meta: HashMap<String, (String, String)>,
    pub views: HashMap<String, Statement>,
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

        Executor {
            catalog,
            tables,
            indexes,
            index_meta: HashMap::new(),
            views: HashMap::new(),
            txn: TransactionManager::new(),
            disk,
        }
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
            if *not_null && final_values[i].is_empty() {
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
            row.insert(col.clone(), val.clone());
        }

        // FOREIGN KEY 검사
        for col in &schema.columns {
            if let Some(fk) = &col.foreign_key {
                let val = row.get(&col.name).cloned().unwrap_or_default();
                if val.is_empty() { continue; }
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
            self.disk.save_table(&table, self.tables.get(&table).unwrap());
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
        // 다른 테이블에서 이 테이블을 참조하는지 확인
        let rows_to_delete: Vec<Row> = self.tables.get(&table)
            .ok_or(format!("Table '{}' not found", table))?
            .iter()
            .filter(|r| Self::matches_condition(r, &condition))
            .cloned()
            .collect();

        // FK 참조 검사
        for del_row in &rows_to_delete {
            for (other_table, other_schema) in &self.catalog.tables.clone() {
                for col in &other_schema.columns {
                    if let Some(fk) = &col.foreign_key {
                        if fk.ref_table == table {
                            let del_val = del_row.get(&fk.ref_column).cloned().unwrap_or_default();
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
                    }
                }
            }
        }

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