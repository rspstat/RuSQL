// Faithful port of the set-operation statements from rusql-core/src/engine/executor.rs
// (Phase 8c): exec_union, exec_intersect, exec_except, and their shared
// apply_set_postprocess/format_set_result helpers.

#include "engine/executor/executor.hpp"
#include "engine/parallel_util.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <unordered_map>

namespace engine {

namespace {

std::optional<double> parse_f64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    double val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return val;
}

int cmp_key(const std::string& a, const std::string& b) {
    auto pa = parse_f64(a), pb = parse_f64(b);
    if (pa && pb) {
        if (*pa < *pb) return -1;
        if (*pa > *pb) return 1;
        return 0;
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

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

std::vector<std::string> row_key(const Row& row, const std::vector<std::string>& cols) {
    std::vector<std::string> key;
    key.reserve(cols.size());
    for (auto& c : cols) {
        auto it = row.find(c);
        key.push_back(it != row.end() ? it->second : std::string());
    }
    return key;
}

} // namespace

void Executor::apply_set_postprocess(std::vector<Row>& result, const std::vector<std::string>& cols, const std::vector<OrderBy>& order_by,
                                      std::optional<std::size_t> limit, std::optional<std::size_t> offset) {
    (void)cols;
    if (!order_by.empty()) {
        auto less = [&](const Row& a, const Row& b) { return row_order_less(a, b, order_by); };
        if (parallel_enabled() && result.size() >= parallel_min_rows()) {
            parallel_sort(result, less); // unstable, matches Rust's par_sort_unstable_by
        } else {
            std::stable_sort(result.begin(), result.end(), less);
        }
    }
    if (offset) {
        std::size_t skip = std::min(*offset, result.size());
        result.erase(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(skip));
    }
    if (limit && result.size() > *limit) result.resize(*limit);
}

std::string Executor::format_set_result(const std::vector<std::string>& cols, const std::vector<Row>& result) {
    if (result.empty()) return "0 rows returned.";

    // Escape headers/cells up front (widths below are computed on the escaped form,
    // matching the visual padding actually written) -- see Executor::escape_cell.
    std::vector<std::string> headers(cols.size());
    for (std::size_t i = 0; i < cols.size(); i++) headers[i] = escape_cell(cols[i]);

    std::vector<std::size_t> col_widths(cols.size());
    for (std::size_t i = 0; i < cols.size(); i++) {
        std::size_t max_val = 0;
        for (auto& row : result) {
            auto it = row.find(cols[i]);
            if (it != row.end()) max_val = std::max(max_val, escape_cell(it->second).size());
        }
        col_widths[i] = std::max(headers[i].size(), max_val);
    }

    std::string sep = "+";
    for (auto w : col_widths) sep += std::string(w + 2, '-') + "+";

    std::string out = sep + "\n|";
    for (std::size_t i = 0; i < cols.size(); i++) out += " " + headers[i] + std::string(col_widths[i] - headers[i].size(), ' ') + " |";
    out += "\n" + sep + "\n";
    for (auto& row : result) {
        out += "|";
        for (std::size_t i = 0; i < cols.size(); i++) {
            auto it = row.find(cols[i]);
            std::string v = it != row.end() ? (it->second == EXECUTOR_NULL_VALUE ? "NULL" : escape_cell(it->second)) : std::string();
            out += " " + v + std::string(col_widths[i] - v.size(), ' ') + " |";
        }
        out += "\n";
    }
    out += sep;
    out += "\n" + std::to_string(result.size()) + " row(s) returned.";
    return out;
}

StringResult Executor::exec_union(SharedDatabase& s, Statement left, Statement right, bool all, std::vector<OrderBy> order_by,
                                   std::optional<std::size_t> limit, std::optional<std::size_t> offset) {
    auto left_out = execute_with_s(s, std::move(left));
    if (left_out.is_err()) return left_out;
    auto right_out = execute_with_s(s, std::move(right));
    if (right_out.is_err()) return right_out;

    auto [left_cols, left_rows] = parse_table_output(left_out.value());
    auto [right_cols, right_rows] = parse_table_output(right_out.value());

    if (left_cols.empty() && right_cols.empty()) return StringResult::Ok("0 rows returned.");

    std::vector<Row> remapped_right;
    remapped_right.reserve(right_rows.size());
    for (auto& rr : right_rows) {
        Row new_row;
        for (std::size_t i = 0; i < left_cols.size(); i++) {
            std::string val;
            if (i < right_cols.size()) {
                auto it = rr.find(right_cols[i]);
                if (it != rr.end()) val = it->second;
            }
            new_row[left_cols[i]] = val;
        }
        new_row["_xmin"] = "1";
        new_row["_xmax"] = "0";
        remapped_right.push_back(std::move(new_row));
    }
    left_rows.insert(left_rows.end(), remapped_right.begin(), remapped_right.end());
    std::vector<Row> result = std::move(left_rows);

    if (!all) {
        std::vector<std::vector<std::string>> seen;
        std::vector<Row> filtered;
        for (auto& row : result) {
            auto key = row_key(row, left_cols);
            if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
                seen.push_back(key);
                filtered.push_back(std::move(row));
            }
        }
        result = std::move(filtered);
    }

    if (!order_by.empty()) {
        auto less = [&](const Row& a, const Row& b) { return row_order_less(a, b, order_by); };
        if (parallel_enabled() && result.size() >= parallel_min_rows()) {
            parallel_sort(result, less); // unstable, matches Rust's par_sort_unstable_by
        } else {
            std::stable_sort(result.begin(), result.end(), less);
        }
    }
    if (offset) {
        std::size_t skip = std::min(*offset, result.size());
        result.erase(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(skip));
    }
    if (limit && result.size() > *limit) result.resize(*limit);

    if (result.empty()) return StringResult::Ok("0 rows returned.");

    const std::vector<std::string>& cols = left_cols.empty() ? right_cols : left_cols;
    return StringResult::Ok(format_set_result(cols, result));
}

StringResult Executor::exec_intersect(SharedDatabase& s, Statement left, Statement right, bool all, std::vector<OrderBy> order_by,
                                       std::optional<std::size_t> limit, std::optional<std::size_t> offset) {
    auto left_out = execute_with_s(s, std::move(left));
    if (left_out.is_err()) return left_out;
    auto right_out = execute_with_s(s, std::move(right));
    if (right_out.is_err()) return right_out;

    auto [left_cols, left_rows] = parse_table_output(left_out.value());
    auto [right_cols, right_rows] = parse_table_output(right_out.value());
    const std::vector<std::string> cols = left_cols.empty() ? right_cols : left_cols;

    std::vector<std::vector<std::string>> right_keys;
    right_keys.reserve(right_rows.size());
    for (auto& rr : right_rows) {
        std::vector<std::string> key;
        for (std::size_t i = 0; i < cols.size(); i++) {
            std::string val;
            if (i < right_cols.size()) {
                auto it = rr.find(right_cols[i]);
                if (it != rr.end()) val = it->second;
            } else {
                auto it = rr.find(cols[i]);
                if (it != rr.end()) val = it->second;
            }
            key.push_back(std::move(val));
        }
        right_keys.push_back(std::move(key));
    }

    std::vector<Row> result;
    std::vector<std::size_t> matched_right;
    for (auto& row : left_rows) {
        auto key = row_key(row, cols);
        if (all) {
            std::optional<std::size_t> pos;
            for (std::size_t i = 0; i < right_keys.size(); i++) {
                if (std::find(matched_right.begin(), matched_right.end(), i) == matched_right.end() && right_keys[i] == key) {
                    pos = i;
                    break;
                }
            }
            if (pos) {
                matched_right.push_back(*pos);
                result.push_back(row);
            }
        } else {
            bool in_right = std::find(right_keys.begin(), right_keys.end(), key) != right_keys.end();
            bool already_in_result = std::any_of(result.begin(), result.end(), [&](const Row& r) { return row_key(r, cols) == key; });
            if (in_right && !already_in_result) result.push_back(row);
        }
    }
    apply_set_postprocess(result, cols, order_by, limit, offset);
    return StringResult::Ok(format_set_result(cols, result));
}

StringResult Executor::exec_except(SharedDatabase& s, Statement left, Statement right, bool all, std::vector<OrderBy> order_by,
                                    std::optional<std::size_t> limit, std::optional<std::size_t> offset) {
    auto left_out = execute_with_s(s, std::move(left));
    if (left_out.is_err()) return left_out;
    auto right_out = execute_with_s(s, std::move(right));
    if (right_out.is_err()) return right_out;

    auto [left_cols, left_rows] = parse_table_output(left_out.value());
    auto [right_cols, right_rows] = parse_table_output(right_out.value());
    const std::vector<std::string> cols = left_cols.empty() ? right_cols : left_cols;

    std::vector<std::vector<std::string>> right_keys;
    right_keys.reserve(right_rows.size());
    for (auto& rr : right_rows) {
        std::vector<std::string> key;
        for (std::size_t i = 0; i < cols.size(); i++) {
            std::string val;
            if (i < right_cols.size()) {
                auto it = rr.find(right_cols[i]);
                if (it != rr.end()) val = it->second;
            } else {
                auto it = rr.find(cols[i]);
                if (it != rr.end()) val = it->second;
            }
            key.push_back(std::move(val));
        }
        right_keys.push_back(std::move(key));
    }

    std::map<std::vector<std::string>, std::size_t> right_counts;
    for (auto& k : right_keys) right_counts[k]++;

    std::vector<Row> result;
    for (auto& row : left_rows) {
        auto key = row_key(row, cols);
        if (all) {
            auto it = right_counts.find(key);
            if (it != right_counts.end() && it->second > 0) {
                it->second--;
            } else {
                result.push_back(row);
            }
        } else {
            bool in_right = std::find(right_keys.begin(), right_keys.end(), key) != right_keys.end();
            bool already_in_result = std::any_of(result.begin(), result.end(), [&](const Row& r) { return row_key(r, cols) == key; });
            if (!in_right && !already_in_result) result.push_back(row);
        }
    }
    apply_set_postprocess(result, cols, order_by, limit, offset);
    return StringResult::Ok(format_set_result(cols, result));
}

} // namespace engine
