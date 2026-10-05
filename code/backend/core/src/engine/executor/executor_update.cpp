// Faithful port of the UPDATE path from rusql-core/src/engine/executor.rs (Phase 8b):
// exec_update, exec_update_inner (row matching, lock acquisition, ENUM/SET/CHECK
// validation, incremental index maintenance, ON UPDATE FK cascade). Row matching for
// `matching_pks` uses matches_condition_with_subquery (Phase 8c), matching the Rust
// original exactly — UPDATE, unlike DELETE, always uses the subquery-aware matcher
// with no fast-path/slow-path split.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <chrono>

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
                                    std::optional<CondExpr> condition, std::optional<std::vector<SelectColumn>> returning,
                                    const PerRowValues* per_row) {
    if (s.views.count(table)) {
        if (auto resolved = resolve_updatable_view(s, table)) {
            auto merged_cond = merge_conditions(resolved->second, condition);
            return exec_update(s, resolved->first, assignments, merged_cond, returning, per_row);
        }
        return StringResult::Err("View '" + strip_db_prefix(table) + "' is not updatable");
    }

    const bool before_trigger = has_trigger(s, table, "BEFORE", "UPDATE"), after_trigger = has_trigger(s, table, "AFTER", "UPDATE");
    std::vector<TriggerRow> trigger_rows;
    if (before_trigger || after_trigger) trigger_rows = trigger_rows_for(s, table, condition, &assignments, per_row);
    if (before_trigger) {
        if (auto tr = fire_triggers(s, table, "BEFORE", "UPDATE", trigger_rows); tr.is_err()) return tr;
    }
    auto result = exec_update_inner(s, table, assignments, condition, returning, per_row);
    if (result.is_ok() && after_trigger) {
        if (auto tr = fire_triggers(s, table, "AFTER", "UPDATE", trigger_rows); tr.is_err()) return tr;
    }
    return result;
}

StringResult Executor::exec_update_inner(SharedDatabase& s, const std::string& table,
                                          const std::vector<std::pair<std::string, ArithExpr>>& assignments,
                                          const std::optional<CondExpr>& condition,
                                          const std::optional<std::vector<SelectColumn>>& returning, const PerRowValues* per_row) {
    const TableSchema* schema0 = s.catalog.get_table(table);
    if (!schema0) return StringResult::Err("Table '" + table + "' not found");
    std::string pk_col = "id";
    std::vector<std::string> pk_cols;
    for (auto& c : schema0->columns) {
        if (c.primary_key) pk_cols.push_back(c.name);
    }
    if (pk_cols.empty()) pk_cols.push_back("id");
    pk_col = pk_cols.front();
    // a column the table does not have used to be written into every row it matched (and read back by name), under "1 row(s) updated"
    for (auto& [c, _] : assignments) {
        if (std::none_of(schema0->columns.begin(), schema0->columns.end(), [&](const ColumnDef& def) { return def.name == c; })) {
            return StringResult::Err("Unknown column '" + c + "' in 'field list'");
        }
    }
    std::vector<std::string> assigned_cols; // the columns this statement sets
    for (auto& [c, _] : assignments) assigned_cols.push_back(c);
    // the parent rows of the foreign keys among them are read while the rows are rewritten
    std::vector<std::string> fk_parents = fk_parent_tables(s, *schema0, assigned_cols);
    // ... and the child rows that reference a column it changes (ON UPDATE RESTRICT)
    for (auto& t : fk_restrict_children(s, table, assigned_cols)) fk_parents.push_back(t);

    // Row-level-concurrency Stage 4: reconstruct the equivalent Statement::Update to
    // reuse table_lock_set_for's exact table-closure computation (target + FK parent/
    // child neighbors + any condition-subquery tables) -- the SAME set already used for
    // table_locks at the dispatch level. Recomputing it here is deterministic (no
    // concurrent DDL can be touching views/triggers/schema while we hold structural+
    // table_locks) and far less error-prone than re-walking CondExpr subqueries by hand.
    // Split into `table` itself (whose table_data_locks needs to escalate to EXCLUSIVE
    // for brief phases below) and the neighbor tables (FK parents/children + subquery
    // tables). NOTE (correctness fix, found via concurrent-reader stress testing): this
    // set is acquired TWICE, in two different modes, at two different points below --
    // SHARED only around the early candidate-matching phase (where a condition subquery
    // might read one of these tables), then released, then re-acquired EXCLUSIVE around
    // the FK cascade phase (which field-mutates these tables in place -- NOT safe under
    // SHARED, same reasoning as the primary table's own mutation phase). It is never
    // held in both modes at once, so there's no risk of a shared-then-exclusive
    // self-deadlock on the same std::shared_mutex.
    Statement lock_probe_stmt;
    lock_probe_stmt.data = Statement::Update{table, assignments, condition, returning};
    std::vector<std::string> neighbor_tables;
    if (auto extra = table_lock_set_for(s, lock_probe_stmt)) {
        for (auto& t : *extra) {
            if (t != table) neighbor_tables.push_back(t);
        }
    }

    // One claim-id for this whole statement (see the identical pattern + rationale in
    // exec_insert_inner): reuses the active explicit transaction's id, or a fresh
    // one-off id (released by RowClaimGuard at statement end) for autocommit. Extends
    // row claims to autocommit UPDATEs too, not just explicit transactions -- needed now
    // that two autocommit UPDATEs on the same table can genuinely run concurrently.
    std::uint64_t claim_txn_id_setup = txn.current_txn_id();
    bool autocommit_claim = (claim_txn_id_setup == 0);
    std::uint64_t claim_txn_id = autocommit_claim ? s.txn_io->next_id() : claim_txn_id_setup;
    RowClaimGuard row_claim_guard(s.lock_mgr, claim_txn_id, /*owns=*/autocommit_claim);
    // Real-blocking-wait stage: one deadline for the WHOLE statement -- see
    // exec_insert_inner's identical field for the rationale.
    auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(lock_wait_timeout_ms);

    // Composite identity key for WHERE-condition row matching. On a composite-PK table,
    // distinct rows can share the same value in just the leading PK column, so reducing
    // a row's identity to `pk_col` alone (as the locking/undo-log/PK-index code below
    // still does, unchanged) would make matching_pks conflate them -- an UPDATE would
    // then touch every row sharing that one column's value, not just the row(s) that
    // actually satisfied `condition`. \x00-joins every PK column's value, mirroring the
    // composite index convention (btree.cpp's cmp_keys segment split).
    auto match_key = [&pk_cols](const Row& r) {
        std::string key;
        for (std::size_t i = 0; i < pk_cols.size(); i++) {
            if (i) key += '\x00';
            auto it = r.find(pk_cols[i]);
            key += (it != r.end() ? it->second : std::string());
        }
        return key;
    };

    // which rows this statement updates: those the WHERE matches, or the ones the caller listed by key
    auto selected = [&](const Row& r) { return per_row ? per_row->count(match_key(r)) > 0 : matches_condexpr(r, condition); };

    auto tit0 = s.tables.find(table);
    if (tit0 == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");

    // MVCC: my_id stays FIXED for the whole statement (identifies this statement's own
    // writes -- is_visible_for_read's `xmin != ctx.self_txn_id` check depends on it never
    // changing across a retry, even though tagging_txn_id() would hand out a fresh id on
    // every call for autocommit). cur_txn, by contrast, is fine to read once too (it's
    // just the current explicit-transaction id, unaffected by retries).
    std::uint64_t my_id = tagging_txn_id(s);
    std::uint64_t cur_txn = txn.current_txn_id();

    // Gap lock: only under RR/Serializable and only for single-column PK tables (V1
    // scope), matching the same gating used at the FOR UPDATE/FOR SHARE call sites.
    // Real-blocking-wait stage: registered once here, before the retry loop below --
    // acquire_gap has no dedup, so calling it again on every retry attempt would leave
    // duplicate gap-lock entries.
    if (cur_txn != 0 && pk_cols.size() == 1 &&
        (txn.isolation_level() == IsolationLevel::RepeatableRead || txn.isolation_level() == IsolationLevel::Serializable)) {
        GapRange range = extract_pk_gap_range(condition, pk_col);
        s.lock_mgr.acquire_gap(table, range.lo, range.lo_inclusive, range.hi, range.hi_inclusive, cur_txn);
    }

    std::size_t count = 0;
    struct UndoEntry {
        std::string key, old_json, new_json;
    };
    std::vector<UndoEntry> undo_entries;
    std::vector<Row> old_rows_all, new_rows_all; // the old and new image of every updated row, in undo_entries order
    std::vector<std::string> final_new_pks;       // the primary key of every new version, in undo_entries order
    std::vector<char> final_new_pk_found;         // ... and whether the new row has that column at all
    // Rows as images are only needed to rewrite a secondary, hash or composite index; the PK B+Tree takes the JSON text the
    // new version already has. (A copy of a row is ~5 us -- two per row doubled a 100,000-row UPDATE's version building.)
    bool need_images = false;
    for (auto& [k, ci] : s.composite_indexes) {
        if (ci.table == table) need_images = true;
    }
    for (auto& [name, meta] : s.index_meta) {
        if (meta.first == table) need_images = true;
    }
    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first == table) need_images = true;
    }
    std::unordered_set<std::string> matching_pks; // final (successful-attempt) value used after the loop too (RETURNING)

    // Real-blocking-wait stage: the candidate scan (SHARED) and the probe+mutate phase
    // (EXCLUSIVE) both restart from scratch on a row-lock conflict -- release table_lock
    // first (must never block while holding table_data_locks/table_locks -- see
    // block_on_row's doc comment), block on just the ONE contested row, then redo
    // everything. write_ctx is recomputed fresh every attempt (self_txn_id stays my_id,
    // but cutoff/in_progress reflect whatever's committed as of NOW) -- important
    // specifically because we may have just woken up from waiting on a transaction that,
    // in the meantime, committed, and UPDATE must always match against the latest
    // committed state (see the original MVCC comment this replaces).
    // Candidate search without cloning the table. The old code copied EVERY visible row (a
    // std::map<string,string> per row) into a vector and re-derived each row's composite key
    // string in two more full passes -- ~3us/row, so 60ms for a 20,000-row table even for
    // `WHERE id = 5`. When the condition has no subquery (the only reason the clone existed:
    // exec_select may reshuffle s.tables underneath a live reference) rows are matched in
    // place and only their POSITIONS are kept; the exclusive phase re-verifies each position
    // (still visible, same composite key) and rescans from scratch if anything shifted in
    // between (a concurrent vacuum/rollback erasing rows). For a bare `pk = literal` on a
    // single-column PK the position comes from the row_pk_pos cache in O(1).
    const bool positions_ok = !condition_has_subquery(condition);
    const std::optional<std::string> pk_eq =
        (positions_ok && pk_cols.size() == 1) ? extract_pk_eq_value(condition, pk_col) : std::nullopt;
    bool bypass_pk_cache = false;  // set after a stale-position retry so the retry really rescans
    bool rebuild_pk_cache = false; // pk-equality missed the cache although the row exists -> repopulate it once

    for (;;) {
        std::vector<Row> candidate_rows;
        std::vector<std::size_t> cand_pos;
        std::vector<std::string> cand_key;
        matching_pks.clear();
        SnapshotCtx write_ctx{my_id, s.txn_io->peek_next_id(), *s.active_txn_ids->lock()};

        // Row-level-concurrency Stage 4: SHARED on `table` for the whole candidate-scan/
        // condition-matching phase below -- nothing here resizes s.tables[table].
        // neighbor_lock (FK/subquery tables, SHARED) only needs to cover THIS block --
        // matches_condition_with_subquery is the only thing here that could touch one of
        // those tables.
        {
            auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/false);
            auto neighbor_lock = acquire_table_data_locks(s, neighbor_tables, /*exclusive=*/false);
            // Cloned first (not iterated in place): matches_condition_with_subquery can
            // invoke exec_select, which for FROM-subqueries/views temporarily inserts/
            // erases entries in s.tables — holding a live reference into s.tables across
            // that call would risk iterator/reference invalidation on rehash.
            if (positions_ok) {
                const auto& rows0 = tit0->second;
                bool via_cache = false;
                if (pk_eq && !bypass_pk_cache) {
                    if (auto mit = s.row_pk_pos.find(table); mit != s.row_pk_pos.end()) {
                        if (auto pit = mit->second.find(*pk_eq); pit != mit->second.end()) {
                            std::size_t pos = pit->second;
                            if (pos < rows0.size() && is_visible_for_read(rows0[pos], write_ctx)) {
                                auto kit = rows0[pos].find(pk_col);
                                if (kit != rows0[pos].end() && kit->second == *pk_eq) {
                                    via_cache = true; // the (single) visible version of this pk
                                    if (selected(rows0[pos])) {
                                        cand_pos.push_back(pos);
                                        cand_key.push_back(match_key(rows0[pos]));
                                    }
                                }
                            }
                        }
                    }
                }
                // Not a bare pk lookup: ask the planner whether an index (secondary, hash, PK range,
                // an indexable leaf of an AND, ...) narrows the search. The index only proposes
                // candidates -- each is checked against the real row (see executor_dml_index.cpp);
                // anything doubtful returns "not usable" and the scan below runs as it always did.
                bool via_index = false;
                if (!via_cache && !bypass_pk_cache && pk_cols.size() == 1) {
                    auto hit = dml_index_positions(s, table, condition, pk_col,
                                                   [&](const Row& r) { return is_visible_for_read(r, write_ctx); }, cur_txn);
                    if (hit.usable) {
                        via_index = true;
                        for (auto p : hit.positions) {
                            cand_pos.push_back(p);
                            cand_key.push_back(match_key(rows0[p]));
                        }
                    } else if (hit.cache_gap) {
                        rebuild_pk_cache = true; // the exclusive phase below repopulates row_pk_pos
                    }
                }
                if (!via_cache && !via_index) {
                    for (std::size_t i = 0; i < rows0.size(); i++) {
                        const Row& r = rows0[i];
                        if (!is_visible_for_read(r, write_ctx)) continue;
                        if (selected(r)) {
                            cand_pos.push_back(i);
                            cand_key.push_back(match_key(r));
                        }
                    }
                    if (pk_eq && !cand_pos.empty()) rebuild_pk_cache = true;
                }
                // RETURNING re-selects the updated rows by composite key after the statement.
                if (returning) {
                    for (auto& k : cand_key) matching_pks.insert(k);
                }
            } else {
                for (auto& r : tit0->second) {
                    if (is_visible_for_read(r, write_ctx)) candidate_rows.push_back(r);
                }
                for (auto& r : candidate_rows) {
                    if (matches_condition_with_subquery(s, r, condition)) {
                        matching_pks.insert(match_key(r));
                    }
                }
            }
        } // table_lock (SHARED) released here -- candidate scan + condition matching only.

        // Row-level-concurrency Stage 4 correctness fix (found via concurrent-reader
        // monotonicity stress testing): the mutate pass below stamps each matched row's
        // OLD version dead (`row["_xmax"] = my_id`) -- which, for an is_visible_for_read
        // check, makes that PK's old row invisible IMMEDIATELY (autocommit's one-off my_id
        // is never in active_txn_ids, so it reads as already-committed) -- but the
        // corresponding NEW version isn't actually appended to `rows` until rows.insert(...)
        // at the very end. Splitting these into a SHARED mutate-phase + a separately
        // re-acquired EXCLUSIVE insert-phase (the original Stage 4 design) left a real
        // window where a concurrent reader, running during the SHARED phase, could observe
        // NEITHER version for that PK -- a phantom disappearance, not just staleness. Fix:
        // the mutate pass (which stamps old rows dead) and rows.insert (which makes new
        // rows visible) must be ONE atomic EXCLUSIVE section so no reader ever sees the
        // gap.
        //
        // Real-blocking-wait stage addition: further split that EXCLUSIVE section into a
        // PROBE pass (claim every matching row, timeout=0, mutate NOTHING) and a MUTATE
        // pass that only runs if every claim in the probe succeeded. This preserves the
        // atomicity invariant above exactly -- on a conflict, table_lock releases having
        // mutated ZERO rows in this attempt, so there is no partial state to expose to a
        // reader and nothing to unwind before retrying (a re-request for an already-held
        // claim from an earlier attempt is free/re-entrant in LockManager).
        std::vector<Row> new_versions; // appended to `rows` only if the whole probe succeeds
        std::vector<Row> attempt_old_images, attempt_new_images; // copies of every row's image before and after, for index maintenance
        std::vector<std::string> new_version_pks;
        std::vector<char> new_version_pk_found;
        std::vector<UndoEntry> attempt_undo_entries;
        bool conflict = false;
        bool stale_positions = false;
        std::string conflict_key;
        {
            auto table_lock = acquire_table_data_locks_mixed(s, {table}, fk_parents);
            auto& rows = tit0->second;

            // The snapshot taken at the top of this attempt is stale by now: this thread may have waited for
            // the exclusive lock while another statement updated the very same row and finished. That
            // statement's id is newer than the old cutoff, so the OLD snapshot still sees the version it
            // killed as alive -- the candidate passed verification, was updated a second time, and the row
            // forked into two live versions (found by running the concurrent-increment test under heavy CPU
            // load: `(id=1 v=179) (id=1 v=479)`). Verify against the state as of NOW; a candidate that
            // changed in between then fails verification and the attempt restarts (stale_positions).
            write_ctx = SnapshotCtx{my_id, s.txn_io->peek_next_id(), *s.active_txn_ids->lock()};

            // The rows this statement will touch, resolved once for both passes below.
            std::vector<Row*> targets;
            if (positions_ok) {
                for (std::size_t i = 0; i < cand_pos.size(); i++) {
                    std::size_t pos = cand_pos[i];
                    if (pos >= rows.size() || !is_visible_for_read(rows[pos], write_ctx) || match_key(rows[pos]) != cand_key[i]) {
                        stale_positions = true; // rows shifted/changed since the shared scan -- start over
                        break;
                    }
                    targets.push_back(&rows[pos]);
                }
            } else {
                for (auto& row : rows) {
                    if (!matching_pks.count(match_key(row))) continue;
                    if (!is_visible_for_read(row, write_ctx)) continue;
                    targets.push_back(&row);
                }
            }
            if (stale_positions) targets.clear();

            for (Row* rowp : targets) {
                Row& row = *rowp;
                auto pkit = row.find(pk_col);
                std::string row_pk = pkit != row.end() ? pkit->second : std::string();
                // Row-level-concurrency Stage 4: unconditional now (was `if (cur_txn !=
                // 0)`, explicit-transaction-only) -- claim_txn_id is always valid (a
                // fresh one-off id for autocommit), so autocommit UPDATEs racing the same
                // row are told apart too.
                LockResult lr = s.lock_mgr.acquire(table, row_pk, claim_txn_id);
                if (lr.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                              std::to_string(lr.holder) + " (UPDATE '" + table + "'. Transaction " + std::to_string(claim_txn_id) +
                                              " aborted.");
                }
                if (lr.kind == LockResult::Kind::Conflict) {
                    conflict = true;
                    conflict_key = row_pk;
                    break;
                }
            }

            if (!conflict && !stale_positions) {
                for (Row* rowp : targets) {
                    // (`targets` already excludes stale dead versions sharing this PK with
                    // the live matched row -- only the live version is ever re-updated.)
                    Row& row = *rowp;
                    auto pkit = row.find(pk_col);
                    std::string row_pk = pkit != row.end() ? pkit->second : std::string();
                    const std::string& key = row_pk;

                    std::string old_json = row_to_json(row);

                    // MVCC: UPDATE now creates a new physical version instead of mutating
                    // `row` in place -- `new_row` holds it; `row` (the OLD version) only
                    // gets its _xmax stamped below, once new_row has passed every
                    // validation check.
                    Row new_row = row;
                    std::vector<std::pair<std::string, std::string>> new_vals;
                    if (per_row) {
                        for (auto& [col, v] : per_row->at(match_key(row))) new_vals.emplace_back(col, v);
                    } else {
                        for (auto& [col, expr] : assignments) new_vals.emplace_back(col, eval_arith(row, expr));
                    }

                    if (auto* schema = s.catalog.get_table(table)) {
                        for (auto& [col_name, val] : new_vals) {
                            if (val == EXECUTOR_NULL_VALUE) continue;
                            auto cit = std::find_if(schema->columns.begin(), schema->columns.end(),
                                                     [&](const ColumnDef& c) { return c.name == col_name; });
                            if (cit == schema->columns.end()) continue;
                            if (auto err = coerce_column_value(*cit, val, new_versions.size() + 1)) return StringResult::Err(*err);
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

                    for (auto& [col, val] : new_vals) new_row[col] = val;

                    if (auto* schema = s.catalog.get_table(table)) {
                        if (auto violation = rewritten_row_violation(s, *schema, new_row, assigned_cols, &row)) return StringResult::Err(*violation);
                        if (auto violation = update_restrict_violation(s, table, row, new_row, assigned_cols)) return StringResult::Err(*violation);
                        for (auto& col : schema->columns) {
                            if (col.check_expr && !eval_check_expr(*col.check_expr, new_row)) {
                                return StringResult::Err("CHECK constraint violated on column '" + col.name + "': " + *col.check_expr);
                            }
                        }
                        for (auto& check : schema->check_constraints) {
                            if (!eval_check_expr(check.expression, new_row)) {
                                return StringResult::Err("CHECK constraint '" + check.name.value_or(check.expression) + "' violated");
                            }
                        }
                    }

                    new_row["_xmin"] = std::to_string(my_id);
                    new_row["_xmax"] = "0";
                    {
                        auto npk = new_row.find(pk_col);
                        new_version_pks.push_back(npk != new_row.end() ? npk->second : std::string());
                        new_version_pk_found.push_back(npk != new_row.end() ? 1 : 0);
                    }

                    attempt_undo_entries.push_back({key, old_json, row_to_json(new_row)});
                    if (need_images) {
                        attempt_old_images.push_back(row);
                        attempt_new_images.push_back(new_row);
                    }
                    new_versions.push_back(std::move(new_row));
                }
                // Nothing is stamped or inserted until EVERY new version has passed every check. Before,
                // each old version was stamped dead inside the loop above, so a CHECK/ENUM error on a LATER
                // row returned with the earlier rows already dead and their new versions thrown away --
                // `UPDATE t SET v = v + 8` failing a CHECK on row 2 silently deleted row 1.
                // (Targets point into `rows`, so this must stay before the insert below.)
                {
                    std::vector<const Row*> olds(targets.begin(), targets.end());
                    if (auto violation = update_unique_violation(s, table, olds, new_versions)) return StringResult::Err(*violation);
                }
                for (Row* rowp : targets) (*rowp)["_xmax"] = std::to_string(my_id);
                const std::size_t first_new_pos = rows.size();
                rows.insert(rows.end(), std::make_move_iterator(new_versions.begin()), std::make_move_iterator(new_versions.end()));
                if (pk_cols.size() == 1) {
                    // Keep the pk -> position cache pointing at the newest live version
                    // (always re-validated on use, so this is purely an accelerator).
                    auto& pos_map = s.row_pk_pos[table];
                    if (rebuild_pk_cache) {
                        pos_map.clear();
                        for (std::size_t i = 0; i < rows.size(); i++) {
                            if (!is_visible_for_read(rows[i], write_ctx)) continue;
                            auto kit = rows[i].find(pk_col);
                            if (kit != rows[i].end()) pos_map[kit->second] = i;
                        }
                    } else {
                        // Erase every old key first, then write the new ones: with a chain such as
                        // `SET id = id + 1` a per-row erase would remove the entry just written for the key
                        // the previous row moved into.
                        for (std::size_t i = 0; i < attempt_undo_entries.size(); i++) {
                            if (new_version_pks[i] != attempt_undo_entries[i].key) pos_map.erase(attempt_undo_entries[i].key);
                        }
                        for (std::size_t i = 0; i < attempt_undo_entries.size(); i++) pos_map[new_version_pks[i]] = first_new_pos + i;
                    }
                }
                // Row-level-concurrency Stage 4/5 correctness fix (found via concurrent-
                // reader monotonicity stress testing): invalidate the query cache HERE,
                // still holding table_data_locks EXCLUSIVE -- execute_sql's own
                // post-execute invalidate_table call runs too late (after this lock has
                // already released), leaving a real window for a reader to hit a stale
                // cache entry. See exec_insert_inner's identical comment.
                s.query_cache.invalidate_table(table);
            }
        } // table_lock (EXCLUSIVE) released here -- probe + mutate + insert as one atomic unit.

        if (stale_positions) {
            bypass_pk_cache = true; // retry with a genuine rescan
            continue;
        }

        if (!conflict) {
            count = attempt_undo_entries.size();
            undo_entries = std::move(attempt_undo_entries);
            old_rows_all = std::move(attempt_old_images);
            new_rows_all = std::move(attempt_new_images);
            final_new_pks = std::move(new_version_pks);
            final_new_pk_found = std::move(new_version_pk_found);
            break;
        }

        // Real-blocking-wait stage, second correctness fix: table_locks[table] (SHARED,
        // held for this whole statement by execute()'s dispatcher) must ALSO be released
        // before a genuine blocking wait -- not just table_data_locks (already released,
        // by scope, above) -- or a concurrent COMMIT needing table_locks EXCLUSIVE on the
        // same table can never proceed, and this statement never gets unblocked either
        // (see release_table_locks_for_block's doc comment in executor.hpp).
        release_table_locks_for_block();
        auto lr2 = block_on_row(s.lock_mgr, table, conflict_key, claim_txn_id, /*exclusive=*/true, lock_deadline);
        reacquire_table_locks_after_block();
        if (lr2.kind == LockResult::Kind::Deadlock) {
            return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                      std::to_string(lr2.holder) + " (UPDATE '" + table + "'. Transaction " + std::to_string(claim_txn_id) +
                                      " aborted.");
        }
        if (lr2.kind != LockResult::Kind::Granted) {
            return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + conflict_key + "' in '" + table +
                                      "' is held by transaction " + std::to_string(lr2.holder) + ". Cannot UPDATE.");
        }
        // else: granted -- loop back, redo the candidate scan and probe+mutate afresh.
    }

    for (auto& u : undo_entries) txn.log_update(table, u.key, u.old_json, u.new_json);

    // Incremental index maintenance (PK B+Tree + composite indexes): previously this
    // cloned the whole table and fully rebuilt both from scratch on every UPDATE,
    // regardless of how few rows actually changed -- replaced with per-row
    // remove-old-key/insert-new-key updates, matching what index_remove_row/
    // index_insert_row already do for secondary/hash indexes just below. `u.key` is the
    // OLD row's pk_col value (captured before this row was touched); the new key is
    // read from `new_row` rather than assumed equal to `u.key`, since the PK column
    // itself can be part of the UPDATE's SET list.
    std::vector<std::string> comp_keys;
    for (auto& [k, ci] : s.composite_indexes) {
        if (ci.table == table) comp_keys.push_back(k);
    }

    // (old_rows_all / new_rows_all were filled while the new versions were built: re-parsing the JSON images of every row
    // took a quarter of a 100,000-row UPDATE)
    auto new_pk_of = [&](std::size_t i) { return final_new_pk_found[i] ? final_new_pks[i] : undo_entries[i].key; };
    bool pk_changes = false;
    for (std::size_t i = 0; i < undo_entries.size(); i++) {
        if (new_pk_of(i) != undo_entries[i].key) {
            pk_changes = true;
            break;
        }
    }

    if (pk_changes) {
        // When a statement changes primary keys, per-row remove-old/insert-new is order-dependent:
        // `SET id = id + 1` on ids 2 and 3 first inserts the entry for the new key 3 (the old row 2), then
        // removes "the old key 3" for the old row 3 -- which erases the entry it has just inserted, and the
        // index loses a live row. So every old entry leaves first, and only then do the new ones enter.
        for (std::size_t i = 0; i < undo_entries.size(); i++) {
            if (new_pk_of(i) != undo_entries[i].key) {
                if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) idx_it->second.remove(undo_entries[i].key);
            }
            if (need_images) {
                for (auto& k : comp_keys) {
                    auto& ci = s.composite_indexes.at(k);
                    if (ci.key_from_row(old_rows_all[i]) != ci.key_from_row(new_rows_all[i])) ci.remove_row(old_rows_all[i]);
                }
            }
        }
        if (need_images) index_replace_rows(s, table, old_rows_all, new_rows_all, pk_col); // secondary + hash: leaving and entering in one pass
        for (std::size_t i = 0; i < undo_entries.size(); i++) {
            if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
                idx_it->second.insert(new_pk_of(i), undo_entries[i].new_json);
            }
            if (need_images) {
                for (auto& k : comp_keys) s.composite_indexes.at(k).insert_row(new_rows_all[i]);
            }
        }
    } else {
        for (std::size_t i = 0; i < undo_entries.size(); i++) {
            if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
                // Row-level-concurrency Stage 4/5 correctness fix (found via concurrent-reader
                // monotonicity stress testing -- root cause of the "0 rows returned" phantom): the key is
                // unchanged here, so ONE insert() overwrites the PK B+Tree entry atomically -- no remove()
                // first, which would leave a window in which AccessPath::PkPoint (lock-free by design)
                // finds no entry for a row that exists.
                idx_it->second.insert(new_pk_of(i), undo_entries[i].new_json);
            }
            if (!need_images) continue;
            const Row& old_row = old_rows_all[i];
            const Row& new_row = new_rows_all[i];
            for (auto& k : comp_keys) {
                // Same atomicity reasoning for the composite indexes: CompositeIndex::insert_row overwrites an
                // existing key in place, so when none of the index's columns changed value remove_row() is skipped.
                auto& ci = s.composite_indexes.at(k);
                if (ci.key_from_row(old_row) != ci.key_from_row(new_row)) ci.remove_row(old_row);
                ci.insert_row(new_row);
            }
        }
        // Secondary / hash indexes: each affected bucket is parsed and rewritten once per statement, not once per row.
        if (need_images) index_replace_rows(s, table, old_rows_all, new_rows_all, pk_col);
    }

    std::vector<std::string> changed_cols;
    for (auto& [c, _] : assignments) changed_cols.push_back(c);

    std::vector<std::pair<std::string, std::vector<ColumnDef>>> other_tables;
    for (auto& [name, schema] : s.catalog.tables) {
        if (name != table) other_tables.emplace_back(name, schema.columns);
    }

    // Row-level-concurrency Stage 4: FK cascade previously took NO LockManager claim at
    // all on the child table's affected rows (safe only because the whole child table
    // was held exclusively via table_locks for the statement's duration). Now that
    // table_locks is SHARED, claim each affected row (single-column PK only, V1 scope --
    // same limitation as everywhere else in this codebase) before mutating it in place,
    // so a concurrent statement racing the SAME child row is told apart instead of both
    // writers silently interleaving. Correctness fix (found via concurrent-reader stress
    // testing): the field mutations below need `other_table`'s table_data_locks
    // EXCLUSIVE, not SHARED -- a plain SELECT scanning `other_table` takes no
    // LockManager claim at all and would otherwise be free to copy a Row while this
    // cascade is mid-mutation of that same Row (undefined behavior). Acquired fresh
    // here (the earlier neighbor_lock, SHARED, was already released after the
    // candidate-matching phase above) since this table set is now needed in a different
    // mode.
    auto single_pk_col = [&](const std::string& tbl) -> std::optional<std::string> {
        auto* sc = s.catalog.get_table(tbl);
        if (!sc) return std::nullopt;
        std::string col;
        std::size_t cnt = 0;
        for (auto& c : sc->columns) {
            if (c.primary_key) {
                if (cnt == 0) col = c.name;
                cnt++;
            }
        }
        return cnt == 1 ? std::optional<std::string>(col) : std::nullopt;
    };
    // Pre-existing bug (predates the real-blocking-wait stage, found while testing it):
    // FK cascade mutated s.tables[other_table] in place but never touched that table's PK
    // B+Tree (s.indexes[other_table], keyed by bare table name), secondary/hash indexes
    // (index_insert_row/index_remove_row), or composite indexes -- a PK-indexed point
    // lookup on the cascaded table kept serving stale data indefinitely, while a full scan
    // was correct. Mirrors the refresh pattern the primary table's own UPDATE already uses
    // just above (and exec_delete_inner's refresh_indexes_for_soft_delete), generalized to
    // take table/pk_col as parameters since cascade operates on `other_table`, not `table`.
    // A cascade never changes the cascaded row's OWN pk_col value (it only ever touches the
    // FK column pointing at `table`'s pk), so the PK B+Tree side only ever needs the
    // single-insert overwrite path, never remove()+insert() for a changed key.
    auto refresh_cascade_indexes = [&](const std::string& tbl, const std::string& pk_col, const Row& old_row, const Row& new_row) {
        auto key_it = new_row.find(pk_col);
        std::string key = key_it != new_row.end() ? key_it->second : std::string();
        if (auto idx_it = s.indexes.find(tbl); idx_it != s.indexes.end()) {
            idx_it->second.insert(key, row_to_json(new_row));
        }
        index_remove_row(s, tbl, old_row, pk_col);
        index_insert_row(s, tbl, new_row);
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table != tbl) continue;
            if (ci.key_from_row(old_row) == ci.key_from_row(new_row)) {
                ci.insert_row(new_row);
                continue;
            }
            ci.remove_row(old_row);
            ci.insert_row(new_row);
        }
    };
    // Real-blocking-wait stage: claim_cascade_row() itself never blocks -- it only probes
    // (timeout=0) and reports the outcome. The nested loop below (undo_entries ->
    // changed_cols -> other_tables -> cols -> per-FkAction row scan, 5 levels deep) is
    // wrapped in the try_cascade_once() lambda so that any conflict can `return` straight
    // out of every level at once (a lambda's `return` only exits the lambda, unlike a
    // loop `break`, so this needs no per-level "check a flag and break" plumbing). The
    // retry loop further below calls try_cascade_once() repeatedly: on NeedsRetry, it
    // releases table_locks (try_cascade_once's own neighbor_write_lock is already
    // released too, by ordinary C++ scope rules, since it's a local inside the lambda)
    // and blocks on just the one contested row, then calls try_cascade_once() again --
    // safe/idempotent to fully restart because the `it->second == old_val` match check
    // naturally excludes any row already mutated by an earlier attempt (its value is now
    // `new_val`, not `old_val`), exactly like DELETE's is_visible()-based idempotency.
    struct CascadeClaim {
        LockResult result;
        std::string pk_val;
    };
    auto claim_cascade_row = [&](const std::string& other_table, const Row& row) -> std::optional<CascadeClaim> {
        auto opc = single_pk_col(other_table);
        if (!opc) return std::nullopt;
        auto pkv_it = row.find(*opc);
        if (pkv_it == row.end()) return std::nullopt;
        return CascadeClaim{s.lock_mgr.acquire(other_table, pkv_it->second, claim_txn_id), pkv_it->second};
    };

    struct CascadeAttemptResult {
        enum class Outcome { Done, NeedsRetry, HardError } outcome;
        std::string conflict_table, conflict_key;
        StringResult err = StringResult::Ok("");
        static CascadeAttemptResult done() { return {Outcome::Done, "", "", StringResult::Ok("")}; }
        static CascadeAttemptResult needs_retry(std::string t, std::string k) {
            return {Outcome::NeedsRetry, std::move(t), std::move(k), StringResult::Ok("")};
        }
        static CascadeAttemptResult hard_error(StringResult e) { return {Outcome::HardError, "", "", std::move(e)}; }
    };

    auto try_cascade_once = [&]() -> CascadeAttemptResult {
    auto neighbor_write_lock = acquire_table_data_locks(s, neighbor_tables, /*exclusive=*/true);
    for (auto& u : undo_entries) {
        Row old_row;
        try {
            old_row = row_from_json(u.old_json);
        } catch (...) {
        }
        for (auto& assign_col : changed_cols) {
            auto oit = old_row.find(assign_col);
            std::string old_val = oit != old_row.end() ? oit->second : std::string();
            std::string new_val;
            if (per_row) {
                if (auto pit = per_row->find(match_key(old_row)); pit != per_row->end()) {
                    if (auto vit = pit->second.find(assign_col); vit != pit->second.end()) new_val = vit->second;
                }
            } else {
                for (auto& [c, expr] : assignments) {
                    if (c == assign_col) {
                        new_val = eval_arith(old_row, expr);
                        break;
                    }
                }
            }
            if (old_val == new_val) continue;

            for (auto& [other_table, cols] : other_tables) {
                for (auto& col : cols) {
                    if (!col.foreign_key || col.foreign_key->ref_table != table || col.foreign_key->ref_column != assign_col) continue;
                    switch (col.foreign_key->on_update) {
                        case FkAction::Restrict: {
                            if (auto oit2 = s.tables.find(other_table); oit2 != s.tables.end()) {
                                bool referenced = index_or_scan_exists(s, other_table, oit2->second, col.name, old_val, is_visible);
                                if (referenced) {
                                    return CascadeAttemptResult::hard_error(StringResult::Err("Foreign key violation (ON UPDATE RESTRICT): '" +
                                                                                                assign_col + "' is referenced by '" + other_table +
                                                                                                "'.'" + col.name + "'"));
                                }
                            }
                            break;
                        }
                        case FkAction::Cascade: {
                            if (auto oit2 = s.tables.find(other_table); oit2 != s.tables.end()) {
                                for (auto& row : oit2->second) {
                                    if (!is_visible(row)) continue;
                                    auto it = row.find(col.name);
                                    if (it != row.end() && it->second == old_val) {
                                        if (auto claim = claim_cascade_row(other_table, row)) {
                                            if (claim->result.kind == LockResult::Kind::Deadlock) {
                                                return CascadeAttemptResult::hard_error(StringResult::Err(
                                                    "Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                                    std::to_string(claim->result.holder) + " (UPDATE cascade on '" + other_table +
                                                    "'). Transaction " + std::to_string(claim_txn_id) + " aborted."));
                                            }
                                            if (claim->result.kind == LockResult::Kind::Conflict) {
                                                return CascadeAttemptResult::needs_retry(other_table, claim->pk_val);
                                            }
                                        }
                                        Row cascade_old_row = row;
                                        row[col.name] = new_val;
                                        if (auto opc = single_pk_col(other_table)) refresh_cascade_indexes(other_table, *opc, cascade_old_row, row);
                                    }
                                }
                            }
                            if (!txn.is_active()) {
                                std::vector<Row> rc = s.tables.at(other_table);
                                s.buffer_pool.write_through(other_table, rc, s.disk);
                            }
                            break;
                        }
                        case FkAction::SetNull: {
                            if (auto oit2 = s.tables.find(other_table); oit2 != s.tables.end()) {
                                for (auto& row : oit2->second) {
                                    if (!is_visible(row)) continue;
                                    auto it = row.find(col.name);
                                    if (it != row.end() && it->second == old_val) {
                                        if (auto claim = claim_cascade_row(other_table, row)) {
                                            if (claim->result.kind == LockResult::Kind::Deadlock) {
                                                return CascadeAttemptResult::hard_error(StringResult::Err(
                                                    "Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                                    std::to_string(claim->result.holder) + " (UPDATE cascade on '" + other_table +
                                                    "'). Transaction " + std::to_string(claim_txn_id) + " aborted."));
                                            }
                                            if (claim->result.kind == LockResult::Kind::Conflict) {
                                                return CascadeAttemptResult::needs_retry(other_table, claim->pk_val);
                                            }
                                        }
                                        Row cascade_old_row = row;
                                        row[col.name] = EXECUTOR_NULL_VALUE;
                                        if (auto opc = single_pk_col(other_table)) refresh_cascade_indexes(other_table, *opc, cascade_old_row, row);
                                    }
                                }
                            }
                            if (!txn.is_active()) {
                                std::vector<Row> rc = s.tables.at(other_table);
                                s.buffer_pool.write_through(other_table, rc, s.disk);
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
                                    if (it != row.end() && it->second == old_val) {
                                        if (auto claim = claim_cascade_row(other_table, row)) {
                                            if (claim->result.kind == LockResult::Kind::Deadlock) {
                                                return CascadeAttemptResult::hard_error(StringResult::Err(
                                                    "Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                                    std::to_string(claim->result.holder) + " (UPDATE cascade on '" + other_table +
                                                    "'). Transaction " + std::to_string(claim_txn_id) + " aborted."));
                                            }
                                            if (claim->result.kind == LockResult::Kind::Conflict) {
                                                return CascadeAttemptResult::needs_retry(other_table, claim->pk_val);
                                            }
                                        }
                                        Row cascade_old_row = row;
                                        row[col.name] = default_val;
                                        if (auto opc = single_pk_col(other_table)) refresh_cascade_indexes(other_table, *opc, cascade_old_row, row);
                                    }
                                }
                            }
                            if (!txn.is_active()) {
                                std::vector<Row> rc = s.tables.at(other_table);
                                s.buffer_pool.write_through(other_table, rc, s.disk);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    // Row-level-concurrency Stage 4/5 correctness fix: invalidate the cache for every
    // FK-cascade-touched table HERE, still holding neighbor_write_lock EXCLUSIVE -- same
    // reasoning as the primary table's own invalidate_table call below.
    for (auto& t : neighbor_tables) s.query_cache.invalidate_table(t);
    return CascadeAttemptResult::done();
    }; // neighbor_write_lock (EXCLUSIVE, a local inside this lambda) released on every return above

    for (;;) {
        auto attempt = try_cascade_once();
        if (attempt.outcome == CascadeAttemptResult::Outcome::Done) break;
        if (attempt.outcome == CascadeAttemptResult::Outcome::HardError) return attempt.err;
        // NeedsRetry: release table_locks (neighbor_write_lock is already released, by
        // ordinary scope rules, since try_cascade_once() already returned) and block on
        // just the one contested row, then restart the whole cascade computation --
        // see try_cascade_once's doc comment above for why a full restart is safe.
        release_table_locks_for_block();
        auto lr2 = block_on_row(s.lock_mgr, attempt.conflict_table, attempt.conflict_key, claim_txn_id, /*exclusive=*/true, lock_deadline);
        reacquire_table_locks_after_block();
        if (lr2.kind == LockResult::Kind::Deadlock) {
            return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                      std::to_string(lr2.holder) + " (UPDATE cascade on '" + attempt.conflict_table + "'). Transaction " +
                                      std::to_string(claim_txn_id) + " aborted.");
        }
        if (lr2.kind != LockResult::Kind::Granted) {
            return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + attempt.conflict_key + "' in '" +
                                      attempt.conflict_table + "' is held by transaction " + std::to_string(lr2.holder) +
                                      ". Cannot cascade UPDATE.");
        }
        // else: granted -- loop back and retry try_cascade_once() from scratch.
    }

    if (!txn.is_active()) {
        // Row-level-concurrency Stage 4: EXCLUSIVE -- maybe_auto_vacuum erases rows
        // (shape-changing), so this whole tail (including the buffer_pool snapshot
        // copy) needs `table`'s table_data_locks exclusive, matching exec_insert_inner.
        auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        // Redo-covered statement: persistence is the redo batch written by execute()'s
        // epilogue, not a whole-table rewrite here.
        if (!redo_covered_stmt_) {
            std::vector<Row> rc = s.tables.at(table);
            s.buffer_pool.write_through(table, rc, s.disk);
        }
        maybe_auto_vacuum(s, table);
        maybe_auto_analyze(s, table);
        s.query_cache.invalidate_table(table);
    }
    maybe_auto_checkpoint(s);

    if (returning) {
        auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/false);
        std::vector<Row> updated_rows;
        for (auto& r : s.tables.at(table)) {
            if (!is_visible(r)) continue;
            if (matching_pks.count(match_key(r))) updated_rows.push_back(r);
        }
        return StringResult::Ok(format_returning_rows(updated_rows, *returning));
    }
    return StringResult::Ok(std::to_string(count) + " row(s) updated.");
}

} // namespace engine
