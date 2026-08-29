// Faithful port of multi-table UPDATE/DELETE from rusql-core/src/engine/executor.rs
// (Phase 8f): exec_multi_update, exec_multi_delete.

#include "engine/executor/executor.hpp"

#include <unordered_set>

#include "engine/join.hpp"

namespace engine {

namespace {
void insert_prefixed(Row& merged, const std::string& tbl, const Row& src) {
    for (auto& [k, v] : src) {
        merged[tbl + "." + k] = v;
        if (!merged.count(k)) merged[k] = v;
    }
}
} // namespace

StringResult Executor::exec_multi_update(SharedDatabase& s, std::vector<std::string> tables, std::vector<Join> joins,
                                          std::vector<std::pair<std::string, ArithExpr>> assignments, std::optional<CondExpr> condition) {
    if (tables.empty()) return StringResult::Err("No tables specified for multi-table UPDATE");
    std::string first_table = tables.front();

    auto tit0 = s.tables.find(first_table);
    if (tit0 == s.tables.end()) return StringResult::Err("Table '" + first_table + "' not found");
    std::vector<Row> current;
    for (auto& r : tit0->second) {
        if (!is_visible(r)) continue;
        Row prefixed;
        insert_prefixed(prefixed, first_table, r);
        current.push_back(std::move(prefixed));
    }

    for (std::size_t i = 1; i < tables.size(); i++) {
        auto& extra_tbl = tables[i];
        auto tit = s.tables.find(extra_tbl);
        if (tit == s.tables.end()) return StringResult::Err("Table '" + extra_tbl + "' not found");
        std::vector<Row> right_rows;
        for (auto& r : tit->second) {
            if (is_visible(r)) right_rows.push_back(r);
        }
        std::vector<Row> out;
        for (auto& left : current) {
            for (auto& right : right_rows) {
                Row merged = left;
                insert_prefixed(merged, extra_tbl, right);
                out.push_back(std::move(merged));
            }
        }
        current = std::move(out);
    }

    for (auto& j : joins) {
        auto tit = s.tables.find(j.table);
        if (tit == s.tables.end()) return StringResult::Err("Table '" + j.table + "' not found");
        std::vector<Row> right_rows;
        for (auto& r : tit->second) {
            if (is_visible(r)) right_rows.push_back(r);
        }
        std::vector<std::string> right_schema_cols;
        if (const auto* sc = s.catalog.get_table(j.table)) {
            for (auto& c : sc->columns) right_schema_cols.push_back(c.name);
        }
        const CondExpr& on_expr = j.on_expr;
        current = nested_loop_join(current, right_rows, j.join_type, j.table, j.using_cols, right_schema_cols,
                                    [&](const Row& merged) { return eval_condexpr(merged, on_expr); });
    }

    std::vector<Row> matched;
    for (auto& r : current) {
        if (matches_condition_with_subquery(s, r, condition)) matched.push_back(std::move(r));
    }

    std::vector<std::string> target_tables = tables;
    for (auto& j : joins) target_tables.push_back(j.table);

    auto resolve_tbl = [&](const std::string& name) -> std::string {
        std::string suffix = "." + name;
        for (auto& t : target_tables) {
            if (t == name || (t.size() >= suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0)) return t;
        }
        return name;
    };

    std::size_t total_count = 0;

    std::vector<std::string> assignment_tables;
    for (auto& [col_expr, _] : assignments) {
        std::string tbl;
        if (auto dot = col_expr.find('.'); dot != std::string::npos) tbl = resolve_tbl(col_expr.substr(0, dot));
        else tbl = first_table;
        if (std::find(assignment_tables.begin(), assignment_tables.end(), tbl) == assignment_tables.end()) assignment_tables.push_back(tbl);
    }

    for (auto& tgt : assignment_tables) {
        const TableSchema* schema = s.catalog.get_table(tgt);
        if (!schema) return StringResult::Err("Table '" + tgt + "' not found");
        std::string pk_col = "id";
        std::vector<std::string> pk_cols;
        for (auto& c : schema->columns) {
            if (c.primary_key) pk_cols.push_back(c.name);
        }
        if (pk_cols.empty()) pk_cols.push_back("id");
        pk_col = pk_cols.front();
        std::string pk_prefix = tgt + ".";

        // Composite identity key, same fix/reasoning as plain UPDATE (executor_update.cpp):
        // a composite-PK target table's rows must be identified by ALL of its PK columns,
        // not just the first, or two distinct rows sharing the leading column's value get
        // conflated (one's assignments silently overwrite/merge into the other's, and both
        // rows end up updated even if only one matched the JOIN+WHERE). \x00-joined,
        // mirroring the composite index convention.
        auto merged_row_key = [&](const Row& merged_row) -> std::optional<std::string> {
            std::string key;
            for (std::size_t i = 0; i < pk_cols.size(); i++) {
                std::string val;
                if (auto it = merged_row.find(pk_prefix + pk_cols[i]); it != merged_row.end()) val = it->second;
                else if (auto it2 = merged_row.find(pk_cols[i]); it2 != merged_row.end()) val = it2->second;
                if (val.empty()) return std::nullopt;
                if (i) key += '\x00';
                key += val;
            }
            return key;
        };
        auto row_key = [&pk_cols](const Row& row) {
            std::string key;
            for (std::size_t i = 0; i < pk_cols.size(); i++) {
                if (i) key += '\x00';
                auto it = row.find(pk_cols[i]);
                key += (it != row.end() ? it->second : std::string());
            }
            return key;
        };

        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> pk_updates;
        for (auto& merged_row : matched) {
            auto pk_val = merged_row_key(merged_row);
            if (!pk_val) continue;
            auto& entry = pk_updates[*pk_val];
            for (auto& [col_expr, rhs_expr] : assignments) {
                std::string tbl_name, bare_col;
                if (auto dot = col_expr.find('.'); dot != std::string::npos) {
                    tbl_name = resolve_tbl(col_expr.substr(0, dot));
                    bare_col = col_expr.substr(dot + 1);
                } else {
                    tbl_name = first_table;
                    bare_col = col_expr;
                }
                if (tbl_name != tgt) continue;
                entry[bare_col] = eval_arith(merged_row, rhs_expr);
            }
        }

        auto rit = s.tables.find(tgt);
        if (rit == s.tables.end()) return StringResult::Err("Table '" + tgt + "' not found");
        auto& rows = rit->second;
        std::vector<std::pair<Row, Row>> updated_pairs; // (old_row, new_row)
        for (auto& row : rows) {
            if (auto uit = pk_updates.find(row_key(row)); uit != pk_updates.end()) {
                Row old_row = row;
                for (auto& [col, val] : uit->second) row[col] = val;
                updated_pairs.emplace_back(std::move(old_row), row);
                total_count++;
            }
        }

        // Incremental index maintenance (PK B+Tree, secondary, hash, composite):
        // previously this cloned the whole table and fully rebuilt every index kind from
        // scratch on every statement regardless of how few rows changed -- replaced with
        // per-row remove-old-key/insert-new-key updates (the PK value itself can be part
        // of the assignments, so the new key is read from `new_row` rather than assumed
        // unchanged).
        for (auto& [old_row, new_row] : updated_pairs) {
            if (auto idx_it = s.indexes.find(tgt); idx_it != s.indexes.end()) {
                auto old_it = old_row.find(pk_col);
                idx_it->second.remove(old_it != old_row.end() ? old_it->second : std::string());
                auto new_it = new_row.find(pk_col);
                nlohmann::json j = new_row;
                idx_it->second.insert(new_it != new_row.end() ? new_it->second : std::string(), j.dump());
            }
            index_remove_row(s, tgt, old_row, pk_col);
            index_insert_row(s, tgt, new_row);
            for (auto& [k, ci] : s.composite_indexes) {
                if (ci.table != tgt) continue;
                ci.remove_row(old_row);
                ci.insert_row(new_row);
            }
        }

        std::vector<Row> rows_clone = s.tables.at(tgt);
        s.buffer_pool.write_page(tgt, rows_clone);
        s.buffer_pool.flush_page(tgt, s.disk);
    }

    maybe_auto_checkpoint(s);
    return StringResult::Ok(std::to_string(total_count) + " row(s) updated.");
}

StringResult Executor::exec_multi_delete(SharedDatabase& s, std::vector<std::string> delete_tables, std::string from_table,
                                          std::vector<Join> joins, std::optional<CondExpr> condition) {
    auto tit0 = s.tables.find(from_table);
    if (tit0 == s.tables.end()) return StringResult::Err("Table '" + from_table + "' not found");
    std::vector<Row> current;
    for (auto& r : tit0->second) {
        if (!is_visible(r)) continue;
        Row prefixed;
        insert_prefixed(prefixed, from_table, r);
        current.push_back(std::move(prefixed));
    }

    for (auto& j : joins) {
        auto tit = s.tables.find(j.table);
        if (tit == s.tables.end()) return StringResult::Err("Table '" + j.table + "' not found");
        std::vector<Row> right_rows;
        for (auto& r : tit->second) {
            if (is_visible(r)) right_rows.push_back(r);
        }
        std::string tbl = j.table;
        std::vector<Row> out;

        if (!j.using_cols.empty()) {
            for (auto& left : current) {
                for (auto& right : right_rows) {
                    bool matches = true;
                    for (auto& col : j.using_cols) {
                        auto lit = left.find(col);
                        auto rit = right.find(col);
                        std::string lv = lit != left.end() ? lit->second : EXECUTOR_NULL_VALUE;
                        std::string rv = rit != right.end() ? rit->second : EXECUTOR_NULL_VALUE;
                        if (!(lv == rv && lv != EXECUTOR_NULL_VALUE)) {
                            matches = false;
                            break;
                        }
                    }
                    if (matches) {
                        Row merged = left;
                        insert_prefixed(merged, tbl, right);
                        out.push_back(std::move(merged));
                    }
                }
            }
            current = std::move(out);
            continue;
        }

        switch (j.join_type) {
            case JoinType::Inner: {
                for (auto& left : current) {
                    for (auto& right : right_rows) {
                        Row merged = left;
                        insert_prefixed(merged, tbl, right);
                        if (eval_condexpr(merged, j.on_expr)) out.push_back(std::move(merged));
                    }
                }
                break;
            }
            case JoinType::Left: {
                std::vector<std::string> right_schema_cols;
                if (const auto* sc = s.catalog.get_table(j.table)) {
                    for (auto& c : sc->columns) right_schema_cols.push_back(c.name);
                }
                for (auto& left : current) {
                    bool matched = false;
                    for (auto& right : right_rows) {
                        Row merged = left;
                        insert_prefixed(merged, tbl, right);
                        if (eval_condexpr(merged, j.on_expr)) {
                            out.push_back(std::move(merged));
                            matched = true;
                        }
                    }
                    if (!matched) {
                        Row merged = left;
                        for (auto& col : right_schema_cols) merged[tbl + "." + col] = EXECUTOR_NULL_VALUE;
                        out.push_back(std::move(merged));
                    }
                }
                break;
            }
            case JoinType::Right: {
                std::vector<std::string> left_cols;
                if (!current.empty()) {
                    for (auto& [k, v] : current.front()) {
                        (void)v;
                        if (!k.empty() && k.front() == '_') continue;
                        if (k.find('.') != std::string::npos) continue;
                        left_cols.push_back(k);
                    }
                }
                for (auto& right : right_rows) {
                    bool matched = false;
                    for (auto& left : current) {
                        Row merged = left;
                        insert_prefixed(merged, tbl, right);
                        if (eval_condexpr(merged, j.on_expr)) {
                            out.push_back(std::move(merged));
                            matched = true;
                        }
                    }
                    if (!matched) {
                        Row merged;
                        for (auto& col : left_cols) merged[col] = EXECUTOR_NULL_VALUE;
                        insert_prefixed(merged, tbl, right);
                        out.push_back(std::move(merged));
                    }
                }
                break;
            }
            case JoinType::Cross:
            case JoinType::Natural:
            case JoinType::FullOuter: {
                for (auto& left : current) {
                    for (auto& right : right_rows) {
                        Row merged = left;
                        insert_prefixed(merged, tbl, right);
                        if (eval_condexpr(merged, j.on_expr)) out.push_back(std::move(merged));
                    }
                }
                break;
            }
        }
        current = std::move(out);
    }

    std::vector<Row> matched;
    for (auto& r : current) {
        if (matches_condition_with_subquery(s, r, condition)) matched.push_back(std::move(r));
    }

    std::size_t total_count = 0;

    for (auto& tgt : delete_tables) {
        const TableSchema* schema = s.catalog.get_table(tgt);
        if (!schema) return StringResult::Err("Table '" + tgt + "' not found");
        std::string pk_col = "id";
        std::vector<std::string> pk_cols;
        for (auto& c : schema->columns) {
            if (c.primary_key) pk_cols.push_back(c.name);
        }
        if (pk_cols.empty()) pk_cols.push_back("id");
        pk_col = pk_cols.front();
        std::string pk_prefix = tgt + ".";

        // Composite identity key, same fix/reasoning as exec_multi_update above and
        // plain UPDATE (executor_update.cpp) -- a composite-PK target table's rows must
        // be identified by ALL of its PK columns, not just the first.
        auto merged_row_key = [&](const Row& r) -> std::optional<std::string> {
            std::string key;
            for (std::size_t i = 0; i < pk_cols.size(); i++) {
                std::string val;
                if (auto it = r.find(pk_prefix + pk_cols[i]); it != r.end()) val = it->second;
                else if (auto it2 = r.find(pk_cols[i]); it2 != r.end()) val = it2->second;
                if (val.empty()) return std::nullopt;
                if (i) key += '\x00';
                key += val;
            }
            return key;
        };
        auto row_key = [&pk_cols](const Row& r) {
            std::string key;
            for (std::size_t i = 0; i < pk_cols.size(); i++) {
                if (i) key += '\x00';
                auto it = r.find(pk_cols[i]);
                key += (it != r.end() ? it->second : std::string());
            }
            return key;
        };

        std::unordered_set<std::string> target_pks;
        for (auto& r : matched) {
            if (auto key = merged_row_key(r)) target_pks.insert(*key);
        }

        auto rit = s.tables.find(tgt);
        if (rit == s.tables.end()) return StringResult::Err("Table '" + tgt + "' not found");
        auto& rows = rit->second;

        std::size_t before = 0;
        for (auto& r : rows) {
            if (is_visible(r)) before++;
        }
        std::vector<Row> deleted_rows;
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                   [&](const Row& r) {
                                       if (!is_visible(r)) return false;
                                       if (target_pks.count(row_key(r)) == 0) return false;
                                       deleted_rows.push_back(r);
                                       return true;
                                   }),
                   rows.end());
        std::size_t after = 0;
        for (auto& r : rows) {
            if (is_visible(r)) after++;
        }
        total_count += before - after;

        // Incremental index maintenance (PK B+Tree + composite): previously this cloned
        // the whole table and fully rebuilt both from scratch on every statement,
        // regardless of how few rows were actually deleted.
        if (auto idx_it = s.indexes.find(tgt); idx_it != s.indexes.end()) {
            for (auto& row : deleted_rows) {
                auto it = row.find(pk_col);
                idx_it->second.remove(it != row.end() ? it->second : std::string());
            }
        }
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table != tgt) continue;
            for (auto& row : deleted_rows) ci.remove_row(row);
        }
        // PLAN.md P2 fix: secondary/hash indexes were never touched here at all
        // (previously stale after a multi-table DELETE).
        for (auto& row : deleted_rows) index_remove_row(s, tgt, row, pk_col);

        std::vector<Row> rows_clone = s.tables.at(tgt);
        s.buffer_pool.write_page(tgt, rows_clone);
        s.buffer_pool.flush_page(tgt, s.disk);
    }

    maybe_auto_checkpoint(s);
    return StringResult::Ok(std::to_string(total_count) + " row(s) deleted.");
}

} // namespace engine
