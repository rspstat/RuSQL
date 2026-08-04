// Gap Lock support (InnoDB-style phantom-read prevention): range extraction from a WHERE
// clause's PK-column leaves, and range-containment comparison. Deliberately kept in the
// executor layer (not LockManager) -- LockManager stores/serves gap ranges as opaque
// strings + a deadlock graph, with zero SQL/comparison semantics of its own.

#include "engine/executor/executor.hpp"

#include <charconv>

#include "engine/planner.hpp"

namespace engine {

namespace {

// Duplicated from executor_eval.cpp's anonymous-namespace parse_f64/cmp_num (a different
// translation unit, so not directly reusable) -- must stay in lockstep with eval_single's
// BETWEEN/Gt/Lt/Gte/Lte comparison semantics so gap-lock containment agrees with how the
// same value would evaluate against the original WHERE clause.
std::optional<double> parse_f64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    double val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return val;
}

// -1/0/1, numeric-priority with lexicographic fallback (mirrors cmp_num + the plain
// string-compare fallback used throughout eval_single).
int cmp_val(const std::string& a, const std::string& b) {
    auto da = parse_f64(a);
    auto db = parse_f64(b);
    if (da && db) return (*da < *db) ? -1 : (*da > *db ? 1 : 0);
    return (a < b) ? -1 : (a > b ? 1 : 0);
}

void narrow_lo(GapRange& range, const std::string& v, bool inclusive) {
    if (!range.lo) {
        range.lo = v;
        range.lo_inclusive = inclusive;
        return;
    }
    int c = cmp_val(v, *range.lo);
    if (c > 0 || (c == 0 && !inclusive && range.lo_inclusive)) {
        range.lo = v;
        range.lo_inclusive = inclusive;
    }
}

void narrow_hi(GapRange& range, const std::string& v, bool inclusive) {
    if (!range.hi) {
        range.hi = v;
        range.hi_inclusive = inclusive;
        return;
    }
    int c = cmp_val(v, *range.hi);
    if (c < 0 || (c == 0 && !inclusive && range.hi_inclusive)) {
        range.hi = v;
        range.hi_inclusive = inclusive;
    }
}

} // namespace

GapRange Executor::extract_pk_gap_range(const std::optional<CondExpr>& condition, const std::string& pk_col) {
    GapRange range; // both bounds nullopt == fully unbounded (whole-table gap)
    if (!condition) return range;

    // collect_and_leaves only flattens AND-joined leaves -- an OR/NOT anywhere in the
    // tree means the leaves under it contribute nothing, which can leave `leaves` empty
    // (falling back to the unbounded range below). That's conservative and safe: a wider
    // gap lock only costs concurrency, never correctness.
    auto leaves = collect_and_leaves(*condition);
    for (auto* cond : leaves) {
        auto* col = std::get_if<ArithExpr::Col>(&cond->left.data);
        if (!col || col->name != pk_col) continue;

        if (auto* lit = std::get_if<ConditionValue::Literal>(&cond->value.data)) {
            const std::string& v = lit->value;
            switch (cond->op) {
                case Operator::Eq:
                    narrow_lo(range, v, true);
                    narrow_hi(range, v, true);
                    break;
                case Operator::Gt:
                    narrow_lo(range, v, false);
                    break;
                case Operator::Gte:
                    narrow_lo(range, v, true);
                    break;
                case Operator::Lt:
                    narrow_hi(range, v, false);
                    break;
                case Operator::Lte:
                    narrow_hi(range, v, true);
                    break;
                default:
                    break; // In/Like/IsNull/... don't narrow a contiguous range
            }
        } else if (auto* bv = std::get_if<ConditionValue::Between>(&cond->value.data)) {
            if (cond->op == Operator::Between) {
                narrow_lo(range, bv->lo, true);
                narrow_hi(range, bv->hi, true);
            }
        }
    }
    return range;
}

bool Executor::gap_range_contains(const GapRange& range, const std::string& value) {
    if (range.lo) {
        int c = cmp_val(value, *range.lo);
        if (c < 0 || (c == 0 && !range.lo_inclusive)) return false;
    }
    if (range.hi) {
        int c = cmp_val(value, *range.hi);
        if (c > 0 || (c == 0 && !range.hi_inclusive)) return false;
    }
    return true;
}

} // namespace engine
