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
#include "engine/explain_trace.hpp"
#include "engine/result_text.hpp"
#include "engine/storage/numeric_key.hpp"

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

void Executor::sync_subquery_caches() {
    const std::uint64_t statement = statement_context_ ? statement_context_->statement : 0;
    if (statement == cache_statement_ && write_serial_ == cache_writes_) return;
    subquery_cache_.clear();
    subquery_scalar_cache_.clear();
    subquery_exists_cache_.clear();
    subquery_correlated_.clear();
    cache_statement_ = statement;
    cache_writes_ = write_serial_;
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
    explain_trace::SubqueryScope timing(correlated); // (EXPLAIN ANALYZE: the runs and the time of the subqueries)
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
    sync_subquery_caches();

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

    // (a, b) [NOT] IN (SELECT x, y ...): a row is in when every pair of its values is equal; a NULL pair is UNKNOWN unless another pair is FALSE, and
    // IN is TRUE for some row that is, else UNKNOWN when one was open, else FALSE (NOT IN the other way round; no row at all: IN FALSE, NOT IN TRUE)
    if (auto* row_left = std::get_if<ArithExpr::Func>(&cond.left.data); row_left && row_left->name == "ROW" && (cond.op == Operator::In || cond.op == Operator::NotIn)) {
        const std::size_t width = row_left->args.size();
        std::vector<std::string> mine;
        std::vector<ValueClass> classes;
        for (const ArithExpr& arg : row_left->args) {
            mine.push_back(eval_arith_exact(row, arg));
            classes.push_back(class_of_expr(arg));
        }
        const std::vector<std::string>* cells = nullptr;
        std::vector<std::string> fresh_cells;
        if (!correlated) {
            if (auto it = subquery_scalar_cache_.find(cache_key); it != subquery_scalar_cache_.end()) cells = &it->second.values;
        }
        if (!cells) {
            fresh_cells = extract_cells_from_output(run_subquery(s, original, row, correlated), width);
            if (!correlated) subquery_scalar_cache_[cache_key].values = fresh_cells;
            cells = &fresh_cells;
        }
        Tri in = Tri::False;
        for (std::size_t r = 0; r + width <= cells->size() && in != Tri::True; r += width) {
            Tri same = Tri::True;
            for (std::size_t i = 0; i < width && same != Tri::False; i++) {
                const std::string& other = (*cells)[r + i];
                if (mine[i] == EXECUTOR_NULL_VALUE || other == EXECUTOR_NULL_VALUE) same = Tri::Unknown;
                else if (compare_classed(classes[i], classes[i], mine[i], other) != 0) same = Tri::False;
            }
            if (same == Tri::True) in = Tri::True;
            else if (same == Tri::Unknown) in = Tri::Unknown;
        }
        if (cond.op == Operator::In) return in;
        return in == Tri::True ? Tri::False : (in == Tri::False ? Tri::True : Tri::Unknown);
    }

    std::string val = eval_arith_exact(row, cond.left);
    const bool left_null = val == EXECUTOR_NULL_VALUE;
    const bool is_in = cond.op == Operator::In || cond.op == Operator::NotIn;
    const bool quantified = cond.quantifier != Quantifier::None; // x op ANY / ALL (subquery): an empty answer decides it even for a NULL x
    if (left_null && !is_in && !quantified) return Tri::Unknown;

    // x IN (subquery): TRUE on a match, otherwise UNKNOWN when the subquery returned a NULL, else FALSE; NOT IN the other way round. A subquery that
    // gives no row decides it whatever x is (a NULL x is not IN nothing, and is NOT IN nothing); with rows a NULL x is UNKNOWN.
    auto membership = [&](bool contains, bool has_null, bool empty) {
        if (left_null) return empty ? (cond.op == Operator::In ? Tri::False : Tri::True) : Tri::Unknown;
        if (cond.op == Operator::In) return contains ? Tri::True : (has_null ? Tri::Unknown : Tri::False);
        return contains ? Tri::False : (has_null ? Tri::Unknown : Tri::True);
    };

    // the values of an IN list are equal when the comparison says so: numbers by value, texts by the default collation ('A' IN (SELECT 'a'))
    const ValueClass in_class = cond.left_class != ValueClass::Unknown ? cond.left_class : class_of_expr(cond.left);
    auto in_key = [&](const std::string& v) { return v == EXECUTOR_NULL_VALUE ? v : (in_class == ValueClass::Text ? collation::key(v) : normalize_numeric_key(v)); };
    auto in_equal = [&](const std::string& a, const std::string& b) {
        if (a == b) return true;
        return in_class == ValueClass::Text ? collation::equal(a, b) : compare_classed(in_class, in_class, a, b) == 0;
    };

    if ((cond.op == Operator::In || cond.op == Operator::NotIn) && !correlated) {
        // Row-level-concurrency Stage 4/5 correctness/perf fix (found via concurrent-reader stress testing): check the cache BEFORE copying
        // or serializing anything -- sub->query.get() is a stable identity for this subquery AST for as long as subquery_cache_ can possibly
        // still hold an entry for it (the cache is cleared at the start of every new top-level statement, and this same condition/AST is
        // reused unchanged across every row exec_select's caller scans). The OLD code did a full Statement copy + JSON serialization of the
        // subquery AST on EVERY row regardless of hit or miss -- for a scan of N rows that's O(N) AST copies/serializations just to compute
        // the key, dwarfing the O(1) hash lookup the cache was supposed to provide.
        if (auto it = subquery_cache_.find(cache_key); it != subquery_cache_.end()) {
            return membership(it->second.count(in_key(val)) > 0, it->second.count(EXECUTOR_NULL_VALUE) > 0, it->second.empty());
        }
        // Cache miss: only now pay for a copy (run_subquery) -- the original AST (still pointed to by `sub->query`, untouched) must survive
        // for the next row's lookup.
        auto vals = extract_values_from_output(run_subquery(s, original, row, false));
        std::unordered_set<std::string> sub_vals;
        for (const std::string& v : vals) sub_vals.insert(in_key(v));
        Tri hit = membership(sub_vals.count(in_key(val)) > 0, sub_vals.count(EXECUTOR_NULL_VALUE) > 0, sub_vals.empty());
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
            return membership(std::any_of(sub_vals.begin(), sub_vals.end(), [&](const std::string& v) { return v != EXECUTOR_NULL_VALUE && in_equal(v, val); }),
                              std::find(sub_vals.begin(), sub_vals.end(), EXECUTOR_NULL_VALUE) != sub_vals.end(), sub_vals.empty());
        case Operator::Eq:
        case Operator::Ne:
        case Operator::Gt:
        case Operator::Lt:
        case Operator::Gte:
        case Operator::Lte: {
            const ValueClass left_class = cond.left_class != ValueClass::Unknown ? cond.left_class : class_of_expr(cond.left);
            auto holds = [&](int c) {
                switch (cond.op) {
                    case Operator::Eq: return tri(c == 0);
                    case Operator::Ne: return tri(c != 0);
                    case Operator::Gt: return tri(c > 0);
                    case Operator::Lt: return tri(c < 0);
                    case Operator::Gte: return tri(c >= 0);
                    default: return tri(c <= 0);
                }
            };
            if (quantified) {
                // ANY: TRUE when some value satisfies it, else UNKNOWN when a NULL (of x or of the subquery) kept it open, else FALSE; ALL: the
                // other way round. Nothing at all is FALSE for ANY and TRUE for ALL, whatever x is.
                const bool any = cond.quantifier == Quantifier::Any;
                if (sub_vals.empty()) return tri(!any);
                bool open = false;
                for (const std::string& v : sub_vals) {
                    Tri t = Tri::Unknown;
                    if (!left_null && v != EXECUTOR_NULL_VALUE) t = holds(compare_classed(left_class, cond.right_class, val, v));
                    if (any && t == Tri::True) return Tri::True;
                    if (!any && t == Tri::False) return Tri::False;
                    if (t == Tri::Unknown) open = true;
                }
                return open ? Tri::Unknown : tri(!any);
            }
            // a scalar subquery that returns more than one row is an error (MySQL 1242); with no row, or a NULL, the comparison is UNKNOWN
            if (sub_vals.size() > 1) throw StatementError("Subquery returns more than 1 row");
            if (sub_vals.empty() || sub_vals.front() == EXECUTOR_NULL_VALUE) return Tri::Unknown;
            return holds(compare_classed(left_class, cond.right_class, val, sub_vals.front()));
        }
        default:
            return Tri::False;
    }
}

// The cells of every row of a result table, row after row, `width` to a row (a subquery that gives another number of columns is an error, MySQL 1241).
std::vector<std::string> Executor::extract_cells_from_output(const std::string& output, std::size_t width) const {
    std::vector<std::string> cells;
    int separators = 0;
    std::size_t pos = 0;
    while (pos <= output.size()) {
        const auto nl = output.find('\n', pos);
        const std::string line = nl == std::string::npos ? output.substr(pos) : output.substr(pos, nl - pos);
        if (!line.empty() && line[0] == '+') {
            separators++;
        } else if (!line.empty() && line[0] == '|' && separators >= 2) {
            std::vector<std::string> row = result_text::split_row(line);
            if (row.size() != width) throw StatementError("Operand should contain " + std::to_string(width) + " column(s)");
            for (auto& cell : row) cells.push_back(std::move(cell));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return cells;
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
            std::vector<std::string> row = result_text::split_row(line);
            vals.push_back(row.empty() ? std::string() : std::move(row.front())); // (an empty string is a value too)
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return vals;
}

} // namespace engine
