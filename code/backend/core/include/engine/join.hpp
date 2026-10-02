#pragma once

// Faithful port of rusql-core/src/engine/join.rs — join execution algorithms.
//
// hash_join's Inner/Left probe phase is parallelized over the global ThreadPool
// (join.cpp's probe_parallel helper), matching the Rust original's unconditional
// `left.par_iter().flat_map(...)` — this parallelization is NOT gated by
// parallel_enabled()/parallel_min_rows(), matching Rust exactly.

#include <functional>
#include <optional>
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

// INNER or LEFT JOIN whose ON condition contains an equality `left_key = right_key` that every matching pair must satisfy,
// without the nested loop: the right rows are hashed by `right_key`, every left row only meets the right rows whose key
// equals its own `left_key` (numeric values compare as numbers, NULL matches nothing), and each such pair still has to
// pass `on_match` -- the whole ON condition -- so the answer is exactly the nested loop's: same rows, same order (left
// order, right order within a left row), and with `left_outer` unmatched left rows padded with NULLs. Candidate pairs are
// only ever skipped, never added. nullopt = a key could not be read from some row; the caller falls back to the loop.
std::optional<std::vector<Row>> hashed_join_verified(const std::vector<Row>& left, const std::vector<Row>& right, const std::string& table,
                                                      bool left_outer, const std::function<const std::string*(const Row&)>& left_key,
                                                      const std::function<const std::string*(const Row&)>& right_key,
                                                      const std::vector<std::string>& right_schema_cols,
                                                      const std::function<bool(const Row&)>& on_match);

std::vector<Row> nested_loop_join(const std::vector<Row>& left, const std::vector<Row>& right, JoinType join_type,
                                   const std::string& table, const std::vector<std::string>& using_cols,
                                   const std::vector<std::string>& right_schema_cols,
                                   const std::function<bool(const Row&)>& on_match);

} // namespace engine
