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
                    mapped["_xmin"] = "1";
                    mapped["_xmax"] = "0";
                    if (std::find(accumulated.begin(), accumulated.end(), mapped) == accumulated.end()) fresh.push_back(std::move(mapped));
                }
                if (fresh.empty()) break;
                accumulated.insert(accumulated.end(), fresh.begin(), fresh.end());
                s.tables[name] = accumulated;
                s.buffer_pool.write_page(name, accumulated);
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
