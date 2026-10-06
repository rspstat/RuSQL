// Faithful port of the subquery-aware WHERE evaluation path from
// rusql-core/src/engine/executor.rs (Phase 8c): matches_condition_with_subquery,
// eval_condexpr_with_subquery, eval_single_with_subquery, extract_values_from_output.
//
// Cache-key deviation (documented, behavior-preserving): the Rust original keys its
// uncorrelated IN/NOT IN subquery cache with `format!("{:?}", sub_stmt)` (Debug output).
// This port keys subquery_cache_ (executor.hpp) by the subquery AST's own address
// instead -- any identity that's stable and unique for as long as a cache entry could
// possibly be looked up works equally well, and the address is dramatically cheaper
// than formatting/serializing the whole AST on every row (see eval_single_with_subquery,
// Row-level-concurrency Stage 4/5 perf fix).
//
// Which subqueries are correlated (answered once per outer row) is not guessed from the text: the binder marks the references to a column of
// an enclosing query and executor_outer.cpp puts the outer row's values in.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>

namespace engine {

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

bool Executor::subquery_is_correlated(const Statement& sub) {
    if (auto it = subquery_correlated_.find(&sub); it != subquery_correlated_.end()) return it->second;
    const bool correlated = refers_outside(sub);
    subquery_correlated_[&sub] = correlated;
    return correlated;
}

// Runs a subquery for one outer row: on a copy (the original stays as parsed, for the next row), with the row's values in place of the columns it
// takes from the outer query. An error inside it is the statement's error, not "no row".
std::string Executor::run_subquery(SharedDatabase& s, const Statement& original, const Row& row, bool correlated) {
    Statement stmt = original;
    if (correlated) substitute_outer(stmt, row);
    CopyScope scope(*this);
    auto* sel = std::get_if<Statement::Select>(&stmt.data);
    StringResult result = sel ? exec_select(s, sel->table, std::move(sel->subquery), sel->distinct, std::move(sel->columns), std::move(sel->condition),
                                             std::move(sel->joins), std::move(sel->order_by), std::move(sel->group_by), std::move(sel->having),
                                             sel->limit, sel->offset, false, false)
                              : execute_with_s(s, std::move(stmt)); // (a UNION / INTERSECT / EXCEPT / WITH)
    if (result.is_err()) throw StatementError(result.error());
    return result.value();
}

Executor::Tri Executor::eval_single_with_subquery(SharedDatabase& s, const Row& row, const Condition& cond) {
    auto tri = [](bool b) { return b ? Tri::True : Tri::False; };
    if (std::holds_alternative<ConditionValue::Literal>(cond.value.data) || std::holds_alternative<ConditionValue::Between>(cond.value.data) ||
        std::holds_alternative<ConditionValue::LiteralList>(cond.value.data) || std::holds_alternative<ConditionValue::Arith>(cond.value.data)) {
        return eval_single3(row, cond);
    }

    auto* sub = std::get_if<ConditionValue::Subquery>(&cond.value.data);
    if (!sub) return Tri::False;

    // One that names no column of the outer query has one answer for the whole statement.
    const Statement& original = *sub->query;
    const bool correlated = subquery_is_correlated(original);
    const void* cache_key = sub->query.get();

    if (cond.op == Operator::Exists || cond.op == Operator::NotExists) {
        if (!correlated) {
            if (auto it = subquery_exists_cache_.find(cache_key); it != subquery_exists_cache_.end()) {
                return tri(cond.op == Operator::Exists ? it->second : !it->second);
            }
        }
        const bool has_rows = run_subquery(s, original, row, correlated).find("0 rows returned") == std::string::npos;
        if (!correlated) subquery_exists_cache_[cache_key] = has_rows;
        return tri(cond.op == Operator::Exists ? has_rows : !has_rows);
    }

    std::string val = eval_arith(row, cond.left);
    if (val == EXECUTOR_NULL_VALUE) return Tri::Unknown;

    // x IN (subquery): TRUE on a match, otherwise UNKNOWN when the subquery returned a NULL, else FALSE; NOT IN the other way round
    auto membership = [&](bool contains, bool has_null) {
        if (cond.op == Operator::In) return contains ? Tri::True : (has_null ? Tri::Unknown : Tri::False);
        return contains ? Tri::False : (has_null ? Tri::Unknown : Tri::True);
    };

    if ((cond.op == Operator::In || cond.op == Operator::NotIn) && !correlated) {
        // Row-level-concurrency Stage 4/5 correctness/perf fix (found via concurrent-reader stress testing): check the cache BEFORE copying
        // or serializing anything -- sub->query.get() is a stable identity for this subquery AST for as long as subquery_cache_ can possibly
        // still hold an entry for it (the cache is cleared at the start of every new top-level statement, and this same condition/AST is
        // reused unchanged across every row exec_select's caller scans). The OLD code did a full Statement copy + JSON serialization of the
        // subquery AST on EVERY row regardless of hit or miss -- for a scan of N rows that's O(N) AST copies/serializations just to compute
        // the key, dwarfing the O(1) hash lookup the cache was supposed to provide.
        if (auto it = subquery_cache_.find(cache_key); it != subquery_cache_.end()) {
            return membership(it->second.count(val) > 0, it->second.count(EXECUTOR_NULL_VALUE) > 0);
        }
        // Cache miss: only now pay for a copy (run_subquery) -- the original AST (still pointed to by `sub->query`, untouched) must survive
        // for the next row's lookup.
        auto vals = extract_values_from_output(run_subquery(s, original, row, false));
        std::unordered_set<std::string> sub_vals(vals.begin(), vals.end());
        Tri hit = membership(sub_vals.count(val) > 0, sub_vals.count(EXECUTOR_NULL_VALUE) > 0);
        subquery_cache_[cache_key] = std::move(sub_vals);
        return hit;
    }

    // Correlated IN/NOT IN, and every other (scalar Eq/Gt/Lt/Gte/Lte) operator, fall through here. A correlated subquery runs once per outer
    // row; one that names no column of the outer query (the scalar `val > (SELECT AVG(val) FROM t)`) runs once per statement and its
    // values are kept.
    const SubqueryAnswer* cached_answer = nullptr;
    if (!correlated) {
        if (auto it = subquery_scalar_cache_.find(cache_key); it != subquery_scalar_cache_.end()) cached_answer = &it->second;
    }
    std::vector<std::string> fresh_vals;
    if (!cached_answer) {
        fresh_vals = extract_values_from_output(run_subquery(s, original, row, correlated));
        if (!correlated) subquery_scalar_cache_[cache_key].values = fresh_vals;
    }
    const std::vector<std::string>& sub_vals = cached_answer ? cached_answer->values : fresh_vals;
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
