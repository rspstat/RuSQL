// Faithful port of the SELECT execution path from rusql-core/src/engine/executor.rs
// (Phase 8b): exec_select, exec_select_with_subquery, format_result, and the
// aggregate-function helpers (agg_label, extract_agg_refs_from_cond,
// compute_agg_from_key). See executor.hpp's exec_select declaration for the specific,
// documented scope exclusions (planner index fast paths, window functions,
// INFORMATION_SCHEMA, FROM `_dual_`, SELECT-list subqueries).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numeric>
#include <unordered_set>

#include "engine/join.hpp"
#include "engine/planner.hpp"
#include "engine/parallel_util.hpp"

namespace engine {

namespace {

std::optional<double> parse_f64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    double val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return val;
}

std::string format_4dp(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

std::string format_num_or_int(double v) {
    if (v == std::trunc(v)) return std::to_string(static_cast<long long>(v));
    return format_4dp(v);
}

int cmp_key(const std::string& a, const std::string& b) {
    auto pa = parse_f64(a);
    auto pb = parse_f64(b);
    if (pa && pb) {
        if (*pa < *pb) return -1;
        if (*pa > *pb) return 1;
        return 0;
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// Mirrors Rust's multi-key Ordering-based ORDER BY comparator as a strict-weak-order
// "less than" predicate. Looks up columns directly via Row::find rather than
// Executor::get_col's dotted-suffix fallback — ORDER BY column names in this port's
// current scope are always the row's own keys (join-qualified names included).
bool row_order_less(const Row& a, const Row& b, const std::vector<OrderBy>& order_by) {
    for (auto& ord : order_by) {
        auto ita = a.find(ord.column);
        auto itb = b.find(ord.column);
        std::string av = ita != a.end() ? ita->second : std::string();
        std::string bv = itb != b.end() ? itb->second : std::string();
        int c = cmp_key(av, bv);
        if (!ord.ascending) c = -c;
        if (c != 0) return c < 0;
    }
    return false;
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
                std::string out = alt.name + "(";
                for (std::size_t i = 0; i < alt.args.size(); i++) {
                    if (i) out += ",";
                    out += arith_to_str(alt.args[i]);
                }
                return out + ")";
            } else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) {
                return arith_to_str(*alt.lhs) + alt.op + arith_to_str(*alt.rhs);
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
    if (std::holds_alternative<AggFunc::CountCase>(func.data)) return "COUNT(CASE)";
    if (std::holds_alternative<AggFunc::SumCase>(func.data)) return "SUM(CASE)";
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
    }
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
    }
}

std::vector<std::string> Executor::extract_agg_refs_from_cond(const CondExpr& expr) {
    std::vector<std::string> out;
    collect_agg_refs_cond(expr, out);
    return out;
}

std::string Executor::compute_agg_from_key(const std::string& key, const std::vector<Row>& grp) {
    std::string ku = key;
    std::transform(ku.begin(), ku.end(), ku.begin(), [](unsigned char c) { return std::toupper(c); });
    if (ku.rfind("COUNT(", 0) == 0) return std::to_string(grp.size());

    auto lp = key.find('(');
    auto rp = key.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp <= lp) return "0";
    std::string inner = key.substr(lp + 1, rp - lp - 1);

    std::vector<double> vals;
    for (auto& r : grp) {
        auto it = r.find(inner);
        if (it != r.end()) {
            if (auto p = parse_f64(it->second)) vals.push_back(*p);
        }
    }
    double v = 0.0;
    if (ku.rfind("SUM(", 0) == 0) {
        v = std::accumulate(vals.begin(), vals.end(), 0.0);
    } else if (ku.rfind("AVG(", 0) == 0) {
        v = vals.empty() ? 0.0 : std::accumulate(vals.begin(), vals.end(), 0.0) / static_cast<double>(vals.size());
    } else if (ku.rfind("MIN(", 0) == 0) {
        v = vals.empty() ? std::numeric_limits<double>::infinity() : *std::min_element(vals.begin(), vals.end());
    } else if (ku.rfind("MAX(", 0) == 0) {
        v = vals.empty() ? -std::numeric_limits<double>::infinity() : *std::max_element(vals.begin(), vals.end());
    }
    return format_num_or_int(v);
}

Row Executor::compute_aggregates(const std::vector<Row>& grp, const std::vector<SelectColumn>& columns, bool allow_parallel) {
    Row out;
    for (auto& col : columns) {
        const AggFunc* func = nullptr;
        std::string col_name, label;
        if (auto* agg = std::get_if<SelectColumn::Agg>(&col.data)) {
            func = &agg->func;
            col_name = agg->col;
            label = agg_label(*func, col_name);
        } else if (auto* agg_a = std::get_if<SelectColumn::AggAlias>(&col.data)) {
            func = &agg_a->func;
            col_name = agg_a->col;
            label = agg_a->alias;
        } else {
            continue;
        }

        if (auto* gc = std::get_if<AggFunc::GroupConcat>(&func->data)) {
            std::vector<std::string> strs;
            for (auto& r : grp) {
                auto it = r.find(col_name);
                if (it != r.end() && it->second != EXECUTOR_NULL_VALUE) strs.push_back(it->second);
            }
            std::string joined;
            for (std::size_t i = 0; i < strs.size(); i++) {
                if (i) joined += gc->separator;
                joined += strs[i];
            }
            out[label] = joined;
            continue;
        }
        if (auto* cc = std::get_if<AggFunc::CountCase>(&func->data)) {
            std::size_t count = 0;
            for (auto& row : grp) {
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
            double sum = 0.0;
            for (auto& row : grp) {
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
                if (val != EXECUTOR_NULL_VALUE && !val.empty()) {
                    if (auto p = parse_f64(val)) sum += *p;
                }
            }
            out[label] = format_arith_result(sum);
            continue;
        }
        if (std::holds_alternative<AggFunc::Min>(func->data) || std::holds_alternative<AggFunc::Max>(func->data)) {
            bool is_min = std::holds_alternative<AggFunc::Min>(func->data);
            std::vector<std::string> raw;
            for (auto& r : grp) {
                auto it = r.find(col_name);
                if (it != r.end() && it->second != EXECUTOR_NULL_VALUE) raw.push_back(it->second);
            }
            std::string res;
            if (raw.empty()) {
                res = EXECUTOR_NULL_VALUE;
            } else {
                std::vector<double> nums;
                for (auto& rv : raw) {
                    if (auto p = parse_f64(rv)) nums.push_back(*p);
                }
                if (nums.size() == raw.size()) {
                    double v = is_min ? *std::min_element(nums.begin(), nums.end()) : *std::max_element(nums.begin(), nums.end());
                    res = format_num_or_int(v);
                } else {
                    res = is_min ? *std::min_element(raw.begin(), raw.end()) : *std::max_element(raw.begin(), raw.end());
                }
            }
            out[label] = res;
            continue;
        }

        // NOTE: Rust only parallelizes this specific value collection for the plain
        // (non-GROUP-BY, whole-result) aggregate call site, never for per-group
        // computation (group row counts are usually small) — hence the allow_parallel
        // flag rather than an unconditional threshold check here.
        std::vector<double> vals;
        if (allow_parallel && parallel_enabled() && grp.size() >= parallel_min_rows()) {
            std::size_t n_chunks = (grp.size() + PARALLEL_CHUNK - 1) / PARALLEL_CHUNK;
            std::vector<std::vector<double>> partial(n_chunks);
            ThreadPool::global().parallel_for(n_chunks, [&](std::size_t ci) {
                std::size_t start = ci * PARALLEL_CHUNK;
                std::size_t end = std::min(start + PARALLEL_CHUNK, grp.size());
                auto& out_vals = partial[ci];
                for (std::size_t i = start; i < end; i++) {
                    auto& r = grp[i];
                    if (col_name == "*") {
                        out_vals.push_back(1.0);
                        continue;
                    }
                    auto it = r.find(col_name);
                    if (it != r.end()) {
                        if (auto p = parse_f64(it->second)) out_vals.push_back(*p);
                    }
                }
            });
            for (auto& chunk : partial) {
                vals.insert(vals.end(), chunk.begin(), chunk.end());
            }
        } else {
            for (auto& r : grp) {
                if (col_name == "*") {
                    vals.push_back(1.0);
                    continue;
                }
                auto it = r.find(col_name);
                if (it != r.end()) {
                    if (auto p = parse_f64(it->second)) vals.push_back(*p);
                }
            }
        }
        auto distinct_vals = [&](const std::vector<Row>& rowsv) {
            std::unordered_set<std::string> seen;
            for (auto& r : rowsv) {
                auto it = r.find(col_name);
                if (it != r.end() && it->second != EXECUTOR_NULL_VALUE) seen.insert(it->second);
            }
            std::vector<double> res;
            for (auto& sv : seen) {
                if (auto p = parse_f64(sv)) res.push_back(*p);
            }
            return res;
        };

        double agg_val = 0.0;
        bool is_avg_like = false;
        if (std::holds_alternative<AggFunc::Count>(func->data)) {
            if (col_name == "*") {
                agg_val = static_cast<double>(grp.size());
            } else {
                std::size_t c = 0;
                for (auto& r : grp) {
                    auto it = r.find(col_name);
                    if (it != r.end() && it->second != EXECUTOR_NULL_VALUE) c++;
                }
                agg_val = static_cast<double>(c);
            }
        } else if (std::holds_alternative<AggFunc::CountDistinct>(func->data)) {
            std::unordered_set<std::string> distinct;
            for (auto& r : grp) {
                auto it = r.find(col_name);
                if (it != r.end() && it->second != EXECUTOR_NULL_VALUE) distinct.insert(it->second);
            }
            agg_val = static_cast<double>(distinct.size());
        } else if (std::holds_alternative<AggFunc::Sum>(func->data)) {
            agg_val = std::accumulate(vals.begin(), vals.end(), 0.0);
        } else if (std::holds_alternative<AggFunc::SumDistinct>(func->data)) {
            auto dv = distinct_vals(grp);
            agg_val = std::accumulate(dv.begin(), dv.end(), 0.0);
        } else if (std::holds_alternative<AggFunc::Avg>(func->data)) {
            agg_val = vals.empty() ? 0.0 : std::accumulate(vals.begin(), vals.end(), 0.0) / static_cast<double>(vals.size());
            is_avg_like = true;
        } else if (std::holds_alternative<AggFunc::AvgDistinct>(func->data)) {
            auto dv = distinct_vals(grp);
            agg_val = dv.empty() ? 0.0 : std::accumulate(dv.begin(), dv.end(), 0.0) / static_cast<double>(dv.size());
            is_avg_like = true;
        } else if (std::holds_alternative<AggFunc::Stddev>(func->data)) {
            if (!vals.empty()) {
                double mean = std::accumulate(vals.begin(), vals.end(), 0.0) / static_cast<double>(vals.size());
                double var = 0.0;
                for (auto v : vals) var += (v - mean) * (v - mean);
                var /= static_cast<double>(vals.size());
                agg_val = std::sqrt(var);
            }
            is_avg_like = true;
        } else if (std::holds_alternative<AggFunc::Variance>(func->data)) {
            if (!vals.empty()) {
                double mean = std::accumulate(vals.begin(), vals.end(), 0.0) / static_cast<double>(vals.size());
                double var = 0.0;
                for (auto v : vals) var += (v - mean) * (v - mean);
                agg_val = var / static_cast<double>(vals.size());
            }
            is_avg_like = true;
        }
        out[label] = is_avg_like ? format_4dp(agg_val) : format_num_or_int(agg_val);
    }
    return out;
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

        auto eval_col_val = [&](const SelectColumn& col) -> std::string {
            if (auto* v = std::get_if<SelectColumn::Func>(&col.data)) return apply_scalar_func(v->name, v->args, eval_row);
            if (auto* v = std::get_if<SelectColumn::Expr>(&col.data)) return eval_arith(eval_row, v->expr);
            if (auto* v = std::get_if<SelectColumn::Column>(&col.data)) {
                auto it = eval_row.find(v->name);
                return it != eval_row.end() ? it->second : v->name;
            }
            if (auto* v = std::get_if<SelectColumn::ColumnAlias>(&col.data)) {
                auto it = eval_row.find(v->name);
                return it != eval_row.end() ? it->second : v->name;
            }
            return "";
        };

        std::vector<std::string> vals;
        vals.reserve(col_defs.size());
        for (auto& cd : col_defs) vals.push_back(eval_col_val(*cd.col));

        std::vector<std::size_t> widths;
        widths.reserve(col_defs.size());
        for (std::size_t i = 0; i < col_defs.size(); i++) widths.push_back(std::max(col_defs[i].header.size(), vals[i].size()));

        std::string sep;
        for (auto w : widths) sep += "+" + std::string(w + 2, '-');
        sep += "+";

        std::string hdr;
        for (std::size_t i = 0; i < col_defs.size(); i++) {
            hdr += "| " + col_defs[i].header + std::string(widths[i] - col_defs[i].header.size(), ' ') + " ";
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

    // ── JOIN 순서 최적화 (cost-based DP, INNER-only; greedy 폴백) ──────────
    joins = reorder_joins_dp(table, std::move(joins), s.tables);

    // ── Planner: 인덱스 / 조인 알고리즘 결정 ──────────────────────────────
    bool has_agg = std::any_of(columns.begin(), columns.end(), [](const SelectColumn& c) {
        return std::holds_alternative<SelectColumn::Agg>(c.data) || std::holds_alternative<SelectColumn::AggAlias>(c.data);
    });
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
    if (use_fast_index_paths && joins.empty() && !has_agg && !has_win && !for_update && !for_share
        && !limit.has_value() && !offset.has_value() && order_by.empty() && !distinct) {
        auto& access = plan.base.access.data;
        if (auto* ap = std::get_if<AccessPath::PkPoint>(&access)) {
            if (auto it = s.indexes.find(table); it != s.indexes.end()) {
                if (auto val_json = it->second.search(ap->key)) {
                    Row row = nlohmann::json::parse(*val_json).get<Row>();
                    if (is_visible_for_read(row, read_ctx)) {
                        std::vector<Row> one{std::move(row)};
                        return format_result(s, std::move(one), columns, table, {});
                    }
                }
                return StringResult::Ok("0 rows returned.");
            }
        } else if (auto* ap = std::get_if<AccessPath::PkBetween>(&access)) {
            if (auto it = s.indexes.find(table); it != s.indexes.end()) {
                std::vector<Row> rows;
                for (auto& j : it->second.range_search(ap->start, ap->end)) {
                    Row r = nlohmann::json::parse(j).get<Row>();
                    if (is_visible_for_read(r, read_ctx)) rows.push_back(std::move(r));
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::PkRange>(&access)) {
            if (auto it = s.indexes.find(table); it != s.indexes.end()) {
                bool inclusive = range_op_inclusive(ap->op);
                auto pairs = range_op_is_lower_bound(ap->op) ? it->second.scan_from(ap->key, inclusive) : it->second.scan_to(ap->key, inclusive);
                std::vector<Row> rows;
                for (auto& [k, j] : pairs) {
                    Row r = nlohmann::json::parse(j).get<Row>();
                    if (is_visible_for_read(r, read_ctx)) rows.push_back(std::move(r));
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::HashPoint>(&access)) {
            if (auto it = s.hash_indexes.find(ap->index_key); it != s.hash_indexes.end()) {
                std::vector<Row> rows;
                for (auto& r : it->second.get(ap->key)) {
                    if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) rows.push_back(r);
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryPoint>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                if (auto json = it->second.search(ap->key)) {
                    if (plan.base.is_covering) {
                        auto arr = nlohmann::json::parse(*json);
                        std::size_t count = 0;
                        for (auto& v : arr) {
                            auto xit = v.find("_xmax");
                            bool zero = xit == v.end() || (xit->is_string() && xit->get<std::string>() == "0");
                            if (zero) count++;
                        }
                        std::vector<Row> synthetic;
                        synthetic.reserve(count);
                        for (std::size_t i = 0; i < count; i++) {
                            Row r;
                            r[ap->col] = ap->key;
                            synthetic.push_back(std::move(r));
                        }
                        return format_result(s, std::move(synthetic), columns, table, {});
                    }
                    std::vector<Row> rows;
                    for (auto& r : nlohmann::json::parse(*json).get<std::vector<Row>>()) {
                        if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) rows.push_back(r);
                    }
                    return format_result(s, std::move(rows), columns, table, {});
                }
                return StringResult::Ok("0 rows returned.");
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryRange>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                bool inclusive = range_op_inclusive(ap->op);
                auto pairs = range_op_is_lower_bound(ap->op) ? it->second.scan_from(ap->key, inclusive) : it->second.scan_to(ap->key, inclusive);
                if (plan.base.is_covering) {
                    std::vector<Row> synthetic;
                    for (auto& [k, json] : pairs) {
                        auto arr = nlohmann::json::parse(json);
                        std::size_t count = 0;
                        for (auto& v : arr) {
                            auto xit = v.find("_xmax");
                            bool zero = xit == v.end() || (xit->is_string() && xit->get<std::string>() == "0");
                            if (zero) count++;
                        }
                        for (std::size_t i = 0; i < count; i++) {
                            Row r;
                            r[ap->col] = k;
                            synthetic.push_back(std::move(r));
                        }
                    }
                    return format_result(s, std::move(synthetic), columns, table, {});
                }
                std::vector<Row> rows;
                for (auto& [k, json] : pairs) {
                    for (auto& r : nlohmann::json::parse(json).get<std::vector<Row>>()) {
                        if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) rows.push_back(r);
                    }
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryBetween>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                std::vector<Row> rows;
                for (auto& json : it->second.range_search(ap->start, ap->end)) {
                    for (auto& r : nlohmann::json::parse(json).get<std::vector<Row>>()) {
                        if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) rows.push_back(r);
                    }
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::CompositeIndexPath>(&access)) {
            auto eq_map = collect_eq_map(*condition);
            if (auto val_json = s.composite_indexes.at(ap->index_name).search_from_eq_map(eq_map)) {
                Row row = nlohmann::json::parse(*val_json).get<Row>();
                if (is_visible_for_read(row, read_ctx)) {
                    std::vector<Row> one{std::move(row)};
                    return format_result(s, std::move(one), columns, table, {});
                }
            }
            return StringResult::Ok("0 rows returned.");
        } else if (auto* ap = std::get_if<AccessPath::CompositeIndexPrefix>(&access)) {
            if (auto it = s.composite_indexes.find(ap->index_name); it != s.composite_indexes.end()) {
                std::vector<Row> rows;
                for (auto& j : it->second.prefix_scan(ap->prefix)) {
                    Row r = nlohmann::json::parse(j).get<Row>();
                    if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) rows.push_back(std::move(r));
                }
                return format_result(s, std::move(rows), columns, table, {});
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryLikePrefix>(&access)) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                std::vector<Row> rows;
                for (auto& [k, j] : it->second.scan_from(ap->prefix, true)) {
                    if (k.compare(0, ap->prefix.size(), ap->prefix) != 0) break;
                    for (auto& r : nlohmann::json::parse(j).get<std::vector<Row>>()) {
                        if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) rows.push_back(r);
                    }
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
            std::vector<std::unordered_set<std::string>> pk_sets;
            for (auto& sub_path : ap->paths) {
                std::unordered_set<std::string> pks;
                if (auto* sp = std::get_if<AccessPath::SecondaryPoint>(&sub_path.data)) {
                    if (auto it = s.indexes.find(sp->index_key); it != s.indexes.end()) {
                        if (auto json = it->second.search(sp->key)) {
                            for (auto& r : nlohmann::json::parse(*json).get<std::vector<Row>>()) {
                                if (is_visible_for_read(r, read_ctx)) {
                                    if (const std::string* v = get_col(r, pk_col)) pks.insert(*v);
                                }
                            }
                        }
                    }
                } else if (auto* hp = std::get_if<AccessPath::HashPoint>(&sub_path.data)) {
                    if (auto it = s.hash_indexes.find(hp->index_key); it != s.hash_indexes.end()) {
                        for (auto& r : it->second.get(hp->key)) {
                            if (is_visible_for_read(r, read_ctx)) {
                                if (const std::string* v = get_col(r, pk_col)) pks.insert(*v);
                            }
                        }
                    }
                }
                pk_sets.push_back(std::move(pks));
            }
            if (!pk_sets.empty()) {
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
    if (use_fast_index_paths && joins.empty() && !has_agg && !has_win && !for_update && !for_share && !distinct
        && !offset.has_value() && order_by.size() == 1 && limit.has_value()) {
        std::size_t lim = *limit;
        const OrderBy& ob = order_by[0];
        auto& access = plan.base.access.data;
        std::vector<Row> topk_rows;
        bool matched = false;
        if (auto* ap = std::get_if<AccessPath::SecondaryRange>(&access); ap && ob.column == ap->col) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                matched = true;
                bool inclusive = range_op_inclusive(ap->op);
                auto pairs = range_op_is_lower_bound(ap->op) ? it->second.scan_from(ap->key, inclusive) : it->second.scan_to(ap->key, inclusive);
                for (auto& [k, j] : pairs) {
                    for (auto& r : nlohmann::json::parse(j).get<std::vector<Row>>()) {
                        if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) topk_rows.push_back(r);
                    }
                }
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryBetween>(&access); ap && ob.column == ap->col) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                matched = true;
                for (auto& j : it->second.range_search(ap->start, ap->end)) {
                    for (auto& r : nlohmann::json::parse(j).get<std::vector<Row>>()) {
                        if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) topk_rows.push_back(r);
                    }
                }
            }
        } else if (auto* ap = std::get_if<AccessPath::SecondaryLikePrefix>(&access); ap && ob.column == ap->col) {
            if (auto it = s.indexes.find(ap->index_key); it != s.indexes.end()) {
                matched = true;
                for (auto& [k, j] : it->second.scan_from(ap->prefix, true)) {
                    if (k.compare(0, ap->prefix.size(), ap->prefix) != 0) break;
                    for (auto& r : nlohmann::json::parse(j).get<std::vector<Row>>()) {
                        if (is_visible_for_read(r, read_ctx) && matches_condition_with_subquery(s, r, condition)) topk_rows.push_back(r);
                    }
                }
            }
        }
        if (matched) {
            if (!ob.ascending) std::reverse(topk_rows.begin(), topk_rows.end());
            if (topk_rows.size() > lim) topk_rows.resize(lim);
            return format_result(s, std::move(topk_rows), columns, table, {});
        }
    }

    std::vector<Row> rows;
    if (auto it = s.tables.find(table); it != s.tables.end()) {
        rows = it->second;
    } else {
        rows = s.buffer_pool.get_page(table, s.disk);
    }

    std::vector<Row> visible_rows;
    visible_rows.reserve(rows.size());
    for (auto& r : rows) {
        if (is_visible_for_read(r, read_ctx)) visible_rows.push_back(std::move(r));
    }

    std::vector<Row> result;
    if (joins.empty()) {
        if (parallel_enabled() && visible_rows.size() >= parallel_min_rows() && condition.has_value()
            && !condition_has_subquery(condition)) {
            // 병렬 SeqScan 필터: 서브쿼리 없는 WHERE는 순수 정적 matches_condexpr로 평가 가능.
            // 청크마다 워커 스레드에 thread_local UDF 컨텍스트를 세팅해 사용자 정의 함수/DATABASE()도 정확히 평가.
            std::size_t n_chunks = (visible_rows.size() + PARALLEL_CHUNK - 1) / PARALLEL_CHUNK;
            std::vector<std::vector<Row>> chunk_results(n_chunks);
            auto uf = s.user_functions;
            std::string cur_db = current_db;
            std::string cur_user = auth_user;
            ThreadPool::global().parallel_for(n_chunks, [&](std::size_t ci) {
                std::size_t start = ci * PARALLEL_CHUNK;
                std::size_t end = std::min(start + PARALLEL_CHUNK, visible_rows.size());
                sync_udf_context(uf, cur_db, cur_user);
                auto& out = chunk_results[ci];
                for (std::size_t i = start; i < end; i++) {
                    if (matches_condexpr(visible_rows[i], condition)) out.push_back(visible_rows[i]);
                }
            });
            for (auto& chunk : chunk_results) {
                for (auto& r : chunk) result.push_back(std::move(r));
            }
        } else {
            for (auto& r : visible_rows) {
                if (matches_condition_with_subquery(s, r, condition)) result.push_back(std::move(r));
            }
        }
    } else {
        std::vector<Row> current = std::move(visible_rows);
        for (std::size_t ji = 0; ji < joins.size(); ji++) {
            auto& j = joins[ji];
            std::vector<Row> right_rows_raw;
            {
                auto it = s.tables.find(j.table);
                if (it == s.tables.end()) return StringResult::Err("Table '" + j.table + "' not found");
                right_rows_raw = it->second;
            }
            std::vector<Row> right_rows;
            right_rows.reserve(right_rows_raw.size());
            for (auto& r : right_rows_raw) {
                if (is_visible_for_read(r, read_ctx)) right_rows.push_back(std::move(r));
            }

            std::vector<std::string> right_schema_cols;
            if (auto* sc = s.catalog.get_table(j.table)) {
                for (auto& c : sc->columns) right_schema_cols.push_back(c.name);
            }

            // Cross/Natural/FullOuter joins always use nested loop (no hash/sort-merge)
            const JoinAlgo::Data* algo = nullptr;
            if (j.join_type != JoinType::Cross && j.join_type != JoinType::Natural && j.join_type != JoinType::FullOuter
                && ji < plan.joins.size()) {
                algo = &plan.joins[ji].algo.data;
            }

            if (algo && std::get_if<JoinAlgo::SortMerge>(algo)) {
                auto* a = std::get_if<JoinAlgo::SortMerge>(algo);
                current = sort_merge_join(current, right_rows, j.join_type, j.table, a->probe_col, a->build_col, right_schema_cols);
            } else if (algo && std::get_if<JoinAlgo::Hash>(algo)) {
                auto* a = std::get_if<JoinAlgo::Hash>(algo);
                current = hash_join(current, right_rows, j.join_type, j.table, a->probe_col, a->build_col, right_schema_cols);
            } else if (algo && std::get_if<JoinAlgo::IndexNL>(algo)) {
                auto* a = std::get_if<JoinAlgo::IndexNL>(algo);
                // Index Nested Loop: probe right table's PK B+Tree per left row.
                // Only applies outside transactions (session_rows path already loaded above).
                if (txn.is_active()) {
                    current = hash_join(current, right_rows, j.join_type, j.table, a->probe_col, a->probe_col, right_schema_cols);
                } else if (auto rit = s.indexes.find(j.table); rit != s.indexes.end()) {
                    std::vector<Row> out;
                    out.reserve(current.size());
                    for (auto& left_row : current) {
                        const std::string* key = get_col(left_row, a->probe_col);
                        if (!key || key->empty() || *key == "NULL") continue;
                        if (auto val_json = rit->second.search(*key)) {
                            Row right_row = nlohmann::json::parse(*val_json).get<Row>();
                            if (is_visible_for_read(right_row, read_ctx)) {
                                Row merged = left_row;
                                merge_right(merged, right_row, j.table);
                                out.push_back(std::move(merged));
                            }
                        }
                    }
                    current = std::move(out);
                } else {
                    current = hash_join(current, right_rows, j.join_type, j.table, a->probe_col, a->probe_col, right_schema_cols);
                }
            } else {
                const CondExpr& on_expr = j.on_expr;
                current = nested_loop_join(current, right_rows, j.join_type, j.table, j.using_cols, right_schema_cols,
                                                       [&on_expr](const Row& merged) { return eval_condexpr(merged, on_expr); });
            }
        }
        for (auto& r : current) {
            if (matches_condition_with_subquery(s, r, condition)) result.push_back(std::move(r));
        }
    }

    if (has_win) result = compute_window_functions(std::move(result), columns);

    if (!order_by.empty()) {
        auto less = [&](const Row& a, const Row& b) { return row_order_less(a, b, order_by); };
        if (parallel_enabled() && result.size() >= parallel_min_rows()) {
            parallel_sort(result, less); // unstable, matches Rust's par_sort_unstable_by
        } else {
            std::stable_sort(result.begin(), result.end(), less);
        }
    }

    if (group_by) {
        std::vector<std::vector<std::string>> group_order;
        std::map<std::vector<std::string>, std::vector<Row>> group_data;
        // 병렬 ON + 충분한 행 → 청크별 독립 map 병렬 구성 후 순차 merge
        // (그룹 수가 적어 par_iter over groups는 오히려 느림 — 행 스캔 단계를 병렬화)
        if (parallel_enabled() && result.size() >= parallel_min_rows()) {
            std::size_t n_chunks = (result.size() + PARALLEL_CHUNK - 1) / PARALLEL_CHUNK;
            std::vector<std::vector<std::vector<std::string>>> partial_order(n_chunks);
            std::vector<std::map<std::vector<std::string>, std::vector<Row>>> partial_data(n_chunks);
            ThreadPool::global().parallel_for(n_chunks, [&](std::size_t ci) {
                std::size_t start = ci * PARALLEL_CHUNK;
                std::size_t end = std::min(start + PARALLEL_CHUNK, result.size());
                auto& order = partial_order[ci];
                auto& map = partial_data[ci];
                for (std::size_t i = start; i < end; i++) {
                    auto& row = result[i];
                    std::vector<std::string> key;
                    key.reserve(group_by->size());
                    for (auto& c : *group_by) {
                        const std::string* v = get_col(row, c);
                        key.push_back(v ? *v : std::string());
                    }
                    if (map.find(key) == map.end()) order.push_back(key);
                    map[key].push_back(row);
                }
            });
            for (std::size_t ci = 0; ci < n_chunks; ci++) {
                for (auto& key : partial_order[ci]) {
                    auto& rows_for_key = partial_data[ci].at(key);
                    if (group_data.find(key) == group_data.end()) group_order.push_back(key);
                    auto& dst = group_data[key];
                    dst.insert(dst.end(), std::make_move_iterator(rows_for_key.begin()), std::make_move_iterator(rows_for_key.end()));
                }
            }
        } else {
            for (auto& row : result) {
                std::vector<std::string> key;
                key.reserve(group_by->size());
                for (auto& c : *group_by) {
                    const std::string* v = get_col(row, c);
                    key.push_back(v ? *v : std::string());
                }
                if (group_data.find(key) == group_data.end()) group_order.push_back(key);
                group_data[key].push_back(row);
            }
        }

        // 그룹별 집계 row 생성: parallel_enabled() 이면 스레드별 1그룹, 아니면 순차
        std::vector<Row> group_rows(group_order.size());
        auto make_group_row = [&](std::size_t gi) {
            auto& key = group_order[gi];
            auto& grp = group_data.at(key);
            Row out;
            for (std::size_t i = 0; i < group_by->size(); i++) out[(*group_by)[i]] = key[i];
            Row agg_row = compute_aggregates(grp, columns);
            for (auto& [k, v] : agg_row) out[k] = v;
            if (having) {
                for (auto& agg_key : extract_agg_refs_from_cond(*having)) {
                    if (!out.count(agg_key)) out[agg_key] = compute_agg_from_key(agg_key, grp);
                }
            }
            group_rows[gi] = std::move(out);
        };
        if (parallel_enabled()) {
            ThreadPool::global().parallel_for(group_order.size(), make_group_row);
        } else {
            for (std::size_t gi = 0; gi < group_order.size(); gi++) make_group_row(gi);
        }

        if (having) {
            std::vector<Row> filtered;
            for (auto& row : group_rows) {
                if (matches_condexpr(row, having)) filtered.push_back(std::move(row));
            }
            group_rows = std::move(filtered);
        }
        if (!order_by.empty()) {
            auto less = [&](const Row& a, const Row& b) { return row_order_less(a, b, order_by); };
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
        return format_result(s, group_rows, columns, table, joins);
    }

    if (having) {
        std::vector<Row> filtered;
        for (auto& row : result) {
            if (matches_condexpr(row, having)) filtered.push_back(std::move(row));
        }
        result = std::move(filtered);
    }

    if (offset) {
        std::size_t skip = std::min(*offset, result.size());
        result.erase(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(skip));
    }
    if (limit && result.size() > *limit) result.resize(*limit);

    if (distinct) {
        std::vector<std::vector<std::string>> seen;
        std::vector<Row> filtered;
        for (auto& row : result) {
            std::vector<std::string> key;
            key.reserve(columns.size());
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
                    auto it = row.find(col->name);
                    val = it != row.end() ? it->second : std::string();
                } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
                    auto it = row.find(ca->name);
                    val = it != row.end() ? it->second : std::string();
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
                key.push_back(std::move(val));
            }
            if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
                seen.push_back(key);
                filtered.push_back(std::move(row));
            }
        }
        result = std::move(filtered);
    }

    if (has_agg) {
        Row agg_row = compute_aggregates(result, columns, /*allow_parallel=*/true);
        std::vector<std::pair<std::string, std::string>> agg_results;
        for (auto& col : columns) {
            std::string label;
            if (auto* agg = std::get_if<SelectColumn::Agg>(&col.data)) label = agg_label(agg->func, agg->col);
            else if (auto* agg_a = std::get_if<SelectColumn::AggAlias>(&col.data)) label = agg_a->alias;
            else continue;
            auto it = agg_row.find(label);
            agg_results.emplace_back(label, it != agg_row.end() ? it->second : std::string());
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
        for (auto& row : result) {
            auto it = row.find(pk_col);
            std::string pk_val = it != row.end() ? it->second : std::string();
            LockResult lr = s.lock_mgr.acquire(table, pk_val, txn_id);
            if (lr.kind == LockResult::Kind::Conflict) {
                return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded (" + std::to_string(lock_wait_timeout_ms) +
                                          "ms); row '" + pk_val + "' in '" + table + "' is held by transaction " + std::to_string(lr.holder) +
                                          ". Retry or SET @lock_wait_timeout=<ms> to adjust.");
            }
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (SELECT FOR UPDATE). Transaction " + std::to_string(txn_id) + " aborted.");
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
        for (auto& row : result) {
            auto it = row.find(pk_col);
            std::string pk_val = it != row.end() ? it->second : std::string();
            LockResult lr = s.lock_mgr.acquire_shared(table, pk_val, txn_id);
            if (lr.kind == LockResult::Kind::Conflict) {
                return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded (" + std::to_string(lock_wait_timeout_ms) +
                                          "ms); row '" + pk_val + "' in '" + table + "' is held exclusively by transaction " +
                                          std::to_string(lr.holder) + ". Retry or SET @lock_wait_timeout=<ms> to adjust.");
            }
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (SELECT FOR SHARE). Transaction " + std::to_string(txn_id) + " aborted.");
            }
        }
    }

    return format_result(s, result, columns, table, joins);
}

StringResult Executor::exec_select_with_subquery(SharedDatabase& s, Statement inner_stmt, const std::string& alias, bool distinct,
                                                   std::vector<SelectColumn> columns, std::optional<CondExpr> condition, std::vector<Join> joins,
                                                   std::vector<OrderBy> order_by, std::optional<std::vector<std::string>> group_by,
                                                   std::optional<CondExpr> having, std::optional<std::size_t> limit,
                                                   std::optional<std::size_t> offset, bool for_update, bool for_share) {
    if (s.tables.count(alias) || s.views.count(alias)) return StringResult::Err("Alias '" + alias + "' conflicts with an existing table or view");

    auto inner_output = execute_with_s(s, std::move(inner_stmt));
    if (inner_output.is_err()) return inner_output;
    auto [col_names, virtual_rows] = parse_table_output(inner_output.value());
    if (col_names.empty()) return StringResult::Ok("0 rows returned.");

    s.tables[alias] = virtual_rows;
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

    s.tables.erase(alias);
    s.buffer_pool.invalidate(alias);
    s.catalog.drop_table(alias);

    return result;
}

StringResult Executor::format_result(SharedDatabase& s, std::vector<Row> result, const std::vector<SelectColumn>& columns,
                                      const std::string& table, const std::vector<Join>& joins) {
    if (result.empty()) return StringResult::Ok("0 rows returned.");

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
            for (auto& row : result) {
                if (auto it = row.find(pk_col); it != row.end()) txn.record_read(table, pk_col, it->second);
            }
        }
    }

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
                std::string val = EXECUTOR_NULL_VALUE;
                if (out.is_ok()) {
                    auto vals = extract_values_from_output(out.value());
                    if (!vals.empty()) val = vals.front();
                }
                uncorr_cache[idx] = val;
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
                        if (out.is_ok()) {
                            auto vals = extract_values_from_output(out.value());
                            if (!vals.empty()) val = vals.front();
                        }
                    }
                    row[key] = val;
                }
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
    resolved_rows.reserve(result.size());
    for (auto& row : result) {
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
            vals.push_back(raw == EXECUTOR_NULL_VALUE ? "NULL" : raw);
        }
        resolved_rows.push_back(std::move(vals));
    }

    std::vector<std::size_t> col_widths(col_defs.size());
    for (std::size_t i = 0; i < col_defs.size(); i++) {
        std::size_t max_val = 0;
        for (auto& row_vals : resolved_rows) max_val = std::max(max_val, row_vals[i].size());
        col_widths[i] = std::max(col_defs[i].first.size(), max_val);
    }

    std::string separator = "+";
    for (auto w : col_widths) separator += std::string(w + 2, '-') + "+";

    std::string output = separator + "\n|";
    for (std::size_t i = 0; i < col_defs.size(); i++) {
        output += " " + col_defs[i].first + std::string(col_widths[i] - col_defs[i].first.size(), ' ') + " |";
    }
    output += "\n" + separator + "\n";
    for (auto& row_vals : resolved_rows) {
        output += "|";
        for (std::size_t i = 0; i < row_vals.size(); i++) output += " " + row_vals[i] + std::string(col_widths[i] - row_vals[i].size(), ' ') + " |";
        output += "\n";
    }
    output += separator;
    output += "\n" + std::to_string(result.size()) + " row(s) returned.";
    return StringResult::Ok(output);
}

} // namespace engine
