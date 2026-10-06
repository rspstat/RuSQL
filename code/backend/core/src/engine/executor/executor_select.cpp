// Faithful port of the SELECT execution path from rusql-core/src/engine/executor.rs
// (Phase 8b): exec_select, exec_select_with_subquery, format_result, and the
// aggregate-function helpers (agg_label, extract_agg_refs_from_cond,
// compute_agg_from_key). See executor.hpp's exec_select declaration for the specific,
// documented scope exclusions (planner index fast paths, window functions,
// INFORMATION_SCHEMA, FROM `_dual_`, SELECT-list subqueries).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_set>

#include "engine/column_text.hpp"
#include "engine/join.hpp"
#include "engine/planner.hpp"
#include "engine/parallel_util.hpp"
#include "engine/numeric_text.hpp"
#include "engine/parser/parser.hpp"
#include "engine/storage/numeric_key.hpp"

namespace engine {

namespace {

std::optional<double> parse_f64(const std::string& s) { return parse_number(s); }

std::string format_4dp(double v) { return format_places(v, 4); }

// MIN or MAX of the values: the smallest / largest, comparing numbers as numbers when every value is one (integers exactly) and as text
// otherwise, and the value itself as it is stored (not a number printed again). NULL for no value.
std::string extreme_text(const std::vector<const std::string*>& values, bool smallest, ValueClass cls) {
    if (values.empty()) return EXECUTOR_NULL_VALUE;
    const bool numeric = cls == ValueClass::Number ||
                         (cls != ValueClass::Text && std::all_of(values.begin(), values.end(), [](const std::string* v) { return parse_f64(*v).has_value(); }));
    const std::string* best = values.front();
    for (const std::string* v : values) {
        const int order = numeric ? compare_numbers(*v, *best) : v->compare(*best);
        if (smallest ? order < 0 : order > 0) best = v;
    }
    return *best;
}

// STDDEV and VARIANCE (of the whole population, as MySQL's) and MEDIAN of the values, 4 places; NULL for no value.
std::string spread_text(const AggFunc& func, const std::vector<const std::string*>& values) {
    if (values.empty()) return EXECUTOR_NULL_VALUE;
    std::vector<double> nums;
    nums.reserve(values.size());
    for (const std::string* v : values) nums.push_back(text_to_number(*v));
    if (std::holds_alternative<AggFunc::Median>(func.data)) {
        std::sort(nums.begin(), nums.end());
        const std::size_t n = nums.size();
        return format_4dp(n % 2 ? nums[n / 2] : (nums[n / 2 - 1] + nums[n / 2]) / 2.0);
    }
    const double mean = std::accumulate(nums.begin(), nums.end(), 0.0) / static_cast<double>(nums.size());
    double variance = 0.0;
    for (double v : nums) variance += (v - mean) * (v - mean);
    variance /= static_cast<double>(nums.size());
    return format_4dp(std::holds_alternative<AggFunc::Stddev>(func.data) ? std::sqrt(variance) : variance);
}

// The values of `values` that differ, the first of each. Numbers differ by value (an expression holds "7" for one row and "7.00" for another, as
// COALESCE(w, 7) does: one number), texts by their text ('7' and '07' are two strings); an argument that is not a text is read as numbers when all
// of its values are, as MIN and MAX read it.
std::vector<const std::string*> distinct_texts(const std::vector<const std::string*>& values, ValueClass cls) {
    const bool by_value = cls != ValueClass::Text && std::all_of(values.begin(), values.end(), [](const std::string* v) { return parse_f64(*v).has_value(); });
    std::unordered_set<std::string> seen;
    std::vector<const std::string*> out;
    for (const std::string* v : values) {
        if (seen.insert(by_value ? normalize_numeric_key(*v) : *v).second) out.push_back(v);
    }
    return out;
}

// NULL sorts before every value (MySQL: first in ASC, last in DESC); NULLs are equal. It used to be compared as the text "NULL", which put
// it between 'Alice' and 'Zed' and after every number.
int cmp_key(const std::string& a, const std::string& b, ValueClass cls) {
    const bool a_null = a == "NULL", b_null = b == "NULL";
    if (a_null || b_null) return a_null == b_null ? 0 : (a_null ? -1 : 1);
    return compare_classed(cls, cls, a, b);
}

// What WHERE's `=` says: equal as numbers when both sides parse as numbers, else equal as text.
bool same_value(const std::string& a, const std::string& b) {
    auto pa = parse_f64(a), pb = parse_f64(b);
    return pa && pb ? compare_numbers(a, b) == 0 : a == b; // (integers exactly)
}

// Values of the B+Tree entries whose key is `key` the way WHERE sees it: a probe for "7" finds the entries "7", "7.0"
// and "07" (an exact-text search found only the first). The keys inside the widened range are checked one by one,
// because the range also reaches the neighbouring doubles.
std::vector<std::string> equal_entries(const BPlusTree& tree, const std::string& key) {
    std::vector<std::string> out;
    auto lo = tree_bound(tree, key, true), hi = tree_bound(tree, key, false);
    if (!lo || !hi) {
        if (auto v = tree.search(key)) out.push_back(std::move(*v));
        return out;
    }
    for (auto& k : tree.range_keys(*lo, *hi)) {
        if (!same_value(key, k)) continue;
        if (auto v = tree.search(k)) out.push_back(std::move(*v));
    }
    return out;
}

// How a column named in ORDER BY is read from a row: Executor::get_col (private, so the callers pass it in). The exact key
// first, then the same "table.col" / bare-name resolution WHERE, GROUP BY and the select list use -- the parser has already
// replaced an alias by its table name. Looking the name up as the row's own key only made `ORDER BY t.col` (a row keeps
// its columns under their bare names) read nothing, so every row compared equal and nothing was sorted.
using RowLookup = const std::string* (*)(const Row&, const std::string&);

// Mirrors Rust's multi-key Ordering-based ORDER BY comparator as a strict-weak-order "less than" predicate.
bool row_order_less(const Row& a, const Row& b, const std::vector<OrderBy>& order_by, RowLookup lookup) {
    for (auto& ord : order_by) {
        const std::string* pa = lookup(a, ord.column);
        const std::string* pb = lookup(b, ord.column);
        std::string av = pa ? *pa : std::string();
        std::string bv = pb ? *pb : std::string();
        int c = cmp_key(av, bv, ord.cls);
        if (!ord.ascending) c = -c;
        if (c != 0) return c < 0;
    }
    return false;
}

// Which of the joined tables does a WHERE conjunct read? The index into PushdownScope::names (0 = the FROM table, then
// the joined tables in order), or -1 when it is not certain. A conjunct that reads one table only can be applied to
// that table's rows BEFORE the join (the join then builds far fewer merged rows); the full WHERE is still evaluated on
// the joined rows afterwards, so a conjunct pushed down is only ever a shortcut that drops rows the WHERE would drop.
struct PushdownScope {
    std::vector<std::string> names; // as the engine names them, "<db>.<table>"
    std::vector<std::string> bare;  // as a query writes them in "<table>.<column>"
    std::vector<std::unordered_set<std::string>> columns;
};

bool names_table(const PushdownScope& sc, std::size_t i, const std::string& qualifier) {
    return qualifier == sc.names[i] || qualifier == sc.bare[i];
}

// The table whose value a column reference reads once the tables are merged: "<table>.<col>" reads that table's
// column (get_col finds a joined table's value under its qualified key, and the FROM table's under the plain key);
// a bare name reads the FIRST table that has it (merge_right never overwrites a key that is already there).
int column_owner(const std::string& name, const PushdownScope& sc) {
    if (auto cut = name.rfind('.'); cut != std::string::npos) {
        std::string q = name.substr(0, cut), c = name.substr(cut + 1);
        for (std::size_t i = 0; i < sc.names.size(); i++) {
            if (names_table(sc, i, q)) return sc.columns[i].count(c) ? static_cast<int>(i) : -1;
        }
        return -1;
    }
    for (std::size_t i = 0; i < sc.columns.size(); i++) {
        if (sc.columns[i].count(name)) return static_cast<int>(i);
    }
    return -1;
}

// Owner of a single comparison. Only `<column> <op> <literal / list / range>` qualifies: a right-hand side that is an
// expression or a subquery, a function on the left, or a literal that could be read as a column name (the evaluation
// looks an identifier-looking literal up as a column first) all answer -1.
int leaf_owner(const Condition& c, const PushdownScope& sc) {
    auto* col = std::get_if<ArithExpr::Col>(&c.left.data);
    if (!col) return -1;
    std::string lower = col->name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (lower == "true" || lower == "false") return -1;
    switch (c.op) {
        case Operator::Eq: case Operator::Ne: case Operator::Gt: case Operator::Lt: case Operator::Gte: case Operator::Lte:
        case Operator::Like: case Operator::NotLike: case Operator::Regexp: case Operator::NotRegexp:
        case Operator::IsNull: case Operator::IsNotNull: case Operator::Between: case Operator::NotBetween:
        case Operator::In: case Operator::NotIn:
            break;
        default:
            return -1;
    }
    if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
        const std::string& v = lit->value;
        bool ident_like = !lit->quoted && !v.empty() && (std::isalpha(static_cast<unsigned char>(v[0])) || v[0] == '_') && !parse_f64(v).has_value();
        if (ident_like) {
            if (v.find('.') != std::string::npos) return -1;
            for (auto& cols : sc.columns) {
                if (cols.count(v)) return -1;
            }
        }
    } else if (!std::holds_alternative<ConditionValue::Between>(c.value.data) && !std::holds_alternative<ConditionValue::LiteralList>(c.value.data)) {
        return -1;
    }
    return column_owner(col->name, sc);
}

int expr_owner(const CondExpr& e, const PushdownScope& sc) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) {
        int l = expr_owner(*a->lhs, sc), r = expr_owner(*a->rhs, sc);
        return l == r ? l : -1;
    }
    if (auto* o = std::get_if<CondExpr::Or>(&e.data)) {
        int l = expr_owner(*o->lhs, sc), r = expr_owner(*o->rhs, sc);
        return l == r ? l : -1;
    }
    if (auto* n = std::get_if<CondExpr::Not>(&e.data)) return expr_owner(*n->inner, sc);
    if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) return leaf_owner(leaf->condition, sc);
    return -1;
}

void and_conjuncts(const CondExpr& e, std::vector<const CondExpr*>& out) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) {
        and_conjuncts(*a->lhs, out);
        and_conjuncts(*a->rhs, out);
    } else {
        out.push_back(&e);
    }
}

// A top-level AND-ed part of the ON condition of the form `<a> = <b>` where exactly one side is "<right table>.<column>"
// of the table being joined: {the other side, that column}. Every pair that satisfies the whole ON satisfies this part.
// `<column> = <constant>` among the AND-ed parts of a WHERE, on a column of `table` (unqualified, or qualified with the table's
// full or bare name), where the constant cannot be read as a column of the table (an identifier-looking literal is looked
// up as a column first): {column, constant}.
std::optional<std::pair<std::string, std::string>> constant_equality_part(const CondExpr& where, const std::string& table,
                                                                           const std::unordered_set<std::string>& columns) {
    std::string bare = table.substr(table.rfind('.') == std::string::npos ? 0 : table.rfind('.') + 1);
    std::vector<const CondExpr*> parts;
    and_conjuncts(where, parts);
    for (const CondExpr* part : parts) {
        auto* leaf = std::get_if<CondExpr::Leaf>(&part->data);
        if (!leaf || leaf->condition.op != Operator::Eq) continue;
        auto* col = std::get_if<ArithExpr::Col>(&leaf->condition.left.data);
        auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data);
        if (!col || !lit) continue;
        // (the hash is of the texts of the column: it answers a text column compared with a string and a number column with a number)
        if (col->cls == ValueClass::Text && !lit->quoted) continue;
        if (col->cls == ValueClass::Number && !parse_number(lit->value)) continue;
        std::string name = col->name;
        if (auto cut = name.rfind('.'); cut != std::string::npos) {
            std::string q = name.substr(0, cut);
            if (q != table && q != bare) continue;
            name = name.substr(cut + 1);
        }
        if (!columns.count(name)) continue;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if (lower == "true" || lower == "false") continue;
        const std::string& v = lit->value;
        bool ident_like = !lit->quoted && !v.empty() && (std::isalpha(static_cast<unsigned char>(v[0])) || v[0] == '_') && !parse_f64(v).has_value();
        if (ident_like && (v.find('.') != std::string::npos || columns.count(v))) continue;
        return std::make_pair(name, v);
    }
    return std::nullopt;
}

struct OnEquality {
    std::string left_ref;  // the side that is not a column of the joined table
    std::string right_col; // the column of the joined table
    ValueClass left_class, right_class; // what each side holds
};

std::optional<OnEquality> equality_part_of_on(const CondExpr& on, const std::string& right_full, const std::string& right_bare,
                                              const std::unordered_set<std::string>& right_cols) {
    auto as_right = [&](const std::string& ref) -> std::optional<std::string> {
        auto cut = ref.rfind('.');
        if (cut == std::string::npos) return std::nullopt;
        std::string q = ref.substr(0, cut), c = ref.substr(cut + 1);
        if ((q == right_full || q == right_bare) && right_cols.count(c)) return c;
        return std::nullopt;
    };
    std::vector<const CondExpr*> parts;
    and_conjuncts(on, parts);
    for (const CondExpr* part : parts) {
        auto* leaf = std::get_if<CondExpr::Leaf>(&part->data);
        if (!leaf || leaf->condition.op != Operator::Eq) continue;
        auto* l = std::get_if<ArithExpr::Col>(&leaf->condition.left.data);
        auto* r = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data);
        if (!l || !r || r->quoted) continue;
        auto rc = as_right(r->value), lc = as_right(l->name);
        if (rc && !lc) return OnEquality{l->name, *rc, leaf->condition.left_class, leaf->condition.right_class};
        if (lc && !rc) return OnEquality{r->value, *lc, leaf->condition.right_class, leaf->condition.left_class};
    }
    return std::nullopt;
}

// One more value of a multi-column key as bytes (length first, so ("ab","c") and ("a","bc") differ): GROUP BY and DISTINCT
// look their keys up in a hash table of these instead of comparing key vectors pairwise.
void append_key_part(std::string& key, const std::string& value) {
    std::uint32_t n = static_cast<std::uint32_t>(value.size());
    key.append(reinterpret_cast<const char*>(&n), sizeof n);
    key += value;
}

// The order row_order_less defines, for a whole row set at once: indexes into `rows`, stable. row_order_less looked both
// columns up in both rows (two hash lookups, two string copies) and parsed both as numbers on EVERY comparison -- about
// 0.45 us each, 16 comparisons per row of a 50,000-row sort. Here each row's keys are read and parsed once.
std::vector<std::size_t> order_rows(const std::vector<const Row*>& rows, const std::vector<OrderBy>& order_by, RowLookup lookup) {
    struct Cell {
        bool numeric = false;
        bool null = false;
        bool is_int = false; // an integer is compared as an integer (a double cannot tell 2^53 + 1 from 2^53)
        std::int64_t whole = 0;
        double num = 0;
        const std::string* text = nullptr;
    };
    static const std::string empty;
    const std::size_t ncols = order_by.size();
    std::vector<Cell> cells(rows.size() * ncols);
    for (std::size_t i = 0; i < rows.size(); i++) {
        for (std::size_t c = 0; c < ncols; c++) {
            const std::string* found = lookup(*rows[i], order_by[c].column);
            Cell& cell = cells[i * ncols + c];
            cell.text = found ? found : &empty;
            cell.null = *cell.text == "NULL";
            // a text column is sorted as text whatever its values look like; a number column as numbers (a text in one by its leading number)
            const ValueClass cls = order_by[c].cls;
            if (cls != ValueClass::Text && !cell.null) {
                if (auto whole = parse_int64_text(*cell.text)) {
                    cell.numeric = cell.is_int = true;
                    cell.whole = *whole;
                    cell.num = static_cast<double>(*whole);
                } else if (auto v = parse_f64(*cell.text)) {
                    cell.numeric = true;
                    cell.num = *v;
                } else if (cls == ValueClass::Number) {
                    cell.numeric = true;
                    cell.num = text_to_number(*cell.text);
                }
            }
        }
    }
    std::vector<std::size_t> order(rows.size());
    for (std::size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        for (std::size_t c = 0; c < ncols; c++) {
            const Cell& x = cells[a * ncols + c];
            const Cell& y = cells[b * ncols + c];
            int cmp;
            if (x.null || y.null) cmp = x.null == y.null ? 0 : (x.null ? -1 : 1); // same as cmp_key
            else if (x.numeric && y.numeric) cmp = x.is_int && y.is_int ? (x.whole < y.whole ? -1 : (x.whole > y.whole ? 1 : 0)) : (x.num < y.num ? -1 : (x.num > y.num ? 1 : 0));
            else cmp = *x.text < *y.text ? -1 : (*x.text > *y.text ? 1 : 0);
            if (!order_by[c].ascending) cmp = -cmp;
            if (cmp != 0) return cmp < 0;
        }
        return false;
    });
    return order;
}

// Faithful port of executor.rs's own free-standing `arith_to_str` — a SEPARATE,
// differently-formatted function from Parser::arith_to_string (which joins Func args
// with ", " and puts spaces around binary operators). This one (used for default
// SelectColumn::Expr headers, both here and in the `_dual_` block below) has no spaces
// at all: "a+b", "POW(2,10)". Using Parser::arith_to_string here was an earlier port
// mistake — the two functions look similar but Rust genuinely has both, and this is
// the one executor.rs's header computation actually calls.
std::string arith_to_str(const ArithExpr& expr) {
    return std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ArithExpr::Col>) return alt.name;
            else if constexpr (std::is_same_v<T, ArithExpr::Num>) return alt.value;
            else if constexpr (std::is_same_v<T, ArithExpr::Str>) return "'" + alt.value + "'";
            else if constexpr (std::is_same_v<T, ArithExpr::Add>) return arith_to_str(*alt.lhs) + "+" + arith_to_str(*alt.rhs);
            else if constexpr (std::is_same_v<T, ArithExpr::Sub>) return arith_to_str(*alt.lhs) + "-" + arith_to_str(*alt.rhs);
            else if constexpr (std::is_same_v<T, ArithExpr::Mul>) return arith_to_str(*alt.lhs) + "*" + arith_to_str(*alt.rhs);
            else if constexpr (std::is_same_v<T, ArithExpr::Div>) return arith_to_str(*alt.lhs) + "/" + arith_to_str(*alt.rhs);
            else if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                if (alt.name == "CASE") return "CASE";
                std::string out = alt.name + "(";
                for (std::size_t i = 0; i < alt.args.size(); i++) {
                    if (i) out += ",";
                    out += arith_to_str(alt.args[i]);
                }
                return out + ")";
            } else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) {
                return arith_to_str(*alt.lhs) + alt.op + arith_to_str(*alt.rhs);
            } else if constexpr (std::is_same_v<T, ArithExpr::Pred>) {
                try {
                    return Parser::cond_to_string(*alt.cond);
                } catch (const ParseError&) {
                    return "";
                }
            } else {
                return "";
            }
        },
        expr.data);
}

// Rust's `_dual_` column-header computation formats SelectColumn::Agg as
// `format!("{:?}({})", func, col)` — i.e. AggFunc's derived Debug output, which is its
// Rust identifier name (PascalCase), NOT the SQL keyword agg_label() produces elsewhere.
// The 3 struct-payload variants (GroupConcat/CountCase/SumCase) would derive-Debug their
// fields too; since aggregates over the single synthetic dual row are a deep, virtually
// never-hit corner (aggregation needs a real row set), this approximates them rather
// than fully replicating nested struct Debug output.
std::string debug_agg_func_dual(const AggFunc& func) {
    if (std::holds_alternative<AggFunc::Count>(func.data)) return "Count";
    if (std::holds_alternative<AggFunc::CountDistinct>(func.data)) return "CountDistinct";
    if (std::holds_alternative<AggFunc::Sum>(func.data)) return "Sum";
    if (std::holds_alternative<AggFunc::SumDistinct>(func.data)) return "SumDistinct";
    if (std::holds_alternative<AggFunc::Avg>(func.data)) return "Avg";
    if (std::holds_alternative<AggFunc::AvgDistinct>(func.data)) return "AvgDistinct";
    if (std::holds_alternative<AggFunc::Min>(func.data)) return "Min";
    if (std::holds_alternative<AggFunc::Max>(func.data)) return "Max";
    if (std::holds_alternative<AggFunc::Stddev>(func.data)) return "Stddev";
    if (std::holds_alternative<AggFunc::Variance>(func.data)) return "Variance";
    if (auto* gc = std::get_if<AggFunc::GroupConcat>(&func.data)) return "GroupConcat { separator: \"" + gc->separator + "\" }";
    if (std::holds_alternative<AggFunc::CountCase>(func.data)) return "CountCase { .. }";
    if (std::holds_alternative<AggFunc::SumCase>(func.data)) return "SumCase { .. }";
    if (std::holds_alternative<AggFunc::BitAnd>(func.data)) return "BitAnd";
    if (std::holds_alternative<AggFunc::BitOr>(func.data)) return "BitOr";
    if (std::holds_alternative<AggFunc::JsonAgg>(func.data)) return "JsonAgg";
    if (std::holds_alternative<AggFunc::ArrayAgg>(func.data)) return "ArrayAgg";
    if (std::holds_alternative<AggFunc::Median>(func.data)) return "Median";
    return "";
}
} // namespace

std::string Executor::window_func_default_label(WindowFunc func) {
    switch (func) {
        case WindowFunc::RowNumber: return "row_number";
        case WindowFunc::Rank: return "rank";
        case WindowFunc::DenseRank: return "dense_rank";
        case WindowFunc::Lag: return "lag";
        case WindowFunc::Lead: return "lead";
        case WindowFunc::FirstValue: return "first_value";
        case WindowFunc::LastValue: return "last_value";
        case WindowFunc::NthValue: return "nth_value";
        case WindowFunc::Ntile: return "ntile";
        case WindowFunc::PercentRank: return "percent_rank";
        case WindowFunc::CumeDist: return "cume_dist";
        case WindowFunc::Sum: return "sum";
        case WindowFunc::Avg: return "avg";
        case WindowFunc::Count: return "count";
        case WindowFunc::Min: return "min";
        case WindowFunc::Max: return "max";
    }
    return "";
}

std::string Executor::agg_label(const AggFunc& func, const std::string& col) {
    // the conditional aggregates of one select list are told apart by the number the parser put behind their placeholder (`__case__2`)
    const std::string case_number = col.rfind("__case__", 0) == 0 ? col.substr(8) : std::string();
    if (std::holds_alternative<AggFunc::Count>(func.data)) return "COUNT(" + col + ")";
    if (std::holds_alternative<AggFunc::CountDistinct>(func.data)) return "COUNT(DISTINCT " + col + ")";
    if (std::holds_alternative<AggFunc::Sum>(func.data)) return "SUM(" + col + ")";
    if (std::holds_alternative<AggFunc::SumDistinct>(func.data)) return "SUM(DISTINCT " + col + ")";
    if (std::holds_alternative<AggFunc::Avg>(func.data)) return "AVG(" + col + ")";
    if (std::holds_alternative<AggFunc::AvgDistinct>(func.data)) return "AVG(DISTINCT " + col + ")";
    if (std::holds_alternative<AggFunc::Min>(func.data)) return "MIN(" + col + ")";
    if (std::holds_alternative<AggFunc::Max>(func.data)) return "MAX(" + col + ")";
    if (std::holds_alternative<AggFunc::Stddev>(func.data)) return "STDDEV(" + col + ")";
    if (std::holds_alternative<AggFunc::Variance>(func.data)) return "VARIANCE(" + col + ")";
    if (std::holds_alternative<AggFunc::GroupConcat>(func.data)) return "GROUP_CONCAT(" + col + ")";
    if (std::holds_alternative<AggFunc::CountCase>(func.data)) return "COUNT(CASE)" + case_number;
    if (std::holds_alternative<AggFunc::SumCase>(func.data)) return "SUM(CASE)" + case_number;
    if (std::holds_alternative<AggFunc::BitAnd>(func.data)) return "BIT_AND(" + col + ")";
    if (std::holds_alternative<AggFunc::BitOr>(func.data)) return "BIT_OR(" + col + ")";
    if (std::holds_alternative<AggFunc::JsonAgg>(func.data)) return "JSON_AGG(" + col + ")";
    if (std::holds_alternative<AggFunc::ArrayAgg>(func.data)) return "ARRAY_AGG(" + col + ")";
    if (std::holds_alternative<AggFunc::Median>(func.data)) return "MEDIAN(" + col + ")";
    return "";
}

void Executor::collect_agg_refs_arith(const ArithExpr& expr, std::vector<std::string>& out) {
    if (auto* v = std::get_if<ArithExpr::Col>(&expr.data)) {
        std::string upper = v->name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        bool is_agg_ref = upper.rfind("COUNT(", 0) == 0 || upper.rfind("SUM(", 0) == 0 || upper.rfind("AVG(", 0) == 0 ||
                           upper.rfind("MIN(", 0) == 0 || upper.rfind("MAX(", 0) == 0;
        if (is_agg_ref && std::find(out.begin(), out.end(), v->name) == out.end()) out.push_back(v->name);
        return;
    }
    if (auto* v = std::get_if<ArithExpr::Add>(&expr.data)) {
        collect_agg_refs_arith(*v->lhs, out);
        collect_agg_refs_arith(*v->rhs, out);
    } else if (auto* v = std::get_if<ArithExpr::Sub>(&expr.data)) {
        collect_agg_refs_arith(*v->lhs, out);
        collect_agg_refs_arith(*v->rhs, out);
    } else if (auto* v = std::get_if<ArithExpr::Mul>(&expr.data)) {
        collect_agg_refs_arith(*v->lhs, out);
        collect_agg_refs_arith(*v->rhs, out);
    } else if (auto* v = std::get_if<ArithExpr::Div>(&expr.data)) {
        collect_agg_refs_arith(*v->lhs, out);
        collect_agg_refs_arith(*v->rhs, out);
    } else if (auto* v = std::get_if<ArithExpr::Func>(&expr.data)) {
        for (auto& a : v->args) collect_agg_refs_arith(a, out);
    } else if (auto* v = std::get_if<ArithExpr::Cmp>(&expr.data)) {
        collect_agg_refs_arith(*v->lhs, out);
        collect_agg_refs_arith(*v->rhs, out);
    } else if (auto* v = std::get_if<ArithExpr::Pred>(&expr.data)) {
        collect_agg_refs_cond(*v->cond, out);
    }
}

namespace {
// The aggregate calls written in the text of a scalar function's argument: `AVG(v)`, `SUM(v)/COUNT(*)` (the parser keeps a
// function's arguments as text). Only a call with a plain argument, as parse_arith_factor builds them.
void collect_agg_refs_text(const std::string& text, std::vector<std::string>& out) {
    static const char* names[] = {"COUNT", "SUM", "AVG", "MIN", "MAX"};
    bool in_string = false; // inside '...': text, never an aggregate call
    for (std::size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\'') in_string = !in_string;
        if (in_string) continue;
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_' || text[i - 1] == '.')) continue;
        for (const char* name : names) {
            const std::size_t n = std::strlen(name);
            if (i + n >= text.size() || text[i + n] != '(') continue;
            bool same = true;
            for (std::size_t k = 0; k < n; k++) same = same && std::toupper(static_cast<unsigned char>(text[i + k])) == name[k];
            if (!same) continue;
            const std::size_t close = text.find(')', i + n + 1);
            if (close == std::string::npos || text.find('(', i + n + 1) < close) break;
            std::string key = std::string(name) + text.substr(i + n, close - (i + n) + 1);
            if (std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
            i = close;
            break;
        }
    }
}
} // namespace

void Executor::column_agg_refs(const SelectColumn& column, std::vector<std::string>& out) {
    if (auto* e = std::get_if<SelectColumn::Expr>(&column.data)) {
        collect_agg_refs_arith(e->expr, out);
    } else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&column.data)) {
        for (auto& b : cw->branches) collect_agg_refs_cond(b.condition, out);
    } else if (auto* f = std::get_if<SelectColumn::Func>(&column.data)) {
        for (auto& a : f->args) collect_agg_refs_text(a, out);
    }
}

std::vector<std::string> Executor::select_agg_refs(const std::vector<SelectColumn>& columns) {
    std::vector<std::string> out;
    for (auto& c : columns) column_agg_refs(c, out);
    return out;
}

bool Executor::column_has_aggregate(const SelectColumn& column) {
    if (std::holds_alternative<SelectColumn::Agg>(column.data) || std::holds_alternative<SelectColumn::AggAlias>(column.data)) return true;
    std::vector<std::string> refs;
    column_agg_refs(column, refs);
    return !refs.empty();
}

bool Executor::columns_have_aggregate(const std::vector<SelectColumn>& columns) {
    return std::any_of(columns.begin(), columns.end(), [](const SelectColumn& c) { return column_has_aggregate(c); });
}

void Executor::collect_agg_refs_cond(const CondExpr& expr, std::vector<std::string>& out) {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) {
        collect_agg_refs_cond(*v->lhs, out);
        collect_agg_refs_cond(*v->rhs, out);
    } else if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) {
        collect_agg_refs_cond(*v->lhs, out);
        collect_agg_refs_cond(*v->rhs, out);
    } else if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) {
        collect_agg_refs_cond(*v->inner, out);
    } else if (auto* v = std::get_if<CondExpr::Leaf>(&expr.data)) {
        collect_agg_refs_arith(v->condition.left, out);
        // an aggregate on the right of the comparison too: `HAVING SUM(v) > AVG(w)`, `CASE WHEN MIN(v) > COUNT(*) ...`
        const auto& value = v->condition.value.data;
        if (auto* lit = std::get_if<ConditionValue::Literal>(&value)) {
            collect_agg_refs_text(lit->value, out);
        } else if (auto* between = std::get_if<ConditionValue::Between>(&value)) {
            collect_agg_refs_text(between->lo, out);
            collect_agg_refs_text(between->hi, out);
        } else if (auto* list = std::get_if<ConditionValue::LiteralList>(&value)) {
            for (auto& item : list->values) collect_agg_refs_text(item, out);
        } else if (auto* arith = std::get_if<ConditionValue::Arith>(&value)) {
            collect_agg_refs_arith(arith->expr, out);
        }
    }
}

std::vector<std::string> Executor::extract_agg_refs_from_cond(const CondExpr& expr) {
    std::vector<std::string> out;
    collect_agg_refs_cond(expr, out);
    return out;
}

// What the `MIN(x)` / `MAX(x)` that a HAVING or a select-list expression names holds: the binder gave the reference (a column named "MAX(code)")
// the class of its argument.
void collect_aggregate_classes(const CondExpr& e, std::unordered_map<std::string, ValueClass>& out);

void collect_aggregate_classes(const ArithExpr& e, std::unordered_map<std::string, ValueClass>& out) {
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
        if (col->cls != ValueClass::Unknown && col->name.find('(') != std::string::npos) out[col->name] = col->cls;
    } else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { collect_aggregate_classes(*v->lhs, out); collect_aggregate_classes(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { collect_aggregate_classes(*v->lhs, out); collect_aggregate_classes(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { collect_aggregate_classes(*v->lhs, out); collect_aggregate_classes(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { collect_aggregate_classes(*v->lhs, out); collect_aggregate_classes(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { collect_aggregate_classes(*v->lhs, out); collect_aggregate_classes(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Pred>(&e.data)) collect_aggregate_classes(*v->cond, out);
    else if (auto* v = std::get_if<ArithExpr::Func>(&e.data)) { for (auto& a : v->args) collect_aggregate_classes(a, out); }
}

void collect_aggregate_classes(const CondExpr& e, std::unordered_map<std::string, ValueClass>& out) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) { collect_aggregate_classes(*a->lhs, out); collect_aggregate_classes(*a->rhs, out); }
    else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { collect_aggregate_classes(*o->lhs, out); collect_aggregate_classes(*o->rhs, out); }
    else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) collect_aggregate_classes(*n->inner, out);
    else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
        collect_aggregate_classes(leaf->condition.left, out);
        if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) collect_aggregate_classes(value->expr, out);
    }
}

// The key under which the rows of `rows` hold an aggregate's argument. The argument is spelled as the query spells it (`b.id`,
// or `orders.amount` for `o.amount` through an alias); a row keeps its columns under their bare names and a joined-in table's
// also as `<db>.<table>.<column>`. Resolved once, on the first row: every row of one result has the same keys.
std::string Executor::resolve_arg_key(const std::vector<const Row*>& rows, const std::string& col) {
    if (rows.empty() || col == "*") return col;
    const Row& first = *rows.front();
    if (first.find(col) != first.end()) return col;
    if (const std::string* found = get_col(first, col)) {
        for (auto& kv : first) {
            if (&kv.second == found) return kv.first;
        }
    }
    return col;
}

// An aggregate that HAVING or a select-list expression/function/CASE asks for by name (`SUM(v)`, `COUNT(DISTINCT x)`), computed
// over the rows of one group with the rules of the select list: NULLs are skipped, COUNT(*) counts rows, SUM/AVG read the numeric
// values, MIN/MAX compare numbers when every value is one and text otherwise, and MIN/MAX, SUM and AVG of nothing are NULL. The answer
// is the one the select list shows (AVG with 4 places) except that a SUM keeps all its decimals.
std::string Executor::compute_agg_from_key(const std::string& key, const std::vector<const Row*>& grp, ValueClass arg_class) {
    std::string ku = key;
    std::transform(ku.begin(), ku.end(), ku.begin(), [](unsigned char c) { return std::toupper(c); });
    const auto lp = key.find('(');
    const auto rp = key.rfind(')');
    const bool is_count = ku.rfind("COUNT(", 0) == 0;
    if (lp == std::string::npos || rp == std::string::npos || rp <= lp) return is_count ? std::to_string(grp.size()) : "0";
    std::string inner = key.substr(lp + 1, rp - lp - 1);
    if (is_count && inner == "*") return std::to_string(grp.size());
    const bool distinct = inner.rfind("DISTINCT ", 0) == 0;
    if (distinct) inner.erase(0, 9);

    // the values of the argument that are not NULL (`COUNT(o.id) = 0` on a LEFT JOIN is how customers without orders are found:
    // every group has a row, only the matched ones have an `o.id`)
    const std::string arg = resolve_arg_key(grp, inner);
    std::vector<const std::string*> present;
    for (const Row* r_ptr : grp) {
        auto it = r_ptr->find(arg);
        if (it != r_ptr->end() && it->second != EXECUTOR_NULL_VALUE) present.push_back(&it->second);
    }
    // (the class the binder gave this reference is the class of the aggregate's own value for COUNT / SUM / AVG, not of its argument: a column
    // keeps its text, an expression is read as numbers when all of its values are)
    if (distinct) present = distinct_texts(present, is_expression_argument(inner) ? ValueClass::Unknown : ValueClass::Text);
    if (is_count) return std::to_string(present.size());

    if (ku.rfind("SUM(", 0) == 0) return sum_of_texts(present, false).value_or(EXECUTOR_NULL_VALUE);
    if (ku.rfind("AVG(", 0) == 0) return average_of_texts(present).value_or(EXECUTOR_NULL_VALUE);
    const bool is_min = ku.rfind("MIN(", 0) == 0;
    if (!is_min && ku.rfind("MAX(", 0) != 0) return "0";
    return extreme_text(present, is_min, arg_class);
}

Row Executor::compute_aggregates(const std::vector<const Row*>& grp, const std::vector<SelectColumn>& columns, bool allow_parallel) {
    Row out;
    for (auto& col : columns) {
        const AggFunc* func = nullptr;
        std::string col_name, label;
        const CondExpr* filter = nullptr;
        ValueClass arg_class = ValueClass::Unknown; // what the argument holds: how MIN / MAX compare
        if (auto* agg = std::get_if<SelectColumn::Agg>(&col.data)) {
            func = &agg->func;
            col_name = agg->source.empty() ? agg->col : agg->source;
            label = agg_label(*func, agg->col);
            arg_class = agg->arg_class;
            if (agg->filter) filter = &*agg->filter;
        } else if (auto* agg_a = std::get_if<SelectColumn::AggAlias>(&col.data)) {
            func = &agg_a->func;
            col_name = agg_a->source.empty() ? agg_a->col : agg_a->source;
            label = agg_a->alias;
            arg_class = agg_a->arg_class;
            if (agg_a->filter) filter = &*agg_a->filter;
        } else {
            continue;
        }

        // FILTER (WHERE ...): narrows just THIS aggregate's row set, independent of the
        // query's own WHERE/HAVING. `grp_ptr` is resolved BEFORE the shadowing
        // declaration below so its initializer never textually mentions `grp` itself
        // (self-referencing a not-yet-initialized reference is undefined behavior) --
        // once shadowed, every existing grp-referencing branch below (GroupConcat/
        // CountCase/SumCase/Min-Max/BitAnd-BitOr/JsonAgg/the generic numeric path)
        // transparently applies to the filtered rows with no other changes needed.
        std::vector<const Row*> filtered_storage;
        const std::vector<const Row*>* grp_ptr = &grp;
        if (filter) {
            for (const Row* r_ptr : grp) {
                const Row& r = *r_ptr;
                if (eval_condexpr(r, *filter)) filtered_storage.push_back(r_ptr);
            }
            grp_ptr = &filtered_storage;
        }
        const std::vector<const Row*>& grp = *grp_ptr;
        col_name = resolve_arg_key(grp, col_name);

        if (auto* gc = std::get_if<AggFunc::GroupConcat>(&func->data)) {
            std::vector<std::string> strs;
            for (const Row* r_ptr : grp) {
                const Row& r = *r_ptr;
                auto it = r.find(col_name);
                if (it != r.end() && it->second != EXECUTOR_NULL_VALUE) strs.push_back(it->second);
            }
            std::string joined;
            for (std::size_t i = 0; i < strs.size(); i++) {
                if (i) joined += gc->separator;
                joined += strs[i];
            }
            out[label] = strs.empty() ? EXECUTOR_NULL_VALUE : joined; // no value to join: NULL, not ''
            continue;
        }
        if (std::holds_alternative<AggFunc::JsonAgg>(func->data) || std::holds_alternative<AggFunc::ArrayAgg>(func->data)) {
            // ARRAY_AGG shares JSON_AGG's exact implementation -- this engine has no
            // distinct array storage type, so both render as a JSON array text value;
            // ArrayAgg only exists as its own AggFunc alternative so labels/EXPLAIN stay
            // faithful to what the user actually wrote.
            nlohmann::json arr = nlohmann::json::array();
            for (const Row* r_ptr : grp) {
                const Row& r = *r_ptr;
                auto it = r.find(col_name);
                if (it == r.end() || it->second == EXECUTOR_NULL_VALUE) {
                    arr.push_back(nullptr);
                    continue;
                }
                // Whole numbers are written as integers so JSON_AGG(id)
                // produces clean [1, 2, 3] instead of [1.0, 2.0, 3.0].
                if (auto p = parse_f64(it->second); p && *p == std::trunc(*p)) arr.push_back(static_cast<std::int64_t>(*p));
                else if (p) arr.push_back(*p);
                else arr.push_back(it->second);
            }
            out[label] = grp.empty() ? EXECUTOR_NULL_VALUE : arr.dump(); // no row: NULL, not []
            continue;
        }
        if (auto* cc = std::get_if<AggFunc::CountCase>(&func->data)) {
            std::size_t count = 0;
            for (const Row* row_ptr : grp) {
                const Row& row = *row_ptr;
                auto resolve = [&](const std::string& sv) -> std::string {
                    const std::string* v = get_col(row, sv);
                    return v ? *v : sv;
                };
                std::string val = cc->else_val ? resolve(*cc->else_val) : EXECUTOR_NULL_VALUE;
                for (auto& b : cc->branches) {
                    if (eval_condexpr(row, b.condition)) {
                        val = resolve(b.result);
                        break;
                    }
                }
                if (!val.empty() && val != EXECUTOR_NULL_VALUE && val != "0") count++;
            }
            out[label] = std::to_string(count);
            continue;
        }
        if (auto* sc = std::get_if<AggFunc::SumCase>(&func->data)) {
            std::vector<std::string> results; // the CASE result of each row that is not NULL
            for (const Row* row_ptr : grp) {
                const Row& row = *row_ptr;
                auto resolve = [&](const std::string& sv) -> std::string {
                    const std::string* v = get_col(row, sv);
                    return v ? *v : sv;
                };
                std::string val = sc->else_val ? resolve(*sc->else_val) : EXECUTOR_NULL_VALUE;
                for (auto& b : sc->branches) {
                    if (eval_condexpr(row, b.condition)) {
                        val = resolve(b.result);
                        break;
                    }
                }
                if (val != EXECUTOR_NULL_VALUE) results.push_back(std::move(val));
            }
            std::vector<const std::string*> present;
            for (const std::string& v : results) present.push_back(&v);
            out[label] = sum_of_texts(present, true).value_or(EXECUTOR_NULL_VALUE);
            continue;
        }
        if (std::holds_alternative<AggFunc::Min>(func->data) || std::holds_alternative<AggFunc::Max>(func->data)) {
            std::vector<const std::string*> present;
            for (const Row* r_ptr : grp) {
                auto it = r_ptr->find(col_name);
                if (it != r_ptr->end() && it->second != EXECUTOR_NULL_VALUE) present.push_back(&it->second);
            }
            out[label] = extreme_text(present, std::holds_alternative<AggFunc::Min>(func->data), arg_class);
            continue;
        }
        if (std::holds_alternative<AggFunc::BitAnd>(func->data) || std::holds_alternative<AggFunc::BitOr>(func->data)) {
            bool is_and = std::holds_alternative<AggFunc::BitAnd>(func->data);
            // BIT_AND over an empty set is the all-1s identity (getting this wrong as 0 would silently zero out any real AND);
            // BIT_OR's identity is a plain 0. The result is an unsigned 64-bit number, as in MySQL (BIT_AND of nothing is
            // 18446744073709551615, and -1 counts as all bits set).
            std::uint64_t acc = is_and ? ~std::uint64_t{0} : 0;
            for (const Row* r_ptr : grp) {
                const Row& r = *r_ptr;
                auto it = r.find(col_name);
                if (it == r.end() || it->second == EXECUTOR_NULL_VALUE) continue;
                const double v = std::clamp(text_to_number(it->second), -9.2e18, 9.2e18);
                const auto n = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(v)));
                acc = is_and ? (acc & n) : (acc | n);
            }
            out[label] = std::to_string(acc);
            continue;
        }

        if (std::holds_alternative<AggFunc::Count>(func->data)) {
            if (col_name == "*") {
                out[label] = std::to_string(grp.size());
                continue;
            }
            std::size_t c = 0;
            for (const Row* r_ptr : grp) {
                auto it = r_ptr->find(col_name);
                if (it != r_ptr->end() && it->second != EXECUTOR_NULL_VALUE) c++;
            }
            out[label] = std::to_string(c);
            continue;
        }
        if (std::holds_alternative<AggFunc::CountDistinct>(func->data)) {
            std::vector<const std::string*> values;
            for (const Row* r_ptr : grp) {
                auto it = r_ptr->find(col_name);
                if (it != r_ptr->end() && it->second != EXECUTOR_NULL_VALUE) values.push_back(&it->second);
            }
            out[label] = std::to_string(distinct_texts(values, arg_class).size());
            continue;
        }

        // The values of the argument that are not NULL. NOTE: Rust only parallelizes this specific value collection for the plain
        // (non-GROUP-BY, whole-result) aggregate call site, never for per-group computation (group row counts are usually small)
        // -- hence the allow_parallel flag rather than an unconditional threshold check here.
        std::vector<const std::string*> present;
        if (allow_parallel && parallel_enabled() && grp.size() >= parallel_min_rows()) {
            std::size_t n_chunks = (grp.size() + PARALLEL_CHUNK - 1) / PARALLEL_CHUNK;
            std::vector<std::vector<const std::string*>> partial(n_chunks);
            ThreadPool::global().parallel_for(n_chunks, [&](std::size_t ci) {
                std::size_t start = ci * PARALLEL_CHUNK;
                std::size_t end = std::min(start + PARALLEL_CHUNK, grp.size());
                for (std::size_t i = start; i < end; i++) {
                    auto it = grp[i]->find(col_name);
                    if (it != grp[i]->end() && it->second != EXECUTOR_NULL_VALUE) partial[ci].push_back(&it->second);
                }
            });
            for (auto& chunk : partial) present.insert(present.end(), chunk.begin(), chunk.end());
        } else {
            for (const Row* r_ptr : grp) {
                auto it = r_ptr->find(col_name);
                if (it != r_ptr->end() && it->second != EXECUTOR_NULL_VALUE) present.push_back(&it->second);
            }
        }

        if (std::holds_alternative<AggFunc::Sum>(func->data)) out[label] = sum_of_texts(present, true).value_or(EXECUTOR_NULL_VALUE);
        else if (std::holds_alternative<AggFunc::SumDistinct>(func->data)) out[label] = sum_of_texts(distinct_texts(present, arg_class), true).value_or(EXECUTOR_NULL_VALUE);
        else if (std::holds_alternative<AggFunc::Avg>(func->data)) out[label] = average_of_texts(present).value_or(EXECUTOR_NULL_VALUE);
        else if (std::holds_alternative<AggFunc::AvgDistinct>(func->data)) out[label] = average_of_texts(distinct_texts(present, arg_class)).value_or(EXECUTOR_NULL_VALUE);
        else out[label] = spread_text(*func, present); // STDDEV, VARIANCE, MEDIAN
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Names in a join: which table a column belongs to, and what `*` stands for
// ---------------------------------------------------------------------------------------------------------------------
namespace {

// What a joined table's columns are called in the joined rows ("<name>.<column>"): its alias when the table is used a second time
// (a self-join), else its own name.
const std::string& join_qualifier(const Join& j) { return j.alias.empty() ? j.table : j.alias; }

std::string last_name_part(const std::string& qualified) {
    auto cut = qualified.rfind('.');
    return cut == std::string::npos ? qualified : qualified.substr(cut + 1);
}

bool has_column(const TableSchema* t, const std::string& name) {
    return t && std::any_of(t->columns.begin(), t->columns.end(), [&](const ColumnDef& c) { return c.name == name; });
}

bool listed(const std::vector<std::string>& names, const std::string& name) { return std::find(names.begin(), names.end(), name) != names.end(); }

CondExpr and_of(std::optional<CondExpr>& so_far, CondExpr next) {
    if (!so_far) return next;
    return CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(*so_far)), std::make_unique<CondExpr>(std::move(next))});
}

} // namespace

std::vector<std::string> Executor::derived_column_names(SharedDatabase& s, const Statement& stmt) {
    std::vector<std::string> names;
    auto* sel = std::get_if<Statement::Select>(&stmt.data);
    if (!sel) return names;
    auto table_columns = [&](const std::string& table) {
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) names.push_back(c.name);
        }
    };
    for (auto& c : sel->columns) {
        if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) names.push_back(last_name_part(col->name));
        else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) names.push_back(ca->alias);
        else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) names.push_back(agg_label(agg->func, agg->col));
        else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) names.push_back(aa->alias);
        else if (auto* fn = std::get_if<SelectColumn::Func>(&c.data)) names.push_back(fn->alias.value_or(fn->name + "()"));
        else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) names.push_back(cw->alias.value_or("CASE"));
        else if (auto* ex = std::get_if<SelectColumn::Expr>(&c.data)) names.push_back(ex->alias.value_or(arith_to_str(ex->expr)));
        else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) names.push_back(wf->alias.value_or(window_func_default_label(wf->func)));
        else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) names.push_back(sq->alias.value_or("(subquery)"));
        else if (auto* all = std::get_if<SelectColumn::All>(&c.data)) {
            if (all->table.empty()) {
                table_columns(sel->table);
                for (auto& j : sel->joins) table_columns(j.table);
            } else {
                table_columns(all->table);
            }
        }
    }
    return names;
}

std::optional<std::string> Executor::resolve_join_columns(SharedDatabase& s, const std::string& table, std::vector<Join>& joins,
                                                          std::vector<SelectColumn>& columns, const std::optional<CondExpr>& condition,
                                                          bool expand_plain_star, std::vector<std::vector<std::string>>& joined_using) {
    joined_using.assign(joins.size(), {});
    // `*` over a join, `t.*` anywhere
    bool star = false;
    for (auto& c : columns) {
        if (auto* all = std::get_if<SelectColumn::All>(&c.data)) star = star || !joins.empty() || !all->table.empty() || expand_plain_star;
    }
    if (joins.empty() && !star) return std::nullopt;

    // The tables of the FROM list, the FROM table first: their schemas (null for one that is not a catalog table, a LATERAL
    // subquery) and the names a query writes before the dot of "<name>.<column>".
    std::vector<const TableSchema*> schemas{s.catalog.get_table(table)};
    std::vector<std::string> full_names{table}, names{last_name_part(table)};
    for (auto& j : joins) {
        schemas.push_back(j.lateral ? nullptr : s.catalog.get_table(j.table));
        full_names.push_back(join_qualifier(j));
        names.push_back(last_name_part(join_qualifier(j)));
    }

    // NATURAL and USING: the columns the two sides share are compared in an ordinary ON condition (so the join types, the hash joins and
    // the NULL rules are the ones of ON); `joined_using` remembers them for `*`, which shows each of them once.
    for (std::size_t i = 0; i < joins.size(); i++) {
        Join& j = joins[i];
        const bool natural = j.join_type == JoinType::Natural;
        if (!natural && j.using_cols.empty()) continue;
        std::vector<std::string> common = j.using_cols;
        if (natural && schemas[i + 1]) {
            for (auto& rc : schemas[i + 1]->columns) {
                for (std::size_t k = 0; k <= i; k++) {
                    if (has_column(schemas[k], rc.name)) {
                        common.push_back(rc.name);
                        break;
                    }
                }
            }
        }
        std::optional<CondExpr> on;
        for (auto& c : common) {
            bool left_has = false;
            for (std::size_t k = 0; k <= i; k++) left_has = left_has || !schemas[k] || has_column(schemas[k], c);
            if (!left_has || (schemas[i + 1] && !has_column(schemas[i + 1], c))) return "Unknown column '" + c + "' in 'from clause'";
            auto class_in = [&](const TableSchema* t) {
                if (t) {
                    for (auto& column : t->columns) {
                        if (column.name == c) return class_of_type(column.data_type);
                    }
                }
                return ValueClass::Unknown;
            };
            ValueClass left_class = ValueClass::Unknown;
            for (std::size_t k = 0; k <= i && left_class == ValueClass::Unknown; k++) left_class = class_in(schemas[k]);
            on = and_of(on, CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{c, left_class}), Operator::Eq,
                                                              ConditionValue(ConditionValue::Literal{names[i + 1] + "." + c}), left_class,
                                                              class_in(schemas[i + 1])}}));
        }
        if (on) j.on_expr = std::move(*on);
        if (natural) j.join_type = common.empty() ? JoinType::Cross : JoinType::Inner;
        j.using_cols.clear();
        joined_using[i] = std::move(common);
    }

    const bool all_known = std::none_of(schemas.begin(), schemas.end(), [](const TableSchema* t) { return t == nullptr; });
    PushdownScope scope;
    if (all_known) {
        scope.names = full_names;
        scope.bare = names;
        for (auto* t : schemas) {
            scope.columns.emplace_back();
            for (auto& c : t->columns) scope.columns.back().insert(c.name);
        }
    }

    // A cross join (`FROM a, b WHERE a.k = b.k`) whose WHERE pairs its rows up by `<table>.<column> = <table>.<column>` is an inner join
    // on that: it can then be hashed instead of making every pair of rows first. The WHERE stays as it is (the same test again).
    if (all_known && condition) {
        std::vector<const CondExpr*> parts;
        and_conjuncts(*condition, parts);
        for (std::size_t i = 0; i < joins.size(); i++) {
            Join& j = joins[i];
            if (j.join_type != JoinType::Cross || j.lateral || j.subquery) continue;
            std::optional<CondExpr> on;
            for (const CondExpr* part : parts) {
                auto* leaf = std::get_if<CondExpr::Leaf>(&part->data);
                if (!leaf || leaf->condition.op != Operator::Eq) continue;
                auto* l = std::get_if<ArithExpr::Col>(&leaf->condition.left.data);
                auto* r = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data);
                if (!l || !r || r->quoted || l->name.find('.') == std::string::npos || r->value.find('.') == std::string::npos) continue;
                const int lo = column_owner(l->name, scope), ro = column_owner(r->value, scope);
                if (lo < 0 || ro < 0 || lo == ro || std::max(lo, ro) != static_cast<int>(i) + 1) continue;
                on = and_of(on, CondExpr(*part));
            }
            if (on) {
                j.on_expr = std::move(*on);
                j.join_type = JoinType::Inner;
            }
        }
    }

    if (!star) return std::nullopt;
    if (!all_known) return std::nullopt; // a table without a schema keeps the old `*` (every column the row holds)

    // `*` and `t.*` become the columns they stand for. A column of the FROM table is read by its own name, one of a joined table by
    // "<name>.<column>" (get_col finds it in the joined row); the header is the part after the dot either way.
    // (after a RIGHT / FULL JOIN ... USING the plain name of a USING column is the merged one, so the FROM table's own is read qualified)
    bool coalesced = false;
    for (std::size_t i = 0; i < joins.size(); i++) {
        coalesced = coalesced || (!joined_using[i].empty() && (joins[i].join_type == JoinType::Right || joins[i].join_type == JoinType::FullOuter));
    }
    auto read_as = [&](std::size_t t, const std::string& column) { return t == 0 && !coalesced ? column : names[t] + "." + column; };
    // is the column of table `t` one a USING / NATURAL join has merged into the first column of its kind (shown once, in front)?
    auto merged = [&](std::size_t t, const std::string& column) {
        if (t >= 1 && listed(joined_using[t - 1], column)) return true;
        for (std::size_t k = 0; k < t; k++) {
            if (has_column(schemas[k], column)) return false;
        }
        for (std::size_t i = t; i < joins.size(); i++) {
            if (listed(joined_using[i], column)) return true;
        }
        return false;
    };
    std::vector<SelectColumn> expanded;
    for (auto& col : columns) {
        auto* all = std::get_if<SelectColumn::All>(&col.data);
        if (!all) {
            expanded.push_back(std::move(col));
            continue;
        }
        if (all->table.empty()) {
            std::vector<std::string> front;
            for (auto& per_join : joined_using) {
                for (auto& c : per_join) {
                    if (!listed(front, c)) front.push_back(c);
                }
            }
            for (auto& c : front) expanded.push_back(SelectColumn(SelectColumn::Column{c}));
            for (std::size_t t = 0; t < schemas.size(); t++) {
                for (auto& c : schemas[t]->columns) {
                    if (!merged(t, c.name)) expanded.push_back(SelectColumn(SelectColumn::Column{read_as(t, c.name)}));
                }
            }
            continue;
        }
        const std::string wanted = last_name_part(all->table);
        std::optional<std::size_t> owner;
        for (std::size_t t = 0; t < schemas.size() && !owner; t++) {
            if (names[t] == wanted) owner = t;
        }
        if (!owner) return "Unknown table '" + all->table + "'";
        for (auto& c : schemas[*owner]->columns) expanded.push_back(SelectColumn(SelectColumn::Column{read_as(*owner, c.name)}));
    }
    columns = std::move(expanded);
    return std::nullopt;
}

StringResult Executor::exec_select(SharedDatabase& s, std::string table, std::optional<std::pair<std::unique_ptr<Statement>, std::string>> subquery,
                                    bool distinct, std::vector<SelectColumn> columns, std::optional<CondExpr> condition, std::vector<Join> joins,
                                    std::vector<OrderBy> order_by, std::optional<std::vector<std::string>> group_by,
                                    std::optional<CondExpr> having, std::optional<std::size_t> limit, std::optional<std::size_t> offset,
                                    bool for_update, bool for_share) {
    if (subquery) {
        return exec_select_with_subquery(s, std::move(*subquery->first), subquery->second, distinct, std::move(columns), std::move(condition),
                                          std::move(joins), std::move(order_by), std::move(group_by), std::move(having), limit, offset, for_update,
                                          for_share);
    }

    // `JOIN (SELECT ...) AS d`: each derived table is evaluated once and read as a temporary table of its alias, as a derived table in
    // FROM is (exec_select_with_subquery).
    if (std::any_of(joins.begin(), joins.end(), [](const Join& j) { return j.subquery && !j.lateral; })) {
        std::vector<std::string> made;
        auto drop_all = [&] {
            for (auto& name : made) {
                temporary_tables_.erase(name);
                s.tables.erase(name);
                s.buffer_pool.invalidate(name);
                s.catalog.drop_table(name);
            }
        };
        for (auto& j : joins) {
            if (!j.subquery || j.lateral) continue;
            const std::string alias = j.subquery->second;
            if (s.tables.count(alias) || s.views.count(alias)) {
                drop_all();
                return StringResult::Err("Alias '" + alias + "' conflicts with an existing table or view");
            }
            auto inner_output = execute_with_s(s, Statement(*j.subquery->first));
            if (inner_output.is_err()) {
                drop_all();
                return inner_output;
            }
            auto [col_names, virtual_rows] = parse_table_output(inner_output.value());
            if (col_names.empty()) col_names = derived_column_names(s, *j.subquery->first); // no rows, so no header
            s.tables[alias] = virtual_rows;
            temporary_tables_.insert(alias);
            s.buffer_pool.write_page(alias, virtual_rows);
            std::vector<ColumnDef> schema_cols;
            for (auto& name : col_names) {
                ColumnDef c;
                c.name = name;
                c.data_type = DataType(DataType::Text{});
                schema_cols.push_back(c);
            }
            s.catalog.create_table(alias, schema_cols);
            made.push_back(alias);
            j.table = alias;
            j.subquery.reset();
        }
        auto result = exec_select(s, std::move(table), std::nullopt, distinct, std::move(columns), std::move(condition), std::move(joins),
                                  std::move(order_by), std::move(group_by), std::move(having), limit, offset, for_update, for_share);
        drop_all();
        return result;
    }

    // FROM-less scalar SELECT (e.g. a bare `SELECT expr;` inside a stored-procedure
    // body): evaluate each column against a single synthetic row made of proc_vars +
    // user_vars (as "@name"), with no table/condition/join/group-by processing at all.
    if (table == "_dual_" || (table.size() >= 7 && table.compare(table.size() - 7, 7, "._dual_") == 0)) {
        struct ColDef {
            std::string header;
            const SelectColumn* col;
        };
        std::vector<ColDef> col_defs;
        for (auto& col : columns) {
            std::string header;
            if (auto* v = std::get_if<SelectColumn::ColumnAlias>(&col.data)) header = v->alias;
            else if (auto* v = std::get_if<SelectColumn::Func>(&col.data)) header = v->alias.value_or(v->name);
            else if (auto* v = std::get_if<SelectColumn::Expr>(&col.data)) header = v->alias.value_or(arith_to_str(v->expr));
            else if (auto* v = std::get_if<SelectColumn::Agg>(&col.data)) header = debug_agg_func_dual(v->func) + "(" + v->col + ")";
            else if (auto* v = std::get_if<SelectColumn::AggAlias>(&col.data)) header = v->alias;
            else if (auto* v = std::get_if<SelectColumn::Column>(&col.data)) header = v->name;
            else if (std::holds_alternative<SelectColumn::All>(col.data)) header = "*";
            else if (auto* v = std::get_if<SelectColumn::CaseWhen>(&col.data)) header = v->alias.value_or("case");
            else if (auto* v = std::get_if<SelectColumn::WinFunc>(&col.data)) header = v->alias.value_or(window_func_default_label(v->func));
            else if (auto* v = std::get_if<SelectColumn::Subquery>(&col.data)) header = v->alias.value_or("(subquery)");
            col_defs.push_back({std::move(header), &col});
        }

        Row eval_row = proc_vars;
        for (auto& [k, v] : user_vars) eval_row["@" + k] = v;

        std::string dual_error; // a column this select cannot evaluate without a table, or a failing subquery
        auto eval_col_val = [&](const SelectColumn& col) -> std::string {
            if (auto* v = std::get_if<SelectColumn::Func>(&col.data)) return apply_scalar_func(v->name, v->args, eval_row);
            if (auto* v = std::get_if<SelectColumn::Expr>(&col.data)) return eval_arith(eval_row, v->expr);
            // (an @variable that was never set is NULL, not its own name)
            if (auto* v = std::get_if<SelectColumn::Column>(&col.data)) {
                auto it = eval_row.find(v->name);
                return it != eval_row.end() ? it->second : (!v->name.empty() && v->name[0] == '@' ? EXECUTOR_NULL_VALUE : v->name);
            }
            if (auto* v = std::get_if<SelectColumn::ColumnAlias>(&col.data)) {
                auto it = eval_row.find(v->name);
                return it != eval_row.end() ? it->second : (!v->name.empty() && v->name[0] == '@' ? EXECUTOR_NULL_VALUE : v->name);
            }
            if (auto* v = std::get_if<SelectColumn::CaseWhen>(&col.data)) {
                auto resolve = [&](const std::string& sv) -> std::string {
                    const std::string* found = get_col(eval_row, sv);
                    return found ? *found : sv;
                };
                for (auto& b : v->branches) {
                    if (eval_condexpr(eval_row, b.condition)) return resolve(b.result);
                }
                return v->else_val ? resolve(*v->else_val) : EXECUTOR_NULL_VALUE;
            }
            if (std::holds_alternative<SelectColumn::Agg>(col.data) || std::holds_alternative<SelectColumn::AggAlias>(col.data)) {
                // a select without FROM is one row: the aggregate is over a group of one row (COUNT(*) is 1)
                Row with_argument = eval_row;
                {
                    const AggFunc* agg_func = nullptr;
                    std::string argument;
                    if (auto* a = std::get_if<SelectColumn::Agg>(&col.data)) { agg_func = &a->func; argument = a->source.empty() ? a->col : a->source; }
                    else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&col.data)) { agg_func = &aa->func; argument = aa->source.empty() ? aa->col : aa->source; }
                    if (agg_func && is_expression_argument(argument)) {
                        const ArithExpr e = Parser::str_to_arith(argument);
                        with_argument[argument] = eval_arith(with_argument, e);
                    }
                }
                const Row aggregated = compute_aggregates({&with_argument}, {col});
                return aggregated.empty() ? std::string(EXECUTOR_NULL_VALUE) : aggregated.begin()->second;
            }
            if (auto* v = std::get_if<SelectColumn::Subquery>(&col.data)) {
                Statement query_copy = *v->query;
                auto* sc = std::get_if<Statement::Select>(&query_copy.data);
                if (!sc) return EXECUTOR_NULL_VALUE;
                auto out = exec_select(s, sc->table, std::move(sc->subquery), sc->distinct, std::move(sc->columns), std::move(sc->condition),
                                        std::move(sc->joins), std::move(sc->order_by), std::move(sc->group_by), std::move(sc->having), sc->limit,
                                        sc->offset, false, false);
                if (out.is_err()) {
                    dual_error = out.error();
                    return "";
                }
                auto found = extract_values_from_output(out.value());
                if (found.size() > 1) {
                    dual_error = "Subquery returns more than 1 row";
                    return "";
                }
                return found.empty() ? std::string(EXECUTOR_NULL_VALUE) : found.front();
            }
            dual_error = "This column needs a table to read from"; // `*`, a window function
            return "";
        };

        // Escape headers/cells up front (widths below are computed on the escaped form,
        // matching the visual padding actually written) -- see Executor::escape_cell.
        std::vector<std::string> headers;
        headers.reserve(col_defs.size());
        for (auto& cd : col_defs) headers.push_back(escape_cell(cd.header));

        std::vector<std::string> vals;
        vals.reserve(col_defs.size());
        for (auto& cd : col_defs) vals.push_back(escape_cell(eval_col_val(*cd.col)));
        if (!dual_error.empty()) return StringResult::Err(dual_error);

        std::vector<std::size_t> widths;
        widths.reserve(col_defs.size());
        for (std::size_t i = 0; i < col_defs.size(); i++) widths.push_back(std::max(headers[i].size(), vals[i].size()));

        std::string sep;
        for (auto w : widths) sep += "+" + std::string(w + 2, '-');
        sep += "+";

        std::string hdr;
        for (std::size_t i = 0; i < col_defs.size(); i++) {
            hdr += "| " + headers[i] + std::string(widths[i] - headers[i].size(), ' ') + " ";
        }
        hdr += "|";

        std::string row_str;
        for (std::size_t i = 0; i < col_defs.size(); i++) {
            row_str += "| " + vals[i] + std::string(widths[i] - vals[i].size(), ' ') + " ";
        }
        row_str += "|";

        return StringResult::Ok(sep + "\n" + hdr + "\n" + sep + "\n" + row_str + "\n" + sep + "\n1 row(s) returned.");
    }

    // INFORMATION_SCHEMA virtual tables
    {
        std::string lower_table = table;
        std::transform(lower_table.begin(), lower_table.end(), lower_table.begin(), [](unsigned char c) { return std::tolower(c); });
        if (auto pos = lower_table.find("information_schema."); pos != std::string::npos) {
            std::string which = table.substr(pos + 19);
            return exec_information_schema(s, which, columns, condition, order_by, limit, offset);
        }
    }

    if (auto it = s.views.find(table); it != s.views.end()) {
        // Temporarily remove the view so exec_select_with_subquery's alias-conflict
        // check (which also checks s.views) doesn't collide with the view's own name.
        Statement view_stmt = std::move(it->second);
        s.views.erase(it);
        auto result = exec_select_with_subquery(s, view_stmt, table, distinct, columns, condition, joins, order_by, group_by, having, limit,
                                                  offset, for_update, for_share);
        s.views[table] = std::move(view_stmt);
        return result;
    }

    if (!s.tables.count(table)) return StringResult::Err("Table '" + table + "' not found");

    // NATURAL / USING, comma joins and `*` read the way SQL says (see resolve_join_columns)
    std::vector<std::vector<std::string>> joined_using;
    if (auto error = resolve_join_columns(s, table, joins, columns, condition, distinct || columns.size() > 1, joined_using)) {
        return StringResult::Err(*error);
    }

    // A table used a second time (a self-join) keeps its alias as its name in the joined rows; the planner, which reasons by table
    // name, then has nothing to say about the join (it would take the two uses for one table): they are joined by the ON condition.
    const bool aliased = std::any_of(joins.begin(), joins.end(), [](const Join& j) { return !j.alias.empty(); });
    // RIGHT / FULL JOIN ... USING (c): the merged column `c` of a right row without a partner is the right row's (a plain `c` and `*`
    // read it), so the NULL padding of the FROM table also carries its columns as "<table>.<column>", which `<alias>.c` reads
    bool coalesce_bare = false;
    for (std::size_t i = 0; i < joins.size(); i++) {
        coalesce_bare = coalesce_bare || (!joined_using[i].empty() && (joins[i].join_type == JoinType::Right || joins[i].join_type == JoinType::FullOuter));
    }

    // ── JOIN 순서 최적화 (cost-based DP, INNER-only; greedy 폴백) ──────────
    joins = reorder_joins_dp(table, std::move(joins), s.tables);

    // ── Planner: 인덱스 / 조인 알고리즘 결정 ──────────────────────────────
    // an aggregate inside an expression, a function or a CASE (`SUM(v) + 1`, `ROUND(AVG(v), 2)`) makes it an aggregate query too:
    // such a statement used to run row by row, with 0 or NULL where the aggregate was
    const std::vector<std::string> expr_agg_refs = select_agg_refs(columns);
    std::unordered_map<std::string, ValueClass> aggregate_classes;
    for (auto& c : columns) {
        if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) collect_aggregate_classes(e->expr, aggregate_classes);
        else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
            for (auto& b : cw->branches) collect_aggregate_classes(b.condition, aggregate_classes);
        }
    }
    if (having) collect_aggregate_classes(*having, aggregate_classes);
    auto aggregate_class = [&](const std::string& ref) {
        auto it = aggregate_classes.find(ref);
        return it == aggregate_classes.end() ? ValueClass::Unknown : it->second;
    };
    bool has_agg = columns_have_aggregate(columns);
    bool has_win = std::any_of(columns.begin(), columns.end(), [](const SelectColumn& c) {
        return std::holds_alternative<SelectColumn::WinFunc>(c.data);
    });
    Planner planner(s.tables, s.indexes, s.index_meta, s.composite_indexes, s.hash_indexes, s.hash_index_meta, s.catalog, s.table_stats);
    SelectPlan plan = planner.plan_covering(table, condition, joins, columns);

    // 인덱스 경로 실행 (집계 / FOR UPDATE / JOIN / LIMIT / ORDER BY 없을 때만)
    // read_ctx: 이 경로들은 s.indexes/s.hash_indexes/s.composite_indexes를 세션 격리
    // 없이 직접 읽으므로(session_tables/snapshot_는 s.tables만 스왑함), 여기서만큼은
    // is_visible의 permissive 체크 대신 이 문장을 실행하는 트랜잭션의 실제 스냅샷
    // 기준으로 가시성을 판단해야 함 — 그래야 다른 세션의 아직 커밋 안 된 INSERT/DELETE가
    // 인덱스 경로를 통해 새어나가지 않음.
    SnapshotCtx read_ctx = current_read_ctx(s);
    // MVCC: every index (PK B+Tree, secondary B+Tree/HashIndex buckets, composite) holds
    // only the LATEST physical version per key -- index_remove_row+index_insert_row purge
    // the old entry and add the new one on every UPDATE, so an older version an open
    // RR/Serializable snapshot still needs can be entirely absent from the index, not just
    // present-but-filtered. For a non-frozen ctx (ReadUncommitted, ReadCommitted, or
    // autocommit) read_ctx was captured moments ago under the same statement-wide lock, so
    // "the index's current latest version" and "what this ctx should see" can never
    // diverge -- no fallback needed. Only a frozen RR/Serializable ctx (captured at a past
    // BEGIN) can be looking for a version older than what the index now holds; for that
    // case, skip these fast paths entirely and fall through to the generic scan below,
    // which reads every physical version directly from s.tables.
    bool use_fast_index_paths = !(txn.is_active() && txn.frozen_ctx().has_value());
    // The comment above is only half the story: "the index's latest version == what this
    // ctx should see" ALSO breaks whenever ANOTHER open transaction has an uncommitted
    // UPDATE/DELETE on the row -- the index then holds that uncommitted version (or no entry
    // at all), so a plain PK lookup reported "0 rows" for a row that is perfectly visible
    // (found while testing UPDATE: session A `BEGIN; UPDATE t SET v=11 WHERE id=1`, session B
    // `SELECT v FROM t WHERE id=1` -> 0 rows, while `WHERE v >= 0` correctly returned 10).
    // Uncommitted versions only come from explicit transactions (autocommit statements are
    // atomic under the table locks), so "is any OTHER transaction open" is the exact test.
    bool others_active = false;
    {
        std::uint64_t self = txn.current_txn_id();
        auto active = s.active_txn_ids->lock();
        for (auto id : *active) {
            if (id != self) { others_active = true; break; }
        }
    }
    // Index entries versus the numeric-equivalence rule (numeric_key.hpp). A B+Tree orders "7", "7.0" and "07" by their
    // text while WHERE treats them as equal, so every range below is widened by one ulp and what it finds is re-checked
    // against the full condition (an index can only ever be a source of candidates). A bound that cannot be widened
    // safely (infinity), or a BETWEEN whose two ends disagree about being numbers (that compares as text), makes the
    // path fall through to the generic scan.
    auto row_ok = [&](const Row& r) { return is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition); };
    auto add_bucket = [&](const std::string& json, std::vector<Row>& out) {
        for (auto& r : rows_from_json(json)) {
            if (row_ok(r)) out.push_back(std::move(r));
        }
    };
    // Covering: the query selects only the indexed column, so a row is just that column's STORED value (not the
    // lookup key -- "7" and "7.00" both answer `price = 7`). Counts the entries whose _xmax is zero.
    auto add_bucket_covering = [&](const std::string& json, const std::string& col, std::vector<Row>& out) {
        for (auto& v : nlohmann::json::parse(json)) {
            auto xit = v.find("_xmax");
            if (!(xit == v.end() || (xit->is_string() && xit->get<std::string>() == "0"))) continue;
            auto cit = v.find(col);
            if (cit == v.end() || !cit->is_string()) continue;
            Row r;
            r[col] = cit->get<std::string>();
            if (matches_condition_with_subquery(s, r, condition)) out.push_back(std::move(r));
        }
    };
    auto add_bucket_any = [&](const std::string& json, const std::string* covering_col, std::vector<Row>& out) {
        if (covering_col) add_bucket_covering(json, *covering_col, out);
        else add_bucket(json, out);
    };
    auto scan_secondary_range = [&](const BPlusTree& tree, RangeOp op, const std::string& key, const std::string* covering_col,
                                    std::vector<Row>& out) {
        bool lower = range_op_is_lower_bound(op);
        auto bound = tree_bound(tree, key, lower);
        if (!bound) return false;
        for (auto& kv : lower ? tree.scan_from(*bound, true) : tree.scan_to(*bound, true)) add_bucket_any(kv.second, covering_col, out);
        return true;
    };
    auto scan_secondary_between = [&](const BPlusTree& tree, const std::string& a, const std::string& b, const std::string* covering_col,
                                      std::vector<Row>& out) {
        double d;
        if (!tree.text_keyed() && parse_number_key(a, d) != parse_number_key(b, d)) return false;
        auto lo = tree_bound(tree, a, true), hi = tree_bound(tree, b, false);
        if (!lo || !hi) return false;
        for (auto& json : tree.range_search(*lo, *hi)) add_bucket_any(json, covering_col, out);
        return true;
    };
    // A LIKE prefix scan stops at the first key that does not start with the prefix. For a prefix that looks like a
    // number that is wrong ("12%": numeric order puts 13 before 120), so only prefixes that cannot be read as a
    // number are scanned.
    auto like_prefix_scannable = [](const std::string& prefix) {
        if (prefix.empty()) return false;
        char c0 = prefix[0];
        return !(std::isdigit(static_cast<unsigned char>(c0)) || c0 == '-' || c0 == '+' || c0 == '.');
    };

    if (use_fast_index_paths && joins.empty() && !has_agg && !has_win && !for_update && !for_share
        && !limit.has_value() && !offset.has_value() && order_by.empty() && !distinct) {
        auto& access = plan.base.access.data;
        // The planner calls an access "covering" when the selected columns all sit in SOME index of the table; the
        // synthetic rows below carry only the one column of the path's own index, so require exactly that.
        const std::string* covering_col = nullptr;
        if (plan.base.is_covering) {
            const std::string* col = nullptr;
            if (auto* p = std::get_if<AccessPath::SecondaryPoint>(&access)) col = &p->col;
            else if (auto* r = std::get_if<AccessPath::SecondaryRange>(&access)) col = &r->col;
            auto only_col = [&](const SelectColumn& c) {
                if (auto* x = std::get_if<SelectColumn::Column>(&c.data)) return x->name == *col;
                if (auto* x = std::get_if<SelectColumn::ColumnAlias>(&c.data)) return x->name == *col;
                return false;
            };
            if (col && std::all_of(columns.begin(), columns.end(), only_col)) covering_col = col;
        }
        if (auto* ap = std::get_if<AccessPath::PkPoint>(&access)) {
            auto lo = index_bound(s.indexes, table, ap->key, true), hi = index_bound(s.indexes, table, ap->key, false);
            if (auto it = s.indexes.find(table); it != s.indexes.end() && lo && hi) {
                auto found = it->second.range_search(*lo, *hi);
                std::vector<Row> rows;
                for (auto& j : found) {
                    Row r = row_from_json(j);
                    if (row_ok(r)) rows.push_back(std::move(r));
                }
                // With no other transaction open the index is final. With one open, the latest version of a row may be
                // invisible to this reader (or the entry gone) while an older visible version exists that the index no
                // longer holds -- so only the plain "one entry, visible" answer is taken from it; anything else
                // (including a lookup that matched several spellings of the key) goes to the generic scan.
                if (!others_active || (found.size() == 1 && rows.size() == 1)) return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (others_active) {
            // Range / secondary / hash / composite index paths silently DROP rows whose latest
            // version is invisible to this reader -- there is no way to tell which. Generic scan.
        } else if (auto* ap = std::get_if<AccessPath::PkBetween>(&access)) {
            double d;
            auto lo = index_bound(s.indexes, table, ap->start, true), hi = index_bound(s.indexes, table, ap->end, false);
            if (auto it = s.indexes.find(table); it != s.indexes.end() && lo && hi && (it->second.text_keyed() || parse_number_key(ap->start, d) == parse_number_key(ap->end, d))) {
                std::vector<Row> rows;
                for (auto& j : it->second.range_search(*lo, *hi)) {
                    Row r = row_from_json(j);
                    if (row_ok(r)) rows.push_back(std::move(r));
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::PkRange>(&access)) {
            bool lower = range_op_is_lower_bound(ap->op);
            auto bound = index_bound(s.indexes, table, ap->key, lower);
            if (auto it = s.indexes.find(table); it != s.indexes.end() && bound) {
                std::vector<Row> rows;
                for (auto& kv : lower ? it->second.scan_from(*bound, true) : it->second.scan_to(*bound, true)) {
                    Row r = row_from_json(kv.second);
                    if (row_ok(r)) rows.push_back(std::move(r));
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::HashPoint>(&access)) {
            // HashIndex buckets by numeric value, so "7", "7.0" and "07" are one bucket: exact.
            if (auto it = s.hash_indexes.find(ap->index_key); it != s.hash_indexes.end()) {
                std::vector<Row> rows;
                for (auto& r : it->second.get(ap->key)) {
                    if (row_ok(r)) rows.push_back(r);
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryPoint>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                std::vector<Row> rows;
                if (scan_secondary_between(it->second, ap->key, ap->key, covering_col, rows)) return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryRange>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                std::vector<Row> rows;
                if (scan_secondary_range(it->second, ap->op, ap->key, covering_col, rows)) return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryBetween>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                std::vector<Row> rows;
                if (scan_secondary_between(it->second, ap->start, ap->end, nullptr, rows)) return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (std::holds_alternative<AccessPath::CompositeIndexPath>(access) || std::holds_alternative<AccessPath::CompositeIndexPrefix>(access)) {
            const std::string& name = std::holds_alternative<AccessPath::CompositeIndexPath>(access)
                                          ? std::get<AccessPath::CompositeIndexPath>(access).index_name
                                          : std::get<AccessPath::CompositeIndexPrefix>(access).index_name;
            // The index fixes a run of leading columns; everything else in the condition is checked on the rows.
            if (auto it = s.composite_indexes.find(name); it != s.composite_indexes.end() && condition) {
                if (auto found = it->second.lookup(planner.constant_eq_map(table, *condition))) {
                    std::vector<Row> rows;
                    for (auto& r : *found) {
                        if (row_ok(r)) rows.push_back(std::move(r));
                    }
                    return format_result(s, std::move(rows), columns, table, {});
                }
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryLikePrefix>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end() && like_prefix_scannable(ap->prefix)) {
                std::vector<Row> rows;
                for (auto& kv : it->second.scan_from(ap->prefix, true)) {
                    if (kv.first.compare(0, ap->prefix.size(), ap->prefix) != 0) break;
                    add_bucket(kv.second, rows);
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::IndexIntersection>(&access)) {
            std::string pk_col;
            if (auto* sc = s.catalog.get_table(table)) {
                for (auto& c : sc->columns) {
                    if (c.primary_key) { pk_col = c.name; break; }
                }
            }
            if (pk_col.empty()) {
                if (auto it = s.tables.find(table); it != s.tables.end() && !it->second.empty() && !it->second[0].empty()) {
                    pk_col = it->second[0].begin()->first;
                }
            }
            // Each sub-path yields a SUPERSET of the pks that can match (numeric widening); the final pass over the
            // table re-checks the whole condition on the real rows.
            bool usable = true;
            std::vector<std::unordered_set<std::string>> pk_sets;
            for (auto& sub_path : ap->paths) {
                std::unordered_set<std::string> pks;
                if (auto* sp = std::get_if<AccessPath::SecondaryPoint>(&sub_path.data)) {
                    auto it = s.indexes.find(sp->index_key);
                    auto lo = index_bound(s.indexes, sp->index_key, sp->key, true), hi = index_bound(s.indexes, sp->index_key, sp->key, false);
                    if (it == s.indexes.end() || !lo || !hi) { usable = false; break; }
                    for (auto& json : it->second.range_search(*lo, *hi)) {
                        for (auto& r : rows_from_json(json)) {
                            if (is_visible_for_read(r, read_ctx)) {
                                if (const std::string* v = get_col(r, pk_col)) pks.insert(*v);
                            }
                        }
                    }
                } else if (auto* hp = std::get_if<AccessPath::HashPoint>(&sub_path.data)) {
                    auto it = s.hash_indexes.find(hp->index_key);
                    if (it == s.hash_indexes.end()) { usable = false; break; }
                    for (auto& r : it->second.get(hp->key)) {
                        if (is_visible_for_read(r, read_ctx)) {
                            if (const std::string* v = get_col(r, pk_col)) pks.insert(*v);
                        }
                    }
                } else {
                    usable = false;
                    break;
                }
                pk_sets.push_back(std::move(pks));
            }
            if (usable && !pk_sets.empty()) {
                std::unordered_set<std::string> intersection = pk_sets[0];
                for (std::size_t i = 1; i < pk_sets.size(); i++) {
                    std::unordered_set<std::string> next;
                    for (auto& k : intersection) if (pk_sets[i].count(k)) next.insert(k);
                    intersection = std::move(next);
                }
                std::vector<Row> rows;
                if (auto it = s.tables.find(table); it != s.tables.end()) {
                    for (auto& r : it->second) {
                        if (!is_visible_for_read(r, read_ctx)) continue;
                        const std::string* v = get_col(r, pk_col);
                        if (v && intersection.count(*v) && matches_condition_with_subquery(s, r, condition)) rows.push_back(r);
                    }
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        }
        // AccessPath::SeqScan → fall through to the generic scan below
    }

    // Top-K 인덱스 경로: ORDER BY 1컬럼 + LIMIT + OFFSET 없음 + 단순 SELECT
    if (use_fast_index_paths && !others_active && joins.empty() && !has_agg && !has_win && !for_update && !for_share && !distinct
        && !offset.has_value() && order_by.size() == 1 && limit.has_value()) {
        std::size_t lim = *limit;
        const OrderBy& ob = order_by[0];
        auto& access = plan.base.access.data;
        std::vector<Row> topk_rows;
        bool matched = false;
        if (auto* ap = std::get_if<AccessPath::SecondaryRange>(&access); ap && ob.column == ap->col) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) matched = scan_secondary_range(it->second, ap->op, ap->key, nullptr, topk_rows);
        } else if (auto* ap = std::get_if<AccessPath::SecondaryBetween>(&access); ap && ob.column == ap->col) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) matched = scan_secondary_between(it->second, ap->start, ap->end, nullptr, topk_rows);
        } else if (auto* ap = std::get_if<AccessPath::SecondaryLikePrefix>(&access); ap && ob.column == ap->col) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end() && like_prefix_scannable(ap->prefix)) {
                matched = true;
                for (auto& kv : it->second.scan_from(ap->prefix, true)) {
                    if (kv.first.compare(0, ap->prefix.size(), ap->prefix) != 0) break;
                    add_bucket(kv.second, topk_rows);
                }
            }
        }
        if (matched) {
            if (!ob.ascending) std::reverse(topk_rows.begin(), topk_rows.end());
            if (topk_rows.size() > lim) topk_rows.resize(lim);
            return format_result(s, std::move(topk_rows), columns, table, {});
        }
    }

    // The table's rows are read IN PLACE (this statement holds the table's data lock shared, so nothing can change the
    // vector under it). A scan used to copy the whole table first -- one hash map per row, ~1 us each -- only to throw
    // away every row that is invisible or fails the WHERE; now only the rows that are returned (or joined) are copied.
    std::vector<Row> buffer_pool_rows;
    const std::vector<Row>* base_rows = nullptr;
    if (auto it = s.tables.find(table); it != s.tables.end()) {
        base_rows = &it->second;
    } else {
        buffer_pool_rows = s.buffer_pool.get_page(table, s.disk);
        base_rows = &buffer_pool_rows;
    }

    // The tables of a join statement with their columns, when every one is a catalog table and no two share a bare name
    // (a "<table>.<column>" reference must name exactly one of them). The two shortcuts below need it.
    PushdownScope scope;
    bool scope_ok = !joins.empty();
    auto add_table = [&](const std::string& name, const std::string& qualifier) {
        auto* sc = s.catalog.get_table(name);
        std::string bare = qualifier.substr(qualifier.rfind('.') == std::string::npos ? 0 : qualifier.rfind('.') + 1);
        if (!sc || std::find(scope.bare.begin(), scope.bare.end(), bare) != scope.bare.end()) {
            scope_ok = false;
            return;
        }
        scope.names.push_back(qualifier);
        scope.bare.push_back(bare);
        scope.columns.emplace_back();
        for (auto& c : sc->columns) scope.columns.back().insert(c.name);
    };
    if (scope_ok) {
        add_table(table, table);
        for (auto& j : joins) {
            if (j.lateral || j.subquery) scope_ok = false;
            if (scope_ok) add_table(j.table, join_qualifier(j));
        }
    }

    // WHERE conjuncts that read a single table, by the table they read (see PushdownScope). Only for plain inner/left
    // joins; everything else joins first and filters afterwards, as before.
    std::vector<std::vector<const CondExpr*>> pushed(joins.size() + 1);
    if (scope_ok && condition && !condition_has_subquery(condition)) {
        bool ok = true;
        for (auto& j : joins) {
            if (!j.using_cols.empty() || (j.join_type != JoinType::Inner && j.join_type != JoinType::Left)) ok = false;
        }
        if (ok) {
            std::vector<const CondExpr*> parts;
            and_conjuncts(*condition, parts);
            for (const CondExpr* part : parts) {
                int owner = expr_owner(*part, scope);
                if (owner == 0) pushed[0].push_back(part);
                else if (owner > 0 && joins[owner - 1].join_type == JoinType::Inner) pushed[owner].push_back(part);
            }
        }
    }

    // A pure-read statement that keeps asking one table for `<column> = <constant>` (a correlated subquery does, once per
    // outer row) builds a hash index on that column for itself on the third ask, and reads the bucket from then on.
    const std::vector<const Row*>* bucket = nullptr;
    if (point_index_allowed_ && joins.empty() && condition && base_rows->size() >= 512 && s.tables.count(table) && !temporary_tables_.count(table)) {
        if (auto* schema = s.catalog.get_table(table)) {
            std::unordered_set<std::string> table_columns;
            for (auto& c : schema->columns) table_columns.insert(c.name);
            if (auto eq = constant_equality_part(*condition, table, table_columns)) {
                std::string key = table + std::string(1, '\0') + eq->first;
                auto cached = point_index_cache_.find(key);
                if (cached == point_index_cache_.end() && ++point_probe_count_[key] >= 3) {
                    StatementPointIndex built;
                    for (auto& r : *base_rows) {
                        if (auto v = r.find(eq->first); v != r.end()) built.buckets[normalize_numeric_key(v->second)].push_back(&r);
                    }
                    cached = point_index_cache_.emplace(key, std::move(built)).first;
                }
                if (cached != point_index_cache_.end()) {
                    static const std::vector<const Row*> none;
                    auto b = cached->second.buckets.find(normalize_numeric_key(eq->second));
                    bucket = b != cached->second.buckets.end() ? &b->second : &none;
                }
            }
        }
    }

    // An aggregate, ORDER BY, LIMIT or DISTINCT over one table never reached the index shortcuts above (they answer a plain
    // SELECT straight from the index), so `SELECT COUNT(*) FROM t WHERE indexed = 5` read every row although EXPLAIN said
    // "Index Scan". The candidate search UPDATE and DELETE use finds the positions of the matching rows (each is checked
    // against the real row and the whole WHERE; anything doubtful makes it "not usable" and the scan below runs as before)
    // and the statement carries on from them exactly as from a scan. The positions come back in table order, so the order of
    // the output does not change either.
    DmlIndexHit index_hit;
    if (joins.empty() && !bucket && condition && use_fast_index_paths && !for_update && !for_share
        && (has_agg || has_win || distinct || limit.has_value() || offset.has_value() || !order_by.empty())
        && !condition_has_subquery(condition) && s.tables.count(table) && !temporary_tables_.count(table)) {
        if (auto* schema = s.catalog.get_table(table)) {
            std::string table_pk;
            for (auto& c : schema->columns) {
                if (c.primary_key) {
                    table_pk = c.name;
                    break;
                }
            }
            if (!table_pk.empty()) {
                index_hit = dml_index_positions(s, table, condition, table_pk,
                                                [&](const Row& r) { return is_visible_for_read(r, read_ctx); }, txn.current_txn_id(),
                                                /*share_divisor=*/32);
            }
        }
    }

    std::vector<Row> visible_rows; // joins only
    std::vector<const Row*> visible_ptrs; // single-table scan
    if (joins.empty() && bucket) {
        visible_ptrs.reserve(bucket->size());
        for (const Row* r : *bucket) {
            if (is_visible_for_read(*r, read_ctx)) visible_ptrs.push_back(r);
        }
    } else if (joins.empty() && index_hit.usable) {
        visible_ptrs.reserve(index_hit.positions.size());
        for (std::size_t pos : index_hit.positions) visible_ptrs.push_back(&(*base_rows)[pos]);
    } else if (joins.empty()) {
        visible_ptrs.reserve(base_rows->size());
        for (auto& r : *base_rows) {
            if (is_visible_for_read(r, read_ctx)) visible_ptrs.push_back(&r);
        }
    } else {
        visible_rows.reserve(base_rows->size());
        for (auto& r : *base_rows) {
            if (!is_visible_for_read(r, read_ctx)) continue;
            bool keep = true;
            for (const CondExpr* f : pushed[0]) {
                if (!eval_condexpr(r, *f)) {
                    keep = false;
                    break;
                }
            }
            if (keep) visible_rows.push_back(r);
        }
    }

    // From here the statement works on `rows_p`, pointers to the rows still in the running. A single-table scan points into
    // the table itself, so ORDER BY, GROUP BY, HAVING, OFFSET/LIMIT, DISTINCT and the aggregates never copy a row (a copied
    // row is a hash map: ~1 us each, and sorting copies moved every one of them several times) and only the rows that are
    // finally returned are copied. Rows that are new -- joined rows, window-function output -- live in `result` instead and
    // are pointed to the same way.
    std::vector<Row> result;
    std::vector<const Row*> rows_p;
    if (joins.empty()) {
        std::vector<const Row*> matched;
        if (parallel_enabled() && visible_ptrs.size() >= parallel_min_rows() && condition.has_value()
            && !condition_has_subquery(condition)) {
            // 병렬 SeqScan 필터: 서브쿼리 없는 WHERE는 순수 정적 matches_condexpr로 평가 가능.
            // 청크마다 워커 스레드에 thread_local UDF 컨텍스트를 세팅해 사용자 정의 함수/DATABASE()도 정확히 평가.
            std::size_t n_chunks = (visible_ptrs.size() + PARALLEL_CHUNK - 1) / PARALLEL_CHUNK;
            std::vector<std::vector<const Row*>> chunk_results(n_chunks);
            auto uf = s.user_functions;
            std::string cur_db = current_db;
            std::string cur_user = auth_user;
            ThreadPool::global().parallel_for(n_chunks, [&](std::size_t ci) {
                std::size_t start = ci * PARALLEL_CHUNK;
                std::size_t end = std::min(start + PARALLEL_CHUNK, visible_ptrs.size());
                sync_udf_context(uf, cur_db, cur_user);
                auto& out = chunk_results[ci];
                for (std::size_t i = start; i < end; i++) {
                    if (matches_condexpr(*visible_ptrs[i], condition)) out.push_back(visible_ptrs[i]);
                }
            });
            for (auto& chunk : chunk_results) matched.insert(matched.end(), chunk.begin(), chunk.end());
        } else {
            for (const Row* r : visible_ptrs) {
                if (matches_condition_with_subquery(s, *r, condition)) matched.push_back(r);
            }
        }
        if (has_win) { // window functions build their output from their own copies of the rows
            result.reserve(matched.size());
            for (const Row* r : matched) result.push_back(*r);
        } else {
            rows_p = std::move(matched);
        }
    } else {
        std::vector<Row> current = std::move(visible_rows);
        for (std::size_t ji = 0; ji < joins.size(); ji++) {
            auto& j = joins[ji];
            const std::string& jq = join_qualifier(j); // what the joined table's columns are called in the merged rows

            // LATERAL JOIN -- Rust 원본에 없음. j.table은 실제 물리 테이블이 아니라 서브쿼리의
            // 별칭이므로, 아래의 정상 조인 경로(s.tables.find(j.table) 이하)에 들어가기 전에
            // 완전히 별도로 가로챈다. 서브쿼리는 왼쪽(current) 행마다 다시 실행되고, 그 자신의
            // 최상위 WHERE절만 바깥 행 값으로 치환된다(V1 축소 범위).
            if (j.lateral && j.subquery) {
                std::vector<Row> new_current;
                std::vector<std::string> right_cols_for_null;
                for (auto& left_row : current) {
                    Statement sub(*j.subquery->first);
                    if (auto* sel = std::get_if<Statement::Select>(&sub.data)) {
                        if (sel->condition) sel->condition = substitute_correlated_condexpr(*sel->condition, left_row);
                    }
                    auto inner_result = execute_with_s(s, std::move(sub));
                    if (inner_result.is_err()) return inner_result;
                    auto [inner_cols, inner_rows] = parse_table_output(inner_result.value());
                    if (!inner_cols.empty() && right_cols_for_null.empty()) right_cols_for_null = inner_cols;

                    bool any_match = false;
                    for (auto& r : inner_rows) {
                        Row merged = left_row;
                        merge_right(merged, r, j.table);
                        if (!eval_condexpr(merged, j.on_expr)) continue;
                        any_match = true;
                        new_current.push_back(std::move(merged));
                    }
                    if (!any_match && j.join_type == JoinType::Left) {
                        Row merged = left_row;
                        null_right(merged, right_cols_for_null, j.table);
                        new_current.push_back(std::move(merged));
                    }
                    // Inner/Cross에서 any_match == false면 표준 join 의미론대로 그 행은 드롭.
                }
                current = std::move(new_current);
                continue;
            }

            std::vector<Row> right_rows;
            {
                auto it = s.tables.find(j.table);
                if (it == s.tables.end()) return StringResult::Err("Table '" + j.table + "' not found");
                right_rows.reserve(it->second.size());
                for (auto& r : it->second) {
                    if (!is_visible_for_read(r, read_ctx)) continue;
                    bool keep = true;
                    for (const CondExpr* f : pushed[ji + 1]) {
                        if (!eval_condexpr(r, *f)) {
                            keep = false;
                            break;
                        }
                    }
                    if (keep) right_rows.push_back(r);
                }
            }

            std::vector<std::string> right_schema_cols;
            if (auto* sc = s.catalog.get_table(j.table)) {
                for (auto& c : sc->columns) right_schema_cols.push_back(c.name);
            }

            // A RIGHT / FULL OUTER JOIN pads a right row that no left row matched with NULL for every key the left rows carry: the
            // columns of the FROM table by their names, those of each table joined before by "<name>.<column>" (and by their own names,
            // which the first table that has one owns). Read from the schemas, so it does not depend on there being a left row at all.
            std::optional<std::vector<std::string>> left_pad_keys;
            if (j.join_type == JoinType::Right || j.join_type == JoinType::FullOuter) {
                std::vector<std::string> keys;
                std::unordered_set<std::string> seen;
                auto add = [&](const std::string& key) {
                    if (seen.insert(key).second) keys.push_back(key);
                };
                bool known = false;
                if (auto* from = s.catalog.get_table(table)) {
                    known = true;
                    for (auto& c : from->columns) {
                        add(c.name);
                        if (coalesce_bare) add(table + "." + c.name);
                    }
                }
                for (std::size_t k = 0; k < ji && known; k++) {
                    auto* sc = joins[k].lateral ? nullptr : s.catalog.get_table(joins[k].table);
                    if (!sc) {
                        known = false;
                        break;
                    }
                    for (auto& c : sc->columns) {
                        add(join_qualifier(joins[k]) + "." + c.name);
                        add(c.name);
                    }
                }
                if (known) left_pad_keys = std::move(keys);
            }
            const std::vector<std::string>* left_pad = left_pad_keys ? &*left_pad_keys : nullptr;

            // Correctness fix: the planner-chosen algo (IndexNL/ReverseIndexNL especially)
            // never NULL-pads an unmatched left row -- a probe miss is just dropped, which
            // is correct ONLY for Inner (and Cross, already excluded below via nested_loop_
            // join's own dedicated Cross handling). A LEFT/RIGHT JOIN previously could still
            // be assigned IndexNL by the planner (only Cross/Natural/FullOuter were
            // excluded), silently degrading to Inner-JOIN semantics whenever a probe missed
            // -- switched from a denylist to an allowlist (Inner only) so no join type can
            // reach an algo that doesn't know how to preserve its own NULL-padding
            // semantics; everything else falls through to nested_loop_join below, which
            // already has explicit, tested Left/Right/FullOuter branches.
            const JoinAlgo::Data* algo = nullptr;
            if (j.join_type == JoinType::Inner && ji < plan.joins.size() && !aliased) {
                algo = &plan.joins[ji].algo.data;
            }

            // A LEFT, RIGHT or FULL OUTER JOIN, or an INNER JOIN the planner has no algorithm for (an ON that is more than one
            // equality), whose ON contains `<left column> = <right table>.<column>` is hashed on that equality instead of a
            // nested loop that builds a merged row for every pair of rows (see hashed_join_verified). So is every INNER JOIN after the first: the
            // planner's algorithms name the left column by its bare name ("id"), which in a row that already holds a joined
            // table is the FROM table's column, not the joined table's `u.id` the ON asked for (`t JOIN u ON u.id = t.grp
            // JOIN w ON w.k = u.id` matched w.k against t.id and lost most of its rows).
            std::optional<std::vector<Row>> hashed;
            const bool nested_planned = !algo || std::holds_alternative<JoinAlgo::NestedLoop>(*algo);
            const bool hashable_type = j.join_type == JoinType::Left || j.join_type == JoinType::Right || j.join_type == JoinType::FullOuter ||
                                       (j.join_type == JoinType::Inner && (nested_planned || ji > 0));
            if (hashable_type && scope_ok && j.using_cols.empty()) {
                std::unordered_set<std::string> right_cols(right_schema_cols.begin(), right_schema_cols.end());
                if (auto eq = equality_part_of_on(j.on_expr, scope.names[ji + 1], scope.bare[ji + 1], right_cols)) {
                    const std::string left_ref = eq->left_ref, right_col = eq->right_col;
                    // two text columns are equal when the texts are ("007" and "7" are not); a text column against a number column
                    // compares by number (the text by its leading number), which no hash of the text can answer: the nested loop does
                    const bool text_vs_number = (eq->left_class == ValueClass::Text && eq->right_class == ValueClass::Number) ||
                                                (eq->left_class == ValueClass::Number && eq->right_class == ValueClass::Text);
                    if (!text_vs_number) {
                        hashed = hashed_join_verified(
                            current, right_rows, jq, j.join_type, [&](const Row& l) { return get_col(l, left_ref); },
                            [&](const Row& r) -> const std::string* {
                                auto it = r.find(right_col);
                                return it != r.end() ? &it->second : nullptr;
                            },
                            right_schema_cols, [&](const Row& merged) { return eval_condexpr(merged, j.on_expr); }, left_pad,
                            eq->left_class == ValueClass::Text && eq->right_class == ValueClass::Text);
                    }
                }
            }

            if (hashed) {
                current = std::move(*hashed);
            } else if (algo && std::get_if<JoinAlgo::SortMerge>(algo)) {
                auto* a = std::get_if<JoinAlgo::SortMerge>(algo);
                current = sort_merge_join(current, right_rows, j.join_type, jq, a->probe_col, a->build_col, right_schema_cols);
            } else if (algo && std::get_if<JoinAlgo::Hash>(algo)) {
                auto* a = std::get_if<JoinAlgo::Hash>(algo);
                current = hash_join(current, right_rows, j.join_type, jq, a->probe_col, a->build_col, right_schema_cols);
            } else if (algo && std::get_if<JoinAlgo::IndexNL>(algo)) {
                auto* a = std::get_if<JoinAlgo::IndexNL>(algo);
                // Index Nested Loop: probe right table's PK B+Tree per left row.
                // Only applies outside transactions (session_rows path already loaded above).
                if (txn.is_active()) {
                    current = hash_join(current, right_rows, j.join_type, jq, a->probe_col, a->right_pk_col, right_schema_cols);
                } else if (auto rit = s.indexes.find(j.table); rit != s.indexes.end()) {
                    std::vector<Row> out;
                    out.reserve(current.size());
                    // Many left rows share a key: look it up (and parse the right rows' JSON) once per distinct key.
                    std::unordered_map<std::string, std::vector<Row>> right_of_key;
                    for (auto& left_row : current) {
                        const std::string* key = get_col(left_row, a->probe_col);
                        if (!key || *key == "NULL") continue;
                        auto cached = right_of_key.find(*key);
                        if (cached == right_of_key.end()) {
                            std::vector<Row> found;
                            for (auto& val_json : equal_entries(rit->second, *key)) {
                                Row right_row = row_from_json(val_json);
                                if (is_visible_for_read(right_row, read_ctx)) found.push_back(std::move(right_row));
                            }
                            cached = right_of_key.emplace(*key, std::move(found)).first;
                        }
                        for (auto& right_row : cached->second) {
                            Row merged = left_row;
                            merge_right(merged, right_row, jq);
                            out.push_back(std::move(merged));
                        }
                    }
                    current = std::move(out);
                } else {
                    current = hash_join(current, right_rows, j.join_type, jq, a->probe_col, a->right_pk_col, right_schema_cols);
                }
            } else if (algo && std::get_if<JoinAlgo::ReverseIndexNL>(algo)) {
                auto* a = std::get_if<JoinAlgo::ReverseIndexNL>(algo);
                // Mirror image of IndexNL above: iterate the (small) RIGHT table, probe an
                // index on the LEFT/base table per right row. Only ever planned for the
                // base<->first-join step (Planner::plan_join's is_first_join gate), where
                // `current` still holds exactly the LEFT/base table's own rows -- never true
                // for a 2nd+ join, where `current` is an accumulated multi-table result with
                // no single backing index. Falls back to hash_join whenever a transaction is
                // active or the expected index is missing, exactly like IndexNL does.
                auto reverse_fallback = [&] {
                    current = hash_join(current, right_rows, j.join_type, jq, a->left_col, a->right_extract_col, right_schema_cols);
                };
                if (txn.is_active()) {
                    reverse_fallback();
                } else if (a->left_is_hash) {
                    if (auto hit = s.hash_indexes.find(a->left_index_key); hit != s.hash_indexes.end()) {
                        std::vector<Row> out;
                        out.reserve(right_rows.size());
                        for (auto& right_row : right_rows) {
                            const std::string* key = get_col(right_row, a->right_extract_col);
                            if (!key || *key == "NULL") continue;
                            for (auto& left_row : hit->second.get(*key)) {
                                if (!is_visible_for_read(left_row, read_ctx)) continue;
                                Row merged = left_row;
                                merge_right(merged, right_row, jq);
                                out.push_back(std::move(merged));
                            }
                        }
                        current = std::move(out);
                    } else {
                        reverse_fallback();
                    }
                } else if (auto lit = s.indexes.find(a->left_index_key); lit != s.indexes.end()) {
                    std::vector<Row> out;
                    out.reserve(right_rows.size());
                    for (auto& right_row : right_rows) {
                        const std::string* key = get_col(right_row, a->right_extract_col);
                        if (!key || *key == "NULL") continue;
                        for (auto& val_json : equal_entries(lit->second, *key)) {
                            // A secondary B+Tree index stores a JSON ARRAY of rows per key
                            // (the column need not be unique); a PK index stores exactly one
                            // Row object per key -- must parse each shape correctly.
                            if (a->left_is_secondary_btree) {
                                for (auto& left_row : rows_from_json(val_json)) {
                                    if (!is_visible_for_read(left_row, read_ctx)) continue;
                                    Row merged = left_row;
                                    merge_right(merged, right_row, jq);
                                    out.push_back(std::move(merged));
                                }
                            } else {
                                Row left_row = row_from_json(val_json);
                                if (is_visible_for_read(left_row, read_ctx)) {
                                    Row merged = left_row;
                                    merge_right(merged, right_row, jq);
                                    out.push_back(std::move(merged));
                                }
                            }
                        }
                    }
                    current = std::move(out);
                } else {
                    reverse_fallback();
                }
            } else {
                const CondExpr& on_expr = j.on_expr;
                current = nested_loop_join(current, right_rows, j.join_type, jq, j.using_cols, right_schema_cols,
                                                       [&on_expr](const Row& merged) { return eval_condexpr(merged, on_expr); }, left_pad);
            }

            // RIGHT / FULL JOIN ... USING (c): a right row without a partner has NULL in the left table's `c`, but the merged column
            // `c` that `*` and an unqualified `c` read is the right row's
            if (j.join_type == JoinType::Right || j.join_type == JoinType::FullOuter) {
                for (auto& c : joined_using[ji]) {
                    const std::string qualified = jq + "." + c;
                    for (auto& row : current) {
                        auto own = row.find(c);
                        auto right_value = row.find(qualified);
                        if (own != row.end() && right_value != row.end() && own->second == JOIN_NULL_VALUE) own->second = right_value->second;
                    }
                }
            }
        }
        if (!condition) {
            result = std::move(current);
        } else {
            for (auto& r : current) {
                if (matches_condition_with_subquery(s, r, condition)) result.push_back(std::move(r));
            }
        }
    }

    // The aggregates whose argument is an expression (`SUM(price * qty)`, `AVG(a + b)`, `COUNT(1)`, `SUM(COALESCE(x, 0))`): every row gets the value of the
    // expression under the text of the argument, and the aggregates and window functions read it as they read a column.
    std::vector<std::string> expression_arguments;
    {
        auto want = [&](const std::string& text) {
            if (is_expression_argument(text) && std::find(expression_arguments.begin(), expression_arguments.end(), text) == expression_arguments.end()) {
                expression_arguments.push_back(text);
            }
        };
        for (auto& c : columns) {
            if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                want(agg->source.empty() ? agg->col : agg->source); // (a conditional aggregate has the placeholder `__case__`, which is not an expression)
            } else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                want(aa->source.empty() ? aa->col : aa->source);
            } else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) {
                if (wf->col) want(*wf->col);
            }
        }
        for (auto& ref : expr_agg_refs) want(aggregate_reference_argument(ref));
        if (having) {
            for (auto& ref : extract_agg_refs_from_cond(*having)) want(aggregate_reference_argument(ref));
        }
    }
    if (!expression_arguments.empty()) {
        std::vector<ArithExpr> arguments;
        for (auto& text : expression_arguments) {
            ArithExpr e = Parser::str_to_arith(text);
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data); col && col->name == text) {
                return StringResult::Err("Cannot read the argument '" + text + "' of an aggregate");
            }
            arguments.push_back(std::move(e));
        }
        if (joins.empty() && !has_win) { // the rows are the table's own: they are copied to carry the values
            result.reserve(rows_p.size());
            for (const Row* r : rows_p) result.push_back(*r);
            rows_p.clear();
        }
        for (Row& r : result) {
            for (std::size_t i = 0; i < arguments.size(); i++) r[expression_arguments[i]] = eval_arith(r, arguments[i]);
        }
    }
    if (has_win) result = compute_window_functions(std::move(result), columns);
    const bool rows_in_table = joins.empty() && !has_win && expression_arguments.empty(); // else rows_p points into `result`
    if (!rows_in_table) {
        rows_p.reserve(result.size());
        for (auto& r : result) rows_p.push_back(&r);
    }

    if (!order_by.empty() && rows_p.size() > 1) { // stable, same order as comparing the rows pairwise with cmp_key
        std::vector<std::size_t> order = order_rows(rows_p, order_by, &Executor::get_col);
        std::vector<const Row*> sorted;
        sorted.reserve(rows_p.size());
        for (std::size_t i : order) sorted.push_back(rows_p[i]);
        rows_p = std::move(sorted);
    }

    if (group_by) {
        // Groups in order of first appearance, found through a hash table of the encoded key values (a group never owns
        // copies of its rows: it holds pointers).
        struct Group {
            std::vector<std::string> key;
            std::vector<const Row*> rows;
        };
        std::vector<Group> groups;
        std::unordered_map<std::string, std::size_t> group_of;
        static const std::string missing;
        std::vector<const std::string*> vals(group_by->size());
        std::string encoded;
        for (const Row* rp : rows_p) {
            encoded.clear();
            for (std::size_t i = 0; i < group_by->size(); i++) {
                const std::string* v = get_col(*rp, (*group_by)[i]);
                vals[i] = v ? v : &missing;
                append_key_part(encoded, *vals[i]);
            }
            auto [it, fresh] = group_of.try_emplace(encoded, groups.size());
            if (fresh) {
                Group g;
                g.key.reserve(vals.size());
                for (const std::string* v : vals) g.key.push_back(*v);
                groups.push_back(std::move(g));
            }
            groups[it->second].rows.push_back(rp);
        }

        // 그룹별 집계 row 생성: parallel_enabled() 이면 스레드별 1그룹, 아니면 순차
        std::vector<Row> group_rows(groups.size());
        auto make_group_row = [&](std::size_t gi) {
            auto& key = groups[gi].key;
            auto& grp = groups[gi].rows;
            Row out;
            for (std::size_t i = 0; i < group_by->size(); i++) out[(*group_by)[i]] = key[i];
            Row agg_row = compute_aggregates(grp, columns);
            for (auto& [k, v] : agg_row) out[k] = v;
            for (auto& ref : expr_agg_refs) {
                if (!out.count(ref)) out[ref] = compute_agg_from_key(ref, grp, aggregate_class(ref));
            }
            if (having) {
                for (auto& agg_key : extract_agg_refs_from_cond(*having)) {
                    if (!out.count(agg_key)) out[agg_key] = compute_agg_from_key(agg_key, grp, aggregate_class(agg_key));
                }
            }
            group_rows[gi] = std::move(out);
        };
        if (parallel_enabled()) {
            ThreadPool::global().parallel_for(groups.size(), make_group_row);
        } else {
            for (std::size_t gi = 0; gi < groups.size(); gi++) make_group_row(gi);
        }

        if (having) {
            std::vector<Row> filtered;
            for (auto& row : group_rows) {
                if (matches_condition_with_subquery(s, row, having)) filtered.push_back(std::move(row));
            }
            group_rows = std::move(filtered);
        }
        if (!order_by.empty()) {
            auto less = [&](const Row& a, const Row& b) { return row_order_less(a, b, order_by, &Executor::get_col); };
            if (parallel_enabled() && group_rows.size() >= parallel_min_rows()) {
                parallel_sort(group_rows, less); // unstable, matches Rust's par_sort_unstable_by
            } else {
                std::stable_sort(group_rows.begin(), group_rows.end(), less);
            }
        }
        if (offset) {
            std::size_t skip = std::min(*offset, group_rows.size());
            group_rows.erase(group_rows.begin(), group_rows.begin() + static_cast<std::ptrdiff_t>(skip));
        }
        if (limit && group_rows.size() > *limit) group_rows.resize(*limit);
        return format_result(s, std::move(group_rows), columns, table, joins);
    }

    if (having && !has_agg) { // (with aggregates and no GROUP BY, HAVING is about the one row of aggregates: below)
        std::vector<const Row*> kept;
        for (const Row* rp : rows_p) {
            if (matches_condition_with_subquery(s, *rp, having)) kept.push_back(rp);
        }
        rows_p = std::move(kept);
    }

    if (offset) {
        std::size_t skip = std::min(*offset, rows_p.size());
        rows_p.erase(rows_p.begin(), rows_p.begin() + static_cast<std::ptrdiff_t>(skip));
    }
    if (limit && rows_p.size() > *limit) rows_p.resize(*limit);

    if (distinct) {
        std::unordered_set<std::string> seen;
        std::vector<const Row*> kept;
        std::string encoded;
        for (const Row* rp : rows_p) {
            const Row& row = *rp;
            encoded.clear();
            for (auto& c : columns) {
                std::string val;
                if (std::holds_alternative<SelectColumn::All>(c.data)) {
                    std::string joined;
                    bool first = true;
                    for (auto& [k, v] : row) {
                        (void)k;
                        if (!first) joined += ",";
                        joined += v;
                        first = false;
                    }
                    val = joined;
                } else if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) {
                    // the same name resolution as the select list: `SELECT DISTINCT t.col` named a key the row does not have
                    // (it holds the bare name), so every row's key was empty and a single row survived
                    const std::string* v = get_col(row, col->name);
                    val = v ? *v : std::string();
                } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
                    const std::string* v = get_col(row, ca->name);
                    val = v ? *v : std::string();
                } else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                    auto it = row.find(agg->col);
                    val = it != row.end() ? it->second : std::string();
                } else if (auto* agg_a = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                    auto it = row.find(agg_a->col);
                    val = it != row.end() ? it->second : std::string();
                } else if (auto* f = std::get_if<SelectColumn::Func>(&c.data)) {
                    val = apply_scalar_func(f->name, f->args, row);
                } else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
                    auto resolve = [&](const std::string& sv) -> std::string {
                        const std::string* v = get_col(row, sv);
                        return v ? *v : sv;
                    };
                    val = cw->else_val ? resolve(*cw->else_val) : std::string();
                    for (auto& b : cw->branches) {
                        if (eval_condexpr(row, b.condition)) {
                            val = resolve(b.result);
                            break;
                        }
                    }
                } else if (auto* ex = std::get_if<SelectColumn::Expr>(&c.data)) {
                    val = eval_arith(row, ex->expr);
                } else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) {
                    std::string key_name = wf->alias.value_or(window_func_default_label(wf->func));
                    auto it = row.find(key_name);
                    val = it != row.end() ? it->second : std::string();
                } else {
                    // SelectColumn::Subquery: matches Rust's exact `String::new()` here —
                    // this DISTINCT/GROUP-BY dedup key computation deliberately doesn't
                    // evaluate the subquery (format_result's separate pre-pass does that
                    // for display purposes only).
                    val.clear();
                }
                append_key_part(encoded, val);
            }
            if (seen.insert(encoded).second) kept.push_back(rp);
        }
        rows_p = std::move(kept);
    }

    if (has_agg) {
        // Predicate lock (SSI phantom detection): an aggregate's result depends on every
        // row currently matching `condition`, so a phantom INSERT into that range can
        // silently change what COUNT/SUM/etc. would return. V1 gap closed: this used to
        // return before ever reaching the plain-SELECT predicate-lock block below, so
        // aggregate queries under SERIALIZABLE registered nothing at all. Mirrors that
        // block exactly (same range-narrowing, same whole-table fallback for a composite
        // or missing PK).
        if (txn.is_active() && txn.isolation_level() == IsolationLevel::Serializable) {
            std::string agg_pk_col = "id";
            std::size_t agg_pk_col_count = 0;
            if (auto* sc = s.catalog.get_table(table)) {
                for (auto& c : sc->columns) {
                    if (c.primary_key) {
                        if (agg_pk_col_count == 0) agg_pk_col = c.name;
                        agg_pk_col_count++;
                    }
                }
            }
            GapRange agg_range = agg_pk_col_count == 1 ? extract_pk_gap_range(condition, agg_pk_col) : GapRange{};
            s.lock_mgr.register_predicate_read(table, agg_range.lo, agg_range.lo_inclusive, agg_range.hi, agg_range.hi_inclusive,
                                                 txn.current_txn_id());
            // JOIN gap closed (see the identical block in the plain-SELECT path below for
            // the full reasoning): whole-table predicate for every joined table.
            for (auto& j : joins) {
                if (j.lateral) continue;
                s.lock_mgr.register_predicate_read(j.table, std::nullopt, true, std::nullopt, true, txn.current_txn_id());
            }
        }

        Row agg_row = compute_aggregates(rows_p, columns, /*allow_parallel=*/true);
        // HAVING without GROUP BY (`SELECT SUM(v) FROM t HAVING SUM(v) > 5`) keeps or drops the one row of aggregates; it used to be applied to the
        // rows before they were aggregated, where `SUM(v)` is nothing, so the aggregates came out over no row at all
        if (having) {
            for (auto& agg_key : extract_agg_refs_from_cond(*having)) {
                if (!agg_row.count(agg_key)) agg_row[agg_key] = compute_agg_from_key(agg_key, rows_p, aggregate_class(agg_key));
            }
            if (!matches_condition_with_subquery(s, agg_row, having)) return StringResult::Ok("0 rows returned.");
        }
        if (!expr_agg_refs.empty()) {
            // `SUM(v) + 1`, `ROUND(AVG(v), 2)`, `CASE WHEN COUNT(*) > 1 ...`: the aggregates inside are computed as HAVING computes
            // them and the columns are evaluated on this one row of aggregates. The plain aggregates keep their place; a column
            // with no aggregate in it is not part of an aggregate result (as before). Formatted like any result, minus the row
            // count line a plain aggregate result does not carry.
            for (auto& ref : expr_agg_refs) {
                if (!agg_row.count(ref)) agg_row[ref] = compute_agg_from_key(ref, rows_p, aggregate_class(ref));
            }
            std::vector<SelectColumn> shown;
            for (auto& col : columns) {
                if (column_has_aggregate(col)) shown.push_back(col);
            }
            auto text = format_result(s, std::vector<Row>{agg_row}, shown, table, joins);
            if (text.is_err()) return text;
            std::string out = text.value();
            static const std::string footer = "\n1 row(s) returned.";
            if (out.size() >= footer.size() && out.compare(out.size() - footer.size(), footer.size(), footer) == 0) out.erase(out.size() - footer.size());
            return StringResult::Ok(out);
        }
        std::vector<std::pair<std::string, std::string>> agg_results;
        for (auto& col : columns) {
            std::string label;
            if (auto* agg = std::get_if<SelectColumn::Agg>(&col.data)) label = agg_label(agg->func, agg->col);
            else if (auto* agg_a = std::get_if<SelectColumn::AggAlias>(&col.data)) label = agg_a->alias;
            else continue;
            auto it = agg_row.find(label);
            // Escaped up front (widths below are computed on the escaped form, matching
            // the visual padding actually written) -- see Executor::escape_cell.
            agg_results.emplace_back(escape_cell(label), escape_cell(it != agg_row.end() ? it->second : std::string()));
        }
        std::vector<std::size_t> widths;
        widths.reserve(agg_results.size());
        for (auto& [k, v] : agg_results) widths.push_back(std::max(k.size(), v.size()));
        std::string sep = "+";
        for (auto w : widths) sep += std::string(w + 2, '-') + "+";
        std::string out = sep + "\n|";
        for (std::size_t i = 0; i < agg_results.size(); i++) out += " " + agg_results[i].first + std::string(widths[i] - agg_results[i].first.size(), ' ') + " |";
        out += "\n" + sep + "\n|";
        for (std::size_t i = 0; i < agg_results.size(); i++) out += " " + agg_results[i].second + std::string(widths[i] - agg_results[i].second.size(), ' ') + " |";
        out += "\n" + sep;
        return StringResult::Ok(out);
    }

    // FOR UPDATE / FOR SHARE walk the rows again, and SELECT-list subqueries write their values into them: those statements
    // work on rows of their own; every other one formats the rows where they are.
    const bool own_rows = for_update || for_share ||
                          std::any_of(columns.begin(), columns.end(), [](const SelectColumn& c) { return std::holds_alternative<SelectColumn::Subquery>(c.data); });
    if (own_rows) { // the rows that are returned become `result`: copied out of the table, or moved out of the rows this statement made
        bool untouched = !rows_in_table && rows_p.size() == result.size(); // every row of `result`, in its own order: nothing to move
        for (std::size_t i = 0; untouched && i < rows_p.size(); i++) untouched = rows_p[i] == &result[i];
        if (!untouched) {
            std::vector<Row> out;
            out.reserve(rows_p.size());
            for (const Row* rp : rows_p) {
                if (rows_in_table) out.push_back(*rp);
                else out.push_back(std::move(result[static_cast<std::size_t>(rp - result.data())]));
            }
            result = std::move(out);
        }
    }

    if (for_update) {
        if (!txn.is_active()) return StringResult::Err("SELECT FOR UPDATE requires an active transaction (BEGIN first).");
        std::uint64_t txn_id = txn.current_txn_id();
        std::string pk_col = "id";
        std::size_t pk_col_count = 0;
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    if (pk_col_count == 0) pk_col = c.name;
                    pk_col_count++;
                }
            }
        }
        // Gap lock: only under RR/Serializable (matches InnoDB -- READ COMMITTED and
        // below allow phantoms by design, so no gap lock is taken there), and only for
        // single-column PK tables (V1 scope, matching extract_pk_eq_value/between_value).
        if (pk_col_count == 1 &&
            (txn.isolation_level() == IsolationLevel::RepeatableRead || txn.isolation_level() == IsolationLevel::Serializable)) {
            GapRange range = extract_pk_gap_range(condition, pk_col);
            s.lock_mgr.acquire_gap(table, range.lo, range.lo_inclusive, range.hi, range.hi_inclusive, txn_id);
        }
        // Real-blocking-wait stage: one deadline for the whole FOR UPDATE lock-acquisition
        // phase, matching exec_insert_inner's identical field.
        auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(lock_wait_timeout_ms);
        for (auto& row : result) {
            auto it = row.find(pk_col);
            std::string pk_val = it != row.end() ? it->second : std::string();
            LockResult lr = s.lock_mgr.acquire(table, pk_val, txn_id);
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (SELECT FOR UPDATE). Transaction " + std::to_string(txn_id) + " aborted.");
            }
            if (lr.kind == LockResult::Kind::Conflict) {
                // `result` is already a materialized snapshot -- blocking here never needs
                // a rescan the way UPDATE's mutation loop does, since nothing has been (or
                // will be) mutated by this loop. Just release both dispatcher-owned guards
                // (table_data_locks AND table_locks -- see release_table_locks_for_block's
                // doc comment: a statement blocked while still holding either can deadlock
                // against a concurrent COMMIT/write needing the other one EXCLUSIVE),
                // block on just this one row, and reacquire before touching `s` again.
                release_table_data_locks_for_block();
                release_table_locks_for_block();
                LockResult lr2 = block_on_row(s.lock_mgr, table, pk_val, txn_id, /*exclusive=*/true, lock_deadline);
                reacquire_table_locks_after_block();
                reacquire_table_data_locks_after_block(s);
                if (lr2.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                              std::to_string(lr2.holder) + " (SELECT FOR UPDATE). Transaction " + std::to_string(txn_id) +
                                              " aborted.");
                }
                if (lr2.kind != LockResult::Kind::Granted) {
                    return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded (" + std::to_string(lock_wait_timeout_ms) +
                                              "ms); row '" + pk_val + "' in '" + table + "' is held by transaction " +
                                              std::to_string(lr2.holder) + ". Retry or SET @lock_wait_timeout=<ms> to adjust.");
                }
            }
        }
    }

    if (for_share) {
        if (!txn.is_active()) return StringResult::Err("SELECT FOR SHARE requires an active transaction (BEGIN first).");
        std::uint64_t txn_id = txn.current_txn_id();
        std::string pk_col = "id";
        std::size_t pk_col_count = 0;
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    if (pk_col_count == 0) pk_col = c.name;
                    pk_col_count++;
                }
            }
        }
        if (pk_col_count == 1 &&
            (txn.isolation_level() == IsolationLevel::RepeatableRead || txn.isolation_level() == IsolationLevel::Serializable)) {
            GapRange range = extract_pk_gap_range(condition, pk_col);
            s.lock_mgr.acquire_gap(table, range.lo, range.lo_inclusive, range.hi, range.hi_inclusive, txn_id);
        }
        // Real-blocking-wait stage: see the FOR UPDATE block above for the full reasoning
        // (identical shape, exclusive=false here).
        auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(lock_wait_timeout_ms);
        for (auto& row : result) {
            auto it = row.find(pk_col);
            std::string pk_val = it != row.end() ? it->second : std::string();
            LockResult lr = s.lock_mgr.acquire_shared(table, pk_val, txn_id);
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (SELECT FOR SHARE). Transaction " + std::to_string(txn_id) + " aborted.");
            }
            if (lr.kind == LockResult::Kind::Conflict) {
                release_table_data_locks_for_block();
                release_table_locks_for_block();
                LockResult lr2 = block_on_row(s.lock_mgr, table, pk_val, txn_id, /*exclusive=*/false, lock_deadline);
                reacquire_table_locks_after_block();
                reacquire_table_data_locks_after_block(s);
                if (lr2.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                              std::to_string(lr2.holder) + " (SELECT FOR SHARE). Transaction " + std::to_string(txn_id) +
                                              " aborted.");
                }
                if (lr2.kind != LockResult::Kind::Granted) {
                    return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded (" + std::to_string(lock_wait_timeout_ms) +
                                              "ms); row '" + pk_val + "' in '" + table + "' is held exclusively by transaction " +
                                              std::to_string(lr2.holder) + ". Retry or SET @lock_wait_timeout=<ms> to adjust.");
                }
            }
        }
    }

    // Predicate lock (SSI phantom detection): a plain, unlocked SELECT under SERIALIZABLE
    // registers its WHERE-range as a predicate so a later phantom INSERT into it can fail
    // *this* transaction's COMMIT instead (see register_predicate_read's doc comment).
    // Unlike Gap Lock (for_update/for_share above), this never blocks the INSERT.
    // V1 gap closed: single-column-PK tables narrow the predicate to the WHERE range
    // (extract_pk_gap_range, same as before); a table with a composite or no PK can't be
    // narrowed this way, so it now registers a fully-unbounded (whole-table) predicate
    // rather than being skipped entirely -- wider than necessary, but never wrong (a
    // phantom anywhere in the table correctly fails this transaction's COMMIT).
    if (!for_update && !for_share && txn.is_active() && txn.isolation_level() == IsolationLevel::Serializable) {
        std::string pk_col = "id";
        std::size_t pk_col_count = 0;
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    if (pk_col_count == 0) pk_col = c.name;
                    pk_col_count++;
                }
            }
        }
        GapRange range = pk_col_count == 1 ? extract_pk_gap_range(condition, pk_col) : GapRange{};
        s.lock_mgr.register_predicate_read(table, range.lo, range.lo_inclusive, range.hi, range.hi_inclusive, txn.current_txn_id());

        // JOIN gap closed: a joined row has no single owning table to narrow a range
        // against (format_result's record_read is scoped to joins.empty() for the same
        // reason -- see its doc comment), so this can't reuse extract_pk_gap_range at all.
        // Register a fully-unbounded (whole-table) predicate for every joined table
        // instead: conservative (any write anywhere in a joined table fails this
        // transaction's COMMIT, not just ones affecting rows actually joined against), but
        // sound -- previously joined tables got no phantom protection whatsoever. Skips
        // `lateral` entries (a subquery alias, not a real catalog table to protect).
        for (auto& j : joins) {
            if (j.lateral) continue;
            s.lock_mgr.register_predicate_read(j.table, std::nullopt, true, std::nullopt, true, txn.current_txn_id());
        }
    }

    if (!own_rows) return format_rows(s, rows_p, columns, table, joins);
    return format_result(s, std::move(result), columns, table, joins);
}

StringResult Executor::exec_select_with_subquery(SharedDatabase& s, Statement inner_stmt, const std::string& alias, bool distinct,
                                                   std::vector<SelectColumn> columns, std::optional<CondExpr> condition, std::vector<Join> joins,
                                                   std::vector<OrderBy> order_by, std::optional<std::vector<std::string>> group_by,
                                                   std::optional<CondExpr> having, std::optional<std::size_t> limit,
                                                   std::optional<std::size_t> offset, bool for_update, bool for_share) {
    if (s.tables.count(alias) || s.views.count(alias)) return StringResult::Err("Alias '" + alias + "' conflicts with an existing table or view");

    const Statement inner_copy = inner_stmt; // (the names of an empty answer's columns come from the statement)
    auto inner_output = execute_with_s(s, std::move(inner_stmt));
    if (inner_output.is_err()) return inner_output;
    auto [col_names, virtual_rows] = parse_table_output(inner_output.value());
    if (col_names.empty()) col_names = derived_column_names(s, inner_copy);
    if (col_names.empty()) return StringResult::Ok("0 rows returned.");

    s.tables[alias] = virtual_rows;
    temporary_tables_.insert(alias);
    s.buffer_pool.write_page(alias, virtual_rows);
    std::vector<ColumnDef> schema_cols;
    for (auto& name : col_names) {
        ColumnDef c;
        c.name = name;
        c.data_type = DataType(DataType::Text{});
        schema_cols.push_back(c);
    }
    s.catalog.create_table(alias, schema_cols);

    auto result = exec_select(s, alias, std::nullopt, distinct, std::move(columns), std::move(condition), std::move(joins), std::move(order_by),
                               std::move(group_by), std::move(having), limit, offset, for_update, for_share);

    temporary_tables_.erase(alias);
    s.tables.erase(alias);
    s.buffer_pool.invalidate(alias);
    s.catalog.drop_table(alias);

    return result;
}

StringResult Executor::format_result(SharedDatabase& s, std::vector<Row> result, const std::vector<SelectColumn>& columns,
                                      const std::string& table, const std::vector<Join>& joins) {
    if (result.empty()) return StringResult::Ok("0 rows returned.");

    // Pre-compute SELECT-list scalar subqueries ("(SELECT ...) [AS alias]" columns)
    // and inject as "__sq_N__" keys into each row. Uncorrelated subqueries (no outer
    // row reference) execute once and get cached; correlated ones are substituted
    // and re-executed per row. Matches Rust's exact pre-pass at the top of
    // format_result, ported here since it was previously stubbed to an empty value.
    {
        std::vector<std::pair<std::size_t, const Statement*>> sq_queries;
        std::size_t sq_idx = 0;
        for (auto& c : columns) {
            if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                sq_queries.emplace_back(sq_idx, sq->query.get());
                sq_idx++;
            }
        }

        if (!sq_queries.empty()) {
            std::unordered_map<std::size_t, std::string> uncorr_cache;
            for (auto& [idx, query_ptr] : sq_queries) {
                auto* sel = std::get_if<Statement::Select>(&query_ptr->data);
                if (!sel) continue;
                bool is_correlated = sel->condition && has_outer_ref(*sel->condition);
                if (is_correlated) continue;

                Statement query_copy = *query_ptr;
                auto* sc = std::get_if<Statement::Select>(&query_copy.data);
                auto out = exec_select(s, sc->table, std::move(sc->subquery), sc->distinct, std::move(sc->columns), std::move(sc->condition),
                                        std::move(sc->joins), std::move(sc->order_by), std::move(sc->group_by), std::move(sc->having), sc->limit,
                                        sc->offset, false, false);
                if (out.is_err()) throw StatementError(out.error());
                auto vals = extract_values_from_output(out.value());
                if (vals.size() > 1) throw StatementError("Subquery returns more than 1 row");
                uncorr_cache[idx] = vals.empty() ? std::string(EXECUTOR_NULL_VALUE) : vals.front();
            }

            for (auto& row : result) {
                for (auto& [idx, query_ptr] : sq_queries) {
                    std::string key = "__sq_" + std::to_string(idx) + "__";
                    if (auto it = uncorr_cache.find(idx); it != uncorr_cache.end()) {
                        row[key] = it->second;
                        continue;
                    }
                    auto* sel = std::get_if<Statement::Select>(&query_ptr->data);
                    std::string val = EXECUTOR_NULL_VALUE;
                    if (sel) {
                        Statement query_copy = *query_ptr;
                        auto* sc = std::get_if<Statement::Select>(&query_copy.data);
                        std::optional<CondExpr> sub_cond =
                            sc->condition ? std::optional<CondExpr>(substitute_correlated_condexpr(*sc->condition, row)) : std::nullopt;
                        auto out = exec_select(s, sc->table, std::move(sc->subquery), sc->distinct, std::move(sc->columns), std::move(sub_cond),
                                                std::move(sc->joins), std::move(sc->order_by), std::move(sc->group_by), std::move(sc->having),
                                                sc->limit, sc->offset, false, false);
                        if (out.is_err()) throw StatementError(out.error());
                        auto vals = extract_values_from_output(out.value());
                        if (vals.size() > 1) throw StatementError("Subquery returns more than 1 row");
                        if (!vals.empty()) val = vals.front();
                    }
                    row[key] = val;
                }
            }
        }
    }

    std::vector<const Row*> rows;
    rows.reserve(result.size());
    for (auto& r : result) rows.push_back(&r);
    return format_rows(s, rows, columns, table, joins);
}

StringResult Executor::format_rows(SharedDatabase& s, const std::vector<const Row*>& rows, const std::vector<SelectColumn>& columns,
                                    const std::string& table, const std::vector<Join>& joins) {
    if (rows.empty()) return StringResult::Ok("0 rows returned.");

    // MVCC Stage 3: a Serializable transaction records every row it reads here (the
    // single choke point nearly every SELECT path -- fast index paths, generic scan,
    // groups -- funnels through) so validate_serializable can check at COMMIT whether any
    // of them were touched by another transaction that has since committed. Scoped to
    // single-table reads (joins.empty()) -- a joined/merged row has no single owning
    // table's PK to key the read-set on, and a real catalog table (not an ephemeral CTE/
    // subquery-derived alias, which s.catalog.get_table wouldn't resolve) to read from.
    if (joins.empty() && txn.is_active() && txn.isolation_level() == IsolationLevel::Serializable) {
        std::string pk_col;
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    pk_col = c.name;
                    break;
                }
            }
        }
        if (!pk_col.empty()) {
            for (const Row* rp : rows) {
                if (auto it = rp->find(pk_col); it != rp->end()) txn.record_read(table, pk_col, it->second);
            }
        }
    }

    struct ColSource {
        enum class Kind { Key, Func, CaseWhen, Expr } kind;
        std::string key;
        std::string func_name;
        std::vector<std::string> func_args;
        std::vector<CaseWhenBranch> branches;
        std::optional<std::string> else_val;
        ArithExpr expr;
    };

    bool has_all = std::any_of(columns.begin(), columns.end(), [](const SelectColumn& c) { return std::holds_alternative<SelectColumn::All>(c.data); });

    std::vector<std::pair<std::string, ColSource>> col_defs;
    if (has_all) {
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                ColSource src;
                src.kind = ColSource::Kind::Key;
                src.key = c.name;
                col_defs.emplace_back(c.name, src);
            }
        }
        for (auto& j : joins) {
            if (auto* sc = s.catalog.get_table(j.table)) {
                for (auto& c : sc->columns) {
                    ColSource src;
                    src.kind = ColSource::Kind::Key;
                    src.key = c.name;
                    col_defs.emplace_back(c.name, src);
                }
            }
        }
    } else {
        std::size_t sq_idx_col = 0;
        for (auto& c : columns) {
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) {
                auto dot = col->name.rfind('.');
                std::string header = dot == std::string::npos ? col->name : col->name.substr(dot + 1);
                ColSource src;
                src.kind = ColSource::Kind::Key;
                src.key = col->name;
                col_defs.emplace_back(header, src);
            } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
                ColSource src;
                src.kind = ColSource::Kind::Key;
                src.key = ca->name;
                col_defs.emplace_back(ca->alias, src);
            } else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                std::string lbl = agg_label(agg->func, agg->col);
                ColSource src;
                src.kind = ColSource::Kind::Key;
                src.key = lbl;
                col_defs.emplace_back(lbl, src);
            } else if (auto* agg_a = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                ColSource src;
                src.kind = ColSource::Kind::Key;
                src.key = agg_a->alias;
                col_defs.emplace_back(agg_a->alias, src);
            } else if (auto* f = std::get_if<SelectColumn::Func>(&c.data)) {
                std::string header = f->alias.value_or(f->name + "()");
                ColSource src;
                src.kind = ColSource::Kind::Func;
                src.func_name = f->name;
                src.func_args = f->args;
                col_defs.emplace_back(header, src);
            } else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
                std::string header = cw->alias.value_or("CASE");
                ColSource src;
                src.kind = ColSource::Kind::CaseWhen;
                src.branches = cw->branches;
                src.else_val = cw->else_val;
                col_defs.emplace_back(header, src);
            } else if (auto* ex = std::get_if<SelectColumn::Expr>(&c.data)) {
                std::string header = ex->alias.value_or(arith_to_str(ex->expr));
                ColSource src;
                src.kind = ColSource::Kind::Expr;
                src.expr = ex->expr;
                col_defs.emplace_back(header, src);
            } else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) {
                std::string header = wf->alias.value_or(window_func_default_label(wf->func));
                ColSource src;
                src.kind = ColSource::Kind::Key;
                src.key = header;
                col_defs.emplace_back(header, src);
            } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                std::string key = "__sq_" + std::to_string(sq_idx_col) + "__";
                sq_idx_col++;
                std::string header = sq->alias.value_or("(subquery)");
                ColSource src;
                src.kind = ColSource::Kind::Key;
                src.key = key;
                col_defs.emplace_back(header, src);
            }
            // SelectColumn::All handled above via has_all.
        }
    }

    std::vector<std::vector<std::string>> resolved_rows;
    resolved_rows.reserve(rows.size());
    for (const Row* row_ptr : rows) {
        const Row& row = *row_ptr;
        std::vector<std::string> vals;
        vals.reserve(col_defs.size());
        for (auto& [header, src] : col_defs) {
            (void)header;
            std::string raw;
            switch (src.kind) {
                case ColSource::Kind::Key: {
                    const std::string* v = get_col(row, src.key);
                    raw = v ? *v : std::string();
                    break;
                }
                case ColSource::Kind::Func:
                    raw = apply_scalar_func(src.func_name, src.func_args, row);
                    break;
                case ColSource::Kind::Expr:
                    raw = eval_arith(row, src.expr);
                    break;
                case ColSource::Kind::CaseWhen: {
                    auto resolve = [&](const std::string& sv) -> std::string {
                        const std::string* v = get_col(row, sv);
                        return v ? *v : sv;
                    };
                    raw = src.else_val ? resolve(*src.else_val) : EXECUTOR_NULL_VALUE;
                    for (auto& b : src.branches) {
                        if (eval_condexpr(row, b.condition)) {
                            raw = resolve(b.result);
                            break;
                        }
                    }
                    break;
                }
            }
            // Escaped up front (widths below are computed on the escaped form, matching
            // the visual padding actually written) -- see Executor::escape_cell.
            vals.push_back(escape_cell(raw == EXECUTOR_NULL_VALUE ? "NULL" : raw));
        }
        resolved_rows.push_back(std::move(vals));
    }

    std::vector<std::string> headers(col_defs.size());
    for (std::size_t i = 0; i < col_defs.size(); i++) headers[i] = escape_cell(col_defs[i].first);

    std::vector<std::size_t> col_widths(col_defs.size());
    for (std::size_t i = 0; i < col_defs.size(); i++) {
        std::size_t max_val = 0;
        for (auto& row_vals : resolved_rows) max_val = std::max(max_val, row_vals[i].size());
        col_widths[i] = std::max(headers[i].size(), max_val);
    }

    std::string separator = "+";
    for (auto w : col_widths) separator += std::string(w + 2, '-') + "+";

    std::string output = separator + "\n|";
    for (std::size_t i = 0; i < col_defs.size(); i++) {
        output += " " + headers[i] + std::string(col_widths[i] - headers[i].size(), ' ') + " |";
    }
    output += "\n" + separator + "\n";
    for (auto& row_vals : resolved_rows) {
        output += "|";
        for (std::size_t i = 0; i < row_vals.size(); i++) output += " " + row_vals[i] + std::string(col_widths[i] - row_vals[i].size(), ' ') + " |";
        output += "\n";
    }
    output += separator;
    output += "\n" + std::to_string(rows.size()) + " row(s) returned.";
    return StringResult::Ok(output);
}

} // namespace engine
