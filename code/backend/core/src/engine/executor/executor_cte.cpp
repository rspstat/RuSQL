// Faithful port of exec_with (WITH ... [RECURSIVE] common table expressions) from
// rusql-core/src/engine/executor.rs (Phase 8c): each CTE is materialized as a
// temporary in-memory table (recursive CTEs over a UNION iterate base case + recursive
// case until no new rows appear, capped at 1000 iterations), the main query runs
// against those virtual tables, and they're torn down afterward.

#include "engine/executor/executor.hpp"

#include <algorithm>

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
                    if (std::find(accumulated.begin(), accumulated.end(), mapped) == accumulated.end()) fresh.push_back(std::move(mapped));
                }
                if (fresh.empty()) {
                    reached_fixed_point = true;
                    break;
                }
                accumulated.insert(accumulated.end(), fresh.begin(), fresh.end());
                s.tables[name] = accumulated;
                s.buffer_pool.write_page(name, accumulated);
            }

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
