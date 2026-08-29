// Faithful port of MERGE from rusql-core/src/engine/executor.rs (Phase 8f): exec_merge.

#include "engine/executor/executor.hpp"

namespace engine {

namespace {
std::string bare_name(const std::string& qualified) {
    auto pos = qualified.rfind('.');
    return pos == std::string::npos ? qualified : qualified.substr(pos + 1);
}

std::string trim_quotes(const std::string& v) {
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') return v.substr(1, v.size() - 2);
    return v;
}
} // namespace

StringResult Executor::exec_merge(SharedDatabase& s, std::string target, std::optional<std::string> target_alias, std::string source,
                                   std::optional<std::string> source_alias, CondExpr on,
                                   std::optional<std::vector<std::pair<std::string, ArithExpr>>> when_matched_update, bool when_matched_delete,
                                   std::optional<CondExpr> when_matched_delete_cond, std::optional<std::vector<std::string>> when_not_matched_columns,
                                   std::vector<std::string> when_not_matched_values) {
    auto sit = s.tables.find(source);
    if (sit == s.tables.end()) return StringResult::Err("Table '" + source + "' not found");
    std::vector<Row> source_rows;
    for (auto& r : sit->second) {
        if (is_visible(r)) source_rows.push_back(r);
    }

    auto tit = s.tables.find(target);
    if (tit == s.tables.end()) return StringResult::Err("Table '" + target + "' not found");
    std::vector<Row> target_rows;
    for (auto& r : tit->second) {
        if (is_visible(r)) target_rows.push_back(r);
    }

    const TableSchema* schema0 = s.catalog.get_table(target);
    if (!schema0) return StringResult::Err("Table '" + target + "' not found");
    std::vector<std::string> pk_cols;
    for (auto& c : schema0->columns) {
        if (c.primary_key) pk_cols.push_back(c.name);
    }
    if (pk_cols.empty()) pk_cols.push_back("id");
    // Single PK column used for secondary/hash index maintenance below
    // (index_remove_row/index_insert_row and the PK B+Tree only ever key on one column --
    // same pre-existing composite-PK approximation already used by exec_multi_delete).
    std::string pk_col = pk_cols.front();
    // Composite identity key, same fix/reasoning as UPDATE (executor_update.cpp) and
    // multi-table UPDATE/DELETE (executor_multi.cpp) -- a composite-PK target table's
    // rows must be identified by ALL of its PK columns, not just the first, or two
    // distinct rows sharing the leading column's value get conflated (MERGE could then
    // update/delete the wrong physical row, or delete both).
    auto row_key = [&pk_cols](const Row& r) {
        std::string key;
        for (std::size_t i = 0; i < pk_cols.size(); i++) {
            if (i) key += '\x00';
            auto it = r.find(pk_cols[i]);
            key += (it != r.end() ? it->second : std::string());
        }
        return key;
    };
    std::vector<std::string> target_col_names;
    for (auto& c : schema0->columns) target_col_names.push_back(c.name);

    std::string target_base = bare_name(target);
    std::string source_base = bare_name(source);

    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> update_rows;
    std::vector<std::string> delete_pks;
    std::vector<Row> insert_rows;

    for (auto& src_row : source_rows) {
        bool found = false;
        for (auto& tgt_row : target_rows) {
            Row merged = tgt_row;
            for (auto& [k, v] : tgt_row) merged[target_base + "." + k] = v;
            if (target_alias) {
                for (auto& [k, v] : tgt_row) merged[*target_alias + "." + k] = v;
            }
            for (auto& [k, v] : src_row) {
                if (!merged.count(k)) merged[k] = v;
                merged[source_base + "." + k] = v;
            }
            if (source_alias) {
                for (auto& [k, v] : src_row) merged[*source_alias + "." + k] = v;
            }

            if (eval_condexpr(merged, on)) {
                std::string pk = row_key(tgt_row);
                found = true;
                bool delete_cond_ok = when_matched_delete_cond ? eval_condexpr(merged, *when_matched_delete_cond) : true;
                if (when_matched_delete && delete_cond_ok) {
                    delete_pks.push_back(pk);
                } else if (when_matched_update) {
                    std::vector<std::pair<std::string, std::string>> resolved;
                    for (auto& [col, expr] : *when_matched_update) resolved.emplace_back(col, eval_arith(merged, expr));
                    update_rows.emplace_back(pk, std::move(resolved));
                }
                break;
            }
        }
        if (!found && !when_not_matched_values.empty()) {
            std::vector<std::string> cols = when_not_matched_columns.value_or(target_col_names);
            Row row;
            for (std::size_t i = 0; i < cols.size(); i++) {
                std::string raw = i < when_not_matched_values.size() ? when_not_matched_values[i] : std::string();
                std::string value;
                if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
                    value = trim_quotes(raw);
                } else if (auto it = src_row.find(raw); it != src_row.end()) {
                    value = it->second;
                } else if (auto dot = raw.find('.'); dot != std::string::npos) {
                    std::string col_part = raw.substr(dot + 1);
                    auto it2 = src_row.find(col_part);
                    value = it2 != src_row.end() ? it2->second : trim_quotes(raw);
                } else {
                    value = trim_quotes(raw);
                }
                row[cols[i]] = value;
            }
            if (const auto* schema = s.catalog.get_table(target)) {
                for (auto& col_def : schema->columns) {
                    if (!row.count(col_def.name) && !col_def.auto_increment) {
                        if (col_def.default_value) row[col_def.name] = *col_def.default_value;
                    }
                }
            }
            insert_rows.push_back(std::move(row));
        }
    }

    std::size_t update_count = update_rows.size();
    std::size_t delete_count = delete_pks.size();
    std::size_t insert_count = insert_rows.size();

    if (auto rit = s.tables.find(target); rit != s.tables.end()) {
        auto& rows = rit->second;
        // PLAN.md P2 fix: MERGE previously touched zero index kinds at all for UPDATE/
        // DELETE (INSERT below had the same gap) -- mirror the remove-old/insert-new
        // pattern already used by plain UPDATE/multi-table UPDATE/DELETE.
        for (auto& [pk, resolved] : update_rows) {
            for (auto& row : rows) {
                if (row_key(row) == pk && is_visible(row)) {
                    Row old_row = row;
                    for (auto& [col, val] : resolved) row[col] = val;
                    if (auto idx_it = s.indexes.find(target); idx_it != s.indexes.end()) {
                        auto old_it = old_row.find(pk_col);
                        idx_it->second.remove(old_it != old_row.end() ? old_it->second : std::string());
                        auto new_it = row.find(pk_col);
                        nlohmann::json j = row;
                        idx_it->second.insert(new_it != row.end() ? new_it->second : std::string(), j.dump());
                    }
                    index_remove_row(s, target, old_row, pk_col);
                    index_insert_row(s, target, row);
                    for (auto& [k, ci] : s.composite_indexes) {
                        if (ci.table != target) continue;
                        ci.remove_row(old_row);
                        ci.insert_row(row);
                    }
                    break;
                }
            }
        }

        std::vector<Row> rows_to_delete;
        for (auto& r : rows) {
            if (std::find(delete_pks.begin(), delete_pks.end(), row_key(r)) != delete_pks.end()) rows_to_delete.push_back(r);
        }
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                   [&](const Row& r) {
                                       return std::find(delete_pks.begin(), delete_pks.end(), row_key(r)) != delete_pks.end();
                                   }),
                   rows.end());
        if (auto idx_it = s.indexes.find(target); idx_it != s.indexes.end()) {
            for (auto& row : rows_to_delete) {
                auto it = row.find(pk_col);
                idx_it->second.remove(it != row.end() ? it->second : std::string());
            }
        }
        for (auto& row : rows_to_delete) index_remove_row(s, target, row, pk_col);
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table != target) continue;
            for (auto& row : rows_to_delete) ci.remove_row(row);
        }
    }

    {
        std::vector<std::pair<std::string, bool>> ai_cols;
        if (const auto* sc = s.catalog.get_table(target)) {
            for (auto& c : sc->columns) ai_cols.emplace_back(c.name, c.auto_increment);
        }
        std::unordered_map<std::string, std::int64_t> local_counters;
        if (const auto* sc = s.catalog.get_table(target)) local_counters = sc->auto_increment_counters;

        for (auto& [col_name, is_ai] : ai_cols) {
            if (is_ai && !local_counters.count(col_name)) {
                std::int64_t max_id = 0;
                if (auto it = s.tables.find(target); it != s.tables.end()) {
                    for (auto& r : it->second) {
                        if (!is_visible(r)) continue;
                        auto cit = r.find(col_name);
                        if (cit == r.end()) continue;
                        try {
                            std::size_t pos;
                            std::int64_t v = std::stoll(cit->second, &pos);
                            if (pos == cit->second.size()) max_id = std::max(max_id, v);
                        } catch (...) {
                        }
                    }
                }
                local_counters[col_name] = max_id;
            }
        }

        std::string txn_id = std::to_string(txn.current_txn_id());
        if (auto it = s.tables.find(target); it != s.tables.end()) {
            for (auto& row : insert_rows) {
                for (auto& [col_name, is_ai] : ai_cols) {
                    if (is_ai && (!row.count(col_name) || row[col_name].empty())) {
                        std::int64_t& counter = local_counters[col_name];
                        counter += 1;
                        row[col_name] = std::to_string(counter);
                    }
                }
                if (!row.count("_xmin")) row["_xmin"] = txn_id;
                if (!row.count("_xmax")) row["_xmax"] = "0";
                // PLAN.md P2 fix: MERGE's INSERT branch previously touched zero index
                // kinds at all -- index while `row` is still valid, before the move below.
                if (auto idx_it = s.indexes.find(target); idx_it != s.indexes.end()) {
                    auto pk_it = row.find(pk_col);
                    nlohmann::json j = row;
                    idx_it->second.insert(pk_it != row.end() ? pk_it->second : std::string(), j.dump());
                }
                index_insert_row(s, target, row);
                for (auto& [k, ci] : s.composite_indexes) {
                    if (ci.table == target) ci.insert_row(row);
                }
                it->second.push_back(std::move(row));
            }
        }
        if (auto* ts = s.catalog.get_table_mut(target)) ts->auto_increment_counters = local_counters;
    }

    if (!txn.is_active()) {
        if (auto it = s.tables.find(target); it != s.tables.end()) {
            std::vector<Row> rows_clone = it->second;
            s.buffer_pool.write_page(target, rows_clone);
            s.buffer_pool.flush_page(target, s.disk);
        }
    }

    return StringResult::Ok("MERGE: " + std::to_string(update_count) + " updated, " + std::to_string(delete_count) + " deleted, " +
                             std::to_string(insert_count) + " inserted.");
}

} // namespace engine
