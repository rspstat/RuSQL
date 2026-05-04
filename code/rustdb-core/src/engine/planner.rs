// src/engine/planner.rs
//
// Cost-based query planner.
// Chooses the best access path for each table scan and join algorithm for each join.

use std::collections::HashMap;
use crate::parser::ast::*;
use crate::engine::executor::Row;
use crate::catalog::schema::Catalog;
use crate::storage::composite_index::CompositeIndex;

// ── Range operator ────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum RangeOp { Gt, Gte, Lt, Lte }

impl RangeOp {
    pub fn inclusive(&self) -> bool { matches!(self, RangeOp::Gte | RangeOp::Lte) }
    pub fn is_lower_bound(&self) -> bool { matches!(self, RangeOp::Gt | RangeOp::Gte) }
    pub fn label(&self) -> &str {
        match self { RangeOp::Gt => ">", RangeOp::Gte => ">=", RangeOp::Lt => "<", RangeOp::Lte => "<=" }
    }
}

// ── Access path ───────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum AccessPath {
    SeqScan,
    PkPoint   { key: String },
    PkBetween { start: String, end: String },
    PkRange   { op: RangeOp, key: String },
    SecondaryPoint { index_key: String, col: String, key: String },
    SecondaryRange { index_key: String, col: String, op: RangeOp, key: String },
    CompositeIndex { index_name: String },
}

// ── Join algorithm ────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum JoinAlgo {
    NestedLoop,
    Hash      { probe_col: String, build_col: String },
    SortMerge { probe_col: String, build_col: String },
}

// ── Plan nodes ────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct TablePlan {
    pub table:       String,
    pub access:      AccessPath,
    pub filter:      Option<CondExpr>,
    pub est_rows:    usize,
    pub est_cost:    f64,
    pub is_covering: bool,
}

#[derive(Debug, Clone)]
pub struct JoinPlan {
    pub right_table: String,
    pub on_expr:     CondExpr,
    pub join_type:   JoinType,
    pub algo:        JoinAlgo,
    pub est_rows:    usize,
    pub est_cost:    f64,
}

#[derive(Debug, Clone)]
pub struct SelectPlan {
    pub base:  TablePlan,
    pub joins: Vec<JoinPlan>,
}

impl SelectPlan {
    pub fn total_cost(&self) -> f64 {
        self.base.est_cost + self.joins.iter().map(|j| j.est_cost).sum::<f64>()
    }
}

// ── Planner ───────────────────────────────────────────────────────────────

pub struct Planner<'a> {
    tables:            &'a HashMap<String, Vec<Row>>,
    indexes:           &'a HashMap<String, crate::storage::btree::BPlusTree>,
    index_meta:        &'a HashMap<String, (String, String)>,
    composite_indexes: &'a HashMap<String, CompositeIndex>,
    catalog:           &'a Catalog,
}

impl<'a> Planner<'a> {
    pub fn new(
        tables:            &'a HashMap<String, Vec<Row>>,
        indexes:           &'a HashMap<String, crate::storage::btree::BPlusTree>,
        index_meta:        &'a HashMap<String, (String, String)>,
        composite_indexes: &'a HashMap<String, CompositeIndex>,
        catalog:           &'a Catalog,
    ) -> Self {
        Self { tables, indexes, index_meta, composite_indexes, catalog }
    }

    pub fn plan(&self, table: &str, condition: &Option<CondExpr>, joins: &[Join]) -> SelectPlan {
        let base = self.plan_table(table, condition);
        let join_plans = joins.iter().map(|j| self.plan_join(&base, j)).collect();
        SelectPlan { base, joins: join_plans }
    }

    pub fn plan_covering(
        &self,
        table: &str,
        condition: &Option<CondExpr>,
        joins: &[Join],
        columns: &[SelectColumn],
    ) -> SelectPlan {
        let mut plan = self.plan(table, condition, joins);
        plan.base.is_covering = Self::is_covering_access(&plan.base.access, columns);
        plan
    }

    fn is_covering_access(access: &AccessPath, columns: &[SelectColumn]) -> bool {
        let index_col = match access {
            AccessPath::SecondaryPoint { col, .. } => col.as_str(),
            AccessPath::SecondaryRange { col, .. } => col.as_str(),
            _ => return false,
        };
        !columns.is_empty() && columns.iter().all(|c| match c {
            SelectColumn::Column(s) => s == index_col,
            SelectColumn::ColumnAlias(s, _) => s == index_col,
            _ => false,
        })
    }

    // ── Table scan ────────────────────────────────────────────────────────

    fn plan_table(&self, table: &str, condition: &Option<CondExpr>) -> TablePlan {
        let total    = self.table_size(table);
        let pk       = self.pk_col(table);
        let access   = self.choose_access(table, condition, pk.as_deref());
        let est_rows = self.estimate_rows(total, &access);
        let est_cost = self.estimate_cost(total, &access);
        TablePlan { table: table.to_string(), access, filter: condition.clone(), est_rows, est_cost, is_covering: false }
    }

    pub fn choose_access(&self, table: &str, condition: &Option<CondExpr>, pk: Option<&str>) -> AccessPath {
        let expr = match condition { Some(e) => e, None => return AccessPath::SeqScan };

        if let CondExpr::Leaf(cond) = expr {
            if let ArithExpr::Col(col_full) = &cond.left {
                let col = col_full.split('.').last().unwrap_or(col_full);
                if pk == Some(col) {
                    if let Some(path) = self.pk_access(cond) { return path; }
                }
                if let Some(idx_key) = self.find_secondary_index(table, col) {
                    if let Some(path) = self.secondary_access(idx_key, col, cond) { return path; }
                }
            }
        }

        let eq_map = collect_eq_map(expr);
        if !eq_map.is_empty() {
            if let Some((name, _)) = self.composite_indexes.iter()
                .find(|(_, ci)| ci.table == table && ci.matches_conditions(&eq_map))
            {
                return AccessPath::CompositeIndex { index_name: name.clone() };
            }
        }

        AccessPath::SeqScan
    }

    fn pk_access(&self, cond: &Condition) -> Option<AccessPath> {
        Some(match (&cond.operator, &cond.value) {
            (Operator::Eq,      ConditionValue::Literal(k))    => AccessPath::PkPoint { key: k.clone() },
            (Operator::Between, ConditionValue::Between(a, b)) => AccessPath::PkBetween { start: a.clone(), end: b.clone() },
            (Operator::Gt,      ConditionValue::Literal(k))    => AccessPath::PkRange { op: RangeOp::Gt,  key: k.clone() },
            (Operator::Gte,     ConditionValue::Literal(k))    => AccessPath::PkRange { op: RangeOp::Gte, key: k.clone() },
            (Operator::Lt,      ConditionValue::Literal(k))    => AccessPath::PkRange { op: RangeOp::Lt,  key: k.clone() },
            (Operator::Lte,     ConditionValue::Literal(k))    => AccessPath::PkRange { op: RangeOp::Lte, key: k.clone() },
            _ => return None,
        })
    }

    fn secondary_access(&self, index_key: String, col: &str, cond: &Condition) -> Option<AccessPath> {
        Some(match (&cond.operator, &cond.value) {
            (Operator::Eq,  ConditionValue::Literal(k)) =>
                AccessPath::SecondaryPoint { index_key, col: col.to_string(), key: k.clone() },
            (Operator::Gt,  ConditionValue::Literal(k)) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Gt,  key: k.clone() },
            (Operator::Gte, ConditionValue::Literal(k)) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Gte, key: k.clone() },
            (Operator::Lt,  ConditionValue::Literal(k)) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Lt,  key: k.clone() },
            (Operator::Lte, ConditionValue::Literal(k)) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Lte, key: k.clone() },
            _ => return None,
        })
    }

    fn find_secondary_index(&self, table: &str, col: &str) -> Option<String> {
        self.index_meta.iter()
            .find(|(_, (t, c))| t == table && c == col)
            .map(|(name, _)| format!("{}_{}", table, name))
    }

    // ── Join planning ─────────────────────────────────────────────────────

    fn plan_join(&self, base: &TablePlan, join: &Join) -> JoinPlan {
        let right_size = self.table_size(&join.table);
        let algo = self.choose_join_algo(base.est_rows, right_size, &join.on_expr, &base.table, &join.table);
        let est_cost = match &algo {
            JoinAlgo::NestedLoop      => (base.est_rows * right_size.max(1)) as f64,
            JoinAlgo::Hash { .. }     => (base.est_rows + right_size) as f64 * 1.5,
            // Sort-Merge: O((N+M)log(N+M)) sort + O(N+M) merge
            JoinAlgo::SortMerge { .. } => {
                let n = (base.est_rows + right_size) as f64;
                n * n.log2().max(1.0) + n
            }
        };
        let est_rows = (base.est_rows + right_size) / 2;
        JoinPlan {
            right_table: join.table.clone(),
            on_expr:     join.on_expr.clone(),
            join_type:   join.join_type.clone(),
            algo, est_rows, est_cost,
        }
    }

    fn choose_join_algo(&self, left_size: usize, right_size: usize, on_expr: &CondExpr, _left_table: &str, right_table: &str) -> JoinAlgo {
        if left_size > 4 || right_size > 4 {
            if let CondExpr::Leaf(cond) = on_expr {
                if cond.operator == Operator::Eq {
                    if let (ArithExpr::Col(lc), ConditionValue::Literal(rv)) = (&cond.left, &cond.value) {
                        let lhs_col = lc.split('.').last().unwrap_or(lc).to_string();
                        let rhs_col = rv.split('.').last().unwrap_or(rv).to_string();
                        let lhs_tbl = lc.split('.').next().unwrap_or("").to_lowercase();
                        let rhs_tbl = rv.split('.').next().unwrap_or("").to_lowercase();
                        let right_bare = right_table.split('.').last().unwrap_or(right_table).to_lowercase();
                        let (probe_col, build_col) = if rhs_tbl == right_bare {
                            (lhs_col, rhs_col)
                        } else if lhs_tbl == right_bare {
                            (rhs_col, lhs_col)
                        } else {
                            return JoinAlgo::NestedLoop;
                        };
                        // 양쪽 모두 대형 테이블이면 Sort-Merge Join, 한쪽만 크면 Hash Join.
                        if left_size > 4 && right_size > 4 {
                            return JoinAlgo::SortMerge { probe_col, build_col };
                        }
                        return JoinAlgo::Hash { probe_col, build_col };
                    }
                }
            }
        }
        JoinAlgo::NestedLoop
    }

    // ── Cost / row estimation ─────────────────────────────────────────────

    pub fn table_size(&self, table: &str) -> usize {
        self.tables.get(table).map(|r| r.len()).unwrap_or(0)
    }

    pub fn pk_col(&self, table: &str) -> Option<String> {
        self.catalog.get_table(table)
            .and_then(|s| s.columns.iter().find(|c| c.primary_key).map(|c| c.name.clone()))
    }

    pub fn estimate_rows(&self, total: usize, access: &AccessPath) -> usize {
        match access {
            AccessPath::SeqScan                                   => total,
            AccessPath::PkPoint { .. } | AccessPath::CompositeIndex { .. } => 1,
            AccessPath::PkBetween { .. } | AccessPath::PkRange { .. }
            | AccessPath::SecondaryRange { .. }                   => (total / 4).max(1),
            AccessPath::SecondaryPoint { .. }                     => (total / 10).max(1),
        }
    }

    pub fn estimate_cost(&self, total: usize, access: &AccessPath) -> f64 {
        let n = (total as f64).max(1.0);
        let log_n = n.log2().max(1.0);
        match access {
            AccessPath::SeqScan               => n,
            AccessPath::PkPoint { .. }        => log_n,
            AccessPath::PkBetween { .. }
            | AccessPath::PkRange { .. }      => log_n + n / 4.0,
            AccessPath::SecondaryPoint { .. } => log_n * 2.0,
            AccessPath::SecondaryRange { .. } => log_n * 2.0 + n / 4.0,
            AccessPath::CompositeIndex { .. } => log_n,
        }
    }

    // ── EXPLAIN output ────────────────────────────────────────────────────

    pub fn explain(&self, plan: &SelectPlan) -> String {
        let total = self.table_size(&plan.base.table);
        let visible = self.tables.get(&plan.base.table)
            .map(|rows| rows.iter()
                .filter(|r| r.get("_xmax").map(|v| v == "0").unwrap_or(true))
                .count())
            .unwrap_or(0);
        let pk = self.pk_col(&plan.base.table).unwrap_or_default();

        let mut out = String::new();
        out.push_str("+--------------------------------------------------+\n");
        out.push_str("|                  QUERY PLAN                      |\n");
        out.push_str("+--------------------------------------------------+\n");
        out.push_str(&fmt_row("Table",        &plan.base.table));
        out.push_str(&fmt_row("Rows (total)", &total.to_string()));
        out.push_str(&fmt_row("Rows (visible)", &visible.to_string()));
        if !pk.is_empty() { out.push_str(&fmt_row("PK", &pk)); }
        out.push_str(&fmt_row("Est. cost",    &format!("{:.1}", plan.total_cost())));
        out.push_str("|                                                  |\n");
        let access_label = if plan.base.is_covering {
            format!("{} (Covering)", self.describe_access(&plan.base.access))
        } else {
            self.describe_access(&plan.base.access)
        };
        out.push_str(&fmt_row("Access", &access_label));
        for jp in &plan.joins {
            out.push_str(&fmt_row("Join", &self.describe_join(jp)));
        }
        out.push_str("+--------------------------------------------------+");
        out
    }

    fn describe_access(&self, access: &AccessPath) -> String {
        match access {
            AccessPath::SeqScan                                   => "Seq Scan".to_string(),
            AccessPath::PkPoint { key }                           => format!("Index Scan  PK = {}", key),
            AccessPath::PkBetween { start, end }                  => format!("Index Range  PK BETWEEN {} AND {}", start, end),
            AccessPath::PkRange { op, key }                       => format!("Index Range  PK {} {}", op.label(), key),
            AccessPath::SecondaryPoint { index_key, col, key }    => format!("Index Scan  {} ({} = {})", index_key, col, key),
            AccessPath::SecondaryRange { index_key, col, op, key }=> format!("Index Range  {} ({} {} {})", index_key, col, op.label(), key),
            AccessPath::CompositeIndex { index_name }             => format!("Composite Index  {}", index_name),
        }
    }

    fn describe_join(&self, jp: &JoinPlan) -> String {
        let algo = match &jp.algo {
            JoinAlgo::NestedLoop => "Nested Loop".to_string(),
            JoinAlgo::Hash { probe_col, build_col } =>
                format!("Hash Join       probe={} build={}", probe_col, build_col),
            JoinAlgo::SortMerge { probe_col, build_col } =>
                format!("Sort-Merge Join probe={} build={}", probe_col, build_col),
        };
        format!("{} → {}  cost≈{:.0}", algo, jp.right_table, jp.est_cost)
    }
}

// ── Formatting helper ─────────────────────────────────────────────────────

fn fmt_row(label: &str, value: &str) -> String {
    let cell = format!("{}: {}", label, value);
    let mut out = String::new();
    let mut first = true;
    for chunk in cell.as_bytes().chunks(48) {
        let s = std::str::from_utf8(chunk).unwrap_or("?");
        if first { out.push_str(&format!("| {:<48} |\n", s)); first = false; }
        else      { out.push_str(&format!("|   {:<46} |\n", s)); }
    }
    out
}

// ── Public helpers (also used by executor) ────────────────────────────────

pub fn extract_equi_join_cols(on_expr: &CondExpr) -> Option<(String, String)> {
    if let CondExpr::Leaf(cond) = on_expr {
        if cond.operator == Operator::Eq {
            if let (ArithExpr::Col(lc), ConditionValue::Literal(rv)) = (&cond.left, &cond.value) {
                let probe = lc.split('.').last().unwrap_or(lc).to_string();
                let build = rv.split('.').last().unwrap_or(rv).to_string();
                return Some((probe, build));
            }
        }
    }
    None
}

pub fn collect_eq_map(expr: &CondExpr) -> HashMap<String, String> {
    let mut map = HashMap::new();
    collect_eq_recursive(expr, &mut map);
    map
}

fn collect_eq_recursive(expr: &CondExpr, map: &mut HashMap<String, String>) {
    match expr {
        CondExpr::And(l, r) => { collect_eq_recursive(l, map); collect_eq_recursive(r, map); }
        CondExpr::Leaf(c) if c.operator == Operator::Eq => {
            if let (ArithExpr::Col(name), ConditionValue::Literal(lit)) = (&c.left, &c.value) {
                let bare = name.split('.').last().unwrap_or(name).to_string();
                map.insert(bare, lit.clone());
            }
        }
        _ => {}
    }
}
