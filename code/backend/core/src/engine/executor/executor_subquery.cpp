// Faithful port of the subquery-aware WHERE evaluation path from
// rusql-core/src/engine/executor.rs (Phase 8c): matches_condition_with_subquery,
// eval_condexpr_with_subquery, has_outer_ref, eval_single_with_subquery,
// extract_values_from_output.
//
// Cache-key deviation (documented, behavior-preserving): the Rust original keys its
// uncorrelated IN/NOT IN subquery cache with `format!("{:?}", sub_stmt)` (Debug output).
// This port uses the subquery Statement's JSON serialization instead — any stable,
// unique-per-distinct-subquery string works equally well as a cache key; the Debug
// text itself is never observable.

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
    auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data);
    if (!lit) return false;
    const std::string& s = lit->value;
    auto dot = s.find('.');
    if (dot == std::string::npos) return false;
    std::string a = s.substr(0, dot);
    std::string rest = s.substr(dot + 1);
    // splitn(2, '.') semantics: the second part keeps any further dots.
    auto is_ident = [](const std::string& p) {
        if (p.empty()) return false;
        if (!(std::isalpha(static_cast<unsigned char>(p[0])) || p[0] == '_')) return false;
        return std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    };
    return is_ident(a) && is_ident(rest);
}

bool Executor::eval_single_with_subquery(SharedDatabase& s, const Row& row, const Condition& cond) {
    if (std::holds_alternative<ConditionValue::Literal>(cond.value.data) || std::holds_alternative<ConditionValue::Between>(cond.value.data) ||
        std::holds_alternative<ConditionValue::LiteralList>(cond.value.data)) {
        return eval_single(row, cond);
    }

    auto* sub = std::get_if<ConditionValue::Subquery>(&cond.value.data);
    if (!sub) return false;
    Statement sub_stmt = *sub->query;

    if (cond.op == Operator::Exists || cond.op == Operator::NotExists) {
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

    std::string cache_key = nlohmann::json(sub_stmt).dump();

    if (cond.op == Operator::In || cond.op == Operator::NotIn) {
        if (auto* sel = std::get_if<Statement::Select>(&sub_stmt.data)) {
            bool is_correlated = sel->condition.has_value() && has_outer_ref(*sel->condition);
            if (!is_correlated) {
                if (auto it = subquery_cache_.find(cache_key); it != subquery_cache_.end()) {
                    bool contains = it->second.count(val) > 0;
                    return cond.op == Operator::In ? contains : !contains;
                }
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
