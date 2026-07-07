#pragma once

// Faithful port of rusql-core/src/engine/join.rs — join execution algorithms.
//
// hash_join's Inner/Left probe phase is parallelized over the global ThreadPool
// (join.cpp's probe_parallel helper), matching the Rust original's unconditional
// `left.par_iter().flat_map(...)` — this parallelization is NOT gated by
// parallel_enabled()/parallel_min_rows(), matching Rust exactly.

#include <functional>
#include <string>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/row.hpp"

namespace engine {

constexpr const char* JOIN_NULL_VALUE = "NULL";

void merge_right(Row& merged, const Row& right, const std::string& table);
void null_right(Row& merged, const std::vector<std::string>& cols, const std::string& table);

std::vector<Row> sort_merge_join(const std::vector<Row>& left, const std::vector<Row>& right, JoinType join_type,
                                  const std::string& table, const std::string& probe_col, const std::string& build_col,
                                  const std::vector<std::string>& right_schema_cols);

std::vector<Row> hash_join(const std::vector<Row>& left, const std::vector<Row>& right, JoinType join_type,
                            const std::string& table, const std::string& probe_col, const std::string& build_col,
                            const std::vector<std::string>& right_schema_cols);

std::vector<Row> nested_loop_join(const std::vector<Row>& left, const std::vector<Row>& right, JoinType join_type,
                                   const std::string& table, const std::vector<std::string>& using_cols,
                                   const std::vector<std::string>& right_schema_cols,
                                   const std::function<bool(const Row&)>& on_match);

} // namespace engine
