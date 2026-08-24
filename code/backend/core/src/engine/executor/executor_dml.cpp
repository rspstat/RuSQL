// Faithful port of the INSERT path and its shared DML infrastructure from
// rusql-core/src/engine/executor.rs (Phase 8b): fire_triggers,
// maybe_auto_checkpoint/maybe_auto_vacuum/maybe_auto_analyze, resolve_updatable_view,
// exec_insert_select, exec_insert, exec_insert_inner.
// MVCC Stage 2: session_swap_in/out (the private per-session table buffer this file used
// to swap into/out of s.tables around in-transaction DML) were retired -- all DML now
// writes directly into s.tables, tagged with the writer's real txn id (see _xmin/_xmax
// and Executor::is_visible_for_read), so no swap is needed for a transaction to see its
// own uncommitted work.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {

std::string current_timestamp_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string join_quoted(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); i++) {
        if (i) out += ", ";
        out += "\"" + values[i] + "\"";
    }
    return out;
}

std::string trim_ws(const std::string& s) {
    auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

// A trigger body's own DML can fire further triggers (directly, or via a chain through
// another table) with no natural termination guarantee -- cap the depth so a runaway
// trigger chain fails loudly instead of recursing until stack overflow while holding
// SharedDatabase's exclusive write lock for the whole time.
constexpr std::size_t TRIGGER_MAX_DEPTH = 32;

struct TriggerDepthGuard {
    std::size_t& depth;
    explicit TriggerDepthGuard(std::size_t& d) : depth(d) { depth++; }
    ~TriggerDepthGuard() { depth--; }
};

} // namespace

StringResult Executor::fire_triggers(SharedDatabase& s, const std::string& table, const std::string& timing, const std::string& event) {
    if (trigger_depth_ >= TRIGGER_MAX_DEPTH) {
        return StringResult::Err("Trigger recursion exceeded maximum depth (" + std::to_string(TRIGGER_MAX_DEPTH) + ")");
    }
    std::vector<std::vector<Statement>> bodies;
    for (auto& [name, def] : s.triggers) {
        auto& [t, ti, ev, body] = def;
        if (t == table && ti == timing && ev == event) bodies.push_back(body);
    }
    TriggerDepthGuard guard(trigger_depth_);
    for (auto& body : bodies) {
        for (auto& stmt : body) execute_with_s(s, stmt);
    }
    return StringResult::Ok("");
}

void Executor::maybe_auto_checkpoint(SharedDatabase& s) {
    if (txn.needs_auto_checkpoint()) {
        s.buffer_pool.flush_all(s.disk);
        bool safe = s.active_txn_ids->lock()->empty();
        txn.do_checkpoint(safe);
    }
}

void Executor::maybe_auto_vacuum(SharedDatabase& s, const std::string& table) {
    constexpr std::size_t AUTO_VACUUM_THRESHOLD = 200;
    // Stage 4: single-table scoped (was a global counter sweeping every table in the DB)
    // -- under per-table locking, this call only ever holds `table`'s own lock, so it must
    // never touch any other table's rows/index/buffer-pool page. Mirrors dml_since_analyze/
    // maybe_auto_analyze's existing per-table pattern.
    std::size_t& counter = s.dml_since_vacuum[table];
    counter += 1;
    if (counter < AUTO_VACUUM_THRESHOLD) return;
    counter = 0;

    auto tit = s.tables.find(table);
    if (tit == s.tables.end()) return;
    std::uint64_t horizon = oldest_active_txn_id(s);
    auto& rows = tit->second;
    std::size_t before = rows.size();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return is_vacuumable(r, horizon); }), rows.end());
    if (rows.size() < before) {
        std::vector<Row> rows_clone = rows;
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            idx_it->second = BPlusTree();
            // PLAN.md P0 fix: see exec_vacuum in executor_maint.cpp for the same fix —
            // resolve the real PK column from the schema instead of grabbing an
            // arbitrary (HashMap-order-dependent) row value.
            std::string pk_col_name;
            if (auto* schema = s.catalog.get_table(table)) {
                for (auto& c : schema->columns) {
                    if (c.primary_key) { pk_col_name = c.name; break; }
                }
                if (pk_col_name.empty() && !schema->columns.empty()) pk_col_name = schema->columns.front().name;
            }
            for (auto& row : rows_clone) {
                auto it = row.find(pk_col_name);
                std::string key = it != row.end() ? it->second : std::string();
                nlohmann::json j = row;
                idx_it->second.insert(key, j.dump());
            }
            s.disk.save_btree_index(table, idx_it->second);
        }
        s.buffer_pool.write_page(table, rows_clone);
        s.buffer_pool.flush_page(table, s.disk);
    }
}

void Executor::maybe_auto_analyze(SharedDatabase& s, const std::string& table) {
    std::size_t total = s.table_stats.count(table) ? s.table_stats.at(table).total_rows : 0;
    std::size_t threshold;
    if (total == 0) threshold = 100;
    else if (total < 10000) threshold = std::max<std::size_t>(total / 10, 50);
    else if (total < 1000000) threshold = std::max<std::size_t>(total / 50, 1000);
    else threshold = std::max<std::size_t>(total / 200, 10000);

    std::size_t current = ++s.dml_since_analyze[table];
    if (current < threshold) return;
    s.dml_since_analyze[table] = 0;
    (void)exec_analyze_table(s, table);
}

std::optional<std::pair<std::string, std::optional<CondExpr>>> Executor::resolve_updatable_view(const SharedDatabase& s,
                                                                                                   const std::string& name) {
    auto it = s.views.find(name);
    if (it == s.views.end()) return std::nullopt;
    if (auto* sel = std::get_if<Statement::Select>(&it->second.data)) {
        bool group_by_empty = !sel->group_by || sel->group_by->empty();
        if (sel->joins.empty() && !sel->distinct && group_by_empty && !sel->subquery) {
            return std::make_pair(sel->table, sel->condition);
        }
    }
    return std::nullopt;
}

StringResult Executor::exec_insert_select(SharedDatabase& s, std::string table, std::optional<std::vector<std::string>> columns,
                                           Statement query, InsertConflict on_conflict, std::optional<std::vector<SelectColumn>> returning) {
    // Row-level-concurrency Stage 4: `query`'s own tables are already covered by
    // table_lock_set_for's InsertSelect branch at the table_locks (SHARED) level, but
    // table_data_locks is deliberately NOT acquired for InsertSelect at that dispatch
    // level (see execute()'s comment) -- exec_insert_inner below needs to escalate the
    // TARGET table to EXCLUSIVE for its own brief push_back phase, which would deadlock
    // against a shared table_data_locks hold taken here on the same thread if it
    // included the target table. So: acquire table_data_locks SHARED for exactly the
    // SOURCE query's table closure (table_lock_set_for recurses through any WHERE/
    // SELECT-list subqueries in `query` too, same as a top-level Select) for the
    // duration of running it, protecting the read scan from a concurrent writer
    // resizing the same vector -- then release before exec_insert_inner runs.
    {
        DataLockGuard src_data_guard;
        if (auto src_tables = table_lock_set_for(s, query)) {
            src_data_guard = acquire_table_data_locks(s, *src_tables, /*exclusive=*/false);
        }
        auto output = execute_with_s(s, std::move(query));
        if (output.is_err()) return output;
        auto [col_names, rows] = parse_table_output(output.value());
        if (rows.empty()) return StringResult::Ok("0 row(s) inserted.");

        std::vector<std::vector<std::string>> all_values;
        all_values.reserve(rows.size());
        for (auto& row : rows) {
            std::vector<std::string> vals;
            vals.reserve(col_names.size());
            for (auto& c : col_names) {
                auto it = row.find(c);
                vals.push_back(it != row.end() ? it->second : std::string());
            }
            all_values.push_back(std::move(vals));
        }
        auto insert_cols = columns ? columns : std::optional<std::vector<std::string>>(col_names);
        return exec_insert(s, table, insert_cols, std::move(all_values), on_conflict, returning);
    }
}

StringResult Executor::exec_insert(SharedDatabase& s, std::string table, std::optional<std::vector<std::string>> col_list,
                                    std::vector<std::vector<std::string>> all_values, InsertConflict on_conflict,
                                    std::optional<std::vector<SelectColumn>> returning) {
    if (s.views.count(table)) {
        if (auto resolved = resolve_updatable_view(s, table)) {
            return exec_insert(s, resolved->first, col_list, std::move(all_values), on_conflict, returning);
        }
        return StringResult::Err("View '" + strip_db_prefix(table) + "' is not updatable (has JOINs, DISTINCT, GROUP BY, or subquery)");
    }

    if (auto tr = fire_triggers(s, table, "BEFORE", "INSERT"); tr.is_err()) return tr;
    auto result = exec_insert_inner(s, table, col_list, std::move(all_values), on_conflict, returning);

    if (result.is_ok()) {
        if (auto tr = fire_triggers(s, table, "AFTER", "INSERT"); tr.is_err()) return tr;
    }
    return result;
}

StringResult Executor::exec_insert_inner(SharedDatabase& s, const std::string& table, const std::optional<std::vector<std::string>>& col_list,
                                          std::vector<std::vector<std::string>> all_values, const InsertConflict& on_conflict,
                                          const std::optional<std::vector<SelectColumn>>& returning) {
    const TableSchema* schema_ptr = s.catalog.get_table(table);
    if (!schema_ptr) return StringResult::Err("Table '" + table + "' not found");
    TableSchema schema = *schema_ptr;

    if (col_list) {
        for (auto& col : *col_list) {
            bool found = std::any_of(schema.columns.begin(), schema.columns.end(), [&](const ColumnDef& c) { return c.name == col; });
            if (!found) return StringResult::Err("Column '" + col + "' not found in table '" + table + "'");
        }
    }

    std::vector<std::string> col_names;
    for (auto& c : schema.columns) col_names.push_back(c.name);
    struct ColConstraint { bool primary_key, not_null, unique, auto_increment; };
    std::vector<ColConstraint> constraints;
    for (auto& c : schema.columns) constraints.push_back({c.primary_key, c.not_null, c.unique, c.auto_increment});

    // Row-level-concurrency prep: previously this took a stack COPY of the counters,
    // mutated it locally across the whole batch, and wrote it back once at the end
    // (schema_mut->auto_increment_counters = local_counters below) -- safe only because
    // the caller held the whole table exclusively for the statement's duration. Two
    // concurrent INSERTs into the same table would each copy the same starting value,
    // each independently compute the same "next" value, and both write it back --
    // duplicate AUTO_INCREMENT values, silently. Fixed by allocating directly against
    // the real Catalog entry, immediately, per row that actually needs one.
    TableSchema* schema_mut_for_ai = s.catalog.get_table_mut(table);
    bool any_auto_increment_allocated = false;

    std::vector<std::vector<std::pair<std::size_t, std::string>>> seen_unique;
    std::vector<std::vector<std::string>> seen_composite_pk;
    std::vector<Row> prepared;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, ArithExpr>>>> pending_updates;

    // Gap lock conflict check needs the single-column PK's name (V1 scope, matching the
    // FOR UPDATE/FOR SHARE/UPDATE/DELETE acquisition sites). Note: schema.primary_key_columns
    // only reflects a table-level composite PRIMARY KEY (col1, col2) constraint -- an inline
    // `id INT PRIMARY KEY` column only sets col.primary_key, so PK columns must be counted
    // this way rather than via that field.
    std::string gap_pk_col;
    std::size_t gap_pk_col_count = 0;
    for (auto& col : schema.columns) {
        if (col.primary_key) {
            if (gap_pk_col_count == 0) gap_pk_col = col.name;
            gap_pk_col_count++;
        }
    }

    // Row-level-concurrency Stage 4: one claim-id per STATEMENT (not per row) -- reuses
    // the active explicit transaction's id (claims released later by its real COMMIT/
    // ROLLBACK) or, for autocommit (no natural release point of its own), a fresh
    // one-off id released by RowClaimGuard's destructor on every exit path. Closes a
    // real TOCTOU: without this, two concurrent INSERTs could each pass the PK/UNIQUE
    // duplicate check below (against the state as of when each one scanned) before
    // either had actually written its row, and both would proceed to insert the "same"
    // supposedly-unique value.
    std::uint64_t cur_txn = txn.current_txn_id();
    bool autocommit_claim = (cur_txn == 0);
    std::uint64_t claim_txn_id = autocommit_claim ? s.txn_io->next_id() : cur_txn;
    RowClaimGuard row_claim_guard(s.lock_mgr, claim_txn_id, /*owns=*/autocommit_claim);
    // Real-blocking-wait stage: one deadline for the WHOLE statement (computed once, not
    // reset per retry) -- every block_on_row() call below shares this budget so repeated
    // conflicts can't add up to far more than @lock_wait_timeout actually allows.
    auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(lock_wait_timeout_ms);

    // Row-level-concurrency Stage 4: table_data_locks SHARED for `table` itself (the
    // duplicate-check scan below only ever reads s.tables[table], never resizes it) plus
    // every FK parent this schema references (the existence check further below reads
    // THEIR s.tables too). Held for the whole read/validate phase, released before the
    // write phase below escalates `table` alone to EXCLUSIVE.
    std::vector<std::string> insert_read_tables{table};
    for (auto& col : schema.columns) {
        if (!col.foreign_key) continue;
        // Table partitioning: lock the ref_table's actual children (where the existence
        // check below really reads), not its permanently-empty phantom logical name.
        if (auto part_info = partition_info_for(s, col.foreign_key->ref_table)) {
            for (auto& def : part_info->partitions) insert_read_tables.push_back(def.child_table);
        } else {
            insert_read_tables.push_back(col.foreign_key->ref_table);
        }
    }
    bool had_updates = false;
    std::vector<Row> updated_rows;
    {
        auto insert_read_lock = acquire_table_data_locks(s, insert_read_tables, /*exclusive=*/false);

    for (auto& values : all_values) {
        std::vector<std::string> positional;
        if (!col_list) {
            if (values.size() != schema.columns.size()) {
                return StringResult::Err("Column count mismatch: expected " + std::to_string(schema.columns.size()) + ", got " +
                                          std::to_string(values.size()));
            }
            positional = std::move(values);
        } else {
            if (col_list->size() != values.size()) {
                return StringResult::Err("Column list length " + std::to_string(col_list->size()) + " doesn't match value count " +
                                          std::to_string(values.size()));
            }
            std::unordered_map<std::string, std::string> col_map;
            for (std::size_t i = 0; i < col_list->size(); i++) col_map[(*col_list)[i]] = values[i];
            for (auto& c : schema.columns) {
                auto it = col_map.find(c.name);
                positional.push_back(it != col_map.end() ? it->second : std::string());
            }
        }

        std::vector<std::string> final_values = positional;

        for (std::size_t i = 0; i < schema.columns.size(); i++) {
            if (!final_values[i].empty()) continue;
            auto& col = schema.columns[i];
            if (col.default_value) {
                if (*col.default_value == NULL_DEFAULT) {
                    final_values[i] = EXECUTOR_NULL_VALUE;
                } else {
                    std::string upper = *col.default_value;
                    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
                    final_values[i] = (upper == "NOW()" || upper == "CURRENT_TIMESTAMP") ? current_timestamp_string() : *col.default_value;
                }
            } else if (std::holds_alternative<DataType::Timestamp>(col.data_type.data)) {
                final_values[i] = current_timestamp_string();
            }
        }

        for (std::size_t i = 0; i < constraints.size(); i++) {
            if (constraints[i].auto_increment && final_values[i].empty()) {
                auto& counter = schema_mut_for_ai->auto_increment_counters[col_names[i]];
                counter += 1;
                final_values[i] = std::to_string(counter);
                any_auto_increment_allocated = true;
            }
        }

        for (std::size_t i = 0; i < constraints.size(); i++) {
            if (constraints[i].not_null && (final_values[i].empty() || final_values[i] == EXECUTOR_NULL_VALUE)) {
                return StringResult::Err("Column '" + col_names[i] + "' cannot be NULL");
            }
        }

        for (std::size_t i = 0; i < schema.columns.size(); i++) {
            auto& col = schema.columns[i];
            const std::string& val = final_values[i];
            if (val.empty() || val == EXECUTOR_NULL_VALUE) continue;
            if (auto* en = std::get_if<DataType::Enum>(&col.data_type.data)) {
                if (std::find(en->values.begin(), en->values.end(), val) == en->values.end()) {
                    std::vector<std::string> quoted;
                    for (auto& a : en->values) quoted.push_back("'" + a + "'");
                    std::string allowed;
                    for (std::size_t k = 0; k < quoted.size(); k++) { if (k) allowed += ", "; allowed += quoted[k]; }
                    return StringResult::Err("Invalid ENUM value '" + val + "' for column '" + col.name + "'. Allowed: " + allowed);
                }
            } else if (auto* se = std::get_if<DataType::Set>(&col.data_type.data)) {
                std::size_t start = 0;
                while (true) {
                    auto comma = val.find(',', start);
                    std::string part = trim_ws(val.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                    if (!part.empty() && std::find(se->values.begin(), se->values.end(), part) == se->values.end()) {
                        std::vector<std::string> quoted;
                        for (auto& a : se->values) quoted.push_back("'" + a + "'");
                        std::string allowed;
                        for (std::size_t k = 0; k < quoted.size(); k++) { if (k) allowed += ", "; allowed += quoted[k]; }
                        return StringResult::Err("Invalid SET value '" + part + "' for column '" + col.name + "'. Allowed: " + allowed);
                    }
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
        }

        {
            std::vector<std::string> pk_cols = schema.primary_key_columns;
            bool is_composite_pk = pk_cols.size() > 1;
            auto tit = s.tables.find(table);
            bool skip_row = false;
            if (tit != s.tables.end()) {
                auto& rows = tit->second;
                if (is_composite_pk) {
                    std::vector<std::string> new_pk_tuple;
                    for (auto& pk : pk_cols) {
                        auto pos = std::find(col_names.begin(), col_names.end(), pk);
                        new_pk_tuple.push_back(pos != col_names.end() ? final_values[static_cast<std::size_t>(pos - col_names.begin())]
                                                                       : std::string());
                    }
                    for (auto& existing : rows) {
                        if (!is_visible(existing)) continue;
                        std::vector<std::string> existing_tuple;
                        for (auto& pk : pk_cols) {
                            auto it = existing.find(pk);
                            existing_tuple.push_back(it != existing.end() ? it->second : std::string());
                        }
                        if (existing_tuple != new_pk_tuple) continue;
                        if (std::holds_alternative<InsertConflict::Abort>(on_conflict.data)) {
                            return StringResult::Err("Duplicate composite primary key (" + join_quoted(new_pk_tuple) + ")");
                        }
                        if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                            skip_row = true;
                            break;
                        }
                        if (auto* upd = std::get_if<InsertConflict::Update>(&on_conflict.data)) {
                            auto pkit = existing.find(col_names[0]);
                            pending_updates.emplace_back(pkit != existing.end() ? pkit->second : std::string(), upd->assignments);
                            skip_row = true;
                            break;
                        }
                    }
                } else {
                    for (std::size_t i = 0; i < constraints.size() && !skip_row; i++) {
                        if (!constraints[i].primary_key && !constraints[i].unique) continue;
                        const std::string& val = final_values[i];
                        std::optional<Row> existing;
                        if (constraints[i].primary_key) {
                            if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
                                if (auto j = idx_it->second.search(val); j && !j->empty()) {
                                    try {
                                        Row r = nlohmann::json::parse(*j).get<Row>();
                                        if (is_visible(r)) existing = std::move(r);
                                    } catch (...) {
                                    }
                                }
                            }
                        } else {
                            std::string hi_key;
                            for (auto& [name, meta] : s.hash_index_meta) {
                                if (meta.first == table && meta.second == col_names[i]) {
                                    hi_key = table + "_" + name;
                                    break;
                                }
                            }
                            if (!hi_key.empty()) {
                                if (auto hit = s.hash_indexes.find(hi_key); hit != s.hash_indexes.end()) {
                                    const auto& bucket = hit->second.get(val);
                                    if (!bucket.empty() && is_visible(bucket.front())) existing = bucket.front();
                                }
                            } else {
                                for (auto& r : rows) {
                                    if (!is_visible(r)) continue;
                                    auto it = r.find(col_names[i]);
                                    if (it != r.end() && it->second == val) {
                                        existing = r;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!existing) continue;
                        if (std::holds_alternative<InsertConflict::Abort>(on_conflict.data)) {
                            return StringResult::Err("Duplicate value '" + val + "' for column '" + col_names[i] + "'");
                        }
                        if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                            skip_row = true;
                        } else if (auto* upd = std::get_if<InsertConflict::Update>(&on_conflict.data)) {
                            auto pkit = existing->find(col_names[0]);
                            pending_updates.emplace_back(pkit != existing->end() ? pkit->second : std::string(), upd->assignments);
                            skip_row = true;
                        }
                    }
                }
            }
            if (skip_row) continue;
        }

        {
            std::vector<std::string> pk_cols_batch = schema.primary_key_columns;
            bool is_composite_batch = pk_cols_batch.size() > 1;
            if (is_composite_batch) {
                std::vector<std::string> new_pk_tuple;
                for (auto& pk : pk_cols_batch) {
                    auto pos = std::find(col_names.begin(), col_names.end(), pk);
                    new_pk_tuple.push_back(pos != col_names.end() ? final_values[static_cast<std::size_t>(pos - col_names.begin())]
                                                                   : std::string());
                }
                for (auto& prev : seen_composite_pk) {
                    if (prev == new_pk_tuple) return StringResult::Err("Duplicate composite primary key (" + join_quoted(new_pk_tuple) + ")");
                }
                seen_composite_pk.push_back(std::move(new_pk_tuple));
            } else {
                std::vector<std::pair<std::size_t, std::string>> this_row_unique;
                for (std::size_t i = 0; i < constraints.size(); i++) {
                    if (constraints[i].primary_key || constraints[i].unique) this_row_unique.emplace_back(i, final_values[i]);
                }
                for (auto& prev : seen_unique) {
                    for (auto& [i, val] : this_row_unique) {
                        for (auto& [pi, pv] : prev) {
                            if (pi == i && pv == val) return StringResult::Err("Duplicate value '" + val + "' for column '" + col_names[i] + "'");
                        }
                    }
                }
                seen_unique.push_back(std::move(this_row_unique));
            }
        }

        Row row;
        for (std::size_t i = 0; i < col_names.size(); i++) row[col_names[i]] = final_values[i].empty() ? EXECUTOR_NULL_VALUE : final_values[i];
        row["_xmin"] = std::to_string(tagging_txn_id(s));
        row["_xmax"] = "0";

        for (auto& col : schema.columns) {
            if (skip_fk_checks) break; // exec_restore()만 사용 -- executor.hpp의 필드 주석 참고
            if (!col.foreign_key) continue;
            auto it = row.find(col.name);
            std::string val = it != row.end() ? it->second : std::string();
            if (val.empty() || val == EXECUTOR_NULL_VALUE) continue;
            // Table partitioning: a partitioned ref_table's own s.tables[...] entry is a
            // permanently-empty phantom (exec_create/executor_partition.cpp) -- scan its
            // children instead, so an FK into a partitioned parent validates correctly
            // instead of always reporting "not found".
            std::vector<const std::vector<Row>*> ref_row_sets;
            if (auto part_info = partition_info_for(s, col.foreign_key->ref_table)) {
                for (auto& def : part_info->partitions) {
                    if (auto child_it = s.tables.find(def.child_table); child_it != s.tables.end()) ref_row_sets.push_back(&child_it->second);
                }
            } else {
                auto ref_it = s.tables.find(col.foreign_key->ref_table);
                if (ref_it == s.tables.end()) return StringResult::Err("Referenced table '" + col.foreign_key->ref_table + "' not found");
                ref_row_sets.push_back(&ref_it->second);
            }
            bool exists = std::any_of(ref_row_sets.begin(), ref_row_sets.end(), [&](const std::vector<Row>* rows) {
                return std::any_of(rows->begin(), rows->end(), [&](const Row& r) {
                    auto rit = r.find(col.foreign_key->ref_column);
                    return rit != r.end() && rit->second == val;
                });
            });
            if (!exists) {
                return StringResult::Err("Foreign key violation: '" + val + "' not found in '" + col.foreign_key->ref_table + "'.'" +
                                          col.foreign_key->ref_column + "'");
            }
        }

        for (auto& col : schema.columns) {
            if (col.check_expr && !eval_check_expr(*col.check_expr, row)) {
                return StringResult::Err("CHECK constraint violated on column '" + col.name + "': " + *col.check_expr);
            }
        }
        for (auto& check : schema.check_constraints) {
            if (!eval_check_expr(check.expression, row)) {
                return StringResult::Err("CHECK constraint '" + check.name.value_or(check.expression) + "' violated");
            }
        }

        // Gap lock conflict check (InnoDB-style phantom-read prevention): applies
        // regardless of THIS transaction's own isolation level/state -- a gap lock
        // protects its holder, not the inserter (see gap_lock.cpp).
        if (gap_pk_col_count == 1) {
            auto pk_it = row.find(gap_pk_col);
            if (pk_it != row.end()) {
                std::uint64_t my_txn_id = txn.current_txn_id();
                // Real-blocking-wait stage: retry-capable outer loop -- each iteration
                // rescans gap_locks_for(table) from scratch (a prior block may have let
                // ONE holder's gap release while others still conflict, or a brand-new gap
                // lock may have appeared) and stops as soon as no conflict remains.
                for (;;) {
                    bool conflict_found = false;
                    std::uint64_t conflict_holder = 0;
                    for (auto& g : s.lock_mgr.gap_locks_for(table)) {
                        if (g.holder == my_txn_id) continue; // a txn's own gap never blocks its own INSERT
                        GapRange range{g.lo, g.hi, g.lo_inclusive, g.hi_inclusive};
                        if (!gap_range_contains(range, pk_it->second)) continue;
                        conflict_found = true;
                        conflict_holder = g.holder;
                        break;
                    }
                    if (!conflict_found) break;
                    LockResult lr = s.lock_mgr.register_gap_conflict(table, my_txn_id, conflict_holder);
                    if (lr.kind == LockResult::Kind::Deadlock) {
                        return StringResult::Err("Deadlock detected: transaction " + std::to_string(my_txn_id) + " waits for transaction " +
                                                  std::to_string(lr.holder) + " (INSERT '" + table + "'). Transaction " +
                                                  std::to_string(my_txn_id) + " aborted.");
                    }
                    // Kind::Conflict (the only other outcome at timeout=0): release both
                    // dispatcher-owned guards -- must never block while holding either, see
                    // release_table_locks_for_block's doc comment -- block on just this
                    // holder's gap lock releasing, then reacquire before touching `s` again.
                    insert_read_lock = DataLockGuard{};
                    release_table_locks_for_block();
                    auto now = std::chrono::steady_clock::now();
                    auto remaining =
                        now < lock_deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(lock_deadline - now) : std::chrono::milliseconds{0};
                    LockResult lr2 = s.lock_mgr.register_gap_conflict(table, my_txn_id, conflict_holder, remaining);
                    reacquire_table_locks_after_block();
                    insert_read_lock = acquire_table_data_locks(s, insert_read_tables, /*exclusive=*/false);
                    if (lr2.kind == LockResult::Kind::Deadlock) {
                        return StringResult::Err("Deadlock detected: transaction " + std::to_string(my_txn_id) + " waits for transaction " +
                                                  std::to_string(lr2.holder) + " (INSERT '" + table + "'). Transaction " +
                                                  std::to_string(my_txn_id) + " aborted.");
                    }
                    if (lr2.kind != LockResult::Kind::Granted) {
                        return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; value '" + pk_it->second + "' for '" + table +
                                                  "'.'" + gap_pk_col + "' falls within a gap lock held by transaction " +
                                                  std::to_string(lr2.holder) + ". Cannot INSERT.");
                    }
                    // else: granted -- loop back and rescan gap_locks_for(table) from scratch.
                }

                // Row-level-concurrency Stage 4: claim this PK value now, under the SAME
                // claim_txn_id for the whole statement -- a concurrent INSERT racing the
                // identical value (which may have passed ITS OWN duplicate check moments
                // ago, before either writer has actually pushed a row) gets told apart
                // here instead of both silently proceeding.
                //
                // Real-blocking-wait stage: on conflict, release insert_read_lock first
                // (must never block while holding table_data_locks -- see block_on_row's
                // doc comment) and block on just this one row. Once granted, re-validate:
                // whoever we were waiting on may have COMMITTED the exact value we want
                // while we slept -- a genuine duplicate now, which the duplicate-check
                // earlier in this same loop iteration ran too soon to have seen.
                LockResult lr = s.lock_mgr.acquire(table, pk_it->second, claim_txn_id);
                if (lr.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                              std::to_string(lr.holder) + " (INSERT '" + table + "'). Transaction " +
                                              std::to_string(claim_txn_id) + " aborted.");
                }
                if (lr.kind == LockResult::Kind::Conflict) {
                    insert_read_lock = DataLockGuard{}; // release -- must not block while holding it
                    // Real-blocking-wait stage, second correctness fix: table_locks[table]
                    // (SHARED, held for this whole statement by execute()'s dispatcher)
                    // must ALSO be released before blocking, or a concurrent COMMIT
                    // needing table_locks EXCLUSIVE on this table can deadlock against us
                    // -- see release_table_locks_for_block's doc comment in executor.hpp.
                    release_table_locks_for_block();
                    lr = block_on_row(s.lock_mgr, table, pk_it->second, claim_txn_id, /*exclusive=*/true, lock_deadline);
                    reacquire_table_locks_after_block();
                    insert_read_lock = acquire_table_data_locks(s, insert_read_tables, /*exclusive=*/false);
                    if (lr.kind == LockResult::Kind::Deadlock) {
                        return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                                  std::to_string(lr.holder) + " (INSERT '" + table + "'). Transaction " +
                                                  std::to_string(claim_txn_id) + " aborted.");
                    }
                    if (lr.kind != LockResult::Kind::Granted) {
                        return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; value '" + pk_it->second + "' for '" + table +
                                                  "'.'" + gap_pk_col + "' is being concurrently inserted/updated by transaction " +
                                                  std::to_string(lr.holder) + ".");
                    }
                    if (auto tit = s.tables.find(table); tit != s.tables.end()) {
                        for (auto& r : tit->second) {
                            if (!is_visible(r)) continue;
                            auto rit = r.find(gap_pk_col);
                            if (rit != r.end() && rit->second == pk_it->second) {
                                return StringResult::Err("Duplicate value '" + pk_it->second + "' for column '" + gap_pk_col + "'");
                            }
                        }
                    }
                }
            }
        }

        prepared.push_back(std::move(row));
    }
    } // insert_read_lock (SHARED) released here -- ON DUPLICATE KEY UPDATE's in-place
      // mutation below needs EXCLUSIVE (see the correctness-fix comment), not SHARED.

    // Row-level-concurrency Stage 4 correctness fix (found via concurrent-reader
    // monotonicity stress testing): ON DUPLICATE KEY UPDATE mutates an EXISTING row in
    // place (`row[col] = ...` below) -- this is NOT safe under SHARED alone, since a
    // plain autocommit SELECT's scan/copy takes no LockManager claim at all and could
    // still hold table_data_locks SHARED at the same moment, racing this mutation at the
    // raw std::map level (undefined behavior). EXCLUSIVE is required for any in-place
    // mutation of an existing row, matching exec_update_inner/exec_delete_inner. The
    // per-row LockManager claim below is still needed too, but only to keep a SECOND
    // WRITER from interleaving field-by-field with this one within the same statement's
    // EXCLUSIVE window (two different pk_val entries can't conflict, but it's cheap
    // insurance and matches the UPDATE path's own pattern).
    had_updates = !pending_updates.empty();
    if (had_updates) {
        auto insert_upsert_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        for (auto& [pk_val, assignments] : pending_updates) {
            LockResult lr = s.lock_mgr.acquire(table, pk_val, claim_txn_id);
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (INSERT ... ON DUPLICATE KEY UPDATE '" + table + "'). Transaction " +
                                          std::to_string(claim_txn_id) + " aborted.");
            }
            if (lr.kind == LockResult::Kind::Conflict) {
                // Real-blocking-wait stage: release insert_upsert_lock (EXCLUSIVE) before
                // blocking -- see block_on_row's doc comment. The row lookup right below
                // always runs fresh AFTER the (possibly blocked) claim is achieved, so no
                // separate re-validation is needed here: whatever this pk_val's row looks
                // like once we're granted is exactly what gets mutated.
                insert_upsert_lock = DataLockGuard{};
                // Real-blocking-wait stage, second correctness fix: also release
                // table_locks[table] before blocking -- see release_table_locks_for_
                // block's doc comment in executor.hpp.
                release_table_locks_for_block();
                lr = block_on_row(s.lock_mgr, table, pk_val, claim_txn_id, /*exclusive=*/true, lock_deadline);
                reacquire_table_locks_after_block();
                insert_upsert_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
                if (lr.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                              std::to_string(lr.holder) + " (INSERT ... ON DUPLICATE KEY UPDATE '" + table + "'). Transaction " +
                                              std::to_string(claim_txn_id) + " aborted.");
                }
                if (lr.kind != LockResult::Kind::Granted) {
                    return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + pk_val + "' in '" + table +
                                              "' is held by transaction " + std::to_string(lr.holder) + ". Cannot UPDATE.");
                }
            }
            if (auto it = s.tables.find(table); it != s.tables.end()) {
                for (auto& row : it->second) {
                    auto pkit = row.find(col_names[0]);
                    if (pkit != row.end() && pkit->second == pk_val && is_visible(row)) {
                        for (auto& [col, aexpr] : assignments) row[col] = eval_arith(row, aexpr);
                        updated_rows.push_back(row);
                        break;
                    }
                }
            }
        }
        // Row-level-concurrency Stage 4/5 correctness fix: see the identical
        // invalidate_table comment on the push_back path above -- must run before this
        // EXCLUSIVE lock releases, not later in execute_sql.
        s.query_cache.invalidate_table(table);
    }
    if (had_updates) {
        std::string pk_col_name = col_names.empty() ? std::string() : col_names[0];
        for (auto& c : schema.columns) {
            if (c.primary_key) {
                pk_col_name = c.name;
                break;
            }
        }
        // Incremental index maintenance: the PK value itself never changes here (rows
        // were matched by exact PK equality above), so each updated row is a same-key
        // upsert -- no need to clone and rebuild the whole table's PK index.
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            for (auto& row : updated_rows) {
                auto it = row.find(pk_col_name);
                std::string k = it != row.end() ? it->second : std::string();
                nlohmann::json j = row;
                idx_it->second.insert(k, j.dump());
            }
        }
    }

    // Counters were already allocated directly against the real Catalog entry above (as
    // each row needed one); only the disk persistence is still batched once per
    // statement here, matching the original single-save behavior.
    if (any_auto_increment_allocated) {
        s.disk.save_schema(table, *s.catalog.get_table(table));
    }

    std::size_t inserted = prepared.size();
    std::vector<Row> returning_rows = returning ? prepared : std::vector<Row>{};

    // Row-level-concurrency Stage 4: EXCLUSIVE only for this short tail -- the actual
    // vector-shape change (push_back) and the paired row_pk_pos[table] update, plus
    // maybe_auto_vacuum (which erases rows -- also shape-changing) and the
    // dml_since_vacuum/dml_since_analyze/table_stats counter touches. s.indexes/
    // s.composite_indexes are separately protected by their own per-instance mutex
    // (Stage 2), not by this lock.
    {
        auto insert_write_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        for (auto& row : prepared) {
            auto pkit = row.find(col_names.empty() ? std::string() : col_names[0]);
            std::string pk_val = pkit != row.end() ? pkit->second : std::string();
            nlohmann::json jrow = row;
            std::string val_json = jrow.dump();

            txn.log_insert(table, pk_val, val_json);

            if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) idx_it->second.insert(pk_val, val_json);

            std::vector<std::string> comp_keys;
            for (auto& [k, ci] : s.composite_indexes) {
                if (ci.table == table) comp_keys.push_back(k);
            }
            for (auto& k : comp_keys) s.composite_indexes.at(k).insert_row(row);

            index_insert_row(s, table, row);

            std::optional<std::string> pk_val_for_idx;
            if (!txn.is_active()) {
                for (auto& c : schema.columns) {
                    if (c.primary_key) {
                        if (auto it = row.find(c.name); it != row.end()) pk_val_for_idx = it->second;
                        break;
                    }
                }
            }
            auto tit = s.tables.find(table);
            if (tit == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");
            std::size_t pos = tit->second.size();
            tit->second.push_back(std::move(row));
            if (pk_val_for_idx) s.row_pk_pos[table][*pk_val_for_idx] = pos;
        }

        if (!txn.is_active()) {
            maybe_auto_vacuum(s, table);
            maybe_auto_analyze(s, table);
        }
        update_stat_rows(s, table, static_cast<std::int64_t>(inserted));

        // Row-level-concurrency Stage 4/5 correctness fix (found via concurrent-reader
        // monotonicity stress testing): invalidate the query cache HERE, still holding
        // table_data_locks EXCLUSIVE, not later in execute_sql (which only runs after
        // this lock has already been released). QueryResultCache::invalidate_table is
        // self-synchronized (its own mutex_) and safe to call under any lock mode, but
        // WHEN it runs relative to this lock's release is what matters: a reader whose
        // cache lookup happens after we release this lock must never be able to observe
        // a still-stale cache entry -- calling invalidate_table before releasing closes
        // that window. execute_sql's own post-execute invalidate_table call is left in
        // place too (harmless no-op redundancy), but is no longer load-bearing.
        s.query_cache.invalidate_table(table);
    }

    maybe_auto_checkpoint(s);
    if (returning) return StringResult::Ok(format_returning_rows(returning_rows, *returning));
    return StringResult::Ok(std::to_string(inserted) + " row(s) inserted.");
}

} // namespace engine
