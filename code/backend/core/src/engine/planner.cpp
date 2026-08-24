#include "engine/planner.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace engine {

namespace {
bool try_parse_f64(const std::string& s, double& out) {
    if (s.empty()) return false;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

std::string bare_col(const std::string& s) {
    auto pos = s.rfind('.');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

std::string table_prefix(const std::string& s) {
    auto pos = s.find('.');
    return pos == std::string::npos ? "" : s.substr(0, pos);
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ieq(const std::string& a, const std::string& b) {
    return to_lower(a) == to_lower(b);
}
} // namespace

bool range_op_inclusive(RangeOp op) { return op == RangeOp::Gte || op == RangeOp::Lte; }
bool range_op_is_lower_bound(RangeOp op) { return op == RangeOp::Gt || op == RangeOp::Gte; }
const char* range_op_label(RangeOp op) {
    switch (op) {
        case RangeOp::Gt: return ">";
        case RangeOp::Gte: return ">=";
        case RangeOp::Lt: return "<";
        default: return "<=";
    }
}

SelectPlan Planner::plan(const std::string& table, const std::optional<CondExpr>& condition, const std::vector<Join>& joins) const {
    TablePlan base = plan_table(table, condition);
    std::vector<JoinPlan> join_plans;
    join_plans.reserve(joins.size());
    // 2번째 이후 조인의 알고리즘 선택(Hash/SortMerge/IndexNL/NestedLoop)이 매번 원본 base 테이블의
    // 행 수만 보고 결정되던 버그 수정 -- current.est_rows를 매 조인 후 누적 갱신해 실제 중간 결과
    // 크기를 반영한다. current.table은 일부러 base.table 그대로 유지: estimate_join_output의
    // NDV(고유값 개수) 조회가 원래도 좌변을 "원본 테이블 하나"로만 다뤄왔던 기존 한계라 이 수정
    // 범위 밖(중간 결과 자체의 컬럼 통계는 추적하지 않음) -- 카디널리티(행 수)만 정확해진다.
    TablePlan current = base;
    for (auto& j : joins) {
        JoinPlan jp = plan_join(current, j);
        current.est_rows = jp.est_rows;
        join_plans.push_back(std::move(jp));
    }
    return SelectPlan{std::move(base), std::move(join_plans)};
}

SelectPlan Planner::plan_covering(const std::string& table, const std::optional<CondExpr>& condition,
                                   const std::vector<Join>& joins, const std::vector<SelectColumn>& columns) const {
    SelectPlan p = plan(table, condition, joins);
    p.base.is_covering = is_covering_access(p.base.access, columns, table);
    return p;
}

bool Planner::is_covering_access(const AccessPath& access, const std::vector<SelectColumn>& columns, const std::string& table) const {
    std::string index_col;
    bool has_index_col = std::visit(
        [&](const auto& alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, AccessPath::SecondaryPoint> || std::is_same_v<T, AccessPath::SecondaryRange> ||
                          std::is_same_v<T, AccessPath::SecondaryBetween> || std::is_same_v<T, AccessPath::SecondaryLikePrefix>) {
                index_col = alt.col;
                return true;
            } else {
                return false;
            }
        },
        access.data);
    if (!has_index_col) return false;

    auto col_name_of = [](const SelectColumn& c) -> std::optional<std::string> {
        if (std::holds_alternative<SelectColumn::Column>(c.data)) return std::get<SelectColumn::Column>(c.data).name;
        if (std::holds_alternative<SelectColumn::ColumnAlias>(c.data)) return std::get<SelectColumn::ColumnAlias>(c.data).name;
        return std::nullopt;
    };

    bool simple = !columns.empty() && std::all_of(columns.begin(), columns.end(), [&](const SelectColumn& c) {
        auto n = col_name_of(c);
        return n && *n == index_col;
    });
    if (simple) return true;

    std::vector<std::string> selected;
    for (auto& c : columns) {
        auto n = col_name_of(c);
        if (n) selected.push_back(*n);
    }
    if (selected.empty()) return false;

    for (auto& [name, ci] : composite_indexes_) {
        (void)name;
        if (ci.table != table) continue;
        bool all_covered = std::all_of(selected.begin(), selected.end(), [&](const std::string& sc) {
            return std::find(ci.columns.begin(), ci.columns.end(), sc) != ci.columns.end();
        });
        if (all_covered) return true;
    }
    return false;
}

TablePlan Planner::plan_table(const std::string& table, const std::optional<CondExpr>& condition) const {
    std::size_t total = table_size(table);
    auto pk = pk_col(table);
    AccessPath access = choose_access(table, condition, pk);
    std::size_t est_rows = estimate_rows(total, access, table);
    double est_cost = estimate_cost(total, access);
    return TablePlan{table, access, condition, est_rows, est_cost, false};
}

AccessPath Planner::choose_access(const std::string& table, const std::optional<CondExpr>& condition, const std::optional<std::string>& pk) const {
    if (!condition) return AccessPath(AccessPath::SeqScan{});
    const CondExpr& expr = *condition;

    if (std::holds_alternative<CondExpr::Leaf>(expr.data)) {
        const Condition& cond = std::get<CondExpr::Leaf>(expr.data).condition;
        if (std::holds_alternative<ArithExpr::Col>(cond.left.data)) {
            const std::string& col_full = std::get<ArithExpr::Col>(cond.left.data).name;
            std::string col = bare_col(col_full);
            if (pk && *pk == col) {
                if (auto path = pk_access(cond, table)) return *path;
            }
            if (cond.op == Operator::Eq) {
                if (std::holds_alternative<ConditionValue::Literal>(cond.value.data)) {
                    const std::string& k = std::get<ConditionValue::Literal>(cond.value.data).value;
                    if (!is_col_ref_in_context(k, table)) {
                        if (auto idx_key = find_hash_index(table, col)) {
                            return AccessPath(AccessPath::HashPoint{*idx_key, col, k});
                        }
                    }
                }
            }
            if (auto idx_key = find_secondary_index(table, col)) {
                if (auto path = secondary_access(*idx_key, col, cond, table)) return *path;
            }
        }
    }

    auto eq_map = collect_eq_map(expr);
    if (!eq_map.empty()) {
        for (auto& [name, ci] : composite_indexes_) {
            if (ci.table == table && ci.matches_conditions(eq_map)) {
                return AccessPath(AccessPath::CompositeIndexPath{name});
            }
        }
        for (auto& [name, ci] : composite_indexes_) {
            if (ci.table != table) continue;
            if (auto prefix = ci.prefix_key_from_eq_map(eq_map)) {
                return AccessPath(AccessPath::CompositeIndexPrefix{name, *prefix});
            }
        }
    }

    if (auto intersection = try_index_intersection(table, expr, pk)) return *intersection;

    return AccessPath(AccessPath::SeqScan{});
}

std::optional<AccessPath> Planner::try_index_intersection(const std::string& table, const CondExpr& expr,
                                                           const std::optional<std::string>& pk) const {
    auto leaves = collect_and_leaves(expr);
    if (leaves.size() < 2) return std::nullopt;

    std::vector<AccessPath> sub_paths;
    for (auto* cond : leaves) {
        if (!std::holds_alternative<ArithExpr::Col>(cond->left.data)) continue;
        const std::string& col_full = std::get<ArithExpr::Col>(cond->left.data).name;
        std::string col = bare_col(col_full);
        if (pk && *pk == col) continue;
        if (cond->op != Operator::Eq) continue;
        if (!std::holds_alternative<ConditionValue::Literal>(cond->value.data)) continue;
        const std::string& k = std::get<ConditionValue::Literal>(cond->value.data).value;
        if (is_col_ref_in_context(k, table)) continue;
        if (auto idx_key = find_hash_index(table, col)) {
            sub_paths.push_back(AccessPath(AccessPath::HashPoint{*idx_key, col, k}));
            continue;
        }
        if (auto idx_key = find_secondary_index(table, col)) {
            sub_paths.push_back(AccessPath(AccessPath::SecondaryPoint{*idx_key, col, k}));
        }
    }
    if (sub_paths.size() >= 2) return AccessPath(AccessPath::IndexIntersection{std::move(sub_paths)});
    return std::nullopt;
}

std::optional<AccessPath> Planner::pk_access(const Condition& cond, const std::string& table) const {
    auto lit = [&]() -> const std::string* {
        return std::holds_alternative<ConditionValue::Literal>(cond.value.data)
                   ? &std::get<ConditionValue::Literal>(cond.value.data).value
                   : nullptr;
    };
    if (cond.op == Operator::Eq) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::PkPoint{*k});
    } else if (cond.op == Operator::Between && std::holds_alternative<ConditionValue::Between>(cond.value.data)) {
        auto& b = std::get<ConditionValue::Between>(cond.value.data);
        return AccessPath(AccessPath::PkBetween{b.lo, b.hi});
    } else if (cond.op == Operator::Gt) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::PkRange{RangeOp::Gt, *k});
    } else if (cond.op == Operator::Gte) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::PkRange{RangeOp::Gte, *k});
    } else if (cond.op == Operator::Lt) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::PkRange{RangeOp::Lt, *k});
    } else if (cond.op == Operator::Lte) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::PkRange{RangeOp::Lte, *k});
    }
    return std::nullopt;
}

namespace {
std::optional<std::string> like_prefix(const std::string& pat) {
    if (pat.empty() || pat.back() != '%') return std::nullopt;
    std::string prefix = pat.substr(0, pat.size() - 1);
    if (prefix.find('%') != std::string::npos || prefix.find('_') != std::string::npos || prefix.empty()) return std::nullopt;
    return prefix;
}
} // namespace

std::optional<AccessPath> Planner::secondary_access(const std::string& index_key, const std::string& col,
                                                     const Condition& cond, const std::string& table) const {
    auto lit = [&]() -> const std::string* {
        return std::holds_alternative<ConditionValue::Literal>(cond.value.data)
                   ? &std::get<ConditionValue::Literal>(cond.value.data).value
                   : nullptr;
    };
    if (cond.op == Operator::Eq) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::SecondaryPoint{index_key, col, *k});
    } else if (cond.op == Operator::Between && std::holds_alternative<ConditionValue::Between>(cond.value.data)) {
        auto& b = std::get<ConditionValue::Between>(cond.value.data);
        return AccessPath(AccessPath::SecondaryBetween{index_key, col, b.lo, b.hi});
    } else if (cond.op == Operator::Gt) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::SecondaryRange{index_key, col, RangeOp::Gt, *k});
    } else if (cond.op == Operator::Gte) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::SecondaryRange{index_key, col, RangeOp::Gte, *k});
    } else if (cond.op == Operator::Lt) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::SecondaryRange{index_key, col, RangeOp::Lt, *k});
    } else if (cond.op == Operator::Lte) {
        if (auto* k = lit(); k && !is_col_ref_in_context(*k, table)) return AccessPath(AccessPath::SecondaryRange{index_key, col, RangeOp::Lte, *k});
    } else if (cond.op == Operator::Like) {
        if (auto* pat = lit()) {
            if (auto prefix = like_prefix(*pat)) return AccessPath(AccessPath::SecondaryLikePrefix{index_key, col, *prefix});
        }
    }
    return std::nullopt;
}

bool Planner::is_col_ref_in_context(const std::string& k, const std::string& table) const {
    auto is_ident = [](const std::string& s) {
        if (s.empty()) return false;
        if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
        return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    };
    if (k.find('.') != std::string::npos) {
        auto pos = k.find('.');
        std::string p1 = k.substr(0, pos);
        std::string p2 = k.substr(pos + 1);
        return is_ident(p1) && is_ident(p2);
    }
    double dummy;
    if ((std::isalpha(static_cast<unsigned char>(k[0])) || k[0] == '_') && !try_parse_f64(k, dummy)) {
        auto* schema = catalog_.get_table(table);
        if (!schema) return false;
        return std::any_of(schema->columns.begin(), schema->columns.end(),
                            [&](const ColumnDef& c) { return ieq(c.name, k); });
    }
    return false;
}

std::optional<std::string> Planner::find_secondary_index(const std::string& table, const std::string& col) const {
    for (auto& [name, tc] : index_meta_) {
        if (tc.first == table && tc.second == col) return table + "_" + name;
    }
    return std::nullopt;
}

std::optional<std::string> Planner::find_hash_index(const std::string& table, const std::string& col) const {
    for (auto& [name, tc] : hash_index_meta_) {
        if (tc.first == table && tc.second == col) return table + "_" + name;
    }
    return std::nullopt;
}

JoinPlan Planner::plan_join(const TablePlan& base, const Join& join) const {
    std::size_t right_size = table_size(join.table);
    JoinAlgo algo = choose_join_algo(base.est_rows, right_size, join.on_expr, base.table, join.table);
    double est_cost = std::visit(
        [&](const auto& alt) -> double {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, JoinAlgo::NestedLoop>) {
                return static_cast<double>(base.est_rows * std::max<std::size_t>(right_size, 1));
            } else if constexpr (std::is_same_v<T, JoinAlgo::Hash>) {
                return static_cast<double>(base.est_rows + right_size) * 1.5;
            } else if constexpr (std::is_same_v<T, JoinAlgo::SortMerge>) {
                double n = static_cast<double>(base.est_rows + right_size);
                return n * std::max(std::log2(n), 1.0) + n;
            } else {
                double log_m = std::max(std::log2(static_cast<double>(right_size)), 1.0);
                return static_cast<double>(base.est_rows) * log_m;
            }
        },
        algo.data);
    std::size_t est_rows = estimate_join_output(base, right_size, join.on_expr, join.table);
    return JoinPlan{join.table, join.on_expr, join.join_type, algo, est_rows, est_cost};
}

std::size_t Planner::estimate_join_output(const TablePlan& base, std::size_t right_size, const CondExpr& on_expr,
                                           const std::string& right_table) const {
    std::size_t left_rows = base.est_rows;
    if (left_rows == 0 || right_size == 0) return 0;

    if (std::holds_alternative<CondExpr::Leaf>(on_expr.data)) {
        const Condition& cond = std::get<CondExpr::Leaf>(on_expr.data).condition;
        if (cond.op == Operator::Eq && std::holds_alternative<ArithExpr::Col>(cond.left.data) &&
            std::holds_alternative<ConditionValue::Literal>(cond.value.data)) {
            const std::string& lc = std::get<ArithExpr::Col>(cond.left.data).name;
            const std::string& rv = std::get<ConditionValue::Literal>(cond.value.data).value;
            std::string lhs_col = bare_col(lc);
            std::string rhs_col = bare_col(rv);
            std::string right_bare = to_lower(bare_col(right_table));
            std::string rv_tbl = to_lower(table_prefix(rv));
            std::string left_col, right_col;
            if (rv_tbl == right_bare) { left_col = lhs_col; right_col = rhs_col; }
            else { left_col = rhs_col; right_col = lhs_col; }

            std::optional<std::size_t> l_ndv, r_ndv;
            if (auto it = table_stats_.find(base.table); it != table_stats_.end()) {
                if (auto cit = it->second.columns.find(left_col); cit != it->second.columns.end() && cit->second.distinct_count > 0)
                    l_ndv = cit->second.distinct_count;
            }
            if (auto it = table_stats_.find(right_table); it != table_stats_.end()) {
                if (auto cit = it->second.columns.find(right_col); cit != it->second.columns.end() && cit->second.distinct_count > 0)
                    r_ndv = cit->second.distinct_count;
            }
            std::size_t ndv;
            if (l_ndv && r_ndv) ndv = std::max(*l_ndv, *r_ndv);
            else if (l_ndv) ndv = *l_ndv;
            else if (r_ndv) ndv = *r_ndv;
            else return std::max<std::size_t>(std::min(left_rows, right_size), 1);

            return std::max<std::size_t>(
                static_cast<std::size_t>(std::ceil(static_cast<double>(left_rows) * static_cast<double>(right_size) / static_cast<double>(ndv))),
                1);
        }
    }
    return std::max<std::size_t>(std::min(left_rows, right_size), 1);
}

JoinAlgo Planner::choose_join_algo(std::size_t left_size, std::size_t right_size, const CondExpr& on_expr,
                                    const std::string& /*left_table*/, const std::string& right_table) const {
    constexpr std::size_t HASH_FACTOR = 3;
    std::size_t nl_cost = left_size * right_size; // saturating_mul: sizes here are in-memory row counts, overflow not a practical concern
    std::size_t hash_cost = (left_size + right_size) * HASH_FACTOR;

    if (nl_cost <= hash_cost) return JoinAlgo(JoinAlgo::NestedLoop{});

    if (std::holds_alternative<CondExpr::Leaf>(on_expr.data)) {
        const Condition& cond = std::get<CondExpr::Leaf>(on_expr.data).condition;
        if (cond.op == Operator::Eq && std::holds_alternative<ArithExpr::Col>(cond.left.data) &&
            std::holds_alternative<ConditionValue::Literal>(cond.value.data)) {
            const std::string& lc = std::get<ArithExpr::Col>(cond.left.data).name;
            const std::string& rv = std::get<ConditionValue::Literal>(cond.value.data).value;
            std::string lhs_col = bare_col(lc);
            std::string rhs_col = bare_col(rv);
            std::string lhs_tbl = to_lower(table_prefix(lc));
            std::string rhs_tbl = to_lower(table_prefix(rv));
            std::string right_bare = to_lower(bare_col(right_table));

            std::string probe_col, build_col;
            if (rhs_tbl == right_bare) { probe_col = lhs_col; build_col = rhs_col; }
            else if (lhs_tbl == right_bare) { probe_col = rhs_col; build_col = lhs_col; }
            else return JoinAlgo(JoinAlgo::NestedLoop{});

            if (auto pk = pk_col(right_table); pk && *pk == build_col) {
                double log_right = std::max(std::log2(static_cast<double>(right_size)), 1.0);
                std::size_t inl_cost = static_cast<std::size_t>(static_cast<double>(left_size) * log_right);
                if (inl_cost <= hash_cost) return JoinAlgo(JoinAlgo::IndexNL{probe_col, build_col});
            }

            std::size_t n = left_size + right_size;
            double log_n_d = std::max(std::log2(static_cast<double>(n)), 1.0);
            std::size_t log_n = static_cast<std::size_t>(log_n_d);
            std::size_t sm_cost = n * log_n;
            if (sm_cost <= hash_cost) return JoinAlgo(JoinAlgo::SortMerge{probe_col, build_col});
            return JoinAlgo(JoinAlgo::Hash{probe_col, build_col});
        }
    }
    return JoinAlgo(JoinAlgo::NestedLoop{});
}

std::size_t Planner::table_size(const std::string& table) const {
    auto it = tables_.find(table);
    return it != tables_.end() ? it->second.size() : 0;
}

std::optional<std::string> Planner::pk_col(const std::string& table) const {
    auto* schema = catalog_.get_table(table);
    if (!schema) return std::nullopt;
    for (auto& c : schema->columns) {
        if (c.primary_key) return c.name;
    }
    return std::nullopt;
}

std::size_t Planner::estimate_rows(std::size_t total, const AccessPath& access, const std::string& table) const {
    return std::visit(
        [&](const auto& alt) -> std::size_t {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, AccessPath::SeqScan>) {
                return total;
            } else if constexpr (std::is_same_v<T, AccessPath::PkPoint> || std::is_same_v<T, AccessPath::CompositeIndexPath> ||
                                  std::is_same_v<T, AccessPath::HashPoint>) {
                return 1;
            } else if constexpr (std::is_same_v<T, AccessPath::PkRange>) {
                double sel = 0.25;
                if (auto pk = pk_col(table)) sel = histogram_sel_range(table, *pk, alt.op, alt.key);
                return std::max<std::size_t>(static_cast<std::size_t>(static_cast<double>(total) * sel), 1);
            } else if constexpr (std::is_same_v<T, AccessPath::PkBetween>) {
                double sel = 0.25;
                if (auto pk = pk_col(table)) sel = histogram_sel_between(table, *pk, alt.start, alt.end);
                return std::max<std::size_t>(static_cast<std::size_t>(static_cast<double>(total) * sel), 1);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryRange>) {
                double sel = histogram_sel_range(table, alt.col, alt.op, alt.key);
                return std::max<std::size_t>(static_cast<std::size_t>(static_cast<double>(total) * sel), 1);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryBetween>) {
                double sel = histogram_sel_between(table, alt.col, alt.start, alt.end);
                return std::max<std::size_t>(static_cast<std::size_t>(static_cast<double>(total) * sel), 1);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryLikePrefix>) {
                return std::max<std::size_t>(total / 10, 1);
            } else if constexpr (std::is_same_v<T, AccessPath::CompositeIndexPrefix>) {
                return std::max<std::size_t>(total / 5, 1);
            } else if constexpr (std::is_same_v<T, AccessPath::IndexIntersection>) {
                std::optional<std::size_t> m;
                for (auto& p : alt.paths) {
                    std::size_t r = estimate_rows(total, p, table);
                    if (!m || r < *m) m = r;
                }
                return m.value_or(1);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryPoint>) {
                // Rust: index_key.split("_idx_").next() always yields Some(...) — the
                // whole string when "_idx_" isn't present, since str::split never
                // returns an empty iterator. The `.split('_').next()` fallback in the
                // original is therefore unreachable; this mirrors the reachable behavior.
                std::string tbl;
                auto idx_pos = alt.index_key.find("_idx_");
                if (idx_pos != std::string::npos) tbl = alt.index_key.substr(0, idx_pos);
                else tbl = alt.index_key;
                if (auto it = table_stats_.find(tbl); it != table_stats_.end()) {
                    if (auto cit = it->second.columns.find(alt.col); cit != it->second.columns.end() && cit->second.distinct_count > 0) {
                        return std::max<std::size_t>(total / cit->second.distinct_count, 1);
                    }
                }
                return std::max<std::size_t>(total / 10, 1);
            } else {
                return 1;
            }
        },
        access.data);
}

double Planner::histogram_sel_range(const std::string& table, const std::string& col, RangeOp op, const std::string& key) const {
    auto it = table_stats_.find(table);
    if (it == table_stats_.end()) return 0.25;
    auto cit = it->second.columns.find(col);
    if (cit == it->second.columns.end() || cit->second.histogram.empty()) return 0.25;
    const auto& hist = cit->second.histogram;

    double n = static_cast<double>(hist.size());
    double key_f;
    bool key_is_num = try_parse_f64(key, key_f);

    double gt_count = 0;
    for (auto& bound : hist) {
        double bound_f;
        bool bound_is_num = try_parse_f64(bound, bound_f);
        bool is_gt;
        if (key_is_num && bound_is_num) is_gt = bound_f > key_f;
        else is_gt = bound > key;
        if (is_gt) gt_count += 1.0;
    }
    double lte_count = n - gt_count;
    if (op == RangeOp::Gt || op == RangeOp::Gte) return std::max(gt_count / n, 0.01);
    return std::max(lte_count / n, 0.01);
}

double Planner::histogram_sel_between(const std::string& table, const std::string& col, const std::string& lo, const std::string& hi) const {
    double sel_lte_hi = histogram_sel_range(table, col, RangeOp::Lte, hi);
    double sel_lt_lo = histogram_sel_range(table, col, RangeOp::Lt, lo);
    return std::max(sel_lte_hi - sel_lt_lo, 0.01);
}

double Planner::estimate_cost(std::size_t total, const AccessPath& access) const {
    double n = std::max(static_cast<double>(total), 1.0);
    double log_n = std::max(std::log2(n), 1.0);
    return std::visit(
        [&](const auto& alt) -> double {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, AccessPath::SeqScan>) return n;
            else if constexpr (std::is_same_v<T, AccessPath::PkPoint>) return log_n;
            else if constexpr (std::is_same_v<T, AccessPath::PkBetween> || std::is_same_v<T, AccessPath::PkRange>) return log_n + n / 4.0;
            else if constexpr (std::is_same_v<T, AccessPath::SecondaryPoint>) return log_n * 2.0;
            else if constexpr (std::is_same_v<T, AccessPath::SecondaryRange> || std::is_same_v<T, AccessPath::SecondaryBetween> ||
                                std::is_same_v<T, AccessPath::SecondaryLikePrefix> || std::is_same_v<T, AccessPath::CompositeIndexPrefix>)
                return log_n * 2.0 + n / 4.0;
            else if constexpr (std::is_same_v<T, AccessPath::CompositeIndexPath>) return log_n;
            else if constexpr (std::is_same_v<T, AccessPath::HashPoint>) return 1.0;
            else if constexpr (std::is_same_v<T, AccessPath::IndexIntersection>) {
                double sum = 0;
                for (auto& p : alt.paths) sum += estimate_cost(total, p);
                return sum + std::sqrt(n);
            } else return n;
        },
        access.data);
}

namespace {
// Counts Unicode codepoints (UTF-8 lead bytes), matching Rust's `str::chars().count()`
// -- NOT byte length. Several EXPLAIN description strings embed multi-byte UTF-8
// symbols ("\xE2\x86\x92" / "\xe2\x88\xa9"), which are 3 bytes but 1 char each; using
// byte length for the width/padding math here (as this function previously did)
// under-pads those lines by 2 columns each.
std::size_t utf8_char_count(const std::string& s) {
    std::size_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) count++;
    }
    return count;
}

// Byte length of the UTF-8 sequence starting at s[pos].
std::size_t utf8_seq_len(const std::string& s, std::size_t pos) {
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

std::string fmt_row(const std::string& label, const std::string& value) {
    constexpr std::size_t W = 72;
    std::string cell = label + ": " + value;
    std::string out;
    std::string remaining = cell;
    bool first = true;
    while (!remaining.empty()) {
        std::string chunk;
        std::string rest;
        if (utf8_char_count(remaining) <= W) {
            chunk = remaining;
            rest = "";
        } else {
            // Walk character-by-character (not byte-by-byte) up to W chars, tracking
            // the byte position of the last space seen. (Rust's own cut-search here
            // compares char_indices()' BYTE offset against the char-count constant W,
            // an inconsistency with its own outer `chars().count() <= W` check --
            // this branch isn't reachable by any current test data, since none of
            // these description strings run long enough to need wrapping, so this
            // uses the more internally-consistent char-index interpretation rather
            // than replicating that byte/char mismatch.)
            std::size_t byte_pos = 0;
            std::size_t char_idx = 0;
            std::size_t last_space_byte = std::string::npos;
            std::size_t w_cut_byte = remaining.size();
            while (byte_pos < remaining.size()) {
                if (char_idx > W) {
                    w_cut_byte = byte_pos;
                    break;
                }
                if (remaining[byte_pos] == ' ') last_space_byte = byte_pos;
                byte_pos += utf8_seq_len(remaining, byte_pos);
                char_idx++;
            }
            std::size_t cut = last_space_byte != std::string::npos ? last_space_byte : w_cut_byte;
            if (cut == 0) cut = w_cut_byte;
            chunk = remaining.substr(0, cut);
            rest = remaining.substr(cut);
            std::size_t start = rest.find_first_not_of(' ');
            rest = start == std::string::npos ? "" : rest.substr(start);
        }
        std::size_t chunk_chars = utf8_char_count(chunk);
        std::ostringstream oss;
        if (first) {
            oss << "| " << chunk << std::string(chunk_chars < W ? W - chunk_chars : 0, ' ') << " |\n";
            first = false;
        } else {
            oss << "|   " << chunk << std::string(chunk_chars < W - 2 ? (W - 2) - chunk_chars : 0, ' ') << " |\n";
        }
        out += oss.str();
        remaining = rest;
    }
    return out;
}
} // namespace

std::string Planner::describe_access(const AccessPath& access) const {
    return std::visit(
        [&](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, AccessPath::SeqScan>) return "Seq Scan";
            else if constexpr (std::is_same_v<T, AccessPath::PkPoint>) return "Index Scan  PK = " + alt.key;
            else if constexpr (std::is_same_v<T, AccessPath::PkBetween>) return "Index Range  PK BETWEEN " + alt.start + " AND " + alt.end;
            else if constexpr (std::is_same_v<T, AccessPath::PkRange>) return std::string("Index Range  PK ") + range_op_label(alt.op) + " " + alt.key;
            else if constexpr (std::is_same_v<T, AccessPath::SecondaryPoint>)
                return "Index Scan  " + alt.index_key + " (" + alt.col + " = " + alt.key + ")";
            else if constexpr (std::is_same_v<T, AccessPath::SecondaryRange>)
                return "Index Range  " + alt.index_key + " (" + alt.col + " " + range_op_label(alt.op) + " " + alt.key + ")";
            else if constexpr (std::is_same_v<T, AccessPath::SecondaryBetween>)
                return "Index Range  " + alt.index_key + " (" + alt.col + " BETWEEN " + alt.start + " AND " + alt.end + ")";
            else if constexpr (std::is_same_v<T, AccessPath::CompositeIndexPath>) return "Composite Index  " + alt.index_name;
            else if constexpr (std::is_same_v<T, AccessPath::CompositeIndexPrefix>) return "Composite Index Prefix  " + alt.index_name;
            else if constexpr (std::is_same_v<T, AccessPath::HashPoint>)
                return "Hash Index Scan  " + alt.index_key + " (" + alt.col + " = " + alt.key + ")";
            else if constexpr (std::is_same_v<T, AccessPath::SecondaryLikePrefix>)
                return "Index Prefix Scan  " + alt.index_key + " (" + alt.col + " LIKE '" + alt.prefix + "%')";
            else if constexpr (std::is_same_v<T, AccessPath::IndexIntersection>) {
                std::string out = "Index Intersection  [";
                for (std::size_t i = 0; i < alt.paths.size(); i++) {
                    if (i) out += " \xE2\x88\xA9 "; // "∩"
                    out += describe_access(alt.paths[i]);
                }
                out += "]";
                return out;
            } else {
                return "";
            }
        },
        access.data);
}

std::string Planner::describe_join(const JoinPlan& jp) const {
    std::string algo = std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, JoinAlgo::NestedLoop>) return "Nested Loop";
            else if constexpr (std::is_same_v<T, JoinAlgo::Hash>) return "Hash Join       probe=" + alt.probe_col + " build=" + alt.build_col;
            else if constexpr (std::is_same_v<T, JoinAlgo::SortMerge>) return "Sort-Merge Join probe=" + alt.probe_col + " build=" + alt.build_col;
            else return "Index NL Join   probe=" + alt.probe_col + " pk=" + alt.right_pk_col;
        },
        jp.algo.data);
    std::ostringstream oss;
    // Matches Rust's `{:.0}` (round to nearest, not truncate) -- static_cast<long long>
    // truncates toward zero, which previously showed e.g. "25" for a cost of 25.6+
    // where Rust's formatting rounds up to "26".
    oss << algo << " \xE2\x86\x92 " << jp.right_table << "  cost\xE2\x89\x88" << std::llround(jp.est_cost);
    return oss.str();
}

std::string Planner::explain(const SelectPlan& plan) const {
    std::size_t total = table_size(plan.base.table);
    std::size_t visible = 0;
    if (auto it = tables_.find(plan.base.table); it != tables_.end()) {
        for (auto& row : it->second) {
            auto xit = row.find("_xmax");
            if (xit == row.end() || xit->second == "0") visible++;
        }
    }
    std::string pk = pk_col(plan.base.table).value_or("");

    std::ostringstream oss;
    oss << "+--------------------------------------------------------------------------+\n";
    oss << "|                            QUERY PLAN                                    |\n";
    oss << "+--------------------------------------------------------------------------+\n";
    oss << fmt_row("Table", plan.base.table);
    oss << fmt_row("Rows (total)", std::to_string(total));
    oss << fmt_row("Rows (visible)", std::to_string(visible));
    if (!pk.empty()) oss << fmt_row("PK", pk);
    {
        std::ostringstream cost_oss;
        cost_oss.precision(1);
        cost_oss << std::fixed << plan.total_cost();
        oss << fmt_row("Est. cost", cost_oss.str());
    }
    oss << "|                                                                          |\n";
    std::string access_label = plan.base.is_covering ? describe_access(plan.base.access) + " (Covering)" : describe_access(plan.base.access);
    oss << fmt_row("Access", access_label);
    for (auto& jp : plan.joins) oss << fmt_row("Join", describe_join(jp));
    oss << "+--------------------------------------------------------------------------+";
    return oss.str();
}

// ── Public helpers ───────────────────────────────────────────────────────────

std::optional<std::pair<std::string, std::string>> extract_equi_join_cols(const CondExpr& on_expr) {
    if (std::holds_alternative<CondExpr::Leaf>(on_expr.data)) {
        const Condition& cond = std::get<CondExpr::Leaf>(on_expr.data).condition;
        if (cond.op == Operator::Eq && std::holds_alternative<ArithExpr::Col>(cond.left.data) &&
            std::holds_alternative<ConditionValue::Literal>(cond.value.data)) {
            std::string probe = bare_col(std::get<ArithExpr::Col>(cond.left.data).name);
            std::string build = bare_col(std::get<ConditionValue::Literal>(cond.value.data).value);
            return std::make_pair(probe, build);
        }
    }
    return std::nullopt;
}

namespace {
void collect_eq_recursive(const CondExpr& expr, std::unordered_map<std::string, std::string>& map) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, CondExpr::And>) {
                collect_eq_recursive(*alt.lhs, map);
                collect_eq_recursive(*alt.rhs, map);
            } else if constexpr (std::is_same_v<T, CondExpr::Leaf>) {
                if (alt.condition.op == Operator::Eq && std::holds_alternative<ArithExpr::Col>(alt.condition.left.data) &&
                    std::holds_alternative<ConditionValue::Literal>(alt.condition.value.data)) {
                    std::string bare = bare_col(std::get<ArithExpr::Col>(alt.condition.left.data).name);
                    map[bare] = std::get<ConditionValue::Literal>(alt.condition.value.data).value;
                }
            }
        },
        expr.data);
}

void collect_and_leaves_rec(const CondExpr& expr, std::vector<const Condition*>& out) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, CondExpr::And>) {
                collect_and_leaves_rec(*alt.lhs, out);
                collect_and_leaves_rec(*alt.rhs, out);
            } else if constexpr (std::is_same_v<T, CondExpr::Leaf>) {
                out.push_back(&alt.condition);
            }
        },
        expr.data);
}
} // namespace

std::unordered_map<std::string, std::string> collect_eq_map(const CondExpr& expr) {
    std::unordered_map<std::string, std::string> map;
    collect_eq_recursive(expr, map);
    return map;
}

std::vector<const Condition*> collect_and_leaves(const CondExpr& expr) {
    std::vector<const Condition*> out;
    collect_and_leaves_rec(expr, out);
    return out;
}

void collect_table_refs_from_expr(const CondExpr& expr, std::unordered_set<std::string>& refs) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, CondExpr::And> || std::is_same_v<T, CondExpr::Or>) {
                collect_table_refs_from_expr(*alt.lhs, refs);
                collect_table_refs_from_expr(*alt.rhs, refs);
            } else if constexpr (std::is_same_v<T, CondExpr::Not>) {
                collect_table_refs_from_expr(*alt.inner, refs);
            } else {
                const Condition& cond = alt.condition;
                if (std::holds_alternative<ArithExpr::Col>(cond.left.data)) {
                    const std::string& c = std::get<ArithExpr::Col>(cond.left.data).name;
                    auto pos = c.rfind('.');
                    if (pos != std::string::npos) refs.insert(c.substr(0, pos));
                }
                if (std::holds_alternative<ConditionValue::Literal>(cond.value.data)) {
                    const std::string& v = std::get<ConditionValue::Literal>(cond.value.data).value;
                    auto pos = v.rfind('.');
                    if (pos != std::string::npos) {
                        std::string prefix = v.substr(0, pos);
                        if (!prefix.empty() && (std::isalpha(static_cast<unsigned char>(prefix[0])) || prefix[0] == '_')) refs.insert(prefix);
                    }
                }
            }
        },
        expr.data);
}

std::vector<Join> reorder_joins_dp(const std::string& base_table, std::vector<Join> joins,
                                    const std::unordered_map<std::string, std::vector<Row>>& tables) {
    std::size_t n = joins.size();
    if (n <= 1) return joins;
    for (auto& j : joins) {
        if (j.join_type != JoinType::Inner && j.join_type != JoinType::Natural) return joins;
    }
    if (n > 8) return reorder_joins_greedy(base_table, std::move(joins), tables);

    auto size_of = [&](const std::string& t) -> std::size_t {
        auto it = tables.find(t);
        return std::max<std::size_t>(it != tables.end() ? it->second.size() : 0, 1);
    };
    std::size_t base_card = size_of(base_table);
    std::size_t full = (std::size_t{1} << n) - 1;

    struct DpEntry {
        bool has = false;
        double cost = 0;
        std::size_t card = 0;
        std::vector<std::size_t> order;
    };
    std::vector<DpEntry> dp(std::size_t{1} << n);
    dp[0] = DpEntry{true, 0.0, base_card, {}};

    for (std::size_t mask = 0; mask < (std::size_t{1} << n); mask++) {
        if (!dp[mask].has) continue;
        double cur_cost = dp[mask].cost;
        std::size_t cur_card = dp[mask].card;
        const auto& order = dp[mask].order;

        std::unordered_set<std::string> available;
        available.insert(base_table);
        if (auto p = base_table.rfind('.'); p != std::string::npos) available.insert(base_table.substr(p + 1));
        for (auto k : order) {
            const std::string& t = joins[k].table;
            available.insert(t);
            if (auto p = t.rfind('.'); p != std::string::npos) available.insert(t.substr(p + 1));
        }

        for (std::size_t j = 0; j < n; j++) {
            if (mask & (std::size_t{1} << j)) continue;
            std::unordered_set<std::string> refs;
            collect_table_refs_from_expr(joins[j].on_expr, refs);
            std::string join_bare = bare_col(joins[j].table);
            bool joinable = std::all_of(refs.begin(), refs.end(), [&](const std::string& r) {
                std::string rb = bare_col(r);
                return available.count(r) || available.count(rb) || r == joins[j].table || rb == join_bare;
            });
            if (!joinable) continue;

            std::size_t rsize = size_of(joins[j].table);
            bool is_equi = std::holds_alternative<CondExpr::Leaf>(joins[j].on_expr.data) &&
                           std::get<CondExpr::Leaf>(joins[j].on_expr.data).condition.op == Operator::Eq;
            double step_cost;
            std::size_t new_card;
            if (is_equi) {
                step_cost = static_cast<double>(cur_card + rsize);
                new_card = std::max(cur_card, rsize);
            } else {
                step_cost = static_cast<double>(cur_card * rsize);
                new_card = cur_card * rsize;
            }
            double new_cost = cur_cost + step_cost;
            std::size_t nmask = mask | (std::size_t{1} << j);
            bool better = !dp[nmask].has || new_cost < dp[nmask].cost;
            if (better) {
                std::vector<std::size_t> no = order;
                no.push_back(j);
                dp[nmask] = DpEntry{true, new_cost, new_card, std::move(no)};
            }
        }
    }

    if (dp[full].has && dp[full].order.size() == n) {
        std::vector<Join> result;
        result.reserve(n);
        for (auto i : dp[full].order) result.push_back(joins[i]);
        return result;
    }
    return reorder_joins_greedy(base_table, std::move(joins), tables);
}

std::vector<Join> reorder_joins_greedy(const std::string& base_table, std::vector<Join> joins,
                                       const std::unordered_map<std::string, std::vector<Row>>& tables) {
    if (joins.size() <= 1) return joins;
    for (auto& j : joins) {
        if (j.join_type != JoinType::Inner && j.join_type != JoinType::Natural) return joins;
    }

    std::unordered_set<std::string> available;
    available.insert(base_table);
    if (auto pos = base_table.rfind('.'); pos != std::string::npos) available.insert(base_table.substr(pos + 1));

    std::vector<Join> remaining = std::move(joins);
    std::vector<Join> reordered;

    while (!remaining.empty()) {
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < remaining.size(); i++) {
            std::unordered_set<std::string> refs;
            collect_table_refs_from_expr(remaining[i].on_expr, refs);
            std::string join_bare = bare_col(remaining[i].table);
            bool joinable = std::all_of(refs.begin(), refs.end(), [&](const std::string& r) {
                std::string rb = bare_col(r);
                return available.count(r) || available.count(rb) || r == remaining[i].table || rb == join_bare;
            });
            if (joinable) candidates.push_back(i);
        }

        std::size_t best = 0;
        if (!candidates.empty()) {
            best = *std::min_element(candidates.begin(), candidates.end(), [&](std::size_t a, std::size_t b) {
                auto sz = [&](std::size_t i) {
                    auto it = tables.find(remaining[i].table);
                    return it != tables.end() ? it->second.size() : 0;
                };
                return sz(a) < sz(b);
            });
        }

        Join chosen = std::move(remaining[best]);
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(best));
        available.insert(chosen.table);
        if (auto pos = chosen.table.rfind('.'); pos != std::string::npos) available.insert(chosen.table.substr(pos + 1));
        reordered.push_back(std::move(chosen));
    }
    return reordered;
}

} // namespace engine
