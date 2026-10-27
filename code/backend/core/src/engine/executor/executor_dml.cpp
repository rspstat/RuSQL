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

#include <functional>
#include <unordered_set>

#include <algorithm>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include "engine/now_text.hpp"
#include "engine/parser/parser.hpp"
#include "engine/sql_value_util.hpp"

namespace engine {

namespace {

// the time of the INSERT as a column with `digits` places of a fraction of a second holds it
std::string current_timestamp_string(int digits) { return time_now_text(digits, false, true); }

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
        if (it != r.end() && collation::equal(it->second, val) && accept(r)) return true;
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
    std::vector<std::string> col_names;
    std::vector<Row> rows;
    // which columns of the SELECT are FLOAT columns: their values are taken as they are stored (not as the six digits they are shown with) and widened where a number column takes them
    std::vector<char> from_float;
    for (const ResultColumnType& type : result_columns_in(s, query)) {
        from_float.push_back(type.column && std::holds_alternative<DataType::Float>(type.column->data_type.data));
    }
    // ... and an item that is not a column but gives a FLOAT as well: MIN / MAX of a FLOAT column, a COALESCE over FLOAT columns (the item at its place of the select list)
    if (auto* select = std::get_if<Statement::Select>(&query.data);
        select && select->columns.size() == from_float.size() &&
        std::none_of(select->columns.begin(), select->columns.end(), [](const SelectColumn& c) { return std::holds_alternative<SelectColumn::All>(c.data); })) {
        for (std::size_t i = 0; i < select->columns.size(); i++) {
            const SelectColumn& item = select->columns[i];
            if (auto* expr = std::get_if<SelectColumn::Expr>(&item.data)) {
                if (single_result(expr->expr)) from_float[i] = 1;
            } else if (auto* agg = std::get_if<SelectColumn::Agg>(&item.data)) {
                if (agg->arg_single) from_float[i] = 1;
            } else if (auto* agg_alias = std::get_if<SelectColumn::AggAlias>(&item.data)) {
                if (agg_alias->arg_single) from_float[i] = 1;
            }
        }
    }
    const bool any_float = std::find(from_float.begin(), from_float.end(), char{1}) != from_float.end();
    {
        DataLockGuard src_data_guard;
        if (auto src_tables = table_lock_set_for(s, query)) {
            src_data_guard = acquire_table_data_locks(s, *src_tables, /*exclusive=*/false);
        }
        const sqlval::StrictWrite strict;
        struct ExactFloats {
            bool& flag;
            bool before;
            ExactFloats(bool& f, bool on) : flag(f), before(f) { if (on) flag = true; }
            ~ExactFloats() { flag = before; }
        } exact_floats(float_exact_, any_float);
        auto output = execute_with_s(s, std::move(query));
        if (output.is_err()) return output;
        if (auto refused = strict.error(); refused && !std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) return StringResult::Err(*refused);
        std::tie(col_names, rows) = parse_table_output(output.value());
    } // (the shared lock on the source tables ends here: the insert takes the target exclusively, and the target may be the source)
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
    // the columns of the SELECT go to the columns named, or (none named) to the table's own in order -- by position, never by the names the SELECT gives
    std::optional<std::vector<std::string>> insert_cols = columns;
    if (!insert_cols) {
        if (const TableSchema* schema = s.catalog.get_table(table)) {
            if (col_names.size() != schema->columns.size()) return StringResult::Err("Column count doesn't match value count at row 1");
            std::vector<std::string> names;
            for (auto& column : schema->columns) names.push_back(column.name);
            insert_cols = std::move(names);
        } else {
            insert_cols = col_names;
        }
    }
    // a FLOAT goes into a column that is not a FLOAT (a DOUBLE, a DECIMAL, an integer, a text) as the single-precision number widened -- 0.1 is 0.10000000149011612 there --, into
    // a FLOAT as it is
    if (any_float && from_float.size() == col_names.size()) {
        if (const TableSchema* schema = s.catalog.get_table(table)) {
            for (std::size_t i = 0; i < from_float.size() && i < insert_cols->size(); i++) {
                if (!from_float[i]) continue;
                auto target = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == (*insert_cols)[i]; });
                if (target == schema->columns.end() || std::holds_alternative<DataType::Float>(target->data_type.data)) continue;
                for (auto& row : all_values) {
                    if (i >= row.size() || row[i] == EXECUTOR_NULL_VALUE || row[i] == INSERT_DEFAULT) continue;
                    row[i] = sqlval::float_as_double_text(row[i]);
                }
            }
        }
    }
    insert_from_select_ = true; // (its rows are numbered in batches of 1, 2, 4 ... as they ask, not all at once)
    return exec_insert(s, table, insert_cols, std::move(all_values), on_conflict, returning);
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
        auto deleted = execute_with_s(s, std::move(del));
        if (deleted.is_ok()) replace_deleted_rows_ += static_cast<std::size_t>(std::strtoull(deleted.value().c_str(), nullptr, 10));
        return deleted;
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
        for (auto& key : schema->unique_keys) { // UNIQUE keys over several columns: the row that has the same combination goes (a NULL in it repeats nothing)
            std::optional<CondExpr> cond;
            bool all_present = true;
            for (auto& column : key.columns) {
                auto val = value_for(values, column);
                if (!val || *val == INSERT_DEFAULT || *val == "NULL") { all_present = false; break; }
                CondExpr leaf = eq_leaf(column, *val);
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
ArithExpr bind_insert_values(const ArithExpr& expr, const std::vector<std::string>& col_names, const std::vector<std::string>& values);

// ... inside a condition (a CASE's WHEN, IF(), a condition used as a value)
CondExpr bind_insert_values_cond(const CondExpr& e, const std::vector<std::string>& col_names, const std::vector<std::string>& values) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) {
        return CondExpr(CondExpr::And{std::make_unique<CondExpr>(bind_insert_values_cond(*a->lhs, col_names, values)),
                                      std::make_unique<CondExpr>(bind_insert_values_cond(*a->rhs, col_names, values))});
    }
    if (auto* o = std::get_if<CondExpr::Or>(&e.data)) {
        return CondExpr(CondExpr::Or{std::make_unique<CondExpr>(bind_insert_values_cond(*o->lhs, col_names, values)),
                                     std::make_unique<CondExpr>(bind_insert_values_cond(*o->rhs, col_names, values))});
    }
    if (auto* n = std::get_if<CondExpr::Not>(&e.data)) {
        return CondExpr(CondExpr::Not{std::make_unique<CondExpr>(bind_insert_values_cond(*n->inner, col_names, values))});
    }
    if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
        Condition c = leaf->condition;
        c.left = bind_insert_values(c.left, col_names, values);
        if (auto* value = std::get_if<ConditionValue::Arith>(&c.value.data)) value->expr = bind_insert_values(value->expr, col_names, values);
        return CondExpr(CondExpr::Leaf{std::move(c)});
    }
    return e;
}

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
            } else if constexpr (std::is_same_v<T, ArithExpr::Pred>) {
                return ArithExpr(ArithExpr::Pred{std::make_unique<CondExpr>(bind_insert_values_cond(*alt.cond, col_names, values))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Subquery>) {
                return ArithExpr(ArithExpr::Subquery{std::make_unique<Statement>(*alt.query), alt.cls});
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
                if (auto v = value_for(values, col.name)) keys.push_back(col.name + '\x1f' + collation::key(*v));
            }
        }
        if (composite) {
            std::string tuple = "\x1epk";
            bool all = true;
            for (auto& pk : schema->primary_key_columns) {
                auto v = value_for(values, pk);
                if (!v) { all = false; break; }
                tuple += '\x1f' + collation::key(*v);
            }
            if (all) keys.push_back(tuple);
        }
        for (std::size_t k = 0; k < schema->unique_keys.size(); k++) { // UNIQUE keys over several columns (a NULL in one never repeats)
            std::string tuple = "\x1eu" + std::to_string(k);
            bool all = true;
            for (auto& column : schema->unique_keys[k].columns) {
                auto v = value_for(values, column);
                if (!v) { all = false; break; }
                tuple += '\x1f' + collation::key(*v);
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
    const bool from_select = std::exchange(insert_from_select_, false);
    if (s.views.count(table)) {
        if (auto resolved = resolve_updatable_view(s, table)) {
            insert_from_select_ = from_select;
            return exec_insert(s, resolved->first, col_list, std::move(all_values), on_conflict, returning);
        }
        return StringResult::Err("View '" + strip_db_prefix(table) + "' is not updatable (has JOINs, DISTINCT, GROUP BY, or subquery)");
    }

    const bool replacing = std::holds_alternative<InsertConflict::Replace>(on_conflict.data);
    replace_deleted_rows_ = 0;
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
        // ... and DELETE refuses a row that a child row restricts, however far down the foreign keys that child is (a child that goes with its parent has children of its
        // own): the whole chain is followed on the rows in memory, so that nothing is deleted before the statement is known to go through
        if (!victims.empty()) {
            std::vector<std::string> below = fk_children_closure(s, table);
            below.erase(std::remove(below.begin(), below.end(), table), below.end()); // (the table itself is held by the statement)
            auto child_lock = acquire_table_data_locks(s, below, /*exclusive=*/false);
            CascadePlan plan;
            if (auto refused = plan_cascade_delete(s, table, victims, nullptr, nullptr, std::nullopt, plan)) return StringResult::Err(*refused);
        }
    }

    if (has_trigger(s, table, "BEFORE", "INSERT")) {
        // NEW of a BEFORE trigger: the values the statement names (a column it leaves out, or a DEFAULT, has no value yet)
        std::vector<std::string> names;
        if (col_list) names = *col_list;
        else if (const TableSchema* schema = s.catalog.get_table(table)) {
            for (auto& c : schema->columns) names.push_back(c.name);
        }
        // ... as the columns will hold them (1.005 into a DECIMAL(8,2) is 1.01 in NEW.price); one that does not fit stays as written, for the INSERT to refuse
        const TableSchema* new_schema = s.catalog.get_table(table);
        std::vector<TriggerRow> new_rows;
        for (auto& values : all_values) {
            Row row;
            for (std::size_t i = 0; i < names.size() && i < values.size(); i++) {
                if (values[i] == INSERT_DEFAULT) continue;
                std::string value = values[i];
                if (new_schema) {
                    auto column = std::find_if(new_schema->columns.begin(), new_schema->columns.end(), [&](const ColumnDef& c) { return c.name == names[i]; });
                    std::string fitted = value;
                    if (column != new_schema->columns.end() && !coerce_column_value(*column, fitted, 1)) value = std::move(fitted);
                }
                row[names[i]] = std::move(value);
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
                                    after_trigger ? &inserted : nullptr, from_select);
    // what the statement affected, kept across the AFTER triggers (their statements set these too): a REPLACE counts a row it replaced twice (the old one goes, the new one comes)
    std::int64_t affected = affected_rows_, matched = matched_rows_;
    if (replacing && result.is_ok() && !returning) affected = static_cast<std::int64_t>(std::strtoull(result.value().c_str(), nullptr, 10) + replace_deleted_rows_);

    if (result.is_ok() && after_trigger) {
        std::vector<TriggerRow> new_rows;
        for (auto& row : inserted) new_rows.push_back(TriggerRow{std::nullopt, std::move(row)});
        if (auto tr = fire_triggers(s, table, "AFTER", "INSERT", new_rows); tr.is_err()) return tr;
    }
    if (result.is_ok()) {
        affected_rows_ = affected;
        matched_rows_ = matched;
    }
    return result;
}

StringResult Executor::exec_insert_inner(SharedDatabase& s, const std::string& table, const std::optional<std::vector<std::string>>& col_list,
                                          std::vector<std::vector<std::string>> all_values, const InsertConflict& on_conflict,
                                          const std::optional<std::vector<SelectColumn>>& returning, bool validate_only,
                                          std::vector<Row>* replace_victims, std::vector<Row>* inserted_rows, bool from_select) {
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
    // The numbers this statement takes from the counters: if it fails before a row has reached the table (a value that is no good for its column) they go back, so
    // such an INSERT leaves no gap in the numbering. `kept` is set when the statement has succeeded; a counter that another statement has moved on in the meantime is
    // left alone. What a statement RESERVES is another matter, as in InnoDB: an INSERT ... VALUES of N rows reserves N numbers when its first row is written (an
    // INSERT ... SELECT reserves 1, then 2, 4, 8 ... as its rows ask for more), a row takes the next of them, a row that is skipped (IGNORE, ON DUPLICATE KEY UPDATE)
    // gives its number back to the reservation, and what is left of it is lost when the statement ends -- also when the statement fails, once a row has reached the table
    // (`burn_reservation`): a duplicate key or a missing parent burns the numbers, a first row that fails the checks of its columns does not.
    struct CounterTake {
        std::string column;
        std::int64_t before, after;
        bool reserved = false;
    };
    std::vector<CounterTake> counter_takes;
    bool burn_reservation = false;
    struct CounterGuard {
        TableSchema* schema;
        std::vector<CounterTake>& takes;
        const bool& burn;
        bool kept = false;
        ~CounterGuard() {
            if (kept || !schema) return;
            for (auto it = takes.rbegin(); it != takes.rend(); ++it) {
                if (it->reserved && burn) continue;
                auto found = schema->auto_increment_counters.find(it->column);
                if (found != schema->auto_increment_counters.end() && found->second == it->after) found->second = it->before;
            }
        }
    } counter_guard{schema_mut_for_ai, counter_takes, burn_reservation};
    struct AutoWindow {
        bool active = false;
        std::int64_t next = 0, end = 0;
    } window;
    std::int64_t next_batch = 1;
    bool reserved_before = false; // (the first reservation of a VALUES statement is as large as the statement)

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
    std::size_t intra_updates_changed = 0; // (the rows of this statement that a later row of it changed through ON DUPLICATE KEY UPDATE)
    std::size_t intra_updates_found = 0;   // (... and those it only found, the changed ones included)
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
        for (auto& fk : col.foreign_keys) {
            // Table partitioning: lock the ref_table's actual children (where the existence
            // check below really reads), not its permanently-empty phantom logical name.
            if (auto part_info = partition_info_for(s, fk.ref_table)) {
                for (auto& def : part_info->partitions) insert_read_tables.push_back(def.child_table);
            } else {
                insert_read_tables.push_back(fk.ref_table);
            }
        }
    }
    {
        auto insert_read_lock = acquire_table_data_locks(s, insert_read_tables, /*exclusive=*/false);

    // UNIQUE keys over several columns: the live rows by key (one scan, made when it is first needed), and the keys this statement has used. A NULL in a key never conflicts.
    struct CompositeUnique {
        const UniqueKey* key;
        std::string label;
        std::unordered_map<std::string, const Row*> existing;
        bool existing_built = false;
        std::unordered_set<std::string> batch;
    };
    std::vector<CompositeUnique> composite_uniques;
    auto composite_key = [](const std::vector<std::string>& columns, const std::function<std::string(const std::string&)>& value, std::string& shown) -> std::optional<std::string> {
        std::string encoded;
        shown.clear();
        for (std::size_t k = 0; k < columns.size(); k++) {
            const std::string v = value(columns[k]);
            if (v == EXECUTOR_NULL_VALUE) return std::nullopt;
            const std::string part = collation::key(v);
            encoded += std::to_string(part.size()) + ":" + part;
            shown += (k ? "-" : "") + v;
        }
        return encoded;
    };
    // (the rows the table holds by each of these keys are put in a hash when they are first looked for: a statement of several rows needs them for every row, a
    // one-row statement only when the cache of UNIQUE values cannot say that no row holds the key -- see unique_probe_proves_absent)
    auto fill_existing = [&](CompositeUnique& cu) {
        if (cu.existing_built) return;
        cu.existing_built = true;
        if (auto tit = s.tables.find(table); tit != s.tables.end()) {
            for (auto& existing_row : tit->second) {
                if (!is_visible(existing_row)) continue;
                std::string shown;
                auto encoded = composite_key(cu.key->columns, [&](const std::string& c) { auto it = existing_row.find(c); return it != existing_row.end() ? it->second : std::string(EXECUTOR_NULL_VALUE); }, shown);
                if (encoded) cu.existing.emplace(std::move(*encoded), &existing_row);
            }
        }
    };
    const bool several_rows = all_values.size() > 1;
    if (!schema.unique_keys.empty()) {
        const std::string bare_table = table.substr(table.rfind('.') == std::string::npos ? 0 : table.rfind('.') + 1);
        for (auto& key : schema.unique_keys) {
            composite_uniques.push_back(CompositeUnique{&key, bare_table + "." + key.name.value_or(key.columns.front()), {}, false, {}});
        }
    }

    // A statement of several rows finds the rows the table already holds with a hash built once -- by the key tuple of a composite primary key, by the
    // value of each UNIQUE column that has no index of its own -- instead of scanning the table for every row (20,000 rows: 9 s for a UNIQUE column, 23 s for
    // a composite key, against 0.1 s for a table with neither). A one-row statement keeps the scan: building the hash would cost as much.
    auto tuple_key = [](const std::vector<std::string>& tuple) { // length-prefixed, so no two different tuples encode alike
        std::string encoded;
        for (auto& v : tuple) {
            const std::string part = collation::key(v);
            encoded += std::to_string(part.size());
            encoded += ':';
            encoded += part;
        }
        return encoded;
    };
    std::unordered_map<std::string, const Row*> existing_by_pk_tuple;
    bool existing_by_pk_tuple_built = false;
    std::vector<std::unordered_map<std::string, const Row*>> existing_by_unique(constraints.size());
    std::vector<char> existing_by_unique_built(constraints.size(), 0);

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
                    final_values[i] = (upper == "NOW()" || upper == "CURRENT_TIMESTAMP") ? current_timestamp_string(fraction_digits_of(col.data_type)) : *col.default_value;
                }
            } else if (std::holds_alternative<DataType::Timestamp>(col.data_type.data)) {
                final_values[i] = current_timestamp_string(fraction_digits_of(col.data_type));
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
        std::int64_t taken_id = 0; // the number this row took from the reservation (0: none), and what LAST_INSERT_ID() said before it
        bool row_set_insert_id = false;
        std::uint64_t id_before_row = last_insert_id_, statement_id_before_row = statement_insert_id_;
        bool explicit_auto_value = false;
        // the row is not inserted (IGNORE, ON DUPLICATE KEY UPDATE): its number goes back to the reservation, and is not the one LAST_INSERT_ID() says
        auto give_back_row_id = [&]() {
            if (taken_id != 0 && window.active && window.next == taken_id + 1) window.next = taken_id;
            if (row_set_insert_id) {
                last_insert_id_ = id_before_row;
                statement_insert_id_ = statement_id_before_row;
                insert_id_unset_ = true;
            }
        };
        for (std::size_t i = 0; i < constraints.size(); i++) {
            if (constraints[i].auto_increment && (final_values[i] == INSERT_DEFAULT || final_values[i] == EXECUTOR_NULL_VALUE || final_values[i] == "0")) {
                if (validate_only) { // the real insert numbers the row: here a number no other row of the statement has
                    final_values[i] = std::to_string(-static_cast<long long>(row_no + 1));
                    continue;
                }
                auto& counter = counter_of(i);
                if (!window.active || window.next > window.end) { // a new reservation: the whole statement's rows the first time, then 1, 2, 4 ...
                    std::int64_t size = next_batch;
                    if (!reserved_before && !from_select) {
                        size = static_cast<std::int64_t>(all_values.size());
                    } else {
                        next_batch *= 2;
                    }
                    reserved_before = true;
                    const std::int64_t before = counter;
                    window = {true, before + 1, before + size};
                    counter = before + size;
                    counter_takes.push_back({col_names[i], before, counter, /*reserved=*/true});
                }
                taken_id = window.next++;
                final_values[i] = std::to_string(taken_id);
                any_auto_increment_allocated = true;
                if (insert_id_unset_) { // the first number this INSERT gave out is what LAST_INSERT_ID() says
                    last_insert_id_ = statement_insert_id_ = static_cast<std::uint64_t>(taken_id);
                    insert_id_unset_ = false;
                    row_set_insert_id = true;
                }
            } else if (constraints[i].auto_increment && final_values[i] != EXECUTOR_NULL_VALUE) {
                explicit_auto_value = true;
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
            if (explicit_auto_value) window.active = false; // (a number given by hand ends the reservation: what is left of it is lost)
            for (std::size_t i = 0; i < constraints.size(); i++) {
                if (!constraints[i].auto_increment || final_values[i] == EXECUTOR_NULL_VALUE) continue;
                std::int64_t v = 0;
                auto res = std::from_chars(final_values[i].data(), final_values[i].data() + final_values[i].size(), v);
                if (res.ec != std::errc()) continue;
                auto& counter = counter_of(i);
                if (v > counter) {
                    counter_takes.push_back({col_names[i], counter, v});
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

        burn_reservation = true; // (the row has passed the checks of its columns: it reaches the table, and a failure from here on keeps the numbers spent)

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

        // the values of this row by column name, for the cache of UNIQUE values (a one-row statement asks it before it scans the table)
        const auto row_value_of = [&](const std::string& c) -> const std::string* {
            auto pos = std::find(col_names.begin(), col_names.end(), c);
            return pos == col_names.end() ? nullptr : &final_values[static_cast<std::size_t>(pos - col_names.begin())];
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
                    // the first visible row of the table with this key (by the table's collation: 'Alice' and 'alice' are one key)
                    const Row* clash = nullptr;
                    if (several_rows) {
                        if (unique_probe_proves_absent(s, table, schema, pk_cols, /*tuple=*/true, /*skip_null=*/false, row_value_of, ProbeUse::NoBuild)) {
                            // (no live row holds the key: nothing to look up, and the hash of the rows the table holds is not made for this row)
                        } else {
                        if (!existing_by_pk_tuple_built) {
                            for (auto& existing : rows) {
                                if (!is_visible(existing)) continue;
                                std::vector<std::string> existing_tuple;
                                for (auto& pk : pk_cols) {
                                    auto it = existing.find(pk);
                                    existing_tuple.push_back(it != existing.end() ? it->second : std::string());
                                }
                                existing_by_pk_tuple.emplace(tuple_key(existing_tuple), &existing);
                            }
                            existing_by_pk_tuple_built = true;
                        }
                        if (auto hit = existing_by_pk_tuple.find(tuple_key(new_pk_tuple)); hit != existing_by_pk_tuple.end()) clash = hit->second;
                        }
                    } else if (!unique_probe_proves_absent(s, table, schema, pk_cols, /*tuple=*/true, /*skip_null=*/false, row_value_of)) {
                        for (auto& existing : rows) {
                            if (!is_visible(existing)) continue;
                            bool same = true;
                            for (std::size_t k = 0; k < pk_cols.size() && same; k++) {
                                auto it = existing.find(pk_cols[k]);
                                same = collation::equal(it != existing.end() ? it->second : std::string(), new_pk_tuple[k]);
                            }
                            if (same) {
                                clash = &existing;
                                break;
                            }
                        }
                    }
                    if (clash) {
                        const Row& existing = *clash;
                        if (std::holds_alternative<InsertConflict::Abort>(on_conflict.data)) {
                            return StringResult::Err("Duplicate composite primary key (" + join_quoted(new_pk_tuple) + ")");
                        }
                        if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                            skip_row = true;
                        } else if (auto* upd = std::get_if<InsertConflict::Update>(&on_conflict.data)) {
                            queue_update(existing, upd->assignments);
                            skip_row = true;
                        } else if (std::holds_alternative<InsertConflict::Replace>(on_conflict.data)) { // the old row goes before the real insert
                            if (replace_victims) replace_victims->push_back(existing);
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
                            } else if (several_rows) {
                                if (unique_probe_proves_absent(s, table, schema, {col_names[i]}, /*tuple=*/false, /*skip_null=*/true, row_value_of, ProbeUse::NoBuild)) {
                                    // (no live row holds the value)
                                } else {
                                if (!existing_by_unique_built[i]) {
                                    for (auto& r : rows) {
                                        if (!is_visible(r)) continue;
                                        auto it = r.find(col_names[i]);
                                        if (it != r.end() && it->second != EXECUTOR_NULL_VALUE) existing_by_unique[i].emplace(collation::key(it->second), &r);
                                    }
                                    existing_by_unique_built[i] = 1;
                                }
                                if (auto hit = existing_by_unique[i].find(collation::key(val)); hit != existing_by_unique[i].end()) existing = *hit->second;
                                }
                            } else if (!unique_probe_proves_absent(s, table, schema, {col_names[i]}, /*tuple=*/false, /*skip_null=*/true, row_value_of)) {
                                for (auto& r : rows) {
                                    if (!is_visible(r)) continue;
                                    auto it = r.find(col_names[i]);
                                    if (it != r.end() && collation::equal(it->second, val)) {
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
            if (!skip_row) {
                for (auto& cu : composite_uniques) {
                    std::string shown;
                    auto encoded = composite_key(cu.key->columns, [&](const std::string& c) { auto pos = std::find(col_names.begin(), col_names.end(), c); return pos != col_names.end() ? final_values[static_cast<std::size_t>(pos - col_names.begin())] : std::string(EXECUTOR_NULL_VALUE); }, shown);
                    if (!encoded) continue;
                    if (unique_probe_proves_absent(s, table, schema, cu.key->columns, /*tuple=*/true, /*skip_null=*/true, row_value_of, several_rows ? ProbeUse::NoBuild : ProbeUse::Lookup)) continue;
                    fill_existing(cu);
                    auto hit = cu.existing.find(*encoded);
                    if (hit == cu.existing.end()) continue;
                    const Row& existing_row = *hit->second;
                    if (std::holds_alternative<InsertConflict::Replace>(on_conflict.data)) { // the old row goes before the real insert
                        if (replace_victims) replace_victims->push_back(existing_row);
                        continue;
                    }
                    if (std::holds_alternative<InsertConflict::Abort>(on_conflict.data)) return StringResult::Err("Duplicate entry '" + shown + "' for key '" + cu.label + "'");
                    if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                        skip_row = true;
                    } else if (auto* upd = std::get_if<InsertConflict::Update>(&on_conflict.data)) {
                        queue_update(existing_row, upd->assignments);
                        skip_row = true;
                    }
                    break;
                }
            }
            if (skip_row) {
                give_back_row_id();
                continue;
            }
        }

        bool repeats_earlier_row = false; // (IGNORE: a row that repeats a key of an earlier row of the statement is skipped)
        for (auto& cu : composite_uniques) { // ... and among the rows of this statement
            std::string shown;
            auto encoded = composite_key(cu.key->columns, [&](const std::string& c) { auto pos = std::find(col_names.begin(), col_names.end(), c); return pos != col_names.end() ? final_values[static_cast<std::size_t>(pos - col_names.begin())] : std::string(EXECUTOR_NULL_VALUE); }, shown);
            if (!encoded) continue;
            if (!cu.batch.insert(std::move(*encoded)).second && !(validate_only && std::holds_alternative<InsertConflict::Replace>(on_conflict.data))) {
                if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                    repeats_earlier_row = true;
                    break;
                }
                return StringResult::Err("Duplicate entry '" + shown + "' for key '" + cu.label + "'");
            }
        }
        if (repeats_earlier_row) {
            give_back_row_id();
            continue;
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
                std::string encoded = tuple_key(new_pk_tuple);
                if (!seen_composite_pk.insert(std::move(encoded)).second && !(validate_only && std::holds_alternative<InsertConflict::Replace>(on_conflict.data))) {
                    if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                        give_back_row_id();
                        continue;
                    }
                    return StringResult::Err("Duplicate composite primary key (" + join_quoted(new_pk_tuple) + ")");
                }
            } else {
                // Same verdict (and the same column named in the message) as the scan this replaces: the earliest earlier
                // row that matches, and among its columns the lowest.
                std::size_t best_row = 0, best_col = 0;
                bool found = false;
                for (std::size_t i = 0; i < constraints.size(); i++) {
                    if (!constraints[i].primary_key && !constraints[i].unique) continue;
                    auto it = seen_unique_first[i].find(collation::key(final_values[i])); // (a NULL is never recorded below)
                    if (it != seen_unique_first[i].end() && (!found || it->second < best_row)) {
                        found = true;
                        best_row = it->second;
                        best_col = i;
                    }
                }
                if (found && !(validate_only && std::holds_alternative<InsertConflict::Replace>(on_conflict.data))) {
                    // a row that repeats a value of an earlier row of the same statement: IGNORE skips it, ON DUPLICATE KEY UPDATE updates that earlier row
                    if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                        give_back_row_id();
                        continue;
                    }
                    if (auto* upd = std::get_if<InsertConflict::Update>(&on_conflict.data); upd && best_row < prepared.size() && !validate_only) {
                        // (that row is not in the table yet: the assignments change the row that is about to be inserted, each seeing the ones before it)
                        Row& target = prepared[best_row];
                        bool target_changed = false;
                        for (auto& [column, expr] : upd->assignments) {
                            std::string value = eval_arith_exact(target, bind_insert_values(expr, col_names, final_values));
                            if (auto column_def = std::find_if(schema.columns.begin(), schema.columns.end(), [&](const ColumnDef& c) { return c.name == column; });
                                column_def != schema.columns.end()) {
                                if (auto err = coerce_column_value(*column_def, value, row_no)) return StringResult::Err(*err);
                            }
                            if (target[column] != value) target_changed = true;
                            target[column] = std::move(value);
                        }
                        intra_updates_found++;
                        if (target_changed) intra_updates_changed++;
                        give_back_row_id();
                        continue;
                    }
                    return StringResult::Err("Duplicate value '" + final_values[best_col] + "' for column '" + col_names[best_col] + "'");
                }
                for (std::size_t i = 0; i < constraints.size(); i++) {
                    if (!constraints[i].primary_key && final_values[i] == EXECUTOR_NULL_VALUE) continue;
                    if (constraints[i].primary_key || constraints[i].unique) seen_unique_first[i].emplace(collation::key(final_values[i]), batch_rows_recorded);
                }
                batch_rows_recorded++;
            }
        }

        Row row;
        for (std::size_t i = 0; i < col_names.size(); i++) row[col_names[i]] = final_values[i];
        row["_xmin"] = std::to_string(tagging_txn_id(s));
        row["_xmax"] = "0";

        // the error for a foreign key whose parent is not there (nullopt: it is), also for a self-referencing table where the row names itself or an earlier row of
        // this very statement; a partitioned parent is read through its children, a deleted parent row is no parent
        auto fk_parent_missing = [&](const ForeignKey& fk, const std::string& val) -> std::optional<std::string> {
            auto violation = fk_child_violation(s, fk, val);
            if (!violation) return std::nullopt;
            const std::string& ref_col = fk.ref_column;
            auto names_val = [&](const Row& r) { auto rit = r.find(ref_col); return rit != r.end() && collation::equal(rit->second, val); };
            const bool in_statement = fk.ref_table == table && (names_val(row) || std::any_of(prepared.begin(), prepared.end(), names_val));
            return in_statement ? std::nullopt : violation;
        };

        // (the checks of the columns come before the foreign key, as in MySQL: a row that fails one of them never reaches the table, so it burns no AUTO_INCREMENT number)
        for (auto& col : schema.columns) {
            if (col.check_expr && !eval_check_expr(*col.check_expr, row)) {
                if (row_no == 1) burn_reservation = false; // (a check of the columns is made before the row is written: a first row that fails it burns nothing)
                return StringResult::Err("CHECK constraint violated on column '" + col.name + "': " + *col.check_expr);
            }
        }
        for (auto& check : schema.check_constraints) {
            if (!eval_check_expr(check.expression, row)) {
                if (row_no == 1) burn_reservation = false;
                return StringResult::Err("CHECK constraint '" + check.name.value_or(check.expression) + "' violated");
            }
        }

        for (auto& col : schema.columns) {
            if (skip_fk_checks) break; // exec_restore()만 사용 -- executor.hpp의 필드 주석 참고
            if (col.foreign_keys.empty()) continue;
            auto it = row.find(col.name);
            std::string val = it != row.end() ? it->second : std::string();
            if (val == EXECUTOR_NULL_VALUE) continue;
            for (auto& fk : col.foreign_keys) {
                if (auto violation = fk_parent_missing(fk, val)) return StringResult::Err(*violation);
            }
        }

        if (validate_only) { // REPLACE's pass over the rows before it deletes anything: the checks above, no locks, no writes
            prepared.push_back(std::move(row));
            continue;
        }

        // Whenever this row lets go of the data lock to wait (for a gap lock, for a claim), the duplicate checks it has made are out of date: another
        // session may have written the very value in the meantime. A key is claimed AFTER its check, so a claim granted after such a wait is looked at again.
        bool dropped_shared = false;

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
                    dropped_shared = true;
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
                    dropped_shared = true;
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
                }
                if (dropped_shared) { // (this wait, or an earlier one of the row: the value may be there now)
                    if (auto tit = s.tables.find(table); tit != s.tables.end()) {
                        for (auto& r : tit->second) {
                            if (!is_visible(r)) continue;
                            auto rit = r.find(gap_pk_col);
                            if (rit != r.end() && collation::equal(rit->second, pk_it->second)) {
                                return StringResult::Err("Duplicate value '" + pk_it->second + "' for column '" + gap_pk_col + "'");
                            }
                        }
                    }
                }
            }
        }

        // The claim of the PRIMARY KEY value above, for the other keys of the row: a UNIQUE column, a UNIQUE (a, b) key, a composite PRIMARY KEY
        // (Executor::unique_key_claims). Two INSERTs of one value both pass the duplicate check -- neither row is in the table yet -- so they meet here:
        // the second waits for the first to finish (its row is there then) and looks for the value again. A statement with a unique value of its own
        // claims it without waiting; the claims are let go at the end of the statement (a transaction's at its COMMIT), like the PRIMARY KEY's.
        bool skip_after_claims = false;
        for (auto& claim : unique_key_claims(schema, table, row)) {
            LockResult lr = s.lock_mgr.acquire(claim.lock_table, claim.lock_key, claim_txn_id);
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (INSERT '" + table + "'). Transaction " + std::to_string(claim_txn_id) + " aborted.");
            }
            if (lr.kind == LockResult::Kind::Conflict) {
                insert_read_lock = DataLockGuard{}; // (never block while holding the data lock, nor the table lock: as for the PRIMARY KEY above)
                release_table_locks_for_block();
                lr = block_on_row(s.lock_mgr, claim.lock_table, claim.lock_key, claim_txn_id, /*exclusive=*/true, lock_deadline);
                reacquire_table_locks_after_block();
                insert_read_lock = acquire_table_data_locks(s, insert_read_tables, /*exclusive=*/false);
                dropped_shared = true;
                if (lr.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                              std::to_string(lr.holder) + " (INSERT '" + table + "'). Transaction " + std::to_string(claim_txn_id) + " aborted.");
                }
                if (lr.kind != LockResult::Kind::Granted) {
                    return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; " + claim.message + " is being concurrently inserted/updated by transaction " +
                                              std::to_string(lr.holder) + ".");
                }
            }
            if (!dropped_shared) continue; // (the data lock was never let go: nobody could have written the value since the check)
            const Row* holder = nullptr; // whoever we waited for (or whoever wrote while we waited for something else) may have written the very value
            if (auto tit = s.tables.find(table); tit != s.tables.end()) {
                for (auto& r : tit->second) {
                    if (is_visible(r) && row_holds_claim(claim, r)) {
                        holder = &r;
                        break;
                    }
                }
            }
            if (!holder) continue;
            if (std::holds_alternative<InsertConflict::Ignore>(on_conflict.data)) {
                skip_after_claims = true;
            } else if (auto* upd = std::get_if<InsertConflict::Update>(&on_conflict.data)) {
                queue_update(*holder, upd->assignments);
                skip_after_claims = true;
            } else {
                return StringResult::Err(claim.message);
            }
            break;
        }
        if (skip_after_claims) continue;

        // The parent of each foreign key: the value the row refers to is claimed (shared: any number of children may), so that a DELETE / UPDATE of the parent row --
        // which claims the value exclusively before it looks for children -- and this INSERT meet; the one that comes second waits. After a wait the parent is looked
        // for again: the row may be gone.
        if (!skip_fk_checks) {
            for (auto& col : schema.columns) {
                for (auto& fk : col.foreign_keys) { // (each foreign key of the column has its own parent to claim)
                    auto it = row.find(col.name);
                    if (it == row.end() || it->second == EXECUTOR_NULL_VALUE) continue;
                    const std::string claim_table = fk_claim_table(fk.ref_table, fk.ref_column);
                    LockResult lr = s.lock_mgr.acquire_shared(claim_table, it->second, claim_txn_id);
                    if (lr.kind == LockResult::Kind::Deadlock) {
                        return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                                  std::to_string(lr.holder) + " (INSERT '" + table + "'). Transaction " + std::to_string(claim_txn_id) + " aborted.");
                    }
                    if (lr.kind == LockResult::Kind::Conflict) {
                        insert_read_lock = DataLockGuard{};
                        release_table_locks_for_block();
                        lr = block_on_row(s.lock_mgr, claim_table, it->second, claim_txn_id, /*exclusive=*/false, lock_deadline);
                        reacquire_table_locks_after_block();
                        insert_read_lock = acquire_table_data_locks(s, insert_read_tables, /*exclusive=*/false);
                        dropped_shared = true;
                        if (lr.kind == LockResult::Kind::Deadlock) {
                            return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                                      std::to_string(lr.holder) + " (INSERT '" + table + "'). Transaction " + std::to_string(claim_txn_id) + " aborted.");
                        }
                        if (lr.kind != LockResult::Kind::Granted) {
                            return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; the parent value '" + it->second + "' in '" + fk.ref_table +
                                                      "' is being deleted or changed by transaction " + std::to_string(lr.holder) + ".");
                        }
                    }
                    if (dropped_shared) {
                        if (auto violation = fk_parent_missing(fk, it->second)) return StringResult::Err(*violation);
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
    if (validate_only) {
        counter_guard.kept = true;
        return StringResult::Ok("");
    }

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
        unique_probe_inserted(s, table, schema, prepared); // (before the rows are moved into the table)

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

    std::size_t updated = 0, found = 0;
    for (auto& [cond, assigns] : pending_updates) {
        auto r = exec_update(s, table, assigns, cond, std::nullopt);
        if (r.is_err()) return r;
        updated += static_cast<std::size_t>(std::strtoull(r.value().c_str(), nullptr, 10));
        found += matched_rows_ > 0 ? static_cast<std::size_t>(matched_rows_) : 0;
    }

    maybe_auto_checkpoint(s);
    counter_guard.kept = true;
    // ON DUPLICATE KEY UPDATE affects 1 for a row it inserted and 2 for a row it changed (0 for one it set to what it holds, 1 with CLIENT_FOUND_ROWS)
    const bool odku = !pending_updates.empty() || intra_updates_found != 0;
    affected_rows_ = odku ? static_cast<std::int64_t>(inserted + 2 * (updated + intra_updates_changed)) : -1;
    matched_rows_ = odku ? static_cast<std::int64_t>(inserted + updated + intra_updates_changed + found + intra_updates_found) : -1;
    if (returning) return StringResult::Ok(format_returning_rows(returning_rows, *returning));
    if (!pending_updates.empty()) return StringResult::Ok(std::to_string(inserted) + " row(s) inserted, " + std::to_string(updated) + " row(s) updated.");
    return StringResult::Ok(std::to_string(inserted) + " row(s) inserted.");
}

} // namespace engine
