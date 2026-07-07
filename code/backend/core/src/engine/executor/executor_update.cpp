// Faithful port of the UPDATE path from rusql-core/src/engine/executor.rs (Phase 8b):
// exec_update, exec_update_inner (row matching, lock acquisition, ENUM/SET/CHECK
// validation, incremental index maintenance, ON UPDATE FK cascade). Row matching for
// `matching_pks` uses matches_condition_with_subquery (Phase 8c), matching the Rust
// original exactly — UPDATE, unlike DELETE, always uses the subquery-aware matcher
// with no fast-path/slow-path split.

#include "engine/executor/executor.hpp"

#include <algorithm>

namespace engine {

namespace {
std::string trim_ws(const std::string& s) {
    auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}
} // namespace

StringResult Executor::exec_update(SharedDatabase& s, std::string table, std::vector<std::pair<std::string, ArithExpr>> assignments,
                                    std::optional<CondExpr> condition, std::optional<std::vector<SelectColumn>> returning) {
    if (s.views.count(table)) {
        if (auto resolved = resolve_updatable_view(s, table)) {
            auto merged_cond = merge_conditions(resolved->second, condition);
            return exec_update(s, resolved->first, assignments, merged_cond, returning);
        }
        return StringResult::Err("View '" + strip_db_prefix(table) + "' is not updatable");
    }

    std::optional<std::vector<std::pair<std::string, std::vector<Row>>>> committed_backups;
    if (txn.is_active()) {
        std::vector<std::pair<std::string, std::vector<Row>>> backups;
        backups.emplace_back(table, session_swap_in(s, table));
        std::vector<std::string> fk_tables;
        for (auto& [tname, tschema] : s.catalog.tables) {
            if (tname == table) continue;
            bool refs = std::any_of(tschema.columns.begin(), tschema.columns.end(), [&](const ColumnDef& c) {
                return c.foreign_key && c.foreign_key->ref_table == table && c.foreign_key->on_update != FkAction::Restrict;
            });
            if (refs) fk_tables.push_back(tname);
        }
        for (auto& t : fk_tables) backups.emplace_back(t, session_swap_in(s, t));
        committed_backups = std::move(backups);
    }

    fire_triggers(s, table, "BEFORE", "UPDATE");
    auto result = exec_update_inner(s, table, assignments, condition, returning);
    if (committed_backups) {
        for (auto& [tname, committed] : *committed_backups) session_swap_out(s, tname, std::move(committed));
    }
    if (result.is_ok()) fire_triggers(s, table, "AFTER", "UPDATE");
    return result;
}

StringResult Executor::exec_update_inner(SharedDatabase& s, const std::string& table,
                                          const std::vector<std::pair<std::string, ArithExpr>>& assignments,
                                          const std::optional<CondExpr>& condition,
                                          const std::optional<std::vector<SelectColumn>>& returning) {
    const TableSchema* schema0 = s.catalog.get_table(table);
    if (!schema0) return StringResult::Err("Table '" + table + "' not found");
    std::string pk_col = "id";
    for (auto& c : schema0->columns) {
        if (c.primary_key) {
            pk_col = c.name;
            break;
        }
    }

    auto tit0 = s.tables.find(table);
    if (tit0 == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");

    // Cloned first (not iterated in place): matches_condition_with_subquery can invoke
    // exec_select, which for FROM-subqueries/views temporarily inserts/erases entries in
    // s.tables — holding a live reference into s.tables across that call would risk
    // iterator/reference invalidation on rehash.
    std::vector<Row> candidate_rows;
    for (auto& r : tit0->second) {
        if (is_visible(r)) candidate_rows.push_back(r);
    }

    std::unordered_set<std::string> matching_pks;
    for (auto& r : candidate_rows) {
        if (matches_condition_with_subquery(s, r, condition)) {
            auto it = r.find(pk_col);
            matching_pks.insert(it != r.end() ? it->second : std::string());
        }
    }

    auto tit = s.tables.find(table);
    std::vector<Row>& rows = tit->second;

    std::size_t count = 0;
    struct UndoEntry {
        std::string key, old_json, new_json;
    };
    std::vector<UndoEntry> undo_entries;
    std::uint64_t cur_txn = txn.current_txn_id();

    for (auto& row : rows) {
        auto pkit = row.find(pk_col);
        std::string row_pk = pkit != row.end() ? pkit->second : std::string();
        if (!matching_pks.count(row_pk)) continue;

        const std::string& key = row_pk;
        if (cur_txn != 0) {
            LockResult lr = s.lock_mgr.acquire(table, key, cur_txn);
            if (lr.kind == LockResult::Kind::Conflict) {
                return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + key + "' in '" + table +
                                          "' is held by transaction " + std::to_string(lr.holder) + ". Cannot UPDATE.");
            }
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(cur_txn) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (UPDATE '" + table + "'. Transaction " + std::to_string(cur_txn) +
                                          " aborted.");
            }
        }

        nlohmann::json old_j = row;
        std::string old_json = old_j.dump();

        std::vector<std::pair<std::string, std::string>> new_vals;
        for (auto& [col, expr] : assignments) new_vals.emplace_back(col, eval_arith(row, expr));

        if (auto* schema = s.catalog.get_table(table)) {
            for (auto& [col_name, val] : new_vals) {
                if (val.empty() || val == EXECUTOR_NULL_VALUE) continue;
                auto cit = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == col_name; });
                if (cit == schema->columns.end()) continue;
                if (auto* en = std::get_if<DataType::Enum>(&cit->data_type.data)) {
                    if (std::find(en->values.begin(), en->values.end(), val) == en->values.end()) {
                        std::string allowed;
                        for (std::size_t k = 0; k < en->values.size(); k++) {
                            if (k) allowed += ", ";
                            allowed += "'" + en->values[k] + "'";
                        }
                        return StringResult::Err("Invalid ENUM value '" + val + "' for column '" + cit->name + "'. Allowed: " + allowed);
                    }
                } else if (auto* se = std::get_if<DataType::Set>(&cit->data_type.data)) {
                    std::size_t start = 0;
                    while (true) {
                        auto comma = val.find(',', start);
                        std::string part = trim_ws(val.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                        if (!part.empty() && std::find(se->values.begin(), se->values.end(), part) == se->values.end()) {
                            std::string allowed;
                            for (std::size_t k = 0; k < se->values.size(); k++) {
                                if (k) allowed += ", ";
                                allowed += "'" + se->values[k] + "'";
                            }
                            return StringResult::Err("Invalid SET value '" + part + "' for column '" + cit->name + "'. Allowed: " + allowed);
                        }
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
            }
        }

        for (auto& [col, val] : new_vals) row[col] = val;

        if (auto* schema = s.catalog.get_table(table)) {
            for (auto& col : schema->columns) {
                if (col.check_expr && !eval_check_expr(*col.check_expr, row)) {
                    return StringResult::Err("CHECK constraint violated on column '" + col.name + "': " + *col.check_expr);
                }
            }
            for (auto& check : schema->check_constraints) {
                if (!eval_check_expr(check.expression, row)) {
                    return StringResult::Err("CHECK constraint '" + check.name.value_or(check.expression) + "' violated");
                }
            }
        }

        nlohmann::json new_j = row;
        undo_entries.push_back({key, old_json, new_j.dump()});
        count++;
    }

    for (auto& u : undo_entries) txn.log_update(table, u.key, u.old_json, u.new_json);

    std::vector<Row> rows_clone = s.tables.at(table);
    if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
        idx_it->second = BPlusTree();
        for (auto& row : rows_clone) {
            auto it = row.find(pk_col);
            std::string k = it != row.end() ? it->second : std::string();
            nlohmann::json j = row;
            idx_it->second.insert(k, j.dump());
        }
    }

    for (auto& u : undo_entries) {
        Row old_row, new_row;
        try {
            old_row = nlohmann::json::parse(u.old_json).get<Row>();
        } catch (...) {
        }
        try {
            new_row = nlohmann::json::parse(u.new_json).get<Row>();
        } catch (...) {
        }
        index_remove_row(s, table, old_row, pk_col);
        index_insert_row(s, table, new_row);
    }

    std::vector<std::string> comp_keys;
    for (auto& [k, ci] : s.composite_indexes) {
        if (ci.table == table) comp_keys.push_back(k);
    }
    for (auto& k : comp_keys) s.composite_indexes.at(k).rebuild(rows_clone);

    std::vector<std::string> changed_cols;
    for (auto& [c, _] : assignments) changed_cols.push_back(c);

    std::vector<std::pair<std::string, std::vector<ColumnDef>>> other_tables;
    for (auto& [name, schema] : s.catalog.tables) {
        if (name != table) other_tables.emplace_back(name, schema.columns);
    }

    for (auto& u : undo_entries) {
        Row old_row;
        try {
            old_row = nlohmann::json::parse(u.old_json).get<Row>();
        } catch (...) {
        }
        for (auto& assign_col : changed_cols) {
            auto oit = old_row.find(assign_col);
            std::string old_val = oit != old_row.end() ? oit->second : std::string();
            std::string new_val;
            for (auto& [c, expr] : assignments) {
                if (c == assign_col) {
                    new_val = eval_arith(old_row, expr);
                    break;
                }
            }
            if (old_val == new_val) continue;

            for (auto& [other_table, cols] : other_tables) {
                for (auto& col : cols) {
                    if (!col.foreign_key || col.foreign_key->ref_table != table || col.foreign_key->ref_column != assign_col) continue;
                    switch (col.foreign_key->on_update) {
                        case FkAction::Restrict: {
                            if (auto oit2 = s.tables.find(other_table); oit2 != s.tables.end()) {
                                bool referenced = std::any_of(oit2->second.begin(), oit2->second.end(), [&](const Row& r) {
                                    if (!is_visible(r)) return false;
                                    auto it = r.find(col.name);
                                    return it != r.end() && it->second == old_val;
                                });
                                if (referenced) {
                                    return StringResult::Err("Foreign key violation (ON UPDATE RESTRICT): '" + assign_col +
                                                              "' is referenced by '" + other_table + "'.'" + col.name + "'");
                                }
                            }
                            break;
                        }
                        case FkAction::Cascade: {
                            if (auto oit2 = s.tables.find(other_table); oit2 != s.tables.end()) {
                                for (auto& row : oit2->second) {
                                    if (!is_visible(row)) continue;
                                    auto it = row.find(col.name);
                                    if (it != row.end() && it->second == old_val) row[col.name] = new_val;
                                }
                            }
                            if (!txn.is_active()) {
                                std::vector<Row> rc = s.tables.at(other_table);
                                s.buffer_pool.write_page(other_table, rc);
                                s.buffer_pool.flush_page(other_table, s.disk);
                            }
                            break;
                        }
                        case FkAction::SetNull: {
                            if (auto oit2 = s.tables.find(other_table); oit2 != s.tables.end()) {
                                for (auto& row : oit2->second) {
                                    if (!is_visible(row)) continue;
                                    auto it = row.find(col.name);
                                    if (it != row.end() && it->second == old_val) row[col.name] = EXECUTOR_NULL_VALUE;
                                }
                            }
                            if (!txn.is_active()) {
                                std::vector<Row> rc = s.tables.at(other_table);
                                s.buffer_pool.write_page(other_table, rc);
                                s.buffer_pool.flush_page(other_table, s.disk);
                            }
                            break;
                        }
                        case FkAction::SetDefault: {
                            std::string default_val = EXECUTOR_NULL_VALUE;
                            if (auto* osc = s.catalog.get_table(other_table)) {
                                auto cit =
                                    std::find_if(osc->columns.begin(), osc->columns.end(), [&](const ColumnDef& c2) { return c2.name == col.name; });
                                if (cit != osc->columns.end() && cit->default_value) default_val = *cit->default_value;
                            }
                            if (auto oit2 = s.tables.find(other_table); oit2 != s.tables.end()) {
                                for (auto& row : oit2->second) {
                                    if (!is_visible(row)) continue;
                                    auto it = row.find(col.name);
                                    if (it != row.end() && it->second == old_val) row[col.name] = default_val;
                                }
                            }
                            if (!txn.is_active()) {
                                std::vector<Row> rc = s.tables.at(other_table);
                                s.buffer_pool.write_page(other_table, rc);
                                s.buffer_pool.flush_page(other_table, s.disk);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!txn.is_active()) {
        std::vector<Row> rc = s.tables.at(table);
        s.buffer_pool.write_page(table, rc);
        s.buffer_pool.flush_page(table, s.disk);
        maybe_auto_vacuum(s);
        maybe_auto_analyze(s, table);
    }
    maybe_auto_checkpoint(s);

    if (returning) {
        std::vector<Row> updated_rows;
        for (auto& r : s.tables.at(table)) {
            if (!is_visible(r)) continue;
            auto it = r.find(pk_col);
            std::string v = it != r.end() ? it->second : std::string();
            if (matching_pks.count(v)) updated_rows.push_back(r);
        }
        return StringResult::Ok(format_returning_rows(updated_rows, *returning));
    }
    return StringResult::Ok(std::to_string(count) + " row(s) updated.");
}

} // namespace engine
