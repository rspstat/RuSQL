use std::collections::{HashMap, HashSet};
use rayon::prelude::*;
use crate::parser::ast::{CondExpr, ArithExpr, ConditionValue, JoinType, Join, Operator};

type Row = HashMap<String, String>;
const NULL_VALUE: &str = "NULL";

// ── 행 병합 헬퍼 ──────────────────────────────────────────────────────────

/// right 행을 merged에 병합한다. 정규화 키(`table.col`)와 bare 키를 모두 삽입.
pub fn merge_right(merged: &mut Row, right: &Row, table: &str) {
    for (k, v) in right.iter() {
        merged.insert(format!("{}.{}", table, k), v.clone());
        merged.entry(k.clone()).or_insert_with(|| v.clone());
    }
}

/// right 측이 매칭되지 않았을 때 NULL로 채운다 (LEFT/FULL OUTER용).
pub fn null_right(merged: &mut Row, cols: &[String], table: &str) {
    for col in cols {
        merged.insert(format!("{}.{}", table, col), NULL_VALUE.to_string());
        merged.entry(col.clone()).or_insert_with(|| NULL_VALUE.to_string());
    }
}

// ── 조인 알고리즘 ─────────────────────────────────────────────────────────

/// Sort-Merge Join. 양쪽을 조인 키로 정렬 후 투 포인터로 병합.
pub fn sort_merge_join(
    left: &[Row],
    right: &[Row],
    join_type: &JoinType,
    table: &str,
    probe_col: &str,
    build_col: &str,
    right_schema_cols: &[String],
) -> Vec<Row> {
    let sort_cmp = |a: &str, b: &str| -> std::cmp::Ordering {
        match (a.parse::<f64>(), b.parse::<f64>()) {
            (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(std::cmp::Ordering::Equal),
            _ => a.cmp(b),
        }
    };
    let key_left = |row: &Row| -> String {
        row.get(probe_col)
            .or_else(|| row.iter().find(|(k, _)| k.ends_with(&format!(".{}", probe_col))).map(|(_, v)| v))
            .cloned()
            .unwrap_or_default()
    };
    let key_right = |row: &Row| -> String {
        row.get(build_col)
            .or_else(|| row.get(&format!("{}.{}", table, build_col)))
            .cloned()
            .unwrap_or_default()
    };

    let mut ls: Vec<Row> = left.to_vec();
    ls.sort_by(|a, b| sort_cmp(&key_left(a), &key_left(b)));
    let mut rs: Vec<Row> = right.to_vec();
    rs.sort_by(|a, b| sort_cmp(&key_right(a), &key_right(b)));

    let mut out = Vec::new();
    match join_type {
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
                        let li0 = li;
                        while li < ls.len() && key_left(&ls[li]) == lk { li += 1; }
                        let ri0 = ri;
                        while ri < rs.len() && key_right(&rs[ri]) == lk { ri += 1; }
                        for l in &ls[li0..li] {
                            for r in &rs[ri0..ri] {
                                let mut merged = l.clone();
                                merge_right(&mut merged, r, table);
                                out.push(merged);
                            }
                        }
                    }
                }
            }
        }
        JoinType::Left => {
            let mut ri_start = 0usize;
            for l in &ls {
                let lk = key_left(l);
                while ri_start < rs.len() && sort_cmp(&key_right(&rs[ri_start]), &lk) == std::cmp::Ordering::Less {
                    ri_start += 1;
                }
                let mut ri = ri_start;
                let mut matched = false;
                while ri < rs.len() && sort_cmp(&key_right(&rs[ri]), &lk) == std::cmp::Ordering::Equal {
                    let mut merged = l.clone();
                    merge_right(&mut merged, &rs[ri], table);
                    out.push(merged);
                    matched = true;
                    ri += 1;
                }
                if !matched {
                    let mut merged = l.clone();
                    null_right(&mut merged, right_schema_cols, table);
                    out.push(merged);
                }
            }
        }
        JoinType::Right => {
            let left_cols: Vec<String> = ls.first()
                .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                .unwrap_or_default();
            let mut li_start = 0usize;
            for r in &rs {
                let rk = key_right(r);
                while li_start < ls.len() && sort_cmp(&key_left(&ls[li_start]), &rk) == std::cmp::Ordering::Less {
                    li_start += 1;
                }
                let mut li = li_start;
                let mut matched = false;
                while li < ls.len() && sort_cmp(&key_left(&ls[li]), &rk) == std::cmp::Ordering::Equal {
                    let mut merged = ls[li].clone();
                    merge_right(&mut merged, r, table);
                    out.push(merged);
                    matched = true;
                    li += 1;
                }
                if !matched {
                    let mut merged: Row = left_cols.iter().map(|c| (c.clone(), NULL_VALUE.to_string())).collect();
                    merge_right(&mut merged, r, table);
                    out.push(merged);
                }
            }
        }
        _ => {}
    }
    out
}

/// Hash Join. Build phase: right를 build_col 기준 해시화. Probe phase: left로 조회.
pub fn hash_join(
    left: &[Row],
    right: &[Row],
    join_type: &JoinType,
    table: &str,
    probe_col: &str,
    build_col: &str,
    right_schema_cols: &[String],
) -> Vec<Row> {
    let mut hash: HashMap<String, Vec<Row>> = HashMap::new();
    for r in right {
        let key = r.get(build_col)
            .or_else(|| r.get(&format!("{}.{}", table, build_col)))
            .cloned()
            .unwrap_or_default();
        hash.entry(key).or_default().push(r.clone());
    }

    // Probe phase: Inner/Left는 read-only 해시맵 조회로 각 행이 독립적 → par_iter 병렬화
    let probe_suffix = format!(".{}", probe_col);
    match join_type {
        JoinType::Inner => {
            left.par_iter().flat_map(|l| {
                let pk = l.get(probe_col)
                    .or_else(|| l.iter().find(|(k, _)| k.ends_with(&probe_suffix)).map(|(_, v)| v))
                    .cloned()
                    .unwrap_or_default();
                hash.get(&pk)
                    .map(|matches| matches.iter().map(|r| {
                        let mut merged = l.clone();
                        merge_right(&mut merged, r, table);
                        merged
                    }).collect::<Vec<_>>())
                    .unwrap_or_default()
            }).collect()
        }
        JoinType::Left => {
            left.par_iter().flat_map(|l| {
                let pk = l.get(probe_col)
                    .or_else(|| l.iter().find(|(k, _)| k.ends_with(&probe_suffix)).map(|(_, v)| v))
                    .cloned()
                    .unwrap_or_default();
                if let Some(matches) = hash.get(&pk) {
                    matches.iter().map(|r| {
                        let mut merged = l.clone();
                        merge_right(&mut merged, r, table);
                        merged
                    }).collect::<Vec<_>>()
                } else {
                    let mut merged = l.clone();
                    null_right(&mut merged, right_schema_cols, table);
                    vec![merged]
                }
            }).collect()
        }
        JoinType::Right => {
            let mut left_hash: HashMap<String, Vec<Row>> = HashMap::new();
            for l in left {
                let key = l.get(probe_col).cloned().unwrap_or_default();
                left_hash.entry(key).or_default().push(l.clone());
            }
            let left_cols: Vec<String> = left.first()
                .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                .unwrap_or_default();
            let mut out = Vec::new();
            for r in right {
                let key = r.get(build_col).cloned().unwrap_or_default();
                if let Some(lefts) = left_hash.get(&key) {
                    for l in lefts {
                        let mut merged = l.clone();
                        merge_right(&mut merged, r, table);
                        out.push(merged);
                    }
                } else {
                    let mut merged: Row = left_cols.iter().map(|c| (c.clone(), NULL_VALUE.to_string())).collect();
                    merge_right(&mut merged, r, table);
                    out.push(merged);
                }
            }
            out
        }
        _ => unreachable!("Cross/Natural joins do not use Hash Join"),
    }
}

/// Nested Loop Join (기본 알고리즘). Cross/Natural/FullOuter 및 비등가 조인 포함.
/// `on_match`: merged row를 받아 ON 조건 통과 여부를 반환하는 클로저.
pub fn nested_loop_join<F>(
    left: &[Row],
    right: &[Row],
    join_type: &JoinType,
    table: &str,
    using_cols: &[String],
    right_schema_cols: &[String],
    on_match: F,
) -> Vec<Row>
where
    F: Fn(&Row) -> bool,
{
    let mut out = Vec::new();

    if !using_cols.is_empty() {
        for l in left {
            for r in right {
                let matches = using_cols.iter().all(|col| {
                    let lv = l.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                    let rv = r.get(col).map(String::as_str).unwrap_or(NULL_VALUE);
                    lv == rv && lv != NULL_VALUE
                });
                if matches {
                    let mut merged = l.clone();
                    merge_right(&mut merged, r, table);
                    out.push(merged);
                }
            }
        }
        return out;
    }

    match join_type {
        JoinType::Inner => {
            for l in left {
                for r in right {
                    let mut merged = l.clone();
                    merge_right(&mut merged, r, table);
                    if on_match(&merged) { out.push(merged); }
                }
            }
        }
        JoinType::Left => {
            for l in left {
                let mut matched = false;
                for r in right {
                    let mut merged = l.clone();
                    merge_right(&mut merged, r, table);
                    if on_match(&merged) { out.push(merged); matched = true; }
                }
                if !matched {
                    let mut merged = l.clone();
                    null_right(&mut merged, right_schema_cols, table);
                    out.push(merged);
                }
            }
        }
        JoinType::Right => {
            let left_cols: Vec<String> = left.first()
                .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                .unwrap_or_default();
            for r in right {
                let mut matched = false;
                for l in left {
                    let mut merged = l.clone();
                    merge_right(&mut merged, r, table);
                    if on_match(&merged) { out.push(merged); matched = true; }
                }
                if !matched {
                    let mut merged: Row = left_cols.iter().map(|c| (c.clone(), NULL_VALUE.to_string())).collect();
                    merge_right(&mut merged, r, table);
                    out.push(merged);
                }
            }
        }
        JoinType::Cross => {
            for l in left {
                for r in right {
                    let mut merged = l.clone();
                    merge_right(&mut merged, r, table);
                    out.push(merged);
                }
            }
        }
        JoinType::Natural => {
            let common_cols: Vec<String> = right_schema_cols.iter()
                .filter(|rc| left.first()
                    .map(|lr| lr.contains_key(*rc) || lr.keys().any(|k| k == *rc))
                    .unwrap_or(false))
                .cloned()
                .collect();
            for l in left {
                for r in right {
                    let mut merged = l.clone();
                    merge_right(&mut merged, r, table);
                    let matches = common_cols.iter().all(|col| {
                        let lv = l.get(col).map(String::as_str).unwrap_or("");
                        let rv = r.get(col).map(String::as_str).unwrap_or("");
                        lv == rv
                    });
                    if matches { out.push(merged); }
                }
            }
        }
        JoinType::FullOuter => {
            let left_cols: Vec<String> = left.first()
                .map(|r| r.keys().filter(|k| !k.starts_with('_') && !k.contains('.')).cloned().collect())
                .unwrap_or_default();
            let mut matched_right: HashSet<usize> = HashSet::new();
            for l in left {
                let mut any_match = false;
                for (ri, r) in right.iter().enumerate() {
                    let mut merged = l.clone();
                    merge_right(&mut merged, r, table);
                    if on_match(&merged) {
                        out.push(merged);
                        matched_right.insert(ri);
                        any_match = true;
                    }
                }
                if !any_match {
                    let mut merged = l.clone();
                    null_right(&mut merged, right_schema_cols, table);
                    out.push(merged);
                }
            }
            for (ri, r) in right.iter().enumerate() {
                if !matched_right.contains(&ri) {
                    let mut merged: Row = left_cols.iter()
                        .map(|c| (c.clone(), NULL_VALUE.to_string()))
                        .collect();
                    merge_right(&mut merged, r, table);
                    out.push(merged);
                }
            }
        }
    }
    out
}

// ── JOIN 순서 최적화 ──────────────────────────────────────────────────────

/// ON 조건에서 테이블 접두사를 추출한다 (의존성 분석용).
pub fn collect_table_refs_from_expr(expr: &CondExpr, refs: &mut HashSet<String>) {
    match expr {
        CondExpr::And(l, r) | CondExpr::Or(l, r) => {
            collect_table_refs_from_expr(l, refs);
            collect_table_refs_from_expr(r, refs);
        }
        CondExpr::Not(inner) => collect_table_refs_from_expr(inner, refs),
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

/// 비용 기반 DP로 INNER JOIN 순서를 최적화 (System-R 스타일).
/// OUTER JOIN 포함 or 테이블 수 > 8이면 greedy로 폴백.
pub fn reorder_joins_dp(base_table: &str, joins: Vec<Join>, tables: &HashMap<String, Vec<Row>>) -> Vec<Join> {
    let n = joins.len();
    if n <= 1 { return joins; }
    if joins.iter().any(|j| !matches!(j.join_type, JoinType::Inner | JoinType::Natural)) {
        return joins;
    }
    if n > 8 { return reorder_joins_greedy(base_table, joins, tables); }

    let size_of = |t: &str| tables.get(t).map(|r| r.len()).unwrap_or(0).max(1);
    let base_card = size_of(base_table);
    let full = (1usize << n) - 1;
    let mut dp: Vec<Option<(f64, usize, Vec<usize>)>> = vec![None; 1 << n];
    dp[0] = Some((0.0, base_card, Vec::new()));

    for mask in 0..(1usize << n) {
        let (cur_cost, cur_card, order) = match dp[mask].clone() { Some(v) => v, None => continue };

        let mut available: HashSet<String> = HashSet::new();
        available.insert(base_table.to_string());
        if let Some(p) = base_table.rfind('.') { available.insert(base_table[p + 1..].to_string()); }
        for &k in &order {
            let t = &joins[k].table;
            available.insert(t.clone());
            if let Some(p) = t.rfind('.') { available.insert(t[p + 1..].to_string()); }
        }

        for j in 0..n {
            if mask & (1 << j) != 0 { continue; }
            let mut refs = HashSet::new();
            collect_table_refs_from_expr(&joins[j].on_expr, &mut refs);
            let join_bare = joins[j].table.split('.').last().unwrap_or(&joins[j].table);
            let joinable = refs.iter().all(|r| {
                let rb = r.split('.').last().unwrap_or(r);
                available.contains(r) || available.contains(rb)
                    || r == &joins[j].table || rb == join_bare
            });
            if !joinable { continue; }

            let rsize = size_of(&joins[j].table);
            let is_equi = matches!(&joins[j].on_expr, CondExpr::Leaf(c) if c.operator == Operator::Eq);
            let (step_cost, new_card) = if is_equi {
                ((cur_card + rsize) as f64, cur_card.max(rsize))
            } else {
                (cur_card.saturating_mul(rsize) as f64, cur_card.saturating_mul(rsize))
            };
            let new_cost = cur_cost + step_cost;
            let nmask = mask | (1 << j);
            let better = match &dp[nmask] { None => true, Some((c, _, _)) => new_cost < *c };
            if better {
                let mut no = order.clone();
                no.push(j);
                dp[nmask] = Some((new_cost, new_card, no));
            }
        }
    }

    match dp[full].take() {
        Some((_, _, order)) if order.len() == n =>
            order.into_iter().map(|i| joins[i].clone()).collect(),
        _ => reorder_joins_greedy(base_table, joins, tables),
    }
}

/// Greedy JOIN 순서 최적화: 의존성을 만족하는 후보 중 가장 작은 테이블 우선.
pub fn reorder_joins_greedy(base_table: &str, joins: Vec<Join>, tables: &HashMap<String, Vec<Row>>) -> Vec<Join> {
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
                collect_table_refs_from_expr(&j.on_expr, &mut refs);
                let join_bare = j.table.split('.').last().unwrap_or(&j.table);
                let joinable = refs.iter().all(|r| {
                    let rb = r.split('.').last().unwrap_or(r);
                    available.contains(r) || available.contains(rb)
                        || r == &j.table || rb == join_bare
                });
                if joinable { Some(i) } else { None }
            })
            .collect();

        let best = if candidates.is_empty() { 0 } else {
            *candidates.iter()
                .min_by_key(|&&i| tables.get(&remaining[i].table).map(|r| r.len()).unwrap_or(0))
                .unwrap()
        };

        let chosen = remaining.remove(best);
        available.insert(chosen.table.clone());
        if let Some(pos) = chosen.table.rfind('.') {
            available.insert(chosen.table[pos + 1..].to_string());
        }
        reordered.push(chosen);
    }
    reordered
}
