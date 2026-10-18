// Faithful port of the expression/condition evaluation helpers from
// rusql-core/src/engine/executor.rs (Phase 8b): get_col, eval_arith,
// format_arith_result, matches_condexpr/eval_condexpr/eval_single, eval_check_expr,
// format_returning_rows, update_stat_rows, and parse_table_output. apply_scalar_func is implemented in executor_scalar_func.cpp.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include "engine/collation.hpp"
#include "engine/executor/builtin_functions.hpp"
#include "engine/numeric_text.hpp"
#include "engine/parser/parser.hpp"
#include "engine/sql_value_util.hpp"

namespace engine {

namespace {

std::optional<double> parse_f64(const std::string& s) { return parse_number(s); }

// ---- + - * / on exact decimals (MySQL's DECIMAL) --------------------------------------------------------------------------------------------
// A tree of + - * / is evaluated as one: the values keep all their digits, and the result is rounded to the number of digits MySQL declares for it
// (a quotient has the dividend's scale plus 4, a product the sum of the scales, a sum the larger one) when the tree is done, so 1 / 3 * 3 is 1.0000
// and not 0.9999. Integers stay integers (BIGINT: overflow is an error), and a text that is no plain number makes the tree a DOUBLE.

struct NumVal {
    bool null = false;
    bool approx = false; // a DOUBLE: the number is `dbl`
    double dbl = 0;
    sqlval::Num num;
    int scale = 0; // the digits MySQL would show
    bool is_int = false;
    long long i = 0;
    bool derived = false; // the result of an operation done on exact decimals: `num` is the value, `dbl` only its double arithmetic
    bool has_num = false; // `num` holds the value (an integer is only `i` until an operation on decimals needs its digits)
};

// The number as a DOUBLE for an operation that is done in floating point: an exact result of an operation below it is converted from the
// decimal it holds (a quotient has 9 digits there), not taken from the double arithmetic.
double as_double(const NumVal& x) {
    if (x.approx || !x.derived) return x.dbl;
    if (x.is_int) return static_cast<double>(x.i);
    return std::strtod(x.num.to_decimal().compact().c_str(), nullptr);
}

const sqlval::Num& num_of(const NumVal& x, sqlval::Num& scratch) {
    if (x.has_num) return x.num;
    scratch = sqlval::Num::from_int(x.i);
    return scratch;
}

// An integer written out (at most 18 digits): the common value of a column or a literal, read without building a decimal.
bool small_int(const std::string& s, long long& out) {
    const std::size_t n = s.size();
    std::size_t at = 0;
    if (n > 0 && s[0] == '-') at = 1;
    if (at == n || n - at > 18) return false;
    long long v = 0;
    for (; at < n; at++) {
        if (s[at] < '0' || s[at] > '9') return false;
        v = v * 10 + (s[at] - '0');
    }
    out = s[0] == '-' ? -v : v;
    return true;
}

// a / b of two integers with 9 digits after the point, cut off (b is not zero); false when the digits would not fit a 64-bit remainder
bool int_quotient(long long a, long long b, sqlval::Num& out) {
    const unsigned long long ua = a < 0 ? 0ULL - static_cast<unsigned long long>(a) : static_cast<unsigned long long>(a);
    const unsigned long long ub = b < 0 ? 0ULL - static_cast<unsigned long long>(b) : static_cast<unsigned long long>(b);
    if (ub > 900000000000000000ULL) return false;
    unsigned long long whole = ua / ub, rest = ua % ub, fraction = 0;
    for (int k = 0; k < 9; k++) {
        rest *= 10;
        fraction = fraction * 10 + rest / ub;
        rest %= ub;
    }
    char digits[48];
    std::snprintf(digits, sizeof digits, "%llu%09llu", whole, fraction);
    const std::string all = digits;
    const std::size_t first = all.find_first_not_of('0');
    out.digits = first == std::string::npos ? "0" : all.substr(first);
    out.scale = 9;
    out.negative = out.digits != "0" && ((a < 0) != (b < 0));
    return true;
}

std::string op_text(char op) { return std::string(1, op); }

NumVal combine(char op, const NumVal& l, const NumVal& r) {
    NumVal v;
    if (l.null || r.null) {
        v.null = true;
        return v;
    }
    v.approx = l.approx || r.approx;
    const double ld = v.approx ? as_double(l) : l.dbl, rd = v.approx ? as_double(r) : r.dbl;
    if (op == '/') {
        const bool zero = v.approx ? rd == 0.0 : (r.is_int ? r.i == 0 : r.num.is_zero());
        if (zero) {
            v.null = true; // (x / 0 is NULL, as in MySQL)
            return v;
        }
        v.dbl = ld / rd;
    } else if (op == '+') {
        v.dbl = ld + rd;
    } else if (op == '-') {
        v.dbl = ld - rd;
    } else {
        v.dbl = ld * rd;
    }
    if (v.approx) {
        if (!std::isfinite(v.dbl)) v.null = true;
        return v;
    }
    v.derived = true;
    if (op != '/' && l.is_int && r.is_int) {
        auto result = int64_arith(op, l.i, r.i);
        if (!result) throw StatementError("BIGINT value is out of range in '(" + std::to_string(l.i) + " " + op_text(op) + " " + std::to_string(r.i) + ")'");
        v.is_int = true;
        v.i = *result;
        return v;
    }
    v.has_num = true;
    sqlval::Num left_scratch, right_scratch;
    if (op == '+' || op == '-') {
        v.num = sqlval::num_add(num_of(l, left_scratch), num_of(r, right_scratch), op == '-');
        v.scale = std::max(l.scale, r.scale);
    } else if (op == '*') {
        v.num = sqlval::num_mul(num_of(l, left_scratch), num_of(r, right_scratch));
        v.scale = std::min(30, l.scale + r.scale);
    } else {
        v.scale = std::min(30, l.scale + 4);
        if (l.is_int && r.is_int && int_quotient(l.i, r.i, v.num)) return v;
        const int internal = ((l.scale + 4 + 8) / 9) * 9;
        v.num = sqlval::num_div(num_of(l, left_scratch), num_of(r, right_scratch), internal, /*cut=*/true); // (MySQL cuts the quotient off after those digits: 2 / 3 holds 0.666666666)
    }
    return v;
}

template <class Leaf>
NumVal number_tree(const ArithExpr& e, Leaf& leaf) {
    if (auto* a = std::get_if<ArithExpr::Add>(&e.data)) { NumVal l = number_tree(*a->lhs, leaf); NumVal r = number_tree(*a->rhs, leaf); return combine('+', l, r); }
    if (auto* a = std::get_if<ArithExpr::Sub>(&e.data)) { NumVal l = number_tree(*a->lhs, leaf); NumVal r = number_tree(*a->rhs, leaf); return combine('-', l, r); }
    if (auto* a = std::get_if<ArithExpr::Mul>(&e.data)) { NumVal l = number_tree(*a->lhs, leaf); NumVal r = number_tree(*a->rhs, leaf); return combine('*', l, r); }
    if (auto* a = std::get_if<ArithExpr::Div>(&e.data)) { NumVal l = number_tree(*a->lhs, leaf); NumVal r = number_tree(*a->rhs, leaf); return combine('/', l, r); }
    const std::string text = leaf(e);
    NumVal v;
    if (text == EXECUTOR_NULL_VALUE) {
        v.null = true;
        return v;
    }
    long long whole = 0;
    if (small_int(text, whole)) {
        v.is_int = true;
        v.i = whole;
        v.dbl = static_cast<double>(whole);
    } else {
        v.dbl = sqlval::leading_number(text);
        if (auto d = text.find_first_of("eE") == std::string::npos ? sqlval::Decimal::parse(text) : std::nullopt) { // (a number written with an exponent is a DOUBLE)
            v.num = sqlval::Num::from_decimal(*d);
            v.has_num = true;
            v.scale = v.num.scale;
            if (v.scale == 0) {
                if (auto i = d->to_int64()) {
                    v.is_int = true;
                    v.i = *i;
                }
            }
        } else {
            v.approx = true;
        }
    }
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data); col && (col->approx || col->cls == ValueClass::Text)) v.approx = true; // (a FLOAT / DOUBLE column or a text column: floating point, as in MySQL)
    if (std::holds_alternative<ArithExpr::Str>(e.data)) v.approx = true; // ('2.5' * 2 is 5, not 5.0: a string is read as a DOUBLE)
    return v;
}

// `exact`: every digit the operations kept, not only the digits the result shows
template <class Leaf>
std::string arithmetic_text(const ArithExpr& e, Leaf leaf, bool exact = false, bool* approx = nullptr) {
    NumVal v = number_tree(e, leaf);
    if (approx) *approx = v.approx;
    if (v.null) return EXECUTOR_NULL_VALUE;
    if (v.approx) return sqlval::format_double(v.dbl);
    if (v.is_int && !v.has_num) return std::to_string(v.i);
    if (exact) return v.num.to_decimal().str(static_cast<std::size_t>(v.num.scale)); // (all the digits it holds, zeros too: ROUND(10.50 / 3, 2) is 3.50)
    return sqlval::num_round(v.num, v.scale).to_decimal().str(static_cast<std::size_t>(v.scale)); // (every digit MySQL shows: 10.50 * 2 is 21.00)
}

// The functions that do arithmetic on a number they are given: FLOOR(10 / 3 * 3) is 9 in MySQL (the product holds 9.999999999), ROUND(8 / 59.27, 2) is 0.13
// (not the 0.14 of the 0.1350 the quotient shows). The others (ABS, GREATEST, IFNULL, CAST ... AS CHAR) hand on the number as it shows.
bool takes_exact_numbers(const std::string& upper) {
    static const std::unordered_set<std::string> names = {"ROUND", "TRUNCATE", "FLOOR", "CEIL", "CEILING", "SQRT", "POW", "POWER", "EXP", "LN", "LOG",
                                                          "LOG2", "LOG10", "SIN", "COS", "TAN", "ASIN", "ACOS", "ATAN", "ATAN2", "COT", "DEGREES",
                                                          "RADIANS", "MOD", "SIGN", "FORMAT"};
    return names.count(upper) > 0;
}

// What a value written without quotes in an IN list or after BETWEEN is: a number, or (a name, @variable) not told.
ValueClass written_class(const std::string& value, bool quoted) { return quoted ? ValueClass::Text : (parse_number(value) ? ValueClass::Number : ValueClass::Unknown); }

// LIKE: `%` any run of characters, `_` one character, the characters compared by the default collation ('A' matches 'a', 'e' matches 'é').
bool like_match(std::string_view val, std::string_view pat) { return collation::like(val, pat); }

// REGEXP does not tell upper case from lower case either (MySQL 8 matches by the collation of the text)
constexpr auto kRegexFlags = std::regex::ECMAScript | std::regex::icase;

} // namespace

ValueClass Executor::class_of_expr(const ArithExpr& expr) {
    if (auto* col = std::get_if<ArithExpr::Col>(&expr.data)) return col->cls;
    if (auto* sq = std::get_if<ArithExpr::Subquery>(&expr.data)) return sq->cls;
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

thread_local Executor::StatementContext* Executor::statement_context_ = nullptr;

Executor::StatementScope::StatementScope(Executor& ex, SharedDatabase& s, bool writes)
    : ex_(ex), context_{&ex, &s, ++ex.statement_serial_}, previous_(statement_context_), writes_(writes) {
    statement_context_ = &context_;
}

Executor::StatementScope::~StatementScope() {
    statement_context_ = previous_;
    if (writes_) ex_.write_serial_++; // (and what the subqueries answered before is thrown away by the next one that asks)
}

std::string Executor::scalar_subquery_value(SharedDatabase& s, const ArithExpr::Subquery& sq, const Row& row, std::uint64_t statement) {
    if (!sq.query) return EXECUTOR_NULL_VALUE;
    if (sq.correlated < 0) sq.correlated = refers_outside(*sq.query) ? 1 : 0;
    const bool correlated = sq.correlated == 1;
    // a subquery that names nothing of the row answers the same for every row of the statement -- until a statement that writes has run
    if (!correlated && sq.answered_in == statement && sq.answered_after == write_serial_) return sq.answer;
    const auto values = extract_values_from_output(run_subquery(s, *sq.query, row, correlated));
    if (values.size() > 1) throw StatementError("Subquery returns more than 1 row");
    std::string value = values.empty() ? std::string(EXECUTOR_NULL_VALUE) : values.front();
    if (!correlated) {
        sq.answer = value;
        sq.answered_in = statement;
        sq.answered_after = write_serial_;
    }
    return value;
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

std::string Executor::eval_arith_exact(const Row& row, const ArithExpr& expr, bool* approx) {
    if (approx) *approx = false;
    if (std::holds_alternative<ArithExpr::Add>(expr.data) || std::holds_alternative<ArithExpr::Sub>(expr.data) ||
        std::holds_alternative<ArithExpr::Mul>(expr.data) || std::holds_alternative<ArithExpr::Div>(expr.data)) {
        return arithmetic_text(expr, [&](const ArithExpr& leaf) { return eval_arith(row, leaf); }, true, approx);
    }
    return eval_arith(row, expr);
}

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
    if (std::holds_alternative<ArithExpr::Add>(expr.data) || std::holds_alternative<ArithExpr::Sub>(expr.data) ||
        std::holds_alternative<ArithExpr::Mul>(expr.data) || std::holds_alternative<ArithExpr::Div>(expr.data)) {
        return arithmetic_text(expr, [&](const ArithExpr& leaf) { return eval_arith(row, leaf); });
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
        {
            std::string upper = v->name;
            for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (is_builtin_function(upper)) {
                const bool exact_args = takes_exact_numbers(upper);
                // the arguments are worked out once, as values (NULL is NULL, not the text of a string), and the function is told what each one is
                std::vector<std::string> raw, values;
                std::vector<char> kinds;
                raw.reserve(v->args.size());
                values.reserve(v->args.size());
                kinds.reserve(v->args.size());
                for (auto& a : v->args) {
                    if (auto* c = std::get_if<ArithExpr::Col>(&a.data)) {
                        raw.push_back(c->name);
                        values.push_back(eval_arith(row, a));
                        kinds.push_back(c->approx || c->cls == ValueClass::Text ? 'D' : 'C');
                    } else if (auto* sv = std::get_if<ArithExpr::Str>(&a.data)) {
                        raw.push_back("'" + sv->value + "'");
                        values.push_back(sv->value);
                        kinds.push_back('S');
                    } else if (auto* n = std::get_if<ArithExpr::Num>(&a.data)) {
                        raw.push_back(n->value);
                        values.push_back(n->value);
                        kinds.push_back('N');
                    } else {
                        bool approx = false;
                        std::string value = exact_args ? eval_arith_exact(row, a, &approx) : eval_arith(row, a);
                        raw.push_back("'" + value + "'");
                        values.push_back(std::move(value));
                        kinds.push_back(approx ? 'D' : 'E');
                    }
                }
                return call_builtin_with_values(upper, raw, values, kinds);
            }
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
    if (auto* v = std::get_if<ArithExpr::Subquery>(&expr.data)) {
        StatementContext* c = statement_context_;
        if (!c) throw StatementError("A subquery is not allowed here");
        return c->ex->scalar_subquery_value(*c->s, *v, row, c->statement);
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
        std::string lv = eval_arith_exact(row, *v->lhs), rv = eval_arith_exact(row, *v->rhs);
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
    // `x IN (SELECT ...)`, `EXISTS (SELECT ...)`, `x > (SELECT ...)` where a condition is only a value or a test of a join (a CASE's WHEN, IF(), a
    // select item, the ON of a JOIN): answered by the statement that is running (WHERE and HAVING go through eval_single_with_subquery themselves)
    if (std::holds_alternative<ConditionValue::Subquery>(cond.value.data)) {
        StatementContext* c = statement_context_;
        if (!c) throw StatementError("A subquery is not allowed here");
        return c->ex->eval_single_with_subquery(*c->s, row, cond);
    }
    // (a comparison reads all the digits of an arithmetic expression: 8 / 59.27 >= 0.135 is false; LIKE and REGEXP read it as it shows)
    const bool compares_numbers = cond.op != Operator::Like && cond.op != Operator::NotLike && cond.op != Operator::Regexp && cond.op != Operator::NotRegexp;
    std::string val = compares_numbers ? eval_arith_exact(row, cond.left) : eval_arith(row, cond.left);
    // how the two sides compare: what each is (set by the binder; an expression that was not bound tells itself)
    const ValueClass left_class = cond.left_class != ValueClass::Unknown ? cond.left_class : class_of_expr(cond.left);

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
        resolved = compares_numbers ? eval_arith_exact(row, av->expr) : eval_arith(row, av->expr);
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
                return tri(std::regex_search(val, std::regex(*effective_lit, kRegexFlags)));
            } catch (...) {
                return Tri::False;
            }
        case Operator::NotRegexp:
            try {
                return tri(!std::regex_search(val, std::regex(*effective_lit, kRegexFlags)));
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
