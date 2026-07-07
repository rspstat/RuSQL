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

#include "engine/parser/parser.hpp"

namespace engine {

namespace {

std::optional<double> parse_f64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    double val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return val;
}

// Returns -1/0/1 (like/unlike Ordering::Less/Equal/Greater) or nullopt if either side
// isn't numeric — mirrors the `cmp_num` closure used throughout eval_single in Rust.
std::optional<int> cmp_num(const std::string& a, const std::string& b) {
    auto da = parse_f64(a);
    auto db = parse_f64(b);
    if (!da || !db) return std::nullopt;
    if (*da < *db) return -1;
    if (*da > *db) return 1;
    return 0;
}

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
    if (auto* v = std::get_if<ArithExpr::Add>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        auto a = parse_f64(lv), b = parse_f64(rv);
        if (a && b) return format_arith_result(*a + *b);
        return lv + rv;
    }
    if (auto* v = std::get_if<ArithExpr::Sub>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        auto a = parse_f64(lv), b = parse_f64(rv);
        return (a && b) ? format_arith_result(*a - *b) : "0";
    }
    if (auto* v = std::get_if<ArithExpr::Mul>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        auto a = parse_f64(lv), b = parse_f64(rv);
        return (a && b) ? format_arith_result(*a * *b) : "0";
    }
    if (auto* v = std::get_if<ArithExpr::Div>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        auto a = parse_f64(lv), b = parse_f64(rv);
        return (a && b && *b != 0.0) ? format_arith_result(*a / *b) : "0";
    }
    if (auto* v = std::get_if<ArithExpr::Func>(&expr.data)) {
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
    if (auto* v = std::get_if<ArithExpr::Cmp>(&expr.data)) {
        std::string lv = eval_arith(row, *v->lhs), rv = eval_arith(row, *v->rhs);
        auto a = parse_f64(lv), b = parse_f64(rv);
        bool result;
        if (a && b) {
            if (v->op == ">") result = *a > *b;
            else if (v->op == "<") result = *a < *b;
            else if (v->op == ">=") result = *a >= *b;
            else if (v->op == "<=") result = *a <= *b;
            else if (v->op == "=") result = std::abs(*a - *b) < 1e-9;
            else result = *a != *b;
        } else {
            if (v->op == "=") result = lv == rv;
            else if (v->op == ">") result = lv > rv;
            else if (v->op == "<") result = lv < rv;
            else if (v->op == ">=") result = lv >= rv;
            else if (v->op == "<=") result = lv <= rv;
            else result = lv != rv;
        }
        return result ? "1" : "0";
    }
    return EXECUTOR_NULL_VALUE;
}

bool Executor::matches_condexpr(const Row& row, const std::optional<CondExpr>& condition) {
    return !condition || eval_condexpr(row, *condition);
}

bool Executor::eval_condexpr(const Row& row, const CondExpr& expr) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) return eval_condexpr(row, *v->lhs) && eval_condexpr(row, *v->rhs);
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) return eval_condexpr(row, *v->lhs) || eval_condexpr(row, *v->rhs);
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) return !eval_condexpr(row, *v->inner);
    if (auto* v = std::get_if<CondExpr::Leaf>(&expr.data)) return eval_single(row, v->condition);
    return false;
}

bool Executor::eval_single(const Row& row, const Condition& cond) {
    std::string val = eval_arith(row, cond.left);

    if (std::holds_alternative<ConditionValue::Subquery>(cond.value.data)) return false;

    if (auto* bv = std::get_if<ConditionValue::Between>(&cond.value.data)) {
        if (val == EXECUTOR_NULL_VALUE) return false;
        bool in_range;
        auto sc = cmp_num(val, bv->lo);
        auto ec = cmp_num(val, bv->hi);
        if (sc && ec) in_range = (*sc != -1) && (*ec != 1);
        else in_range = (val >= bv->lo) && (val <= bv->hi);
        return cond.op == Operator::NotBetween ? !in_range : in_range;
    }

    if (auto* ll = std::get_if<ConditionValue::LiteralList>(&cond.value.data)) {
        if (val == EXECUTOR_NULL_VALUE) return false;
        if (cond.op == Operator::In) {
            for (auto& item : ll->values) {
                auto a = parse_f64(val), b = parse_f64(item);
                if (a && b ? (*a == *b) : (val == item)) return true;
            }
            return false;
        }
        if (cond.op == Operator::NotIn) {
            for (auto& item : ll->values) {
                auto a = parse_f64(val), b = parse_f64(item);
                if (a && b ? (*a == *b) : (val == item)) return false;
            }
            return true;
        }
        return false;
    }

    auto* lit_v = std::get_if<ConditionValue::Literal>(&cond.value.data);
    if (!lit_v) return false;
    const std::string& lit = lit_v->value;

    std::string resolved;
    bool is_ident_like = !lit.empty() && (std::isalpha(static_cast<unsigned char>(lit[0])) || lit[0] == '_') && !parse_f64(lit).has_value();
    const std::string* effective_lit = &lit;
    if (is_ident_like) {
        if (const std::string* v = get_col(row, lit)) {
            resolved = *v;
            effective_lit = &resolved;
        }
    }

    if (cond.op == Operator::IsNull) return val == EXECUTOR_NULL_VALUE || val.empty();
    if (cond.op == Operator::IsNotNull) return val != EXECUTOR_NULL_VALUE && !val.empty();
    if (val == EXECUTOR_NULL_VALUE) return false;
    if (*effective_lit == "__NULL__") return false;

    switch (cond.op) {
        case Operator::Eq: {
            auto a = parse_f64(val), b = parse_f64(*effective_lit);
            return (a && b) ? (*a == *b) : (val == *effective_lit);
        }
        case Operator::Ne: {
            auto a = parse_f64(val), b = parse_f64(*effective_lit);
            return (a && b) ? (*a != *b) : (val != *effective_lit);
        }
        case Operator::In:
        case Operator::NotIn:
        case Operator::Exists:
        case Operator::NotExists:
            return false;
        case Operator::Like:
            return like_match(val, *effective_lit);
        case Operator::NotLike:
            return !like_match(val, *effective_lit);
        case Operator::Regexp:
            try {
                return std::regex_search(val, std::regex(*effective_lit));
            } catch (...) {
                return false;
            }
        case Operator::NotRegexp:
            try {
                return !std::regex_search(val, std::regex(*effective_lit));
            } catch (...) {
                return true;
            }
        case Operator::Between:
        case Operator::NotBetween:
            return false;
        case Operator::Gt: {
            auto c = cmp_num(val, *effective_lit);
            return c ? (*c == 1) : (val > *effective_lit);
        }
        case Operator::Lt: {
            auto c = cmp_num(val, *effective_lit);
            return c ? (*c == -1) : (val < *effective_lit);
        }
        case Operator::Gte: {
            auto c = cmp_num(val, *effective_lit);
            return c ? (*c != -1) : (val >= *effective_lit);
        }
        case Operator::Lte: {
            auto c = cmp_num(val, *effective_lit);
            return c ? (*c != 1) : (val <= *effective_lit);
        }
        default:
            return false;
    }
}

bool Executor::eval_check_expr(const std::string& expr, const Row& row) {
    Parser parser("SELECT 1 FROM __check__ WHERE " + expr);
    auto result = parser.parse();
    if (result.is_ok()) {
        if (auto* sel = std::get_if<Statement::Select>(&result.value().data)) {
            if (sel->condition) return eval_condexpr(row, *sel->condition);
        }
    }
    return true;
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
                auto bar = line.find('|', start);
                std::string cell = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!cell.empty()) {
                    auto ws0 = cell.find_first_not_of(" \t");
                    if (ws0 == std::string::npos) cells.push_back("");
                    else cells.push_back(cell.substr(ws0, cell.find_last_not_of(" \t") - ws0 + 1));
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
                row["_xmin"] = "1";
                row["_xmax"] = "0";
                rows.push_back(std::move(row));
            }
        }
    }
    return {col_names, rows};
}

} // namespace engine
