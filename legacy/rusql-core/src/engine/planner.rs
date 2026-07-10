// src/engine/planner.rs
//
// Cost-based query planner.
// Chooses the best access path for each table scan and join algorithm for each join.

use std::collections::HashMap;
use crate::parser::ast::*;
use crate::engine::executor::{Row, TableStats};
use crate::catalog::schema::Catalog;
use crate::storage::composite_index::CompositeIndex;
use crate::storage::hash_index::HashIndex;

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
    SecondaryBetween { index_key: String, col: String, start: String, end: String },
    CompositeIndex { index_name: String },
    /// 복합 인덱스 프리픽스 스캔: 앞 K 컬럼이 등호, 나머지는 필터로 처리
    CompositeIndexPrefix { index_name: String, prefix: String },
    HashPoint { index_key: String, col: String, key: String },
    /// 보조 인덱스 LIKE 프리픽스 스캔: 'prefix%' 패턴
    SecondaryLikePrefix { index_key: String, col: String, prefix: String },
    /// AND 조건 다중 인덱스 교차: 각 단순 경로의 PK 집합을 교집합하여 최종 결과 행 수 감소.
    /// 예) WHERE a=1 AND b=2 → [SecondaryPoint(a), SecondaryPoint(b)] 교집합
    IndexIntersection { paths: Vec<AccessPath> },
}

// ── Join algorithm ────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum JoinAlgo {
    NestedLoop,
    Hash      { probe_col: String, build_col: String },
    SortMerge { probe_col: String, build_col: String },
    /// Index Nested Loop: for each left row, probe right table's PK B+Tree in O(log n).
    /// Avoids loading all right rows into memory. Ideal for FK→PK joins.
    IndexNL   { probe_col: String, right_pk_col: String },
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
    hash_indexes:      &'a HashMap<String, HashIndex>,
    hash_index_meta:   &'a HashMap<String, (String, String)>,
    catalog:           &'a Catalog,
    table_stats:       &'a HashMap<String, TableStats>,
}

impl<'a> Planner<'a> {
    pub fn new(
        tables:            &'a HashMap<String, Vec<Row>>,
        indexes:           &'a HashMap<String, crate::storage::btree::BPlusTree>,
        index_meta:        &'a HashMap<String, (String, String)>,
        composite_indexes: &'a HashMap<String, CompositeIndex>,
        hash_indexes:      &'a HashMap<String, HashIndex>,
        hash_index_meta:   &'a HashMap<String, (String, String)>,
        catalog:           &'a Catalog,
        table_stats:       &'a HashMap<String, TableStats>,
    ) -> Self {
        Self { tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, table_stats }
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
        plan.base.is_covering = self.is_covering_access(&plan.base.access, columns, table);
        plan
    }

    fn is_covering_access(&self, access: &AccessPath, columns: &[SelectColumn], table: &str) -> bool {
        let index_col = match access {
            AccessPath::SecondaryPoint { col, .. } => col.as_str(),
            AccessPath::SecondaryRange { col, .. } => col.as_str(),
            AccessPath::SecondaryBetween { col, .. } => col.as_str(),
            AccessPath::SecondaryLikePrefix { col, .. } => col.as_str(),
            _ => return false,
        };
        // Single-column covering: all selected columns are the indexed column
        let simple = !columns.is_empty() && columns.iter().all(|c| match c {
            SelectColumn::Column(s) => s == index_col,
            SelectColumn::ColumnAlias(s, _) => s == index_col,
            _ => false,
        });
        if simple { return true; }
        // Composite index covering: any composite index covers all selected columns
        let selected: Vec<&str> = columns.iter().filter_map(|c| match c {
            SelectColumn::Column(s) => Some(s.as_str()),
            SelectColumn::ColumnAlias(s, _) => Some(s.as_str()),
            _ => None,
        }).collect();
        if selected.is_empty() { return false; }
        self.composite_indexes.values().any(|ci| {
            ci.table == table && selected.iter().all(|sc| ci.columns.iter().any(|ic| ic == sc))
        })
    }

    // ── Table scan ────────────────────────────────────────────────────────

    fn plan_table(&self, table: &str, condition: &Option<CondExpr>) -> TablePlan {
        let total    = self.table_size(table);
        let pk       = self.pk_col(table);
        let access   = self.choose_access(table, condition, pk.as_deref());
        let est_rows = self.estimate_rows(total, &access, table);
        let est_cost = self.estimate_cost(total, &access);
        TablePlan { table: table.to_string(), access, filter: condition.clone(), est_rows, est_cost, is_covering: false }
    }

    pub fn choose_access(&self, table: &str, condition: &Option<CondExpr>, pk: Option<&str>) -> AccessPath {
        let expr = match condition { Some(e) => e, None => return AccessPath::SeqScan };

        if let CondExpr::Leaf(cond) = expr {
            if let ArithExpr::Col(col_full) = &cond.left {
                let col = col_full.split('.').last().unwrap_or(col_full);
                if pk == Some(col) {
                    if let Some(path) = self.pk_access(cond, table) { return path; }
                }
                // Hash Index: 등호 조건에서 B+Tree보다 우선
                if cond.operator == Operator::Eq {
                    if let ConditionValue::Literal(k) = &cond.value {
                        if !self.is_col_ref_in_context(k, table) {
                            if let Some(idx_key) = self.find_hash_index(table, col) {
                                return AccessPath::HashPoint { index_key: idx_key, col: col.to_string(), key: k.clone() };
                            }
                        }
                    }
                }
                if let Some(idx_key) = self.find_secondary_index(table, col) {
                    if let Some(path) = self.secondary_access(idx_key, col, cond, table) { return path; }
                }
            }
        }

        let eq_map = collect_eq_map(expr);
        if !eq_map.is_empty() {
            // 모든 컬럼 등호: 정확한 포인트 조회
            if let Some((name, _)) = self.composite_indexes.iter()
                .find(|(_, ci)| ci.table == table && ci.matches_conditions(&eq_map))
            {
                return AccessPath::CompositeIndex { index_name: name.clone() };
            }
            // 앞 K 컬럼만 등호, 나머지는 범위/필터: 프리픽스 스캔
            for (name, ci) in self.composite_indexes.iter() {
                if ci.table != table { continue; }
                if let Some(prefix) = ci.prefix_key_from_eq_map(&eq_map) {
                    return AccessPath::CompositeIndexPrefix { index_name: name.clone(), prefix };
                }
            }
        }

        // AND 조건에서 각 컬럼이 별도 인덱스를 가질 때 IndexIntersection 사용.
        // 단일 인덱스보다 선택도가 높을 수 있으므로 최소 2개 인덱스가 있는 경우에만 활성화.
        if let Some(intersection) = self.try_index_intersection(table, expr, pk) {
            return intersection;
        }

        AccessPath::SeqScan
    }

    /// AND 조건 트리에서 인덱스 접근 가능한 리프 조건들을 수집해 IndexIntersection 반환.
    /// 2개 미만이면 None (단일 인덱스 경로 또는 SeqScan이 더 나음).
    fn try_index_intersection(&self, table: &str, expr: &CondExpr, pk: Option<&str>) -> Option<AccessPath> {
        let leaves = collect_and_leaves(expr);
        if leaves.len() < 2 { return None; }

        let mut sub_paths: Vec<AccessPath> = Vec::new();
        for cond in &leaves {
            if let ArithExpr::Col(col_full) = &cond.left {
                let col = col_full.split('.').last().unwrap_or(col_full);
                // PK 조건은 이미 PkPoint로 처리됐으므로 여기서는 보조 인덱스만
                if pk == Some(col) { continue; }
                if cond.operator == Operator::Eq {
                    if let ConditionValue::Literal(k) = &cond.value {
                        if !self.is_col_ref_in_context(k, table) {
                            // Hash index 우선
                            if let Some(idx_key) = self.find_hash_index(table, col) {
                                sub_paths.push(AccessPath::HashPoint {
                                    index_key: idx_key, col: col.to_string(), key: k.clone()
                                });
                                continue;
                            }
                            // B+Tree secondary
                            if let Some(idx_key) = self.find_secondary_index(table, col) {
                                sub_paths.push(AccessPath::SecondaryPoint {
                                    index_key: idx_key, col: col.to_string(), key: k.clone()
                                });
                            }
                        }
                    }
                }
            }
        }
        if sub_paths.len() >= 2 {
            Some(AccessPath::IndexIntersection { paths: sub_paths })
        } else {
            None
        }
    }

    fn pk_access(&self, cond: &Condition, table: &str) -> Option<AccessPath> {
        Some(match (&cond.operator, &cond.value) {
            (Operator::Eq,      ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, table) => AccessPath::PkPoint { key: k.clone() },
            (Operator::Between, ConditionValue::Between(a, b)) => AccessPath::PkBetween { start: a.clone(), end: b.clone() },
            (Operator::Gt,      ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, table) => AccessPath::PkRange { op: RangeOp::Gt,  key: k.clone() },
            (Operator::Gte,     ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, table) => AccessPath::PkRange { op: RangeOp::Gte, key: k.clone() },
            (Operator::Lt,      ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, table) => AccessPath::PkRange { op: RangeOp::Lt,  key: k.clone() },
            (Operator::Lte,     ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, table) => AccessPath::PkRange { op: RangeOp::Lte, key: k.clone() },
            _ => return None,
        })
    }

    fn secondary_access(&self, index_key: String, col: &str, cond: &Condition, _table: &str) -> Option<AccessPath> {
        Some(match (&cond.operator, &cond.value) {
            (Operator::Eq,  ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, _table) =>
                AccessPath::SecondaryPoint { index_key, col: col.to_string(), key: k.clone() },
            (Operator::Between, ConditionValue::Between(a, b)) =>
                AccessPath::SecondaryBetween { index_key, col: col.to_string(), start: a.clone(), end: b.clone() },
            (Operator::Gt,  ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, _table) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Gt,  key: k.clone() },
            (Operator::Gte, ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, _table) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Gte, key: k.clone() },
            (Operator::Lt,  ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, _table) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Lt,  key: k.clone() },
            (Operator::Lte, ConditionValue::Literal(k)) if !self.is_col_ref_in_context(k, _table) =>
                AccessPath::SecondaryRange { index_key, col: col.to_string(), op: RangeOp::Lte, key: k.clone() },
            // LIKE 'prefix%': 뒤에 % 하나만 있고 앞에 와일드카드 없는 패턴 → 프리픽스 스캔
            (Operator::Like, ConditionValue::Literal(pat)) => {
                if let Some(prefix) = like_prefix(pat) {
                    return Some(AccessPath::SecondaryLikePrefix { index_key, col: col.to_string(), prefix });
                }
                return None;
            }
            _ => return None,
        })
    }

    /// Returns true if `k` is a column reference (not a literal value).
    /// Dotted references (e.g. `t2.col`) are always column refs.
    /// Bare alphabetic identifiers are checked against the table's catalog columns.
    fn is_col_ref_in_context(&self, k: &str, table: &str) -> bool {
        if k.contains('.') {
            // Only treat as a column ref if both parts around the first dot are valid identifiers.
            // This prevents email addresses like "alice.j@co.com" from being treated as col refs.
            let parts: Vec<&str> = k.splitn(2, '.').collect();
            let is_ident = |s: &str| {
                !s.is_empty()
                && s.chars().next().map(|c| c.is_alphabetic() || c == '_').unwrap_or(false)
                && s.chars().all(|c| c.is_alphanumeric() || c == '_')
            };
            return parts.len() == 2 && is_ident(parts[0]) && is_ident(parts[1]);
        }
        if k.chars().next().map(|c| c.is_alphabetic() || c == '_').unwrap_or(false)
            && k.parse::<f64>().is_err()
        {
            return self.catalog.get_table(table)
                .map(|s| s.columns.iter().any(|c| c.name.eq_ignore_ascii_case(k)))
                .unwrap_or(false);
        }
        false
    }

    fn find_secondary_index(&self, table: &str, col: &str) -> Option<String> {
        self.index_meta.iter()
            .find(|(_, (t, c))| t == table && c == col)
            .map(|(name, _)| format!("{}_{}", table, name))
    }

    fn find_hash_index(&self, table: &str, col: &str) -> Option<String> {
        self.hash_index_meta.iter()
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
            // IndexNL: O(N * log M) — N left rows × log M right index probe
            JoinAlgo::IndexNL { .. } => {
                let log_m = (right_size as f64).log2().max(1.0);
                base.est_rows as f64 * log_m
            }
        };
        let est_rows = self.estimate_join_output(base, right_size, &join.on_expr, &join.table);
        JoinPlan {
            right_table: join.table.clone(),
            on_expr:     join.on_expr.clone(),
            join_type:   join.join_type.clone(),
            algo, est_rows, est_cost,
        }
    }

    /// Estimate join output rows using column NDV from ANALYZE stats when available.
    /// Equi-join with stats:    left * right / max(NDV_left, NDV_right)
    /// Equi-join without stats: min(left, right)  (conservative, beats the old average)
    /// Non-equi / complex:      min(left, right)
    fn estimate_join_output(&self, base: &TablePlan, right_size: usize, on_expr: &CondExpr, right_table: &str) -> usize {
        let left_rows = base.est_rows;
        if left_rows == 0 || right_size == 0 {
            return 0;
        }
        if let CondExpr::Leaf(cond) = on_expr {
            if cond.operator == Operator::Eq {
                if let (ArithExpr::Col(lc), ConditionValue::Literal(rv)) = (&cond.left, &cond.value) {
                    let lhs_col = lc.split('.').last().unwrap_or(lc);
                    let rhs_col = rv.split('.').last().unwrap_or(rv);
                    let right_bare = right_table.split('.').last().unwrap_or(right_table).to_lowercase();
                    let rv_tbl = rv.split('.').next().unwrap_or("").to_lowercase();
                    let (left_col, right_col) = if rv_tbl == right_bare {
                        (lhs_col, rhs_col)
                    } else {
                        (rhs_col, lhs_col)
                    };
                    let l_ndv = self.table_stats.get(&base.table)
                        .and_then(|ts| ts.columns.get(left_col))
                        .map(|cs| cs.distinct_count)
                        .filter(|&n| n > 0);
                    let r_ndv = self.table_stats.get(right_table)
                        .and_then(|ts| ts.columns.get(right_col))
                        .map(|cs| cs.distinct_count)
                        .filter(|&n| n > 0);
                    let ndv = match (l_ndv, r_ndv) {
                        (Some(l), Some(r)) => l.max(r),
                        (Some(n), None) | (None, Some(n)) => n,
                        (None, None) => return left_rows.min(right_size).max(1),
                    };
                    return ((left_rows as f64 * right_size as f64 / ndv as f64).ceil() as usize).max(1);
                }
            }
        }
        left_rows.min(right_size).max(1)
    }

    fn choose_join_algo(&self, left_size: usize, right_size: usize, on_expr: &CondExpr, _left_table: &str, right_table: &str) -> JoinAlgo {
        // 비용 모델:
        //   NestedLoop  cost = left * right
        //   Hash        cost ≈ (left + right) * HASH_FACTOR
        //   SortMerge   cost ≈ (left + right) * log2(left + right)
        //   IndexNL     cost ≈ left * log2(right)
        //
        // NestedLoop이 Hash보다 유리한 조건: left * right < (left + right) * HASH_FACTOR
        // → 두 테이블 곱이 작을 때 NestedLoop 선택. HASH_FACTOR=3 → 임계 약 10 이하.
        // 하드코딩 `> 4` 대신 실제 비용 비교로 교체.
        const HASH_FACTOR: usize = 3;
        let nl_cost = left_size.saturating_mul(right_size);
        let hash_cost = left_size.saturating_add(right_size).saturating_mul(HASH_FACTOR);

        if nl_cost <= hash_cost {
            return JoinAlgo::NestedLoop;
        }

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

                    // IndexNL: build_col이 right 테이블의 PK → O(left × log right)
                    // Hash보다 유리: left * log(right) < (left + right) * HASH_FACTOR
                    if let Some(pk) = self.pk_col(right_table) {
                        if pk == build_col {
                            let log_right = (right_size as f64).log2().max(1.0) as usize;
                            let inl_cost = left_size.saturating_mul(log_right);
                            if inl_cost <= hash_cost {
                                return JoinAlgo::IndexNL { probe_col, right_pk_col: build_col };
                            }
                        }
                    }

                    // Sort-Merge vs Hash: 양쪽이 모두 대형이면 Sort-Merge (이미 정렬된 경우 우위)
                    let n = left_size + right_size;
                    let log_n = (n as f64).log2().max(1.0) as usize;
                    let sm_cost = n.saturating_mul(log_n);
                    if sm_cost <= hash_cost {
                        return JoinAlgo::SortMerge { probe_col, build_col };
                    }
                    return JoinAlgo::Hash { probe_col, build_col };
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

    pub fn estimate_rows(&self, total: usize, access: &AccessPath, table: &str) -> usize {
        match access {
            AccessPath::SeqScan                                   => total,
            AccessPath::PkPoint { .. } | AccessPath::CompositeIndex { .. }
            | AccessPath::HashPoint { .. }                        => 1,
            AccessPath::PkRange { op, key } => {
                let sel = self.pk_col(table)
                    .map(|pk| self.histogram_sel_range(table, &pk, op, key))
                    .unwrap_or(0.25);
                ((total as f64 * sel) as usize).max(1)
            }
            AccessPath::PkBetween { start, end } => {
                let sel = self.pk_col(table)
                    .map(|pk| self.histogram_sel_between(table, &pk, start, end))
                    .unwrap_or(0.25);
                ((total as f64 * sel) as usize).max(1)
            }
            AccessPath::SecondaryRange { col, op, key, .. } => {
                let sel = self.histogram_sel_range(table, col, op, key);
                ((total as f64 * sel) as usize).max(1)
            }
            AccessPath::SecondaryBetween { col, start, end, .. } => {
                let sel = self.histogram_sel_between(table, col, start, end);
                ((total as f64 * sel) as usize).max(1)
            }
            AccessPath::SecondaryLikePrefix { .. } => (total / 10).max(1),
            AccessPath::CompositeIndexPrefix { .. } => (total / 5).max(1),
            AccessPath::IndexIntersection { paths } => {
                // 교집합 행 수 ≈ min(각 경로 행 수) — 실제로는 더 적을 수 있음
                paths.iter()
                    .map(|p| self.estimate_rows(total, p, table))
                    .min()
                    .unwrap_or(1)
            }
            AccessPath::SecondaryPoint { index_key, col, .. } => {
                // Use real cardinality from ANALYZE TABLE stats when available.
                // index_key is "{table}_{index_name}"; table is the prefix before "_idx_".
                let tbl = index_key.split("_idx_").next()
                    .unwrap_or(index_key.split('_').next().unwrap_or(""));
                if let Some(ts) = self.table_stats.get(tbl) {
                    if let Some(cs) = ts.columns.get(col) {
                        if cs.distinct_count > 0 {
                            return (total / cs.distinct_count).max(1);
                        }
                    }
                }
                (total / 10).max(1)
            }
        }
    }

    /// Selectivity estimate for a range condition using the equi-depth histogram.
    /// Returns a fraction in [0.01, 1.0]. Falls back to 0.25 when no histogram exists.
    fn histogram_sel_range(&self, table: &str, col: &str, op: &RangeOp, key: &str) -> f64 {
        let hist = match self.table_stats.get(table).and_then(|ts| ts.columns.get(col)) {
            Some(cs) if !cs.histogram.is_empty() => &cs.histogram,
            _ => return 0.25,
        };
        let n = hist.len() as f64;
        let key_f = key.parse::<f64>();
        // Count buckets whose upper bound satisfies the comparison direction.
        let gt_count = hist.iter().filter(|bound| match (&key_f, bound.parse::<f64>()) {
            (Ok(kv), Ok(bv)) => bv > *kv,
            _ => bound.as_str() > key,
        }).count() as f64;
        let lte_count = n - gt_count;
        match op {
            RangeOp::Gt | RangeOp::Gte => (gt_count / n).max(0.01),
            RangeOp::Lt | RangeOp::Lte => (lte_count / n).max(0.01),
        }
    }

    /// Selectivity for BETWEEN [lo, hi] = sel(<= hi) - sel(< lo).
    fn histogram_sel_between(&self, table: &str, col: &str, lo: &str, hi: &str) -> f64 {
        let sel_lte_hi = self.histogram_sel_range(table, col, &RangeOp::Lte, hi);
        let sel_lt_lo  = self.histogram_sel_range(table, col, &RangeOp::Lt,  lo);
        (sel_lte_hi - sel_lt_lo).max(0.01)
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
            AccessPath::SecondaryRange { .. } | AccessPath::SecondaryBetween { .. }
            | AccessPath::SecondaryLikePrefix { .. } | AccessPath::CompositeIndexPrefix { .. } => log_n * 2.0 + n / 4.0,
            AccessPath::CompositeIndex { .. } => log_n,
            AccessPath::HashPoint { .. }      => 1.0, // O(1)
            AccessPath::IndexIntersection { paths } => {
                // 각 경로 비용 합산 + 교집합 연산 (PK set intersection)
                paths.iter().map(|p| self.estimate_cost(total, p)).sum::<f64>() + n.sqrt()
            }
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
        out.push_str("+--------------------------------------------------------------------------+\n");
        out.push_str("|                            QUERY PLAN                                    |\n");
        out.push_str("+--------------------------------------------------------------------------+\n");
        out.push_str(&fmt_row("Table",          &plan.base.table));
        out.push_str(&fmt_row("Rows (total)",   &total.to_string()));
        out.push_str(&fmt_row("Rows (visible)", &visible.to_string()));
        if !pk.is_empty() { out.push_str(&fmt_row("PK", &pk)); }
        out.push_str(&fmt_row("Est. cost",      &format!("{:.1}", plan.total_cost())));
        out.push_str("|                                                                          |\n");
        let access_label = if plan.base.is_covering {
            format!("{} (Covering)", self.describe_access(&plan.base.access))
        } else {
            self.describe_access(&plan.base.access)
        };
        out.push_str(&fmt_row("Access", &access_label));
        for jp in &plan.joins {
            out.push_str(&fmt_row("Join", &self.describe_join(jp)));
        }
        out.push_str("+--------------------------------------------------------------------------+");
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
            AccessPath::SecondaryBetween { index_key, col, start, end } => format!("Index Range  {} ({} BETWEEN {} AND {})", index_key, col, start, end),
            AccessPath::CompositeIndex { index_name }             => format!("Composite Index  {}", index_name),
            AccessPath::CompositeIndexPrefix { index_name, .. }   => format!("Composite Index Prefix  {}", index_name),
            AccessPath::HashPoint { index_key, col, key }         => format!("Hash Index Scan  {} ({} = {})", index_key, col, key),
            AccessPath::SecondaryLikePrefix { index_key, col, prefix } => format!("Index Prefix Scan  {} ({} LIKE '{}%')", index_key, col, prefix),
            AccessPath::IndexIntersection { paths } => {
                let parts: Vec<String> = paths.iter().map(|p| self.describe_access(p)).collect();
                format!("Index Intersection  [{}]", parts.join(" ∩ "))
            }
        }
    }

    fn describe_join(&self, jp: &JoinPlan) -> String {
        let algo = match &jp.algo {
            JoinAlgo::NestedLoop => "Nested Loop".to_string(),
            JoinAlgo::Hash { probe_col, build_col } =>
                format!("Hash Join       probe={} build={}", probe_col, build_col),
            JoinAlgo::SortMerge { probe_col, build_col } =>
                format!("Sort-Merge Join probe={} build={}", probe_col, build_col),
            JoinAlgo::IndexNL { probe_col, right_pk_col } =>
                format!("Index NL Join   probe={} pk={}", probe_col, right_pk_col),
        };
        format!("{} → {}  cost≈{:.0}", algo, jp.right_table, jp.est_cost)
    }
}

// ── Formatting helper ─────────────────────────────────────────────────────

fn fmt_row(label: &str, value: &str) -> String {
    const W: usize = 72;
    let cell = format!("{}: {}", label, value);
    let mut out = String::new();
    let mut remaining = cell.as_str();
    let mut first = true;
    while !remaining.is_empty() {
        let (chunk, rest) = if remaining.chars().count() <= W {
            (remaining, "")
        } else {
            // split at last space within W chars, fall back to W if no space
            let cut = remaining.char_indices()
                .take_while(|(i, _)| *i <= W)
                .filter(|(_, c)| *c == ' ')
                .last()
                .map(|(i, _)| i)
                .unwrap_or_else(|| remaining.char_indices().nth(W).map(|(i, _)| i).unwrap_or(remaining.len()));
            (&remaining[..cut], remaining[cut..].trim_start())
        };
        if first {
            out.push_str(&format!("| {:<W$} |\n", chunk, W = W));
            first = false;
        } else {
            out.push_str(&format!("|   {:<W$} |\n", chunk, W = W - 2));
        }
        remaining = rest;
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

/// AND 체인에서 모든 리프(단일 Condition) 수집 (OR/NOT은 무시)
pub fn collect_and_leaves(expr: &CondExpr) -> Vec<&Condition> {
    let mut out = Vec::new();
    collect_and_leaves_rec(expr, &mut out);
    out
}

fn collect_and_leaves_rec<'a>(expr: &'a CondExpr, out: &mut Vec<&'a Condition>) {
    match expr {
        CondExpr::And(l, r) => {
            collect_and_leaves_rec(l, out);
            collect_and_leaves_rec(r, out);
        }
        CondExpr::Leaf(c) => out.push(c),
        _ => {}
    }
}

/// 'prefix%' 패턴에서 prefix를 추출한다. 중간 와일드카드가 있으면 None.
fn like_prefix(pat: &str) -> Option<String> {
    let prefix = pat.strip_suffix('%')?;
    if prefix.contains('%') || prefix.contains('_') || prefix.is_empty() {
        return None;
    }
    Some(prefix.to_string())
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
