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
#include <chrono>

namespace engine {

namespace {
std::optional<double> parse_f64(const std::string& s) { return parse_number(s); }

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
bool cond_has_qualified_col(const CondExpr& expr);

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
            else if constexpr (std::is_same_v<T, ArithExpr::Pred>)
                return cond_has_qualified_col(*alt.cond);
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

bool cond_has_qualified_col(const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) return cond_has_qualified_col(*v->lhs) || cond_has_qualified_col(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) return cond_has_qualified_col(*v->lhs) || cond_has_qualified_col(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) return cond_has_qualified_col(*v->inner);
    auto* leaf = std::get_if<CondExpr::Leaf>(&expr.data);
    if (!leaf) return false;
    if (arith_has_qualified_col(leaf->condition.left)) return true;
    if (auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) return looks_like_qualified_col(lit->value);
    if (auto* ar = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) return arith_has_qualified_col(ar->expr);
    return false;
}

bool cond_has_dotted_col(const CondExpr& expr);

// Can substitute_correlated_condexpr change this condition for some outer row? It replaces a literal that contains a dot and
// a column reference with a dot by the outer row's value, so a condition with neither is the same for every row and the
// subquery that carries it has one answer per statement. (A number such as 1.5 contains a dot but never names a column.)
bool arith_has_dotted_col(const ArithExpr& expr) {
    return std::visit(
        [](const auto& alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ArithExpr::Col>) return alt.name.find('.') != std::string::npos;
            else if constexpr (std::is_same_v<T, ArithExpr::Pred>) return cond_has_dotted_col(*alt.cond);
            else if constexpr (std::is_same_v<T, ArithExpr::Add> || std::is_same_v<T, ArithExpr::Sub> || std::is_same_v<T, ArithExpr::Mul> ||
                                std::is_same_v<T, ArithExpr::Div> || std::is_same_v<T, ArithExpr::Cmp>)
                return arith_has_dotted_col(*alt.lhs) || arith_has_dotted_col(*alt.rhs);
            else if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                for (auto& a : alt.args) {
                    if (arith_has_dotted_col(a)) return true;
                }
                return false;
            } else
                return false;
        },
        expr.data);
}

bool cond_has_dotted_col(const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) return cond_has_dotted_col(*v->lhs) || cond_has_dotted_col(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) return cond_has_dotted_col(*v->lhs) || cond_has_dotted_col(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) return cond_has_dotted_col(*v->inner);
    auto* leaf = std::get_if<CondExpr::Leaf>(&expr.data);
    if (!leaf) return false;
    if (arith_has_dotted_col(leaf->condition.left)) return true;
    if (auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
        return lit->value.find('.') != std::string::npos && !parse_f64(lit->value).has_value();
    }
    if (auto* ar = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) return arith_has_dotted_col(ar->expr);
    return false;
}

bool cond_may_be_substituted(const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) return cond_may_be_substituted(*v->lhs) || cond_may_be_substituted(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) return cond_may_be_substituted(*v->lhs) || cond_may_be_substituted(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) return cond_may_be_substituted(*v->inner);
    auto* leaf = std::get_if<CondExpr::Leaf>(&expr.data);
    if (!leaf) return false;
    if (auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
        return lit->value.find('.') != std::string::npos && !parse_f64(lit->value).has_value();
    }
    if (auto* ar = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) return arith_has_dotted_col(ar->expr);
    return false;
}
} // namespace

bool Executor::matches_condition_with_subquery(SharedDatabase& s, const Row& row, const std::optional<CondExpr>& condition) {
    return !condition || eval_condexpr_with_subquery(s, row, *condition);
}

bool Executor::eval_condexpr_with_subquery(SharedDatabase& s, const Row& row, const CondExpr& expr) {
    return eval_cond3_with_subquery(s, row, expr) == Tri::True;
}

Executor::Tri Executor::eval_cond3_with_subquery(SharedDatabase& s, const Row& row, const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) {
        Tri l = eval_cond3_with_subquery(s, row, *v->lhs);
        if (l == Tri::False) return Tri::False;
        Tri r = eval_cond3_with_subquery(s, row, *v->rhs);
        if (r == Tri::False) return Tri::False;
        return (l == Tri::True && r == Tri::True) ? Tri::True : Tri::Unknown;
    }
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) {
        Tri l = eval_cond3_with_subquery(s, row, *v->lhs);
        if (l == Tri::True) return Tri::True;
        Tri r = eval_cond3_with_subquery(s, row, *v->rhs);
        if (r == Tri::True) return Tri::True;
        return (l == Tri::False && r == Tri::False) ? Tri::False : Tri::Unknown;
    }
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) {
        Tri i = eval_cond3_with_subquery(s, row, *v->inner);
        return i == Tri::True ? Tri::False : i == Tri::False ? Tri::True : Tri::Unknown;
    }
    if (auto* v = std::get_if<CondExpr::Leaf>(&expr.data)) return eval_single_with_subquery(s, row, v->condition);
    return Tri::False;
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

Executor::Tri Executor::eval_single_with_subquery(SharedDatabase& s, const Row& row, const Condition& cond) {
    auto tri = [](bool b) { return b ? Tri::True : Tri::False; };
    if (std::holds_alternative<ConditionValue::Literal>(cond.value.data) || std::holds_alternative<ConditionValue::Between>(cond.value.data) ||
        std::holds_alternative<ConditionValue::LiteralList>(cond.value.data) || std::holds_alternative<ConditionValue::Arith>(cond.value.data)) {
        return eval_single3(row, cond);
    }

    auto* sub = std::get_if<ConditionValue::Subquery>(&cond.value.data);
    if (!sub) return Tri::False;

    if (cond.op == Operator::Exists || cond.op == Operator::NotExists) {
        // An EXISTS whose condition cannot depend on the outer row has one answer for the whole statement.
        const void* exists_key = sub->query.get();
        auto* peek = std::get_if<Statement::Select>(&sub->query->data);
        if (peek && !(peek->condition && cond_may_be_substituted(*peek->condition))) {
            if (auto it = subquery_exists_cache_.find(exists_key); it != subquery_exists_cache_.end()) {
                return tri(cond.op == Operator::Exists ? it->second : !it->second);
            }
        }
        Statement sub_stmt = *sub->query;
        if (auto* sel = std::get_if<Statement::Select>(&sub_stmt.data)) {
            const bool cacheable = !(sel->condition && cond_may_be_substituted(*sel->condition));
            auto sub_cond = sel->condition;
            if (sub_cond) sub_cond = substitute_correlated_condexpr(*sub_cond, row);
            auto result = exec_select(s, sel->table, std::move(sel->subquery), sel->distinct, sel->columns, sub_cond, sel->joins, sel->order_by,
                                       sel->group_by, sel->having, sel->limit, sel->offset, false, false);
            if (result.is_err()) throw StatementError(result.error()); // (an error inside the subquery is the statement's error, not "no row")
            bool has_rows = result.value().find("0 rows returned") == std::string::npos;
            if (cacheable) subquery_exists_cache_[exists_key] = has_rows;
            return tri(cond.op == Operator::Exists ? has_rows : !has_rows);
        }
        return Tri::False;
    }

    std::string val = eval_arith(row, cond.left);
    if (val == EXECUTOR_NULL_VALUE) return Tri::Unknown;

    // x IN (subquery): TRUE on a match, otherwise UNKNOWN when the subquery returned a NULL, else FALSE; NOT IN the other way round
    auto membership = [&](bool contains, bool has_null) {
        if (cond.op == Operator::In) return contains ? Tri::True : (has_null ? Tri::Unknown : Tri::False);
        return contains ? Tri::False : (has_null ? Tri::Unknown : Tri::True);
    };

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
                    return membership(it->second.count(val) > 0, it->second.count(EXECUTOR_NULL_VALUE) > 0);
                }
                // Cache miss: only now pay for a copy -- exec_select needs to move
                // fields out of it (sel->subquery), and the original AST (still pointed
                // to by `sub->query`, untouched) must survive for the next row's lookup.
                Statement sub_stmt = *sub->query;
                auto* sel = std::get_if<Statement::Select>(&sub_stmt.data);
                auto result = exec_select(s, sel->table, std::move(sel->subquery), sel->distinct, sel->columns, sel->condition, sel->joins,
                                           sel->order_by, sel->group_by, sel->having, sel->limit, sel->offset, false, false);
                if (result.is_err()) throw StatementError(result.error());
                auto vals = extract_values_from_output(result.value());
                std::unordered_set<std::string> sub_vals(vals.begin(), vals.end());
                Tri hit = membership(sub_vals.count(val) > 0, sub_vals.count(EXECUTOR_NULL_VALUE) > 0);
                subquery_cache_[cache_key] = std::move(sub_vals);
                return hit;
            }
        }
    }

    // Correlated IN/NOT IN, and every other (scalar Eq/Gt/Lt/Gte/Lte) operator, fall
    // through here. A correlated subquery needs a fresh per-row copy, since
    // substitute_correlated_condexpr's result varies per row and exec_select moves
    // fields out of it. One whose condition cannot depend on the outer row (the scalar
    // `val > (SELECT AVG(val) FROM t)`) runs once per statement and its values are kept.
    const SubqueryAnswer* cached_answer = nullptr;
    auto* peek = std::get_if<Statement::Select>(&sub->query->data);
    if (peek && !(peek->condition && cond_may_be_substituted(*peek->condition))) {
        if (auto it = subquery_scalar_cache_.find(sub->query.get()); it != subquery_scalar_cache_.end()) cached_answer = &it->second;
    }
    Statement sub_stmt = cached_answer ? Statement() : *sub->query;
    auto* sel = std::get_if<Statement::Select>(&sub_stmt.data);
    if (cached_answer || sel) {
        std::vector<std::string> fresh_vals;
        const std::vector<std::string>* sub_vals_ptr = nullptr;
        if (cached_answer) {
            sub_vals_ptr = &cached_answer->values;
        } else {
            const bool cacheable = !(sel->condition && cond_may_be_substituted(*sel->condition));
            auto sub_cond = sel->condition;
            if (sub_cond) sub_cond = substitute_correlated_condexpr(*sub_cond, row);
            auto result = exec_select(s, sel->table, std::move(sel->subquery), sel->distinct, sel->columns, sub_cond, sel->joins, sel->order_by,
                                       sel->group_by, sel->having, sel->limit, sel->offset, false, false);
            if (result.is_err()) throw StatementError(result.error());
            fresh_vals = extract_values_from_output(result.value());
            if (cacheable) subquery_scalar_cache_[sub->query.get()].values = fresh_vals;
            sub_vals_ptr = &fresh_vals;
        }
        const std::vector<std::string>& sub_vals = *sub_vals_ptr;
        switch (cond.op) {
            case Operator::In:
            case Operator::NotIn:
                return membership(std::find(sub_vals.begin(), sub_vals.end(), val) != sub_vals.end(),
                                  std::find(sub_vals.begin(), sub_vals.end(), EXECUTOR_NULL_VALUE) != sub_vals.end());
            case Operator::Eq:
            case Operator::Ne:
            case Operator::Gt:
            case Operator::Lt:
            case Operator::Gte:
            case Operator::Lte: {
                // a scalar subquery that returns more than one row is an error (MySQL 1242); with no row, or a NULL, the comparison is UNKNOWN
                if (sub_vals.size() > 1) throw StatementError("Subquery returns more than 1 row");
                if (sub_vals.empty() || sub_vals.front() == EXECUTOR_NULL_VALUE) return Tri::Unknown;
                const std::string& rhs = sub_vals.front();
                const int c = compare_classed(cond.left_class != ValueClass::Unknown ? cond.left_class : class_of_expr(cond.left), cond.right_class, val, rhs);
                switch (cond.op) {
                    case Operator::Eq: return tri(c == 0);
                    case Operator::Ne: return tri(c != 0);
                    case Operator::Gt: return tri(c > 0);
                    case Operator::Lt: return tri(c < 0);
                    case Operator::Gte: return tri(c >= 0);
                    default: return tri(c <= 0);
                }
            }
            default:
                return Tri::False;
        }
    }
    return Tri::False;
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
                vals.push_back(first_cell.substr(a, b - a + 1));
            } else {
                vals.push_back(""); // an empty string is a value (it used to be dropped)
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return vals;
}

} // namespace engine
