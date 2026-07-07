// Faithful port of EXPLAIN / EXPLAIN ANALYZE from rusql-core/src/engine/executor.rs
// (Phase 8f): exec_explain, exec_explain_analyze.

#include "engine/executor/executor.hpp"

#include <chrono>
#include <sstream>

#include "engine/planner.hpp"

namespace engine {

namespace {

// Approximates Rust's `{:?}` Debug format for a non-SELECT Statement in the "not a
// SELECT" EXPLAIN error message: extracts just the variant tag name (e.g. "Insert")
// via the existing externally-tagged JSON encoding, rather than replicating Rust's
// full recursive field-by-field Debug output for all ~80 Statement variants — this
// is purely a diagnostic message, not compared against structurally.
std::string statement_tag(const Statement& stmt) {
    nlohmann::json j = stmt;
    if (j.is_string()) return j.get<std::string>();
    if (j.is_object() && !j.empty()) return j.begin().key();
    return "?";
}

// Matches Rust's str::lines(): empty input yields zero lines, and a trailing '\n'
// ends the last line rather than introducing a spurious trailing empty one.
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    std::size_t start = 0;
    while (start < s.size()) {
        auto nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string pad_center(const std::string& v, std::size_t width) {
    if (v.size() >= width) return v;
    std::size_t total = width - v.size();
    std::size_t left = total / 2;
    std::size_t right = total - left;
    return std::string(left, ' ') + v + std::string(right, ' ');
}

std::string pad_right(const std::string& v, std::size_t width) {
    if (v.size() >= width) return v;
    return v + std::string(width - v.size(), ' ');
}

} // namespace

StringResult Executor::exec_explain(const SharedDatabase& s, Statement stmt) const {
    auto* sel = std::get_if<Statement::Select>(&stmt.data);
    if (!sel) return StringResult::Ok("EXPLAIN: " + statement_tag(stmt) + " → not a SELECT");
    if (sel->subquery) return StringResult::Ok("EXPLAIN: Subquery-based SELECT → SUBQUERY SCAN");

    Planner planner(s.tables, s.indexes, s.index_meta, s.composite_indexes, s.hash_indexes, s.hash_index_meta, s.catalog, s.table_stats);
    SelectPlan plan = planner.plan_covering(sel->table, sel->condition, sel->joins, sel->columns);
    return StringResult::Ok(planner.explain(plan));
}

StringResult Executor::exec_explain_analyze(SharedDatabase& s, Statement stmt) {
    constexpr const char* SEP = "+--------------------------------------------------------------------------+";
    constexpr std::size_t W = 72;
    std::string blank = "| " + pad_right("", W) + " |";
    auto fmt_stat = [](const std::string& text) { return "| " + pad_right(text, W) + " |\n"; };
    std::string hdr = "|" + pad_center("QUERY PLAN (ANALYZE)", 74) + "|";

    std::vector<std::string> plan_lines;
    std::size_t est_rows = 0;
    if (auto* sel = std::get_if<Statement::Select>(&stmt.data)) {
        if (sel->subquery) {
            plan_lines.push_back(fmt_stat("Access: SUBQUERY SCAN"));
        } else {
            Planner planner(s.tables, s.indexes, s.index_meta, s.composite_indexes, s.hash_indexes, s.hash_index_meta, s.catalog, s.table_stats);
            SelectPlan plan = planner.plan_covering(sel->table, sel->condition, sel->joins, sel->columns);
            est_rows = plan.base.est_rows;
            auto all_lines = split_lines(planner.explain(plan));
            for (std::size_t i = 3; i < all_lines.size(); i++) {
                if (!all_lines[i].empty() && all_lines[i].front() == '+') continue;
                plan_lines.push_back(all_lines[i]);
            }
        }
    }

    auto start = std::chrono::steady_clock::now();
    auto result = execute_with_s(s, std::move(stmt));
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (result.is_err()) return result;
    std::string result_str = result.value();

    std::optional<std::size_t> actual_rows_opt;
    for (auto& line : split_lines(result_str)) {
        if (line.find("row(s) returned") != std::string::npos) {
            std::size_t start_ws = line.find_first_not_of(" \t");
            if (start_ws != std::string::npos) {
                std::size_t end_ws = line.find_first_of(" \t", start_ws);
                std::string first_tok = line.substr(start_ws, end_ws == std::string::npos ? std::string::npos : end_ws - start_ws);
                try {
                    std::size_t pos;
                    std::size_t n = static_cast<std::size_t>(std::stoull(first_tok, &pos));
                    if (pos == first_tok.size()) actual_rows_opt = n;
                } catch (...) {
                }
            }
            break;
        }
    }
    std::size_t actual_rows;
    if (actual_rows_opt) {
        actual_rows = *actual_rows_opt;
    } else {
        std::size_t count = 0;
        for (auto& line : split_lines(result_str)) {
            if (!line.empty() && line.front() == '|' && line.find("---") == std::string::npos) {
                std::size_t s0 = line.find_first_not_of(" \t");
                std::size_t e0 = line.find_last_not_of(" \t");
                std::string trimmed = s0 == std::string::npos ? "" : line.substr(s0, e0 - s0 + 1);
                if (trimmed != "|") count++;
            }
        }
        actual_rows = count > 0 ? count - 1 : 0;
    }

    double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    std::string time_str;
    {
        char buf[64];
        if (elapsed_ms < 1000.0) {
            std::snprintf(buf, sizeof(buf), "%.3f ms", elapsed_ms);
        } else {
            std::snprintf(buf, sizeof(buf), "%.3f sec", elapsed_ms / 1000.0);
        }
        time_str = buf;
    }

    std::string accuracy_str;
    if (est_rows == 0) {
        accuracy_str = "N/A";
    } else {
        double ratio =
            static_cast<double>(std::min(est_rows, actual_rows)) / static_cast<double>(std::max({est_rows, actual_rows, std::size_t(1)})) * 100.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f%%", ratio);
        accuracy_str = buf;
    }

    std::string rows_stat = "Estimated rows : " + pad_right(std::to_string(est_rows), 8) + " Actual rows : " +
                             pad_right(std::to_string(actual_rows), 8) + " Accuracy : " + accuracy_str;
    std::string time_stat = "Execution time : " + time_str;

    std::string out;
    out += std::string(SEP) + "\n";
    out += hdr + "\n";
    out += std::string(SEP) + "\n";
    for (auto& line : plan_lines) out += line + "\n";
    out += blank + "\n";
    out += fmt_stat(rows_stat);
    out += fmt_stat(time_stat);
    out += SEP;
    return StringResult::Ok(out);
}

} // namespace engine
