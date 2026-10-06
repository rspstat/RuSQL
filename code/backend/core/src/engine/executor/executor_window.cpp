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

namespace engine {

namespace {
std::optional<double> parse_f64(const std::string& s) { return parse_number(s); }

int cmp_key(const std::string& a, const std::string& b, ValueClass cls) {
    const bool a_null = a == "NULL", b_null = b == "NULL"; // NULL sorts before every value
    if (a_null || b_null) return a_null == b_null ? 0 : (a_null ? -1 : 1);
    return compare_classed(cls, cls, a, b);
}

std::string fmt4(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}
} // namespace

std::pair<std::size_t, std::size_t> Executor::frame_bounds(std::size_t pos, std::size_t len, const std::optional<WindowFrame>& frame,
                                                             bool has_order) {
    if (frame) {
        auto resolve = [&](const FrameBound& b) -> std::size_t {
            if (std::holds_alternative<FrameBound::UnboundedPreceding>(b.data)) return 0;
            if (auto* p = std::get_if<FrameBound::Preceding>(&b.data)) return pos >= p->n ? pos - p->n : 0;
            if (std::holds_alternative<FrameBound::CurrentRow>(b.data)) return pos;
            if (auto* f = std::get_if<FrameBound::Following>(&b.data)) return std::min(pos + f->n, len == 0 ? 0 : len - 1);
            return len == 0 ? 0 : len - 1; // UnboundedFollowing
        };
        return {resolve(frame->start), resolve(frame->end)};
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
        return as == bs;
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
                pk.push_back(v ? *v : std::string());
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
                            values[sorted[pos]] = "NULL";
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
                            values[sorted[pos]] = "NULL";
                        }
                    }
                    break;
                }
                case WindowFunc::FirstValue: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
                        (void)end;
                        const std::string* v = get_col(rows[sorted[start]], col_name);
                        values[sorted[pos]] = v ? *v : "NULL";
                    }
                    break;
                }
                case WindowFunc::LastValue: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
                        (void)start;
                        const std::string* v = get_col(rows[sorted[end]], col_name);
                        values[sorted[pos]] = v ? *v : "NULL";
                    }
                    break;
                }
                case WindowFunc::NthValue: {
                    std::string col_name = wf->col.value_or("");
                    std::size_t nth = static_cast<std::size_t>(std::max<long long>(wf->offset, 1)) - 1;
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
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
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
                        std::vector<const std::string*> frame_values; // the values in the frame that are not NULL
                        for (std::size_t i = start; i <= end; i++) {
                            const std::string* v = get_col(rows[sorted[i]], col_name);
                            if (v && *v != EXECUTOR_NULL_VALUE) frame_values.push_back(v);
                        }
                        values[sorted[pos]] = sum_of_texts(frame_values, false).value_or(EXECUTOR_NULL_VALUE);
                    }
                    break;
                }
                case WindowFunc::Avg: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
                        std::vector<const std::string*> frame_values; // the values in the frame that are not NULL
                        for (std::size_t i = start; i <= end; i++) {
                            const std::string* v = get_col(rows[sorted[i]], col_name);
                            if (v && *v != EXECUTOR_NULL_VALUE) frame_values.push_back(v);
                        }
                        values[sorted[pos]] = average_of_texts(frame_values).value_or(EXECUTOR_NULL_VALUE);
                    }
                    break;
                }
                case WindowFunc::Count: {
                    std::string col_name = wf->col.value_or("");
                    for (std::size_t pos = 0; pos < sorted.size(); pos++) {
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
                        std::size_t count;
                        if (col_name == "*") {
                            count = end - start + 1;
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
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
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
                        auto [start, end] = frame_bounds(pos, total, wf->frame, has_order);
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
                        for (auto idx : sorted) values[idx] = "0.0000";
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
