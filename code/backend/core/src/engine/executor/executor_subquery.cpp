// Faithful port of the subquery-aware WHERE evaluation path from
// rusql-core/src/engine/executor.rs (Phase 8c): matches_condition_with_subquery,
// eval_condexpr_with_subquery, has_outer_ref, eval_single_with_subquery,
// extract_values_from_output.
//
// Cache-key deviation (documented, behavior-preserving): the Rust original keys its
// uncorrelated IN/NOT IN subquery cache with `format!("{:?}", sub_stmt)` (Debug output).
// This port keys subquery_cache_ (executor.hpp) by the subquery AST's own address
// instead -- any identity that's stable and unique for as long as a cache entry could
// possibly be looked up works equally well, and the address is dramatically cheaper
// than formatting/serializing the whole AST on every row (see eval_single_with_subquery,
// Row-level-concurrency Stage 4/5 perf fix).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <charconv>

namespace engine {

namespace {
std::optional<double> parse_f64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    double val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return val;
}

bool looks_like_qualified_col(const std::string& s) {
    auto dot = s.find('.');
    if (dot == std::string::npos) return false;
    std::string a = s.substr(0, dot);
    std::string rest = s.substr(dot + 1);
    auto is_ident = [](const std::string& p) {
        if (p.empty()) return false;
        if (!(std::isalpha(static_cast<unsigned char>(p[0])) || p[0] == '_')) return false;
        return std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    };
    return is_ident(a) && is_ident(rest);
}

// PLAN.md P0 fix follow-up: ConditionValue::Arith's RHS is now a full expression tree
// (see the WHERE-RHS-arithmetic fix), so a qualified outer-table reference like
// `p.lead_id = employee.id` can appear nested inside it (or, in the simple case with
// no operators at all, be the whole tree) rather than as a bare Literal. Walk the tree
// to preserve has_outer_ref's original Literal-based correlation heuristic.
bool arith_has_qualified_col(const ArithExpr& expr) {
    return std::visit(
        [](const auto& alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ArithExpr::Col>) return looks_like_qualified_col(alt.name);
            else if constexpr (std::is_same_v<T, ArithExpr::Add> || std::is_same_v<T, ArithExpr::Sub> ||
                                std::is_same_v<T, ArithExpr::Mul> || std::is_same_v<T, ArithExpr::Div>)
                return arith_has_qualified_col(*alt.lhs) || arith_has_qualified_col(*alt.rhs);
            else if constexpr (std::is_same_v<T, ArithExpr::Cmp>)
                return arith_has_qualified_col(*alt.lhs) || arith_has_qualified_col(*alt.rhs);
            else if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                for (auto& a : alt.args) {
                    if (arith_has_qualified_col(a)) return true;
                }
                return false;
            } else
                return false;
        },
        expr.data);
}
} // namespace

bool Executor::matches_condition_with_subquery(SharedDatabase& s, const Row& row, const std::optional<CondExpr>& condition) {
    return !condition || eval_condexpr_with_subquery(s, row, *condition);
}

bool Executor::eval_condexpr_with_subquery(SharedDatabase& s, const Row& row, const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) return eval_condexpr_with_subquery(s, row, *v->lhs) && eval_condexpr_with_subquery(s, row, *v->rhs);
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) return eval_condexpr_with_subquery(s, row, *v->lhs) || eval_condexpr_with_subquery(s, row, *v->rhs);
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) return !eval_condexpr_with_subquery(s, row, *v->inner);
    if (auto* v = std::get_if<CondExpr::Leaf>(&expr.data)) return eval_single_with_subquery(s, row, v->condition);
    return false;
}

bool Executor::has_outer_ref(const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) return has_outer_ref(*v->lhs) || has_outer_ref(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) return has_outer_ref(*v->lhs) || has_outer_ref(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) return has_outer_ref(*v->inner);
    auto* leaf = std::get_if<CondExpr::Leaf>(&expr.data);
    if (!leaf) return false;
    if (auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
        return looks_like_qualified_col(lit->value);
    }
    if (auto* ar = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) {
        return arith_has_qualified_col(ar->expr);
    }
    return false;
}

bool Executor::eval_single_with_subquery(SharedDatabase& s, const Row& row, const Condition& cond) {
    if (std::holds_alternative<ConditionValue::Literal>(cond.value.data) || std::holds_alternative<ConditionValue::Between>(cond.value.data) ||
        std::holds_alternative<ConditionValue::LiteralList>(cond.value.data) || std::holds_alternative<ConditionValue::Arith>(cond.value.data)) {
        return eval_single(row, cond);
    }

    auto* sub = std::get_if<ConditionValue::Subquery>(&cond.value.data);
    if (!sub) return false;

    if (cond.op == Operator::Exists || cond.op == Operator::NotExists) {
        Statement sub_stmt = *sub->query;
        if (auto* sel = std::get_if<Statement::Select>(&sub_stmt.data)) {
            auto sub_cond = sel->condition;
            if (sub_cond) sub_cond = substitute_correlated_condexpr(*sub_cond, row);
            auto result = exec_select(s, sel->table, std::move(sel->subquery), sel->distinct, sel->columns, sub_cond, sel->joins, sel->order_by,
                                       sel->group_by, sel->having, sel->limit, sel->offset, false, false);
            bool has_rows = result.is_ok() && result.value().find("0 rows returned") == std::string::npos;
            return cond.op == Operator::Exists ? has_rows : !has_rows;
        }
        return false;
    }

    std::string val = eval_arith(row, cond.left);
    if (val == EXECUTOR_NULL_VALUE) return false;

    if (cond.op == Operator::In || cond.op == Operator::NotIn) {
        if (auto* sel_peek = std::get_if<Statement::Select>(&sub->query->data)) {
            bool is_correlated = sel_peek->condition.has_value() && has_outer_ref(*sel_peek->condition);
            if (!is_correlated) {
                // Row-level-concurrency Stage 4/5 correctness/perf fix (found via
                // concurrent-reader stress testing): check the cache BEFORE copying or
                // serializing anything -- sub->query.get() is a stable identity for this
                // subquery AST for as long as subquery_cache_ can possibly still hold an
                // entry for it (the cache is cleared at the start of every new top-level
                // statement, and this same condition/AST is reused unchanged across every
                // row exec_select's caller scans). The OLD code did a full Statement copy
                // + JSON serialization of the subquery AST on EVERY row regardless of hit
                // or miss (the cache only ever saved the exec_select call itself) -- for a
                // scan of N rows that's O(N) AST copies/serializations just to compute the
                // key, dwarfing the O(1) hash lookup the cache was supposed to provide.
                const void* cache_key = sub->query.get();
                if (auto it = subquery_cache_.find(cache_key); it != subquery_cache_.end()) {
                    bool contains = it->second.count(val) > 0;
                    return cond.op == Operator::In ? contains : !contains;
                }
                // Cache miss: only now pay for a copy -- exec_select needs to move
                // fields out of it (sel->subquery), and the original AST (still pointed
                // to by `sub->query`, untouched) must survive for the next row's lookup.
                Statement sub_stmt = *sub->query;
                auto* sel = std::get_if<Statement::Select>(&sub_stmt.data);
                auto result = exec_select(s, sel->table, std::move(sel->subquery), sel->distinct, sel->columns, sel->condition, sel->joins,
                                           sel->order_by, sel->group_by, sel->having, sel->limit, sel->offset, false, false);
                if (result.is_ok()) {
                    auto vals = extract_values_from_output(result.value());
                    std::unordered_set<std::string> sub_vals(vals.begin(), vals.end());
                    bool contains = sub_vals.count(val) > 0;
                    bool hit = cond.op == Operator::In ? contains : !contains;
                    subquery_cache_[cache_key] = std::move(sub_vals);
                    return hit;
                }
                return false;
            }
        }
    }

    // Correlated IN/NOT IN, and every other (scalar Eq/Gt/Lt/Gte/Lte) operator, fall
    // through here -- always need a fresh per-row copy regardless of caching, since
    // substitute_correlated_condexpr's result varies per row and exec_select moves
    // fields out of it.
    Statement sub_stmt = *sub->query;
    if (auto* sel = std::get_if<Statement::Select>(&sub_stmt.data)) {
        auto sub_cond = sel->condition;
        if (sub_cond) sub_cond = substitute_correlated_condexpr(*sub_cond, row);
        auto result = exec_select(s, sel->table, std::move(sel->subquery), sel->distinct, sel->columns, sub_cond, sel->joins, sel->order_by,
                                   sel->group_by, sel->having, sel->limit, sel->offset, false, false);
        if (!result.is_ok()) return false;
        auto sub_vals = extract_values_from_output(result.value());
        switch (cond.op) {
            case Operator::In:
                return std::find(sub_vals.begin(), sub_vals.end(), val) != sub_vals.end();
            case Operator::NotIn:
                return std::find(sub_vals.begin(), sub_vals.end(), val) == sub_vals.end();
            case Operator::Eq: {
                if (sub_vals.empty()) return false;
                auto a = parse_f64(val), b = parse_f64(sub_vals.front());
                return (a && b) ? (*a == *b) : (sub_vals.front() == val);
            }
            case Operator::Gt:
            case Operator::Lt:
            case Operator::Gte:
            case Operator::Lte: {
                if (sub_vals.empty()) return false;
                double a = parse_f64(val).value_or(0.0);
                double b = parse_f64(sub_vals.front()).value_or(0.0);
                switch (cond.op) {
                    case Operator::Gt: return a > b;
                    case Operator::Lt: return a < b;
                    case Operator::Gte: return a >= b;
                    case Operator::Lte: return a <= b;
                    default: return false;
                }
            }
            default:
                return false;
        }
    }
    return false;
}

std::vector<std::string> Executor::extract_values_from_output(const std::string& output) const {
    std::vector<std::string> vals;
    bool header_passed = false;
    int separator_count = 0;

    std::size_t pos = 0;
    while (pos <= output.size()) {
        auto nl = output.find('\n', pos);
        std::string line = nl == std::string::npos ? output.substr(pos) : output.substr(pos, nl - pos);
        if (!line.empty() && line[0] == '+') {
            separator_count++;
            if (separator_count == 2) header_passed = true;
        } else if (!line.empty() && line[0] == '|' && header_passed) {
            std::size_t start = 1;
            auto bar = line.find('|', start);
            std::string first_cell = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            auto a = first_cell.find_first_not_of(' ');
            if (a != std::string::npos) {
                auto b = first_cell.find_last_not_of(' ');
                std::string v = first_cell.substr(a, b - a + 1);
                if (!v.empty()) vals.push_back(v);
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return vals;
}

} // namespace engine
