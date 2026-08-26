#pragma once

// Faithful port of ColumnStats/TableStats from rusql-core/src/engine/executor.rs
// (defined here rather than in a not-yet-ported executor module, since the
// cost-based planner needs them; the full ANALYZE TABLE collection logic that
// populates these is ported alongside the executor in Phase 8).

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct ColumnStats {
    std::size_t distinct_count = 0;
    std::size_t null_count = 0;
    std::optional<std::string> min_val;
    std::optional<std::string> max_val;
    /// Equi-depth histogram: NUM_HISTOGRAM_BUCKETS upper-bound values (sorted).
    std::vector<std::string> histogram;
    /// Most-common values (PostgreSQL-style MCV): up to NUM_MCV_ENTRIES (value, exact
    /// observed row count) pairs, sorted by count descending. Non-MCV values fall back
    /// to the histogram/NDV path in the planner.
    std::vector<std::pair<std::string, std::size_t>> mcv;
};

constexpr std::size_t NUM_HISTOGRAM_BUCKETS = 10;
constexpr std::size_t NUM_MCV_ENTRIES = 10;

struct TableStats {
    std::size_t total_rows = 0;
    std::unordered_map<std::string, ColumnStats> columns;
};

} // namespace engine
