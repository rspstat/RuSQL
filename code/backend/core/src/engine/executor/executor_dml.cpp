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

#include <unordered_set>

#include <algorithm>
#include <charconv>
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

} // namespace

bool Executor::index_or_scan_exists(const SharedDatabase& s, const std::string& table, const std::vector<Row>& table_rows,
                                      const std::string& column, const std::string& val, const std::function<bool(const Row&)>& accept) {
    // PK index: keyed directly by table name, single-column PK only. PK values are unique
    // by definition, so a miss (or an `accept` rejection of the sole hit) here is final --
    // no other row could possibly also have this PK value.
    //
    // NOTE: `TableSchema::primary_key_columns` is NOT the right field to check here -- it's
    // only populated from an explicit table-level `PRIMARY KEY (a, b)` constraint clause
    // (see exec_create's `has_explicit_pk = !primary_key_columns.empty()`, executor_ddl.cpp),
    // so it's empty for the overwhelmingly common inline case (`id INT PRIMARY KEY`) even
    // though that IS a single-column PK. Detecting it correctly means scanning each
    // ColumnDef's own `primary_key` flag and counting, exactly like the pre-existing
    // FOR-UPDATE/UNIQUE-check code elsewhere in this file already does.
    std::string schema_pk_col;
    std::size_t schema_pk_count = 0;
    if (const auto* schema = s.catalog.get_table(table)) {
        for (auto& c : schema->columns) {
            if (c.primary_key) {
                if (schema_pk_count == 0) schema_pk_col = c.name;
                schema_pk_count++;
            }
        }
    }
    if (schema_pk_count == 1 && schema_pk_col == column) {
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            if (auto j = idx_it->second.search(val); j && !j->empty()) {
                try {
                    return accept(row_from_json(*j));
                } catch (...) {
                }
            }
            return false;
        }
    }
    // Secondary B+Tree index: stores only one value per key, so it can positively confirm a
    // match but can't rule one out for a non-unique column -- fall through to the next index
    // type (or the linear scan) rather than returning false when it comes up empty/rejected.
    for (auto& [idx_key, meta] : s.index_meta) {
        if (meta.first != table || meta.second != column) continue;
        if (auto idx_it = s.indexes.find(idx_key); idx_it != s.indexes.end()) {
            if (auto j = idx_it->second.search(val); j && !j->empty()) {
                try {
                    if (accept(row_from_json(*j))) return true;
                } catch (...) {
                }
            }
        }
        break;
    }
    // Hash index: the bucket already holds every physical row sharing this value, so it's
    // authoritative either way.
    for (auto& [idx_key, meta] : s.hash_index_meta) {
        if (meta.first != table || meta.second != column) continue;
        if (auto hit = s.hash_indexes.find(idx_key); hit != s.hash_indexes.end()) {
            for (auto& r : hit->second.get(val)) {
                if (accept(r)) return true;
            }
            return false;
        }
    }
    // No index covers this column -- linear scan, exactly the pre-existing behavior.
    for (auto& r : table_rows) {
        auto it = r.find(column);
        if (it != r.end() && it->second == val && accept(r)) return true;
    }
    return false;
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
    auto tit = s.tables.find(table);
    if (tit == s.tables.end()) return;
    // The threshold scales with the table (10% of its versions, never below the old fixed
    // 200): a vacuum is O(table) -- it rebuilds the PK index from every remaining row and
    // rewrites the table file -- so with a flat 200 the amortized cost of each INSERT/UPDATE/
    // DELETE grew with table size (measured: a 247ms stall every 200 updates at 20k rows,
    // 677ms at 50k). Same idea as PostgreSQL's autovacuum_vacuum_scale_factor.
    const std::size_t threshold = std::max<std::size_t>(AUTO_VACUUM_THRESHOLD, tit->second.size() / 10);
    if (counter < threshold) return;
    counter = 0;
    std::uint64_t horizon = oldest_active_txn_id(s);
    auto& rows = tit->second;
    std::size_t before = rows.size();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return is_vacuumable(r, horizon); }), rows.end());
    if (rows.size() < before) {
        std::vector<Row> rows_clone = rows;
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
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
            idx_it->second = build_pk_tree(rows_clone, pk_col_name, idx_it->second.kinds());
        }
        s.buffer_pool.write_through(table, rows_clone, s.disk);
        redo_mark_flushed(s, table); // vacuum physically removed versions the redo log may still describe
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
                vals.push_back(it != row.end() ? it->second : INSERT_DEFAULT);
            }
            all_values.push_back(std::move(vals));
        }
        auto insert_cols = columns ? columns : std::optional<std::vector<std::string>>(col_names);
        return exec_insert(s, table, insert_cols, std::move(all_values), on_conflict, returning);
    }
}

// REPLACE INTO: for each incoming row, delete any existing row that conflicts on a
// PK/UNIQUE column -- via a real DELETE statement through execute_with_s (reuses that
// path's full locking/index/trigger correctness rather than mutating s.tables directly;
// the same recursive execute_with_s-from-within-a-statement pattern CTEs/UNION/LATERAL/
// exec_insert_select already rely on). A DELETE matching zero rows is a harmless no-op,
// so this doesn't need to pre-check whether a conflict actually exists.
//
// V1 scope: only considers PK/UNIQUE columns that have an explicit (non-empty, non-NULL)
// value in the given row -- a column relying on DEFAULT/AUTO_INCREMENT to coincidentally
// produce a duplicate isn't detected. Matches the overwhelmingly common real-world
// REPLACE INTO usage (PK/UNIQUE columns are given explicitly; that's the point of doing
// a REPLACE INTO in the first place).
StringResult Executor::replace_delete_conflicts(SharedDatabase& s, const std::string& table,
                                                 const std::optional<std::vector<std::string>>& col_list,
                                                 const std::vector<std::vector<std::string>>& all_values) {
    const TableSchema* schema = s.catalog.get_table(table);
    if (!schema) return StringResult::Ok(""); // let the real INSERT surface the "not found" error

    auto value_for = [&](const std::vector<std::string>& values, const std::string& col_name) -> std::optional<std::string> {
        if (col_list) {
            auto pos = std::find(col_list->begin(), col_list->end(), col_name);
            if (pos == col_list->end()) return std::nullopt;
            std::size_t idx = static_cast<std::size_t>(pos - col_list->begin());
            return idx < values.size() ? std::optional<std::string>(values[idx]) : std::nullopt;
        }
        auto pos = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == col_name; });
        if (pos == schema->columns.end()) return std::nullopt;
        std::size_t idx = static_cast<std::size_t>(pos - schema->columns.begin());
        return idx < values.size() ? std::optional<std::string>(values[idx]) : std::nullopt;
    };
    auto eq_leaf = [](const std::string& col, const std::string& val) {
        return CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{col}), Operator::Eq, ConditionValue(ConditionValue::Literal{val, true})}});
    };
    auto run_delete = [&](CondExpr cond) -> StringResult {
        Statement del(Statement::Delete{table, std::move(cond), std::nullopt});
        return execute_with_s(s, std::move(del));
    };

    for (auto& values : all_values) {
        // A column's own `primary_key` flag is set for BOTH an inline single-column PK
        // (`id INT PRIMARY KEY`) AND each individual column of a table-level composite
        // PRIMARY KEY (a, b) -- schema.primary_key_columns is only ever populated for the
        // latter (matching the rest of this file's existing is_composite_pk convention,
        // e.g. exec_insert_inner's own conflict-detection loop above). Treating a
        // composite-PK column as independently unique here would be wrong: `a=1` alone
        // can match many rows when only the (a,b) *pair* is actually unique -- that case
        // is handled correctly by the AND-combined composite block below instead.
        bool is_composite_pk_table = schema->primary_key_columns.size() > 1;
        for (auto& col : schema->columns) {
            bool solo_pk = col.primary_key && !is_composite_pk_table;
            if (!solo_pk && !col.unique) continue;
            auto val = value_for(values, col.name);
            if (!val || *val == INSERT_DEFAULT || *val == "NULL") continue;
            auto del_result = run_delete(eq_leaf(col.name, *val));
            if (del_result.is_err()) return del_result;
        }
        // Table-level composite PRIMARY KEY (col1, col2): only handled if every column
        // has an explicit value (a partial composite key can't safely target one row).
        if (is_composite_pk_table) {
            std::optional<CondExpr> cond;
            bool all_present = true;
            for (auto& pk_col : schema->primary_key_columns) {
                auto val = value_for(values, pk_col);
                if (!val || *val == INSERT_DEFAULT || *val == "NULL") { all_present = false; break; }
                CondExpr leaf = eq_leaf(pk_col, *val);
                cond = cond ? CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(*cond)), std::make_unique<CondExpr>(std::move(leaf))})
                            : std::move(leaf);
            }
            if (all_present && cond) {
                auto del_result = run_delete(std::move(*cond));
                if (del_result.is_err()) return del_result;
            }
        }
    }
    return StringResult::Ok("");
}

namespace {
// ON DUPLICATE KEY UPDATE: VALUES(col) is the value the statement was going to insert into `col`.
ArithExpr bind_insert_values(const ArithExpr& expr, const std::vector<std::string>& col_names, const std::vector<std::string>& values) {
    return std::visit(
        [&](const auto& alt) -> ArithExpr {
            using T = std::decay_t<decltype(alt)>;
            auto bind = [&](const std::unique_ptr<ArithExpr>& p) { return std::make_unique<ArithExpr>(bind_insert_values(*p, col_names, values)); };
            if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                if (alt.name == "VALUES" && alt.args.size() == 1) {
                    if (auto* c = std::get_if<ArithExpr::Col>(&alt.args[0].data)) {
                        std::string bare = c->name.substr(c->name.rfind('.') == std::string::npos ? 0 : c->name.rfind('.') + 1);
                        for (std::size_t i = 0; i < col_names.size() && i < values.size(); i++) {
                            if (col_names[i] == bare) return ArithExpr(ArithExpr::Str{values[i]});
                        }
                    }
                }
                std::vector<ArithExpr> args;
                for (auto& a : alt.args) args.push_back(bind_insert_values(a, col_names, values));
                return ArithExpr(ArithExpr::Func{alt.name, std::move(args)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Add>) {
                return ArithExpr(ArithExpr::Add{bind(alt.lhs), bind(alt.rhs)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Sub>) {
                return ArithExpr(ArithExpr::Sub{bind(alt.lhs), bind(alt.rhs)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Mul>) {
                return ArithExpr(ArithExpr::Mul{bind(alt.lhs), bind(alt.rhs)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Div>) {
                return ArithExpr(ArithExpr::Div{bind(alt.lhs), bind(alt.rhs)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) {
                return ArithExpr(ArithExpr::Cmp{bind(alt.lhs), alt.op, bind(alt.rhs)});
            } else {
                return ArithExpr(alt);
            }
        },
        expr.data);
}
} // namespace

// REPLACE INTO ... VALUES (1,'a'), (1,'b') leaves 'b': a row that a LATER row of the same statement replaces (same PRIMARY KEY, or the
// same value in a UNIQUE column, as far as the statement states them) is dropped before anything is checked or deleted.
std::vector<std::vector<std::string>> Executor::replace_supersede(SharedDatabase& s, const std::string& table,
                                                                  const std::optional<std::vector<std::string>>& col_list,
                                                                  std::vector<std::vector<std::string>> all_values) {
    const TableSchema* schema = s.catalog.get_table(table);
    if (!schema || all_values.size() < 2) return all_values;
    auto value_for = [&](const std::vector<std::string>& values, const std::string& col_name) -> std::optional<std::string> {
        std::size_t idx;
        if (col_list) {
            auto pos = std::find(col_list->begin(), col_list->end(), col_name);
            if (pos == col_list->end()) return std::nullopt;
            idx = static_cast<std::size_t>(pos - col_list->begin());
        } else {
            auto pos = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == col_name; });
            if (pos == schema->columns.end()) return std::nullopt;
            idx = static_cast<std::size_t>(pos - schema->columns.begin());
        }
        if (idx >= values.size() || values[idx] == INSERT_DEFAULT || values[idx] == EXECUTOR_NULL_VALUE) return std::nullopt;
        return values[idx];
    };
    const bool composite = schema->primary_key_columns.size() > 1;
    auto keys_of = [&](const std::vector<std::string>& values) {
        std::vector<std::string> keys;
        for (auto& col : schema->columns) {
            if ((col.primary_key && !composite) || col.unique) {
                if (auto v = value_for(values, col.name)) keys.push_back(col.name + '\x1f' + *v);
            }
        }
        if (composite) {
            std::string tuple = "\x1epk";
            bool all = true;
            for (auto& pk : schema->primary_key_columns) {
                auto v = value_for(values, pk);
                if (!v) { all = false; break; }
                tuple += '\x1f' + *v;
            }
            if (all) keys.push_back(tuple);
        }
        return keys;
    };
    std::unordered_set<std::string> later;
    std::vector<std::vector<std::string>> kept;
    for (std::size_t i = all_values.size(); i-- > 0;) {
        auto keys = keys_of(all_values[i]);
        bool superseded = std::any_of(keys.begin(), keys.end(), [&](const std::string& k) { return later.count(k) > 0; });
        if (superseded) continue;
        for (auto& k : keys) later.insert(k);
        kept.push_back(std::move(all_values[i]));
    }
    std::reverse(kept.begin(), kept.end());
    return kept;
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

    const bool replacing = std::holds_alternative<InsertConflict::Replace>(on_conflict.data);
    std::vector<std::vector<std::string>> replace_rows; // every row of a REPLACE: each one deletes what it conflicts with
    if (replacing) {
        // A REPLACE that fails must leave the table as it was (it used to delete the old rows first and then fail on the new one:
        // `REPLACE INTO t VALUES (1, NULL)` into a NOT NULL column lost row 1). So every check the real INSERT makes runs first,
        // without writing anything, and only then do the old rows go. A row that a later row of the statement replaces is checked
        // and deletes its conflicts like any other, but is not inserted.
        replace_rows = all_values;
        all_values = replace_supersede(s, table, col_list, std::move(all_values));
        std::vector<Row> victims; // the rows the replacement deletes
        if (auto checked = exec_insert_inner(s, table, col_list, replace_rows, on_conflict, std::nullopt, /*validate_only=*/true, &victims); checked.is_err()) return checked;
        // ... and DELETE refuses a row that a child row restricts: that too is found before anything is deleted
        if (!victims.empty()) {
            auto child_lock = acquire_table_data_locks(s, fk_child_tables(s, table), /*exclusive=*/false);
            for (auto& v : victims) {
                if (auto violation = delete_restrict_violation(s, table, v)) return StringResult::Err(*violation);
            }
        }
    }

    if (has_trigger(s, table, "BEFORE", "INSERT")) {
        // NEW of a BEFORE trigger: the values the statement names (a column it leaves out, or a DEFAULT, has no value yet)
        std::vector<std::string> names;
        if (col_list) names = *col_list;
        else if (const TableSchema* schema = s.catalog.get_table(table)) {
            for (auto& c : schema->columns) names.push_back(c.name);
        }
        std::vector<TriggerRow> new_rows;
        for (auto& values : all_values) {
            Row row;
            for (std::size_t i = 0; i < names.size() && i < values.size(); i++) {
                if (values[i] != INSERT_DEFAULT) row[names[i]] = values[i];
            }
            new_rows.push_back(TriggerRow{std::nullopt, std::move(row)});
        }
        if (auto tr = fire_triggers(s, table, "BEFORE", "INSERT", new_rows); tr.is_err()) return tr;
        // what the triggers made of NEW is what is inserted: a value they changed, a column they set that the statement left out
        std::vector<std::string> added;
        for (auto& row : new_rows) {
            if (!row.new_row) continue;
            for (auto& [column, value] : *row.new_row) {
                (void)value;
                if (std::find(names.begin(), names.end(), column) == names.end() && std::find(added.begin(), added.end(), column) == added.end() &&
                    column.size() > 0 && column[0] != '_') {
                    added.push_back(column);
                }
            }
        }
        for (std::size_t i = 0; i < all_values.size() && i < new_rows.size(); i++) {
            if (!new_rows[i].new_row) continue;
            for (std::size_t j = 0; j < names.size() && j < all_values[i].size(); j++) {
                if (auto it = new_rows[i].new_row->find(names[j]); it != new_rows[i].new_row->end()) all_values[i][j] = it->second;
            }
            all_values[i].resize(names.size(), std::string(INSERT_DEFAULT));
            for (auto& column : added) {
                auto it = new_rows[i].new_row->find(column);
                all_values[i].push_back(it != new_rows[i].new_row->end() ? it->second : std::string(INSERT_DEFAULT));
            }
        }
        if (!added.empty()) {
            for (auto& column : added) names.push_back(column);
            col_list = names;
        }
    }
    if (replacing) {
        if (auto del_result = replace_delete_conflicts(s, table, col_list, replace_rows); del_result.is_err()) return del_result;
        on_conflict = InsertConflict(InsertConflict::Abort{}); // conflicts are gone now; a real remaining duplicate should still error
    }
    const bool after_trigger = has_trigger(s, table, "AFTER", "INSERT");
    std::vector<Row> inserted;
    auto result = exec_insert_inner(s, table, col_list, std::move(all_values), on_conflict, returning, false, nullptr,
                                    after_trigger ? &inserted : nullptr);

    if (result.is_ok() && after_trigger) {
        std::vector<TriggerRow> new_rows;
        for (auto& row : inserted) new_rows.push_back(TriggerRow{std::nullopt, std::move(row)});
        if (auto tr = fire_triggers(s, table, "AFTER", "INSERT", new_rows); tr.is_err()) return tr;
    }
    return result;
}

StringResult Executor::exec_insert_inner(SharedDatabase& s, const std::string& table, const std::optional<std::vector<std::string>>& col_list,
                                          std::vector<std::vector<std::string>> all_values, const InsertConflict& on_conflict,
                                          const std::optional<std::vector<SelectColumn>>& returning, bool validate_only,
                                          std::vector<Row>* replace_victims, std::vector<Row>* inserted_rows) {
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
    // The column the PK B+Tree, the undo log and the pk -> row lookups are keyed by: the primary-key column (the
    // first one of a composite key), or the first column of a table without a primary key. This used to be
    // `col_names[0]` everywhere -- wrong whenever the PK is not the first column (`CREATE TABLE t (name ..., id INT
    // PRIMARY KEY)`): `WHERE id = 2` then found nothing, because the PK index was keyed by `name`.
    std::string key_col = col_names.empty() ? std::string() : col_names[0];
    for (auto& c : schema.columns) {
        if (c.primary_key) {
            key_col = c.name;
            break;
        }
    }
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

    // Duplicates WITHIN this statement: for every PRIMARY KEY / UNIQUE column the first row of the statement that carried
    // each value (and, for a composite primary key, the encoded tuples seen). This used to scan every earlier row of the
    // statement for every row -- quadratic in the number of rows of one INSERT (a 5,000-row batch compared 12.5 million
    // pairs of values).
    std::vector<std::unordered_map<std::string, std::size_t>> seen_unique_first(constraints.size());
    std::unordered_set<std::string> seen_composite_pk;
    std::size_t batch_rows_recorded = 0;
    std::vector<Row> prepared;
    prepared.reserve(all_values.size());
    // ON DUPLICATE KEY UPDATE: the row a conflicting row hits (as a condition on its primary key) and the assignments to apply to it,
    // with VALUES(col) already replaced by the value this statement was going to insert. They run as ordinary UPDATE statements
    // after the insert (own MVCC version, undo log, constraints, indexes, triggers) -- the old code rewrote the row in place.
    std::vector<std::pair<CondExpr, std::vector<std::pair<std::string, ArithExpr>>>> pending_updates;
    std::size_t row_no = 0; // 1-based row of the statement, for type error messages

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
                positional.push_back(it != col_map.end() ? it->second : INSERT_DEFAULT);
            }
        }

        std::vector<std::string> final_values = std::move(positional);

        for (std::size_t i = 0; i < schema.columns.size(); i++) {
            if (final_values[i] != INSERT_DEFAULT) continue;
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

        // AUTO_INCREMENT: a left-out value, NULL or 0 takes the next number; an explicit number moves the counter up to it (it used to
        // leave the counter alone, so the next generated number collided with a row that was inserted by number)
        auto counter_of = [&](std::size_t i) -> std::int64_t& {
            auto& counters = schema_mut_for_ai->auto_increment_counters;
            auto it = counters.find(col_names[i]);
            if (it == counters.end()) { // a table without a counter yet: continue after the largest number it holds
                std::int64_t largest = 0;
                if (auto tit = s.tables.find(table); tit != s.tables.end()) {
                    for (auto& r : tit->second) {
                        auto cit = r.find(col_names[i]);
                        if (cit == r.end() || !is_visible(r)) continue;
                        std::int64_t v = 0;
                        auto res = std::from_chars(cit->second.data(), cit->second.data() + cit->second.size(), v);
                        if (res.ec == std::errc() && res.ptr == cit->second.data() + cit->second.size()) largest = std::max(largest, v);
                    }
                }
                it = counters.emplace(col_names[i], largest).first;
            }
            return it->second;
        };
        for (std::size_t i = 0; i < constraints.size(); i++) {
            if (constraints[i].auto_increment && (final_values[i] == INSERT_DEFAULT || final_values[i] == EXECUTOR_NULL_VALUE || final_values[i] == "0")) {
                if (validate_only) { // the real insert numbers the row: here a number no other row of the statement has
                    final_values[i] = std::to_string(-static_cast<long long>(row_no + 1));
                    continue;
                }
                auto& counter = counter_of(i);
                counter += 1;
                final_values[i] = std::to_string(counter);
                any_auto_increment_allocated = true;
            }
        }

        for (auto& v : final_values) {
            if (v == INSERT_DEFAULT) v = EXECUTOR_NULL_VALUE; // left out and no default: NULL
        }
        for (std::size_t i = 0; i < constraints.size(); i++) {
            if ((constraints[i].not_null || constraints[i].primary_key) && final_values[i] == EXECUTOR_NULL_VALUE) {
                return StringResult::Err("Column '" + col_names[i] + "' cannot be NULL");
            }
        }

        row_no++;
        for (std::size_t i = 0; i < schema.columns.size(); i++) {
            if (auto err = coerce_column_value(schema.columns[i], final_values[i], row_no)) return StringResult::Err(*err);
        }
        if (!validate_only) {
            for (std::size_t i = 0; i < constraints.size(); i++) {
                if (!constraints[i].auto_increment || final_values[i] == EXECUTOR_NULL_VALUE) continue;
                std::int64_t v = 0;
                auto res = std::from_chars(final_values[i].data(), final_values[i].data() + final_values[i].size(), v);
                if (res.ec != std::errc()) continue;
                auto& counter = counter_of(i);
                if (v > counter) {
                    counter = v;
                    any_auto_increment_allocated = true; // the schema file keeps the counter
                }
            }
        }

        for (std::size_t i = 0; i < schema.columns.size(); i++) {
            auto& col = schema.columns[i];
            const std::string& val = final_values[i];
            if (val == EXECUTOR_NULL_VALUE) continue;
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

        // identifies `existing` by its primary key (all of a composite one) and queues the assignments for it
        auto queue_update = [&](const Row& existing, const std::vector<std::pair<std::string, ArithExpr>>& assigns) {
            std::optional<CondExpr> cond;
            const std::vector<std::string> id_cols = schema.primary_key_columns.size() > 1 ? schema.primary_key_columns : std::vector<std::string>{key_col};
            for (auto& c : id_cols) {
                auto it = existing.find(c);
                CondExpr leaf = CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{c}), Operator::Eq,
                                                                  ConditionValue(ConditionValue::Literal{it != existing.end() ? it->second : std::string(), true})}});
                cond = cond ? CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(*cond)), std::make_unique<CondExpr>(std::move(leaf))}) : std::move(leaf);
            }
            std::vector<std::pair<std::string, ArithExpr>> bound;
            for (auto& [col, expr] : assigns) bound.emplace_back(col, bind_insert_values(expr, col_names, final_values));
            pending_updates.emplace_back(std::move(*cond), std::move(bound));
        };

        {
            const std::vector<std::string>& pk_cols = schema.primary_key_columns;
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
                            queue_update(existing, upd->assignments);
                            skip_row = true;
                            break;
                        }
                        if (std::holds_alternative<InsertConflict::Replace>(on_conflict.data)) { // the old row goes before the real insert
                            if (replace_victims) replace_victims->push_back(existing);
                            break;
                        }
                    }
                } else {
                    for (std::size_t i = 0; i < constraints.size() && !skip_row; i++) {
                        if (!constraints[i].primary_key && !constraints[i].unique) continue;
                        const std::string& val = final_values[i];
                        if (!constraints[i].primary_key && val == EXECUTOR_NULL_VALUE) continue;
                        std::optional<Row> existing;
                        if (constraints[i].primary_key) {
                            if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
                                if (auto j = idx_it->second.search(val); j && !j->empty()) {
                                    try {
                                        Row r = row_from_json(*j);
                                        if (is_visible(r)) existing = std::move(r);
                                    } catch (...) {
                                    }
                                }
                            }
                        } else {
                            std::string hi_key;
                            for (auto& [name, meta] : s.hash_index_meta) {
                                if (meta.first == table && meta.second == col_names[i]) {
                                    hi_key = name;
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
                        if (std::holds_alternative<InsertConflict::Replace>(on_conflict.data)) { // the old row goes before the real insert
                            if (replace_victims) replace_victims->push_back(*existing);
                            continue;
                        }
                        if (std::holds_alternative<InsertConflict::Abort>(on_conflict.data)) {
                            return StringResult::Err("Duplicate value '" + val + "' for column '" + col_names[i] + "'");
                        }
                        if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                            skip_row = true;
                        } else if (auto* upd = std::get_if<InsertConflict::Update>(&on_conflict.data)) {
                            queue_update(*existing, upd->assignments);
                            skip_row = true;
                        }
                    }
                }
            }
            if (skip_row) continue;
        }

        {
            const std::vector<std::string>& pk_cols_batch = schema.primary_key_columns;
            if (pk_cols_batch.size() > 1) {
                std::vector<std::string> new_pk_tuple;
                for (auto& pk : pk_cols_batch) {
                    auto pos = std::find(col_names.begin(), col_names.end(), pk);
                    new_pk_tuple.push_back(pos != col_names.end() ? final_values[static_cast<std::size_t>(pos - col_names.begin())]
                                                                   : std::string());
                }
                std::string encoded; // length-prefixed, so no two different tuples encode alike
                for (auto& v : new_pk_tuple) {
                    encoded += std::to_string(v.size());
                    encoded += ':';
                    encoded += v;
                }
                if (!seen_composite_pk.insert(std::move(encoded)).second && !(validate_only && std::holds_alternative<InsertConflict::Replace>(on_conflict.data))) {
                    return StringResult::Err("Duplicate composite primary key (" + join_quoted(new_pk_tuple) + ")");
                }
            } else {
                // Same verdict (and the same column named in the message) as the scan this replaces: the earliest earlier
                // row that matches, and among its columns the lowest.
                std::size_t best_row = 0, best_col = 0;
                bool found = false;
                for (std::size_t i = 0; i < constraints.size(); i++) {
                    if (!constraints[i].primary_key && !constraints[i].unique) continue;
                    auto it = seen_unique_first[i].find(final_values[i]); // (a NULL is never recorded below)
                    if (it != seen_unique_first[i].end() && (!found || it->second < best_row)) {
                        found = true;
                        best_row = it->second;
                        best_col = i;
                    }
                }
                if (found && !(validate_only && std::holds_alternative<InsertConflict::Replace>(on_conflict.data))) {
                    return StringResult::Err("Duplicate value '" + final_values[best_col] + "' for column '" + col_names[best_col] + "'");
                }
                for (std::size_t i = 0; i < constraints.size(); i++) {
                    if (!constraints[i].primary_key && final_values[i] == EXECUTOR_NULL_VALUE) continue;
                    if (constraints[i].primary_key || constraints[i].unique) seen_unique_first[i].emplace(final_values[i], batch_rows_recorded);
                }
                batch_rows_recorded++;
            }
        }

        Row row;
        for (std::size_t i = 0; i < col_names.size(); i++) row[col_names[i]] = final_values[i];
        row["_xmin"] = std::to_string(tagging_txn_id(s));
        row["_xmax"] = "0";

        for (auto& col : schema.columns) {
            if (skip_fk_checks) break; // exec_restore()만 사용 -- executor.hpp의 필드 주석 참고
            if (!col.foreign_key) continue;
            auto it = row.find(col.name);
            std::string val = it != row.end() ? it->second : std::string();
            if (val == EXECUTOR_NULL_VALUE) continue;
            // (a partitioned parent is read through its children; a deleted parent row is no parent)
            if (auto violation = fk_child_violation(s, col, val)) {
                // ... unless it names the row itself or an earlier row of this very statement (a self-referencing table)
                const std::string& ref_col = col.foreign_key->ref_column;
                auto names_val = [&](const Row& r) { auto rit = r.find(ref_col); return rit != r.end() && rit->second == val; };
                const bool in_statement = col.foreign_key->ref_table == table && (names_val(row) || std::any_of(prepared.begin(), prepared.end(), names_val));
                if (!in_statement) return StringResult::Err(*violation);
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

        if (validate_only) { // REPLACE's pass over the rows before it deletes anything: the checks above, no locks, no writes
            prepared.push_back(std::move(row));
            continue;
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

        // Predicate lock conflict check (SSI phantom detection): unlike the gap-lock loop
        // above, this never blocks the INSERT -- it just flags the *reading* transaction (a
        // SERIALIZABLE holder that scanned a range covering this new row) so its own COMMIT
        // fails later with a serialization error, mirroring PostgreSQL's non-blocking SIREAD
        // predicate check. Applies regardless of THIS (inserting) transaction's own
        // isolation level -- the predicate protects its holder, not the inserter.
        // Deliberately independent of gap_pk_col_count: a composite/no-PK table can't be
        // narrowed to a [lo,hi] range (see extract_pk_gap_range's single pk_col parameter),
        // so the SELECT side (executor_select.cpp) registers a fully-unbounded (whole-table)
        // predicate for it instead -- that predicate matches every inserted row regardless
        // of PK shape, so no per-row PK value is needed to test it.
        {
            auto pk_val_it = gap_pk_col_count == 1 ? row.find(gap_pk_col) : row.end();
            for (auto& p : s.lock_mgr.predicate_reads_for(table)) {
                if (p.holder == txn.current_txn_id()) continue; // a txn's own reads never block its own INSERT
                bool hit;
                if (pk_val_it != row.end()) {
                    GapRange range{p.lo, p.hi, p.lo_inclusive, p.hi_inclusive};
                    hit = gap_range_contains(range, pk_val_it->second);
                } else {
                    // No single PK value to test a bounded range against -- only a
                    // fully-unbounded predicate (the composite/no-PK fallback) can be
                    // resolved without one, and it always matches.
                    hit = !p.lo && !p.hi;
                }
                if (hit) s.lock_mgr.flag_predicate_violation(p.holder);
            }
        }

        prepared.push_back(std::move(row));
    }
    } // insert_read_lock (SHARED) released here
    if (validate_only) return StringResult::Ok("");

    // Counters were already allocated directly against the real Catalog entry above (as
    // each row needed one); only the disk persistence is still batched once per
    // statement here, matching the original single-save behavior.
    if (any_auto_increment_allocated) {
        s.disk.save_schema(table, *s.catalog.get_table(table));
    }

    std::size_t inserted = prepared.size();
    std::vector<Row> returning_rows = returning ? prepared : std::vector<Row>{};
    if (inserted_rows) *inserted_rows = prepared; // (the rows an AFTER trigger runs for)

    // Row-level-concurrency Stage 4: EXCLUSIVE only for this short tail -- the actual
    // vector-shape change (push_back) and the paired row_pk_pos[table] update, plus
    // maybe_auto_vacuum (which erases rows -- also shape-changing) and the
    // dml_since_vacuum/dml_since_analyze/table_stats counter touches. s.indexes/
    // s.composite_indexes are separately protected by their own per-instance mutex
    // (Stage 2), not by this lock.
    {
        auto insert_write_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        auto tit = s.tables.find(table);
        if (tit == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");
        auto idx_it = s.indexes.find(table);
        std::vector<CompositeIndex*> composite;
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table == table) composite.push_back(&ci);
        }
        for (auto& row : prepared) {
            auto pkit = row.find(key_col);
            std::string pk_val = pkit != row.end() ? pkit->second : std::string();
            std::string val_json = row_to_json(row);

            txn.log_insert(table, pk_val, val_json);

            if (idx_it != s.indexes.end()) idx_it->second.insert(pk_val, val_json);
            for (auto* ci : composite) ci->insert_row(row);
        }
        // Secondary and hash indexes: every affected bucket is parsed and rewritten once for the whole statement
        // (one JSON array of rows per key), instead of once per row -- quadratic for an index on a column with few
        // distinct values. Same helper UPDATE and DELETE use.
        index_replace_rows(s, table, std::vector<Row>{}, prepared, key_col);

        // autocommit only: pk -> position cache, filled while the rows are appended
        std::string pos_col;
        if (!txn.is_active()) {
            for (auto& c : schema.columns) {
                if (c.primary_key) {
                    pos_col = c.name;
                    break;
                }
            }
        }
        auto& rows_vec = tit->second;
        if (rows_vec.size() + prepared.size() > rows_vec.capacity()) {
            rows_vec.reserve(std::max(rows_vec.capacity() * 2, rows_vec.size() + prepared.size()));
        }
        std::unordered_map<std::string, std::size_t>* pos_map = nullptr;
        if (!pos_col.empty() && !prepared.empty()) {
            pos_map = &s.row_pk_pos[table];
            pos_map->reserve(pos_map->size() + prepared.size());
        }
        for (auto& row : prepared) {
            std::size_t pos = rows_vec.size();
            std::optional<std::string> pos_key;
            if (pos_map) {
                if (auto it = row.find(pos_col); it != row.end()) pos_key = it->second;
            }
            rows_vec.push_back(std::move(row));
            if (pos_key) (*pos_map)[*pos_key] = pos;
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

    std::size_t updated = 0;
    for (auto& [cond, assigns] : pending_updates) {
        auto r = exec_update(s, table, assigns, cond, std::nullopt);
        if (r.is_err()) return r;
        updated += static_cast<std::size_t>(std::strtoull(r.value().c_str(), nullptr, 10));
    }

    maybe_auto_checkpoint(s);
    if (returning) return StringResult::Ok(format_returning_rows(returning_rows, *returning));
    if (!pending_updates.empty()) return StringResult::Ok(std::to_string(inserted) + " row(s) inserted, " + std::to_string(updated) + " row(s) updated.");
    return StringResult::Ok(std::to_string(inserted) + " row(s) inserted.");
}

} // namespace engine
