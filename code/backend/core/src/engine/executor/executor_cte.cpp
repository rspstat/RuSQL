// Faithful port of exec_with (WITH ... [RECURSIVE] common table expressions) from
// rusql-core/src/engine/executor.rs (Phase 8c): each CTE is materialized as a
// temporary in-memory table (recursive CTEs over a UNION iterate base case + recursive
// case until no new rows appear, capped at 1000 iterations), the main query runs
// against those virtual tables, and they're torn down afterward.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <unordered_set>

namespace engine {

StringResult Executor::exec_with(SharedDatabase& s, std::vector<std::pair<std::string, std::unique_ptr<Statement>>> ctes, Statement query,
                                  bool recursive) {
    std::vector<std::string> cte_names;

    for (auto& [name, body] : ctes) {
        if (s.tables.count(name) || s.views.count(name)) {
            for (auto& n : cte_names) {
                s.tables.erase(n);
                s.indexes.erase(n);
                s.buffer_pool.invalidate(n);
                s.catalog.drop_table(n);
            }
            return StringResult::Err("CTE name '" + name + "' conflicts with an existing table or view");
        }

        std::vector<std::string> col_names;
        std::vector<Row> rows;

        auto* union_stmt = std::get_if<Statement::Union>(&body->data);
        if (recursive && union_stmt) {
            Statement left = std::move(*union_stmt->left);
            Statement right = std::move(*union_stmt->right);

            auto base_out = execute_with_s(s, std::move(left));
            if (base_out.is_err()) {
                for (auto& n : cte_names) {
                    s.tables.erase(n);
                    s.indexes.erase(n);
                    s.buffer_pool.invalidate(n);
                    s.catalog.drop_table(n);
                }
                return base_out;
            }
            auto [cols, accumulated] = parse_table_output(base_out.value());

            std::vector<ColumnDef> schema_cols;
            for (auto& c : cols) {
                ColumnDef cd;
                cd.name = c;
                cd.data_type = DataType(DataType::Text{});
                schema_cols.push_back(cd);
            }
            s.catalog.create_table(name, schema_cols);
            s.tables[name] = accumulated;
            s.buffer_pool.write_page(name, accumulated);
            s.indexes[name] = BPlusTree();

            // Perf: dedup used to be std::find(accumulated.begin(), accumulated.end(), mapped)
            // -- an O(n) linear scan per candidate row, making the whole recursive-accumulation
            // loop O(n^2) in the number of rows produced (PLAN.md Section E). Every `mapped` row
            // in this loop has exactly `cols`'s columns plus the two constant "_xmin"/"_xmax"
            // sentinels (see below), so joining `cols`'s values in their fixed vector order is a
            // faithful stand-in for Row equality here -- lets dedup use an O(1)-average hash set
            // instead. `seen` mirrors `accumulated` only (updated once per iteration, after that
            // iteration's whole new_rows batch is processed) so within-iteration duplicates are
            // still NOT deduped against each other, exactly matching the original std::find
            // behavior (which only ever checked against `accumulated`, never against `fresh`).
            auto row_key = [&cols](const Row& r) {
                std::string key;
                for (auto& c : cols) {
                    auto it = r.find(c);
                    key += it != r.end() ? it->second : std::string();
                    key += '\x01';
                }
                return key;
            };
            std::unordered_set<std::string> seen;
            seen.reserve(accumulated.size());
            for (auto& r : accumulated) seen.insert(row_key(r));

            // Perf (semi-naive evaluation): the loop below used to re-execute `right`
            // against the FULL accumulated-so-far table every iteration, re-deriving
            // already-known relationships over and over -- O(iterations * accumulated
            // size) even with the O(1) dedup fix above. Classic semi-naive recursive-query
            // evaluation (Datalog's standard optimization for monotonic recursion) instead
            // joins the recursive term against only the DELTA (rows added in the previous
            // iteration): any row derivable by joining against an OLDER row was already
            // derived in that older row's own iteration, so re-deriving it again from the
            // full table is pure waste -- it'll just get filtered out by `seen` anyway.
            // This is only sound when `right` references the CTE exactly once and has no
            // other construct that would see a different (wrong) answer when its input
            // shrinks from "everything so far" to "just the delta": no FROM-subquery, no
            // LATERAL/subquery join, no GROUP BY, no aggregate or scalar-subquery SELECT
            // column, no subquery in WHERE/HAVING, and no LIMIT/OFFSET (which would return
            // a different row SET, not just a slower-to-compute same set, over a smaller
            // input). Whenever any of these appear, fall back to the original always-safe
            // full-accumulation behavior unchanged -- never a correctness trade, only a
            // perf one that's foregone for shapes this analysis can't yet prove safe for.
            bool semi_naive_ok = false;
            if (auto* right_sel = std::get_if<Statement::Select>(&right.data)) {
                semi_naive_ok = !right_sel->subquery && !right_sel->group_by && !right_sel->limit && !right_sel->offset &&
                                !condition_has_subquery(right_sel->condition) && !condition_has_subquery(right_sel->having);
                if (semi_naive_ok) {
                    for (auto& col : right_sel->columns) {
                        if (std::holds_alternative<SelectColumn::Agg>(col.data) || std::holds_alternative<SelectColumn::AggAlias>(col.data) ||
                            std::holds_alternative<SelectColumn::Subquery>(col.data)) {
                            semi_naive_ok = false;
                            break;
                        }
                    }
                }
                if (semi_naive_ok) {
                    std::size_t cte_refs = (right_sel->table == name) ? 1 : 0;
                    for (auto& j : right_sel->joins) {
                        if (j.lateral || j.subquery) {
                            semi_naive_ok = false;
                            break;
                        }
                        if (j.table == name) cte_refs++;
                    }
                    if (semi_naive_ok && cte_refs != 1) semi_naive_ok = false;
                }
            }
            // The first iteration's delta is the base case itself -- nothing has been
            // derived yet, so joining against the base case IS the correct starting input.
            std::vector<Row> delta = accumulated;

            // Regression: this loop used to have no way to tell "stopped because no new
            // rows appeared" (correct fixed point) apart from "stopped because we hit the
            // iteration cap" (recursion didn't converge) -- both fell through to the exact
            // same code below, silently returning whatever partial rows had accumulated so
            // far with nothing telling the caller the result might be incomplete. Real
            // MySQL fails a recursive CTE that exceeds its depth limit by default
            // (ER_CTE_RECURSION_LIMIT) rather than silently truncating, and this project's
            // own newer, more deliberate precedent for an analogous cap (procedure
            // WHILE/LOOP/REPEAT, executor_proc.cpp) also fails with an error rather than
            // silently returning a partial result -- match both here.
            bool reached_fixed_point = false;
            for (int iter = 0; iter < 1000; iter++) {
                if (semi_naive_ok) {
                    s.tables[name] = delta;
                    s.buffer_pool.write_page(name, delta);
                } else {
                    s.tables[name] = accumulated;
                    s.buffer_pool.write_page(name, accumulated);
                }
                auto rec_out = execute_with_s(s, right);
                if (rec_out.is_err()) {
                    for (auto& n : cte_names) {
                        s.tables.erase(n);
                        s.indexes.erase(n);
                        s.buffer_pool.invalidate(n);
                        s.catalog.drop_table(n);
                    }
                    s.tables.erase(name);
                    s.indexes.erase(name);
                    s.buffer_pool.invalidate(name);
                    s.catalog.drop_table(name);
                    return rec_out;
                }
                auto [rec_cols, new_rows] = parse_table_output(rec_out.value());

                std::vector<Row> fresh;
                std::vector<std::string> fresh_keys;
                for (auto& rec_row : new_rows) {
                    Row mapped;
                    for (std::size_t i = 0; i < cols.size(); i++) {
                        std::string val;
                        if (i < rec_cols.size()) {
                            auto it = rec_row.find(rec_cols[i]);
                            if (it != rec_row.end()) val = it->second;
                        }
                        mapped[cols[i]] = val;
                    }
                    // MVCC: "0" is the permanent "always visible" sentinel (matches the
                    // base case's rows, which have no _xmin key at all and so default to
                    // the same 0 via parse_txn_id) -- this synthetic, single-statement-
                    // lifetime CTE table isn't real MVCC-tracked data, so a real txn id
                    // (or the old hardcoded "1") would make is_visible_for_read's cutoff
                    // check spuriously reject these rows depending on the current global
                    // txn-id counter, breaking the recursive accumulation loop below.
                    mapped["_xmin"] = "0";
                    mapped["_xmax"] = "0";
                    std::string key = row_key(mapped);
                    if (!seen.count(key)) {
                        fresh_keys.push_back(std::move(key));
                        fresh.push_back(std::move(mapped));
                    }
                }
                if (fresh.empty()) {
                    reached_fixed_point = true;
                    break;
                }
                for (auto& k : fresh_keys) seen.insert(std::move(k));
                accumulated.insert(accumulated.end(), fresh.begin(), fresh.end());
                if (semi_naive_ok) delta = std::move(fresh);
            }

            // Whatever the loop above left in s.tables[name] mid-iteration (possibly just
            // the last delta, under semi-naive evaluation) -- restore the FULL accumulated
            // result before anything downstream (the main query below, or a later sibling
            // CTE) reads this CTE's table.
            s.tables[name] = accumulated;
            s.buffer_pool.write_page(name, accumulated);

            if (!reached_fixed_point) {
                for (auto& n : cte_names) {
                    s.tables.erase(n);
                    s.indexes.erase(n);
                    s.buffer_pool.invalidate(n);
                    s.catalog.drop_table(n);
                }
                s.tables.erase(name);
                s.indexes.erase(name);
                s.buffer_pool.invalidate(name);
                s.catalog.drop_table(name);
                return StringResult::Err("Recursive CTE '" + name +
                                          "' did not reach a fixed point after 1000 iterations "
                                          "(still producing new rows) -- aborted instead of returning an incomplete result. "
                                          "Check the recursive term terminates, or the recursion may be genuinely unbounded.");
            }

            cte_names.push_back(name);
            auto result = execute_with_s(s, std::move(query));
            for (auto& n : cte_names) {
                s.tables.erase(n);
                s.indexes.erase(n);
                s.buffer_pool.invalidate(n);
                s.catalog.drop_table(n);
            }
            return result;
        }

        auto output = execute_with_s(s, std::move(*body));
        if (output.is_err()) {
            for (auto& n : cte_names) {
                s.tables.erase(n);
                s.indexes.erase(n);
                s.buffer_pool.invalidate(n);
                s.catalog.drop_table(n);
            }
            return output;
        }
        std::tie(col_names, rows) = parse_table_output(output.value());

        std::vector<ColumnDef> schema_cols;
        for (auto& c : col_names) {
            ColumnDef cd;
            cd.name = c;
            cd.data_type = DataType(DataType::Text{});
            schema_cols.push_back(cd);
        }
        s.catalog.create_table(name, schema_cols);
        s.tables[name] = rows;
        s.buffer_pool.write_page(name, rows);
        s.indexes[name] = BPlusTree();
        cte_names.push_back(name);
    }

    auto result = execute_with_s(s, std::move(query));

    for (auto& name : cte_names) {
        s.tables.erase(name);
        s.indexes.erase(name);
        s.buffer_pool.invalidate(name);
        s.catalog.drop_table(name);
    }

    return result;
}

} // namespace engine
