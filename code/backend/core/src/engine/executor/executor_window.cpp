// Faithful port of window-function evaluation from rusql-core/src/engine/executor.rs
// (Phase 8c): frame_bounds, win_order_eq, compute_window_functions (RowNumber, Rank,
// DenseRank, Lag, Lead, FirstValue, LastValue, NthValue, Sum, Avg, Count, Min, Max,
// Ntile, PercentRank, CumeDist).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

#include "engine/numeric_text.hpp"
#include "engine/sql_value_util.hpp"

namespace engine {

namespace {
std::optional<double> parse_f64(const std::string& s) { return parse_number(s); }

// ---- RANGE frames with an offset: the frame is a range of ORDER BY VALUES ------------------------------------------------------------------------------
// `RANGE BETWEEN 10 PRECEDING AND CURRENT ROW` reaches the rows whose ORDER BY value is at most 10 below the current row's (not the 10 rows before it);
// `RANGE BETWEEN INTERVAL 7 DAY PRECEDING AND CURRENT ROW` does the same for a date. The window orders by exactly one numeric or temporal value. A row whose value
// is NULL has the other NULL rows for a frame that ends at an offset, and the NULL rows are not in the frame of any other row.

bool is_offset_bound(const FrameBound& b) { return std::holds_alternative<FrameBound::Preceding>(b.data) || std::holds_alternative<FrameBound::Following>(b.data); }

// the INTERVAL unit of an offset bound ("" for a plain number); nothing for a bound that has no offset
const std::string* bound_unit(const FrameBound& b) {
    if (auto* p = std::get_if<FrameBound::Preceding>(&b.data)) return &p->unit;
    if (auto* f = std::get_if<FrameBound::Following>(&b.data)) return &f->unit;
    return nullptr;
}

// the date and time as microseconds since 1970-01-01 (as if UTC)
long double temporal_micro(const sqlval::DateTime& t) { return static_cast<long double>(t.epoch_seconds()) * 1000000.0L + static_cast<long double>(t.micro); }

// the date `months` later (or earlier): the day is cut back to the last day of a shorter month (2024-01-31 + 1 MONTH is 2024-02-29)
sqlval::DateTime add_months(sqlval::DateTime t, long long months) {
    long long index = t.year * 12 + (t.month - 1) + months;
    t.year = index >= 0 ? index / 12 : -((-index + 11) / 12);
    t.month = static_cast<int>(index - t.year * 12) + 1;
    t.day = std::min(t.day, sqlval::days_in_month(t.year, t.month));
    return t;
}

// The microseconds of an INTERVAL unit that has a fixed length (MONTH, QUARTER and YEAR have none: they are counted on the calendar)
std::optional<long double> unit_micro(const std::string& unit) {
    if (unit == "MICROSECOND") return 1.0L;
    if (unit == "SECOND") return 1e6L;
    if (unit == "MINUTE") return 60e6L;
    if (unit == "HOUR") return 3600e6L;
    if (unit == "DAY") return 86400e6L;
    if (unit == "WEEK") return 7 * 86400e6L;
    return std::nullopt;
}

// The ORDER BY values of one partition, in the order of the partition, as a position on one number line that grows along the order (a DESC order flips the
// sign, a date is its microseconds): the rows with a value are `first` to `last`, the NULL rows come before or after them.
struct RangeAxis {
    bool temporal = false;
    std::size_t first = 0, last = 0;
    bool any = false;
    std::vector<long double> position; // of the rows first..last
    std::vector<std::string> text;     // their ORDER BY values as written (a month offset is worked out on the date)
};

int cmp_key(const std::string& a, const std::string& b, ValueClass cls) {
    const bool a_null = a == "NULL", b_null = b == "NULL"; // NULL sorts before every value
    if (a_null || b_null) return a_null == b_null ? 0 : (a_null ? -1 : 1);
    return compare_classed(cls, cls, a, b);
}

// PERCENT_RANK and CUME_DIST are DOUBLEs: the shortest text that reads back as the number (0.14285714285714285)
std::string fmt4(double v) { return format_shortest(v); }
} // namespace

std::pair<std::size_t, std::size_t> Executor::frame_bounds(std::size_t pos, std::size_t len, const std::optional<WindowFrame>& frame,
                                                             bool has_order) {
    if (frame) {
        // A frame is the rows from its start to its end, cut to the partition. One that holds no row (`ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING` on the last row,
        // `ROWS BETWEEN 2 PRECEDING AND 1 PRECEDING` on the first) is (1, 0): the start is after the end.
        const long long here = static_cast<long long>(pos), last = static_cast<long long>(len) - 1;
        auto first_row = [&](const FrameBound& b) -> long long {
            if (auto* p = std::get_if<FrameBound::Preceding>(&b.data)) return here - static_cast<long long>(p->n);
            if (auto* f = std::get_if<FrameBound::Following>(&b.data)) return here + static_cast<long long>(f->n);
            if (std::holds_alternative<FrameBound::CurrentRow>(b.data)) return here;
            return std::holds_alternative<FrameBound::UnboundedPreceding>(b.data) ? 0 : last;
        };
        auto last_row = [&](const FrameBound& b) -> long long {
            if (auto* p = std::get_if<FrameBound::Preceding>(&b.data)) return here - static_cast<long long>(p->n);
            if (auto* f = std::get_if<FrameBound::Following>(&b.data)) return here + static_cast<long long>(f->n);
            if (std::holds_alternative<FrameBound::CurrentRow>(b.data)) return here;
            return std::holds_alternative<FrameBound::UnboundedFollowing>(b.data) ? last : -1;
        };
        const long long start = std::max<long long>(first_row(frame->start), 0), end = std::min(last_row(frame->end), last);
        if (start > end) return {1, 0};
        return {static_cast<std::size_t>(start), static_cast<std::size_t>(end)};
    }
    if (has_order) return {0, pos};
    return {0, len == 0 ? 0 : len - 1};
}

bool Executor::win_order_eq(const Row& a, const Row& b, const std::vector<OrderBy>& order_by) {
    return std::all_of(order_by.begin(), order_by.end(), [&](const OrderBy& ord) {
        const std::string* av = get_col(a, ord.column);
        const std::string* bv = get_col(b, ord.column);
        std::string as = av ? *av : std::string();
        std::string bs = bv ? *bv : std::string();
        return cmp_key(as, bs, ord.cls) == 0; // (peers: equal by the comparison -- 'Alice' and 'alice' are one rank)
    });
}

std::vector<Row> Executor::compute_window_functions(std::vector<Row> rows, const std::vector<SelectColumn>& columns) {
    for (auto& col : columns) {
        auto* wf = std::get_if<SelectColumn::WinFunc>(&col.data);
        if (!wf) continue;

        std::string label = wf->alias.value_or(window_func_default_label(wf->func));

        std::size_t n = rows.size();
        std::vector<std::string> values(n);

        std::vector<std::vector<std::string>> partition_order;
        std::map<std::vector<std::string>, std::vector<std::size_t>> partition_map;
        for (std::size_t i = 0; i < rows.size(); i++) {
            std::vector<std::string> pk;
            for (auto& c : wf->partition_by) {
                const std::string* v = get_col(rows[i], c);
                pk.push_back(v ? collation::key(*v) : std::string());
            }
            if (partition_map.find(pk) == partition_map.end()) partition_order.push_back(pk);
            partition_map[pk].push_back(i);
        }

        for (auto& pk : partition_order) {
            std::vector<std::size_t> sorted = partition_map.at(pk);
            if (!wf->order_by.empty()) {
                std::stable_sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) {
                    for (auto& ord : wf->order_by) {
                        const std::string* avp = get_col(rows[a], ord.column);
                        const std::string* bvp = get_col(rows[b], ord.column);
                        std::string av = avp ? *avp : std::string();
                        std::string bv = bvp ? *bvp : std::string();
                        int c = cmp_key(av, bv, ord.cls);
                        if (!ord.ascending) c = -c;
                        if (c != 0) return c < 0;
                    }
                    return false;
                });
            }

            bool has_order = !wf->order_by.empty();
            std::size_t total = sorted.size();

            // The rows that tie in the ORDER BY are peers (with no ORDER BY every row is a peer of every other). A RANGE frame -- and the default frame, which is
            // RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW -- takes the peers of the current row with it: `SUM(v) OVER (ORDER BY g)` is the same for the rows of one g.
            std::vector<std::size_t> peer_first(total), peer_last(total);
            for (std::size_t i = 0; i < total;) {
                std::size_t j = i;
                if (has_order) {
                    while (j + 1 < total && win_order_eq(rows[sorted[i]], rows[sorted[j + 1]], wf->order_by)) j++;
                } else {
                    j = total - 1;
                }
                for (std::size_t k = i; k <= j; k++) { peer_first[k] = i; peer_last[k] = j; }
                i = j + 1;
            }
            // RANGE with an offset: the ORDER BY values of the partition on a number line (see RangeAxis)
            const bool range_offsets = wf->frame && wf->frame->unit == FrameUnit::Range && (is_offset_bound(wf->frame->start) || is_offset_bound(wf->frame->end));
            RangeAxis axis;
            if (range_offsets) {
                const char* const needs_one = "Window '<unnamed window>' with RANGE N PRECEDING/FOLLOWING frame requires exactly one ORDER BY expression, of numeric or temporal type";
                if (wf->order_by.size() != 1) throw StatementError(needs_one);
                const std::string* start_unit = bound_unit(wf->frame->start);
                const std::string* end_unit = bound_unit(wf->frame->end);
                const bool interval = (start_unit && !start_unit->empty()) || (end_unit && !end_unit->empty());
                const bool plain_number = (start_unit && start_unit->empty()) || (end_unit && end_unit->empty());
                const double sign = wf->order_by[0].ascending ? 1.0 : -1.0;
                for (std::size_t i = 0; i < total; i++) {
                    const std::string* v = get_col(rows[sorted[i]], wf->order_by[0].column);
                    if (!v || *v == EXECUTOR_NULL_VALUE) continue;
                    std::optional<sqlval::DateTime> when;
                    if (!axis.any) {
                        axis.any = true;
                        axis.first = i;
                        when = parse_number(*v) ? std::nullopt : sqlval::parse_datetime(*v);
                        axis.temporal = when && when->has_date;
                        if (!axis.temporal && !parse_number(*v)) throw StatementError(needs_one);
                        if (axis.temporal && plain_number) throw StatementError("Window '<unnamed window>' with RANGE frame has ORDER BY expression of datetime type. Only INTERVAL bound value allowed.");
                        if (!axis.temporal && interval) throw StatementError("Window '<unnamed window>' with RANGE frame has ORDER BY expression of numeric type, INTERVAL bound value not allowed.");
                    }
                    axis.last = i;
                    long double position;
                    if (axis.temporal) {
                        if (!when) when = sqlval::parse_datetime(*v);
                        position = when ? temporal_micro(*when) : 0.0L;
                    } else {
                        position = static_cast<long double>(parse_number(*v).value_or(0.0));
                    }
                    axis.position.push_back(position * static_cast<long double>(sign));
                    axis.text.push_back(*v);
                }
            }
            // where the offset of `b` puts the row `pos` (a row with a value), as a position on the number line of the axis
            auto offset_target = [&](const FrameBound& b, std::size_t pos) -> long double {
                const bool before = std::holds_alternative<FrameBound::Preceding>(b.data);
                const double amount = before ? std::get<FrameBound::Preceding>(b.data).amount : std::get<FrameBound::Following>(b.data).amount;
                const std::string& unit = *bound_unit(b);
                const long double sign = wf->order_by[0].ascending ? 1.0L : -1.0L;
                const long double here = axis.position[pos - axis.first]; // (already times the sign)
                const long double step = before ? -1.0L : 1.0L;           // along the order of the rows
                if (!axis.temporal) return here + step * static_cast<long double>(amount);
                if (auto micro = unit_micro(unit)) return here + step * static_cast<long double>(amount) * *micro;
                // MONTH, QUARTER, YEAR: on the calendar, in the direction of the values (a DESC order runs them backwards)
                const long long months = static_cast<long long>(std::llround(amount)) * (unit == "YEAR" ? 12 : unit == "QUARTER" ? 3 : 1);
                auto when = sqlval::parse_datetime(axis.text[pos - axis.first]);
                if (!when) return here;
                return temporal_micro(add_months(*when, static_cast<long long>(step * sign) * months)) * sign;
            };
            auto frame_of = [&](std::size_t pos) {
                if (range_offsets) {
                    const bool has_value = axis.any && pos >= axis.first && pos <= axis.last;
                    auto index_of = [&](const FrameBound& b, bool is_start) -> long long {
                        if (std::holds_alternative<FrameBound::UnboundedPreceding>(b.data)) return 0;
                        if (std::holds_alternative<FrameBound::UnboundedFollowing>(b.data)) return static_cast<long long>(total) - 1;
                        if (std::holds_alternative<FrameBound::CurrentRow>(b.data) || !has_value) return static_cast<long long>(is_start ? peer_first[pos] : peer_last[pos]);
                        const long double target = offset_target(b, pos);
                        if (is_start) { // the first row whose value reaches the target
                            const auto at = std::lower_bound(axis.position.begin(), axis.position.end(), target);
                            return static_cast<long long>(axis.first) + (at - axis.position.begin());
                        }
                        const auto at = std::upper_bound(axis.position.begin(), axis.position.end(), target); // the last row whose value does not pass it
                        return static_cast<long long>(axis.first) + (at - axis.position.begin()) - 1;
                    };
                    const long long start = index_of(wf->frame->start, true), end = index_of(wf->frame->end, false);
                    if (start > end || start >= static_cast<long long>(total) || end < 0) return std::make_pair(std::size_t{1}, std::size_t{0});
                    return std::make_pair(static_cast<std::size_t>(std::max<long long>(start, 0)), static_cast<std::size_t>(end));
                }
                auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
                if (!wf->frame) {
                    if (has_order) end = peer_last[pos];
                } else if (wf->frame->unit == FrameUnit::Range) {
                    if (std::holds_alternative<FrameBound::CurrentRow>(wf->frame->start.data)) start = peer_first[pos];
                    if (std::holds_alternative<FrameBound::CurrentRow>(wf->frame->end.data)) end = peer_last[pos];
                }
                return std::make_pair(start, end);
            };

            switch (wf->func) {
                case WindowFunc::RowNumber:
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) values[sorted[pos]] = std::to_string(pos + 1);
                    break;
                case WindowFunc::Rank: {
                    std::size_t rank = 1, i = 0;
                    while (i < sorted.size()) {
                        std::size_t j = i;
                        while (j + 1 < sorted.size() && win_order_eq(rows[sorted[i]], rows[sorted[j + 1]], wf->order_by)) j++;
                        for (std::size_t k = i; k <= j; k++) values[sorted[k]] = std::to_string(rank);
                        rank += j - i + 1;
                        i = j + 1;
                    }
                    break;
                }
                case WindowFunc::DenseRank: {
                    std::size_t rank = 1, i = 0;
                    while (i < sorted.size()) {
                        std::size_t j = i;
                        while (j + 1 < sorted.size() && win_order_eq(rows[sorted[i]], rows[sorted[j + 1]], wf->order_by)) j++;
                        for (std::size_t k = i; k <= j; k++) values[sorted[k]] = std::to_string(rank);
                        rank++;
                        i = j + 1;
                    }
                    break;
                }
                case WindowFunc::Lag: {
                    std::string col_name = wf->col.value_or("");
                    std::size_t off = static_cast<std::size_t>(std::llabs(static_cast<long long>(wf->offset)));
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        if (pos >= off) {
                            const std::string* v = get_col(rows[sorted[pos - off]], col_name);
                            values[sorted[pos]] = v ? *v : "NULL";
                        } else {
                            values[sorted[pos]] = wf->default_value.value_or("NULL");
                        }
                    }
                    break;
                }
                case WindowFunc::Lead: {
                    std::string col_name = wf->col.value_or("");
                    std::size_t off = static_cast<std::size_t>(std::llabs(static_cast<long long>(wf->offset)));
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        if (pos + off < sorted.size()) {
                            const std::string* v = get_col(rows[sorted[pos + off]], col_name);
                            values[sorted[pos]] = v ? *v : "NULL";
                        } else {
                            values[sorted[pos]] = wf->default_value.value_or("NULL");
                        }
                    }
                    break;
                }
                case WindowFunc::FirstValue: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        if (start > end) { values[sorted[pos]] = "NULL"; continue; } // (a frame with no row)
                        const std::string* v = get_col(rows[sorted[start]], col_name);
                        values[sorted[pos]] = v ? *v : "NULL";
                    }
                    break;
                }
                case WindowFunc::LastValue: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        if (start > end) { values[sorted[pos]] = "NULL"; continue; }
                        const std::string* v = get_col(rows[sorted[end]], col_name);
                        values[sorted[pos]] = v ? *v : "NULL";
                    }
                    break;
                }
                case WindowFunc::NthValue: {
                    std::string col_name = wf->col.value_or("");
                    std::size_t nth = static_cast<std::size_t>(std::max<long long>(wf->offset, 1)) - 1;
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        std::string val = "NULL";
                        if (start + nth <= end && start + nth < sorted.size()) {
                            const std::string* v = get_col(rows[sorted[start + nth]], col_name);
                            if (v) val = *v;
                        }
                        values[sorted[pos]] = val;
                    }
                    break;
                }
                case WindowFunc::Sum: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        std::vector<const std::string*> frame_values; // the values in the frame that are not NULL
                        for (std::size_t i = start; i <= end; i++) {
                            const std::string* v = get_col(rows[sorted[i]], col_name);
                            if (v && *v != EXECUTOR_NULL_VALUE) frame_values.push_back(v);
                        }
                        values[sorted[pos]] = sum_of_texts(frame_values, false, wf->col_approx).value_or(EXECUTOR_NULL_VALUE);
                    }
                    break;
                }
                case WindowFunc::Avg: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        std::vector<const std::string*> frame_values; // the values in the frame that are not NULL
                        for (std::size_t i = start; i <= end; i++) {
                            const std::string* v = get_col(rows[sorted[i]], col_name);
                            if (v && *v != EXECUTOR_NULL_VALUE) frame_values.push_back(v);
                        }
                        values[sorted[pos]] = average_of_texts(frame_values, wf->col_approx).value_or(EXECUTOR_NULL_VALUE);
                    }
                    break;
                }
                case WindowFunc::Count: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        std::size_t count;
                        if (col_name == "*") {
                            count = end - start + 1; // (a frame with no row is (1, 0): the count wraps round to 0)
                        } else {
                            count = 0;
                            for (std::size_t i = start; i <= end; i++) {
                                const std::string* v = get_col(rows[sorted[i]], col_name);
                                if (v && *v != "NULL") count++;
                            }
                        }
                        values[sorted[pos]] = std::to_string(count);
                    }
                    break;
                }
                case WindowFunc::Min: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        std::optional<std::string> best;
                        for (std::size_t i = start; i <= end; i++) {
                            const std::string* v = get_col(rows[sorted[i]], col_name);
                            if (!v || *v == "NULL") continue;
                            if (!best || cmp_key(*v, *best, wf->col_class) < 0) best = *v;
                        }
                        values[sorted[pos]] = best.value_or("NULL");
                    }
                    break;
                }
                case WindowFunc::Max: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_of(pos);
                        std::optional<std::string> best;
                        for (std::size_t i = start; i <= end; i++) {
                            const std::string* v = get_col(rows[sorted[i]], col_name);
                            if (!v || *v == "NULL") continue;
                            if (!best || cmp_key(*v, *best, wf->col_class) > 0) best = *v;
                        }
                        values[sorted[pos]] = best.value_or("NULL");
                    }
                    break;
                }
                case WindowFunc::Ntile: {
                    std::size_t n_buckets = static_cast<std::size_t>(std::max<long long>(wf->offset, 1));
                    if (total > 0) {
                        std::size_t n_eff = std::min(n_buckets, total);
                        for (std::size_t pos = 0; pos < sorted.size(); pos++) values[sorted[pos]] = std::to_string(pos * n_eff / total + 1);
                    }
                    break;
                }
                case WindowFunc::PercentRank: {
                    if (total <= 1) {
                        for (auto idx : sorted) values[idx] = "0";
                    } else {
                        std::vector<std::size_t> ranks(total, 1);
                        std::size_t i = 0;
                        while (i < sorted.size()) {
                            std::size_t rank = i + 1;
                            std::size_t j = i;
                            while (j + 1 < sorted.size() && win_order_eq(rows[sorted[i]], rows[sorted[j + 1]], wf->order_by)) j++;
                            for (std::size_t k = i; k <= j; k++) ranks[k] = rank;
                            i = j + 1;
                        }
                        for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                            double pr = static_cast<double>(ranks[pos] - 1) / static_cast<double>(total - 1);
                            values[sorted[pos]] = fmt4(pr);
                        }
                    }
                    break;
                }
                case WindowFunc::CumeDist: {
                    if (total > 0) {
                        std::size_t i = 0;
                        while (i < sorted.size()) {
                            std::size_t j = i;
                            while (j + 1 < sorted.size() && win_order_eq(rows[sorted[i]], rows[sorted[j + 1]], wf->order_by)) j++;
                            double cd = static_cast<double>(j + 1) / static_cast<double>(total);
                            for (std::size_t k = i; k <= j; k++) values[sorted[k]] = fmt4(cd);
                            i = j + 1;
                        }
                    }
                    break;
                }
            }
        }

        for (std::size_t i = 0; i < rows.size(); i++) rows[i][label] = values[i];
    }
    return rows;
}

} // namespace engine
