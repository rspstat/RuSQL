// Faithful port of the expression/condition evaluation helpers from
// rusql-core/src/engine/executor.rs (Phase 8b): get_col, eval_arith,
// format_arith_result, matches_condexpr/eval_condexpr/eval_single, eval_check_expr,
// substitute_correlated_condexpr, format_returning_rows, update_stat_rows, and
// parse_table_output. apply_scalar_func is implemented in executor_scalar_func.cpp.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string_view>

#include "engine/numeric_text.hpp"
#include "engine/parser/parser.hpp"

namespace engine {

namespace {

// `lv op rv` ('+', '-' or '*') when both texts are integers and the result fits: an integer, exact (a double would round above 2^53).
std::optional<std::string> exact_int_text(char op, const std::string& lv, const std::string& rv) {
    auto a = parse_int64_text(lv), b = parse_int64_text(rv);
    if (!a || !b) return std::nullopt;
    if (auto result = int64_arith(op, *a, *b)) return std::to_string(*result);
    return std::nullopt;
}

std::optional<double> parse_f64(const std::string& s) { return parse_number(s); }

// What a value written without quotes in an IN list or after BETWEEN is: a number, or (a name, @variable) not told.
ValueClass written_class(const std::string& value, bool quoted) { return quoted ? ValueClass::Text : (parse_number(value) ? ValueClass::Number : ValueClass::Unknown); }

bool like_match(std::string_view val, std::string_view pat) {
    if (pat.empty()) return val.empty();
    if (val.empty()) {
        if (pat.front() == '%') return like_match(val, pat.substr(1));
        return false;
    }
    if (pat.front() == '%') return like_match(val.substr(1), pat) || like_match(val, pat.substr(1));
    if (pat.front() == '_') return like_match(val.substr(1), pat.substr(1));
    return val.front() == pat.front() && like_match(val.substr(1), pat.substr(1));
}

} // namespace

ValueClass Executor::class_of_expr(const ArithExpr& expr) {
    if (auto* col = std::get_if<ArithExpr::Col>(&expr.data)) return col->cls;
    if (std::holds_alternative<ArithExpr::Str>(expr.data)) return ValueClass::Text;
    if (auto* f = std::get_if<ArithExpr::Func>(&expr.data)) {
        // what a function of several values holds is what all of them hold (a NULL says nothing); values of different kinds, or not known, are not known
        auto common = [](const std::vector<const ArithExpr*>& values) {
            ValueClass kind = ValueClass::Unknown;
            for (const ArithExpr* value : values) {
                if (auto* str = std::get_if<ArithExpr::Str>(&value->data); str && str->value == EXECUTOR_NULL_VALUE) continue;
                const ValueClass k = class_of_expr(*value);
                if (k == ValueClass::Unknown || (kind != ValueClass::Unknown && k != kind)) return ValueClass::Unknown;
                kind = k;
            }
            return kind;
        };
        std::vector<const ArithExpr*> values;
        if (f->name == "CASE") { // its results
            for (std::size_t i = 1; i < f->args.size(); i += 2) values.push_back(&f->args[i]);
            if (f->args.size() % 2 == 1) values.push_back(&f->args.back());
            return common(values);
        }
        if (f->name == "COALESCE" || f->name == "IFNULL" || f->name == "GREATEST" || f->name == "LEAST") {
            for (auto& a : f->args) values.push_back(&a);
            return common(values);
        }
        if (f->name == "NULLIF" && !f->args.empty()) return class_of_expr(f->args[0]);
        return function_result_class(f->name);
    }
    return ValueClass::Number; // a number, + - * /, a comparison, a condition
}

const std::string* Executor::get_col(const Row& row, const std::string& col) {
    if (auto it = row.find(col); it != row.end()) return &it->second;

    if (auto dot = col.rfind('.'); dot != std::string::npos) {
        std::string suffix = "." + col.substr(0, dot) + "." + col.substr(dot + 1);
        for (auto& [k, v] : row) {
            if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) return &v;
        }
        std::string col_part = col.substr(dot + 1);
        auto it2 = row.find(col_part);
        return it2 != row.end() ? &it2->second : nullptr;
    }

    std::string suffix = "." + col;
    const std::string* found = nullptr;
    int count = 0;
    for (auto& [k, v] : row) {
        if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found = &v;
            if (++count > 1) return nullptr;
        }
    }
    return count == 1 ? found : nullptr;
}

std::string Executor::format_arith_result(double f) {
    double frac = f - std::trunc(f);
    if (std::abs(frac) < 1e-9 && std::abs(f) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(f));
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << f;
    std::string s = oss.str();
    auto last = s.find_last_not_of('0');
    s.erase(last + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// apply_scalar_func is implemented in executor_scalar_func.cpp.

std::string Executor::eval_arith(const Row& row, const ArithExpr& expr) {
    if (auto* v = std::get_if<ArithExpr::Col>(&expr.data)) {
        std::string lower = v->name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower == "true") return "true";
        if (lower == "false") return "false";
        if (const std::string* val = get_col(row, v->name)) return *val;
        return EXECUTOR_NULL_VALUE;
    }
    if (auto* v = std::get_if<ArithExpr::Num>(&expr.data)) return v->value;
    if (auto* v = std::get_if<ArithExpr::Str>(&expr.data)) return v->value;
    // NULL in, NULL out: `NULL + 1` is NULL (not the text "NULL1"), `NULL * 2` is NULL (not 0); x / 0 is NULL, as in MySQL.
    if (auto* v = std::get_if<ArithExpr::Add>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        if (lv == EXECUTOR_NULL_VALUE || rv == EXECUTOR_NULL_VALUE) return EXECUTOR_NULL_VALUE;
        if (auto exact = exact_int_text('+', lv, rv)) return *exact;
        return format_arith_result(text_to_number(lv) + text_to_number(rv));
    }
    if (auto* v = std::get_if<ArithExpr::Sub>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        if (lv == EXECUTOR_NULL_VALUE || rv == EXECUTOR_NULL_VALUE) return EXECUTOR_NULL_VALUE;
        if (auto exact = exact_int_text('-', lv, rv)) return *exact;
        return format_arith_result(text_to_number(lv) - text_to_number(rv));
    }
    if (auto* v = std::get_if<ArithExpr::Mul>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        if (lv == EXECUTOR_NULL_VALUE || rv == EXECUTOR_NULL_VALUE) return EXECUTOR_NULL_VALUE;
        if (auto exact = exact_int_text('*', lv, rv)) return *exact;
        return format_arith_result(text_to_number(lv) * text_to_number(rv));
    }
    if (auto* v = std::get_if<ArithExpr::Div>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        if (lv == EXECUTOR_NULL_VALUE || rv == EXECUTOR_NULL_VALUE) return EXECUTOR_NULL_VALUE;
        const double divisor = text_to_number(rv);
        if (divisor == 0.0) return EXECUTOR_NULL_VALUE;
        return format_arith_result(text_to_number(lv) / divisor);
    }
    if (auto* v = std::get_if<ArithExpr::Func>(&expr.data)) {
        if (v->name == "CASE") {
            // CASE(when1, then1, when2, then2, .. [, else]): the first branch whose condition is true (not false, not unknown) gives the value
            const std::size_t branches = v->args.size() / 2;
            for (std::size_t i = 0; i < branches; i++) {
                if (eval_arith(row, v->args[2 * i]) == "1") return eval_arith(row, v->args[2 * i + 1]);
            }
            return v->args.size() % 2 == 1 ? eval_arith(row, v->args.back()) : std::string(EXECUTOR_NULL_VALUE);
        }
        std::vector<std::string> str_args;
        str_args.reserve(v->args.size());
        for (auto& a : v->args) {
            if (auto* c = std::get_if<ArithExpr::Col>(&a.data)) str_args.push_back(c->name);
            else if (auto* sv = std::get_if<ArithExpr::Str>(&a.data)) str_args.push_back("'" + sv->value + "'");
            else if (auto* n = std::get_if<ArithExpr::Num>(&a.data)) str_args.push_back(n->value);
            else str_args.push_back("'" + eval_arith(row, a) + "'");
        }
        return apply_scalar_func(v->name, str_args, row);
    }
    if (auto* v = std::get_if<ArithExpr::Pred>(&expr.data)) {
        // a condition as a value: 1, 0 or (unknown) NULL
        switch (eval_cond3(row, *v->cond)) {
            case Tri::True: return "1";
            case Tri::False: return "0";
            case Tri::Unknown: break;
        }
        return EXECUTOR_NULL_VALUE;
    }
    if (auto* v = std::get_if<ArithExpr::Cmp>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        if (lv == EXECUTOR_NULL_VALUE || rv == EXECUTOR_NULL_VALUE) return EXECUTOR_NULL_VALUE; // a comparison with NULL is NULL
        const int order = compare_classed(class_of_expr(*v->lhs), class_of_expr(*v->rhs), lv, rv);
        bool result;
        if (v->op == ">") result = order > 0;
        else if (v->op == "<") result = order < 0;
        else if (v->op == ">=") result = order >= 0;
        else if (v->op == "<=") result = order <= 0;
        else if (v->op == "=") result = order == 0;
        else result = order != 0;
        return result ? "1" : "0";
    }
    return EXECUTOR_NULL_VALUE;
}

bool Executor::matches_condexpr(const Row& row, const std::optional<CondExpr>& condition) {
    return !condition || eval_condexpr(row, *condition);
}

bool Executor::eval_condexpr(const Row& row, const CondExpr& expr) { return eval_cond3(row, expr) == Tri::True; }

Executor::Tri Executor::eval_cond3(const Row& row, const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) {
        Tri l = eval_cond3(row, *v->lhs);
        if (l == Tri::False) return Tri::False;
        Tri r = eval_cond3(row, *v->rhs);
        if (r == Tri::False) return Tri::False;
        return (l == Tri::True && r == Tri::True) ? Tri::True : Tri::Unknown;
    }
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) {
        Tri l = eval_cond3(row, *v->lhs);
        if (l == Tri::True) return Tri::True;
        Tri r = eval_cond3(row, *v->rhs);
        if (r == Tri::True) return Tri::True;
        return (l == Tri::False && r == Tri::False) ? Tri::False : Tri::Unknown;
    }
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) {
        Tri i = eval_cond3(row, *v->inner);
        return i == Tri::True ? Tri::False : i == Tri::False ? Tri::True : Tri::Unknown;
    }
    if (auto* v = std::get_if<CondExpr::Leaf>(&expr.data)) return eval_single3(row, v->condition);
    return Tri::False;
}

bool Executor::eval_single(const Row& row, const Condition& cond) { return eval_single3(row, cond) == Tri::True; }

// A comparison with a NULL operand is UNKNOWN (so is NOT of it); IS NULL / IS NOT NULL are the only predicates that answer for NULL;
// `x IN (1, NULL)` is TRUE or UNKNOWN, `x NOT IN (1, NULL)` FALSE or UNKNOWN.
Executor::Tri Executor::eval_single3(const Row& row, const Condition& cond) {
    auto tri = [](bool b) { return b ? Tri::True : Tri::False; };
    std::string val = eval_arith(row, cond.left);
    // how the two sides compare: what each is (set by the binder; an expression that was not bound tells itself)
    const ValueClass left_class = cond.left_class != ValueClass::Unknown ? cond.left_class : class_of_expr(cond.left);

    if (std::holds_alternative<ConditionValue::Subquery>(cond.value.data)) return Tri::False;

    if (auto* bv = std::get_if<ConditionValue::Between>(&cond.value.data)) {
        if (val == EXECUTOR_NULL_VALUE) return Tri::Unknown;
        // `x BETWEEN lo AND hi` is `x >= lo AND x <= hi`: a NULL bound (a variable that holds NULL) makes its side UNKNOWN, and the other side can
        // still say FALSE (so BETWEEN never selects and NOT BETWEEN selects what the other bound rules out)
        auto side = [&](const std::string& bound, bool quoted, bool lower) {
            if (bound == EXECUTOR_NULL_VALUE) return Tri::Unknown;
            const int order = compare_classed(left_class, written_class(bound, quoted), val, bound);
            return tri(lower ? order >= 0 : order <= 0);
        };
        const Tri low = side(bv->lo, bv->lo_quoted, true), high = side(bv->hi, bv->hi_quoted, false);
        const Tri inside = (low == Tri::False || high == Tri::False) ? Tri::False : (low == Tri::True && high == Tri::True ? Tri::True : Tri::Unknown);
        if (cond.op != Operator::NotBetween) return inside;
        return inside == Tri::True ? Tri::False : (inside == Tri::False ? Tri::True : Tri::Unknown);
    }

    if (auto* ll = std::get_if<ConditionValue::LiteralList>(&cond.value.data)) {
        if (val == EXECUTOR_NULL_VALUE) return Tri::Unknown;
        const bool has_null = std::any_of(ll->values.begin(), ll->values.end(), [](const std::string& v) { return v == EXECUTOR_NULL_VALUE; });
        if (cond.op == Operator::In || cond.op == Operator::NotIn) {
            for (std::size_t i = 0; i < ll->values.size(); i++) {
                if (ll->values[i] == EXECUTOR_NULL_VALUE) continue; // (a NULL in the list is UNKNOWN below, never equal)
                const bool quoted = i < ll->quoted.size() && ll->quoted[i];
                if (compare_classed(left_class, written_class(ll->values[i], quoted), val, ll->values[i]) == 0) return tri(cond.op == Operator::In);
            }
            return has_null ? Tri::Unknown : tri(cond.op == Operator::NotIn);
        }
        return Tri::False;
    }

    std::string resolved;
    const std::string* effective_lit = nullptr;
    ValueClass right_class = cond.right_class;

    // PLAN.md P0 fix: the RHS of a comparison is now a full arithmetic expression
    // (ConditionValue::Arith) rather than always a single-token Literal, so
    // `WHERE v > id + 100` actually evaluates `id + 100` instead of silently
    // dropping the `+ 100`. eval_arith already resolves any Col references inside
    // the expression, so no separate column-lookup step is needed here (unlike
    // the Literal branch below, which still needs its own ident-vs-literal check).
    if (auto* av = std::get_if<ConditionValue::Arith>(&cond.value.data)) {
        resolved = eval_arith(row, av->expr);
        effective_lit = &resolved;
        if (right_class == ValueClass::Unknown) right_class = class_of_expr(av->expr);
    } else if (auto* lit_v = std::get_if<ConditionValue::Literal>(&cond.value.data)) {
        const std::string& lit = lit_v->value;
        // a quoted value is a string, never the name of a column
        bool is_ident_like = !lit_v->quoted && !lit.empty() && (std::isalpha(static_cast<unsigned char>(lit[0])) || lit[0] == '_') && !parse_f64(lit).has_value();
        effective_lit = &lit;
        if (is_ident_like) {
            if (const std::string* v = get_col(row, lit)) {
                resolved = *v;
                effective_lit = &resolved;
            }
        }
        if (right_class == ValueClass::Unknown) right_class = written_class(lit, lit_v->quoted);
    } else {
        return Tri::False;
    }

    if (cond.op == Operator::IsNull) return tri(val == EXECUTOR_NULL_VALUE);
    if (cond.op == Operator::IsNotNull) return tri(val != EXECUTOR_NULL_VALUE);
    if (val == EXECUTOR_NULL_VALUE) return Tri::Unknown;
    if (*effective_lit == "__NULL__" || *effective_lit == EXECUTOR_NULL_VALUE) return Tri::Unknown;

    switch (cond.op) {
        case Operator::Eq: return tri(compare_classed(left_class, right_class, val, *effective_lit) == 0);
        case Operator::Ne: return tri(compare_classed(left_class, right_class, val, *effective_lit) != 0);
        case Operator::In:
        case Operator::NotIn:
        case Operator::Exists:
        case Operator::NotExists:
            return Tri::False;
        case Operator::Like:
            return tri(like_match(val, *effective_lit));
        case Operator::NotLike:
            return tri(!like_match(val, *effective_lit));
        case Operator::Regexp:
            try {
                return tri(std::regex_search(val, std::regex(*effective_lit)));
            } catch (...) {
                return Tri::False;
            }
        case Operator::NotRegexp:
            try {
                return tri(!std::regex_search(val, std::regex(*effective_lit)));
            } catch (...) {
                return Tri::True;
            }
        case Operator::Between:
        case Operator::NotBetween:
            return Tri::False;
        case Operator::Gt: return tri(compare_classed(left_class, right_class, val, *effective_lit) > 0);
        case Operator::Lt: return tri(compare_classed(left_class, right_class, val, *effective_lit) < 0);
        case Operator::Gte: return tri(compare_classed(left_class, right_class, val, *effective_lit) >= 0);
        case Operator::Lte: return tri(compare_classed(left_class, right_class, val, *effective_lit) <= 0);
        default:
            return Tri::False;
    }
}

bool Executor::eval_check_expr(const std::string& expr, const Row& row) {
    Parser parser("SELECT 1 FROM __check__ WHERE " + expr);
    auto result = parser.parse();
    if (result.is_ok()) {
        if (auto* sel = std::get_if<Statement::Select>(&result.value().data)) {
            // a CHECK constraint is violated only when its condition is FALSE: UNKNOWN (a NULL operand) passes
            if (sel->condition) return eval_cond3(row, *sel->condition) != Tri::False;
        }
    }
    return true;
}

// PLAN.md P0 fix follow-up: mirrors substitute_correlated_condexpr's Literal-based
// outer-row substitution, but walks a full ArithExpr tree (needed now that a
// ConditionValue::Arith RHS can embed an outer-table column reference anywhere
// inside an expression, e.g. `WHERE d.id = e.dept_id + 0`, not just as the whole RHS).
ArithExpr Executor::substitute_arith_outer_refs(const ArithExpr& expr, const Row& outer_row) {
    return std::visit(
        [&](const auto& alt) -> ArithExpr {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ArithExpr::Col>) {
                if (alt.name.find('.') != std::string::npos) {
                    if (const std::string* rv = get_col(outer_row, alt.name)) return ArithExpr(ArithExpr::Str{*rv});
                }
                return ArithExpr(alt);
            } else if constexpr (std::is_same_v<T, ArithExpr::Add>) {
                return ArithExpr(ArithExpr::Add{std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.lhs, outer_row)),
                                                 std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.rhs, outer_row))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Sub>) {
                return ArithExpr(ArithExpr::Sub{std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.lhs, outer_row)),
                                                 std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.rhs, outer_row))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Mul>) {
                return ArithExpr(ArithExpr::Mul{std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.lhs, outer_row)),
                                                 std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.rhs, outer_row))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Div>) {
                return ArithExpr(ArithExpr::Div{std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.lhs, outer_row)),
                                                 std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.rhs, outer_row))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) {
                return ArithExpr(ArithExpr::Cmp{std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.lhs, outer_row)), alt.op,
                                                 std::make_unique<ArithExpr>(substitute_arith_outer_refs(*alt.rhs, outer_row))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                std::vector<ArithExpr> args;
                args.reserve(alt.args.size());
                for (auto& a : alt.args) args.push_back(substitute_arith_outer_refs(a, outer_row));
                return ArithExpr(ArithExpr::Func{alt.name, std::move(args)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Pred>) {
                return ArithExpr(ArithExpr::Pred{std::make_unique<CondExpr>(substitute_correlated_condexpr(*alt.cond, outer_row))});
            } else {
                return ArithExpr(alt);
            }
        },
        expr.data);
}

CondExpr Executor::substitute_correlated_condexpr(const CondExpr& expr, const Row& outer_row) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) {
        return CondExpr(CondExpr::And{std::make_unique<CondExpr>(substitute_correlated_condexpr(*v->lhs, outer_row)),
                                       std::make_unique<CondExpr>(substitute_correlated_condexpr(*v->rhs, outer_row))});
    }
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) {
        return CondExpr(CondExpr::Or{std::make_unique<CondExpr>(substitute_correlated_condexpr(*v->lhs, outer_row)),
                                      std::make_unique<CondExpr>(substitute_correlated_condexpr(*v->rhs, outer_row))});
    }
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) {
        return CondExpr(CondExpr::Not{std::make_unique<CondExpr>(substitute_correlated_condexpr(*v->inner, outer_row))});
    }
    if (auto* v = std::get_if<CondExpr::Leaf>(&expr.data)) {
        Condition new_cond = v->condition;
        if (auto* lit = std::get_if<ConditionValue::Literal>(&v->condition.value.data)) {
            if (lit->value.find('.') != std::string::npos) {
                if (const std::string* rv = get_col(outer_row, lit->value)) {
                    new_cond.value = ConditionValue(ConditionValue::Literal{*rv});
                }
            }
        } else if (auto* ar = std::get_if<ConditionValue::Arith>(&v->condition.value.data)) {
            new_cond.value = ConditionValue(ConditionValue::Arith{substitute_arith_outer_refs(ar->expr, outer_row)});
        }
        return CondExpr(CondExpr::Leaf{std::move(new_cond)});
    }
    return expr;
}

std::string Executor::format_returning_rows(const std::vector<Row>& rows, const std::vector<SelectColumn>& cols) {
    if (rows.empty()) return "(0 rows)";

    std::vector<std::pair<std::string, std::string>> headers; // (display, lookup-key)
    bool has_all = std::any_of(cols.begin(), cols.end(), [](const SelectColumn& c) { return std::holds_alternative<SelectColumn::All>(c.data); });
    if (has_all) {
        for (auto& [k, _] : rows.front()) {
            if (!k.empty() && k[0] != '_') headers.emplace_back(k, k);
        }
    } else {
        for (auto& c : cols) {
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) headers.emplace_back(col->name, col->name);
            else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) headers.emplace_back(ca->alias, ca->name);
        }
    }

    std::vector<std::vector<std::string>> data;
    data.reserve(rows.size());
    for (auto& row : rows) {
        std::vector<std::string> vals;
        vals.reserve(headers.size());
        for (auto& [_, key] : headers) {
            auto it = row.find(key);
            vals.push_back(it != row.end() ? it->second : EXECUTOR_NULL_VALUE);
        }
        data.push_back(std::move(vals));
    }

    std::vector<std::size_t> widths(headers.size());
    for (std::size_t i = 0; i < headers.size(); i++) {
        std::size_t mv = 0;
        for (auto& row_vals : data) mv = std::max(mv, row_vals[i].size());
        widths[i] = std::max(headers[i].first.size(), mv);
    }

    auto pad = [](const std::string& s, std::size_t w) { return " " + s + std::string(w - s.size(), ' ') + " "; };
    auto sep_line = [&]() {
        std::string sep = "+";
        for (auto w : widths) sep += std::string(w + 2, '-') + "+";
        return sep;
    };

    std::string out = sep_line() + "\n|";
    for (std::size_t i = 0; i < headers.size(); i++) out += pad(headers[i].first, widths[i]) + "|";
    out += "\n" + sep_line() + "\n";
    for (auto& row_vals : data) {
        out += "|";
        for (std::size_t i = 0; i < row_vals.size(); i++) out += pad(row_vals[i], widths[i]) + "|";
        out += "\n";
    }
    out += sep_line();
    return out;
}

void Executor::update_stat_rows(SharedDatabase& s, const std::string& table, std::int64_t delta) {
    auto& stats = s.table_stats[table];
    std::int64_t updated = static_cast<std::int64_t>(stats.total_rows) + delta;
    stats.total_rows = static_cast<std::size_t>(std::max<std::int64_t>(updated, 0));
}

std::string Executor::escape_cell(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '|') out += "\\|";
        else out += c;
    }
    return out;
}

namespace {
// Reverses Executor::escape_cell() on a cell substring already extracted (and
// whitespace-trimmed) by parse_table_output(). A lone unescaped '\' should never occur
// in properly-escaped input (escape_cell() always escapes '\' itself first), so the
// fallback below is defensive only.
std::string unescape_cell(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); i++) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[i + 1];
            if (n == '\\') { out += '\\'; i++; }
            else if (n == 'n') { out += '\n'; i++; }
            else if (n == '|') { out += '|'; i++; }
            else out += v[i];
        } else {
            out += v[i];
        }
    }
    return out;
}
// Finds the next delimiter in `line` starting at `start`, skipping any escaped '\X'
// pair (so an escaped '\|' is never mistaken for a real cell boundary).
std::size_t find_unescaped_bar(const std::string& line, std::size_t start) {
    for (std::size_t i = start; i < line.size(); i++) {
        if (line[i] == '\\' && i + 1 < line.size()) { i++; continue; }
        if (line[i] == '|') return i;
    }
    return std::string::npos;
}
}

std::pair<std::vector<std::string>, std::vector<Row>> Executor::parse_table_output(const std::string& output) {
    std::vector<std::string> col_names;
    std::vector<Row> rows;

    std::size_t pos = 0;
    std::vector<std::string> lines;
    while (pos <= output.size()) {
        auto nl = output.find('\n', pos);
        lines.push_back(nl == std::string::npos ? output.substr(pos) : output.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (lines.empty() || lines.front().empty() || lines.front()[0] != '+') return {{}, {}};

    bool header_parsed = false;
    for (auto& line : lines) {
        if (line.empty()) continue;
        if (line[0] == '+') continue;
        if (line[0] == '|') {
            // Matches Rust's `line.split('|').filter(|s| !s.is_empty())`: split on EVERY
            // '|' (including the line's own leading/trailing ones, which produce
            // zero-length boundary segments to be filtered out), keeping any segment
            // with length > 0 -- including a whitespace-only segment (an empty cell's
            // padding), which survives the filter and only becomes "" after the
            // subsequent trim. The previous version filtered out whitespace-only
            // segments too (checking for a non-whitespace char rather than length > 0),
            // silently dropping empty cells and shifting every later column in the row
            // one position left.
            std::vector<std::string> cells;
            std::size_t start = 0;
            for (;;) {
                auto bar = find_unescaped_bar(line, start);
                std::string cell = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!cell.empty()) {
                    auto ws0 = cell.find_first_not_of(" \t");
                    if (ws0 == std::string::npos) cells.push_back("");
                    else cells.push_back(unescape_cell(cell.substr(ws0, cell.find_last_not_of(" \t") - ws0 + 1)));
                }
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            if (!header_parsed) {
                col_names = cells;
                header_parsed = true;
            } else {
                Row row;
                for (std::size_t i = 0; i < col_names.size(); i++) row[col_names[i]] = i < cells.size() ? cells[i] : "";
                // MVCC: "0" is the permanent "always visible" sentinel. These rows are
                // reconstructed from another query's already-materialized text output
                // (CTE/subquery/UNION temp tables, INSERT-SELECT source rows) -- ephemeral,
                // single-statement-lifetime data with no real transaction identity of its
                // own, so a real or fake nonzero txn id here would make is_visible_for_read
                // spuriously reject them depending on the current global txn-id counter.
                row["_xmin"] = "0";
                row["_xmax"] = "0";
                rows.push_back(std::move(row));
            }
        }
    }
    return {col_names, rows};
}

} // namespace engine
