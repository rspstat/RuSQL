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
                                    std::optional<CondExpr> condition, std::optional<std::vector<SelectColumn>> returning) {
    if (s.views.count(table)) {
        if (auto resolved = resolve_updatable_view(s, table)) {
            auto merged_cond = merge_conditions(resolved->second, condition);
            return exec_update(s, resolved->first, assignments, merged_cond, returning);
        }
        return StringResult::Err("View '" + strip_db_prefix(table) + "' is not updatable");
    }

    if (auto tr = fire_triggers(s, table, "BEFORE", "UPDATE"); tr.is_err()) return tr;
    auto result = exec_update_inner(s, table, assignments, condition, returning);
    if (result.is_ok()) {
        if (auto tr = fire_triggers(s, table, "AFTER", "UPDATE"); tr.is_err()) return tr;
    }
    return result;
}

StringResult Executor::exec_update_inner(SharedDatabase& s, const std::string& table,
                                          const std::vector<std::pair<std::string, ArithExpr>>& assignments,
                                          const std::optional<CondExpr>& condition,
                                          const std::optional<std::vector<SelectColumn>>& returning) {
    const TableSchema* schema0 = s.catalog.get_table(table);
    if (!schema0) return StringResult::Err("Table '" + table + "' not found");
    std::string pk_col = "id";
    std::vector<std::string> pk_cols;
    for (auto& c : schema0->columns) {
        if (c.primary_key) pk_cols.push_back(c.name);
    }
    if (pk_cols.empty()) pk_cols.push_back("id");
    pk_col = pk_cols.front();

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
    for (;;) {
        std::vector<Row> candidate_rows;
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
            for (auto& r : tit0->second) {
                if (is_visible_for_read(r, write_ctx)) candidate_rows.push_back(r);
            }
            for (auto& r : candidate_rows) {
                if (matches_condition_with_subquery(s, r, condition)) {
                    matching_pks.insert(match_key(r));
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
        std::vector<UndoEntry> attempt_undo_entries;
        bool conflict = false;
        std::string conflict_key;
        {
            auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
            auto& rows = tit0->second;

            for (auto& row : rows) {
                if (!matching_pks.count(match_key(row))) continue;
                if (!is_visible_for_read(row, write_ctx)) continue;
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

            if (!conflict) {
                for (auto& row : rows) {
                    if (!matching_pks.count(match_key(row))) continue;
                    // A stale dead version sharing this PK with the live matched row (from
                    // an earlier UPDATE on the same key, still un-vacuumed) -- skip, only
                    // the live version should ever be re-updated.
                    if (!is_visible_for_read(row, write_ctx)) continue;

                    auto pkit = row.find(pk_col);
                    std::string row_pk = pkit != row.end() ? pkit->second : std::string();
                    const std::string& key = row_pk;

                    nlohmann::json old_j = row;
                    std::string old_json = old_j.dump();

                    // MVCC: UPDATE now creates a new physical version instead of mutating
                    // `row` in place -- `new_row` holds it; `row` (the OLD version) only
                    // gets its _xmax stamped below, once new_row has passed every
                    // validation check.
                    Row new_row = row;
                    std::vector<std::pair<std::string, std::string>> new_vals;
                    for (auto& [col, expr] : assignments) new_vals.emplace_back(col, eval_arith(row, expr));

                    if (auto* schema = s.catalog.get_table(table)) {
                        for (auto& [col_name, val] : new_vals) {
                            if (val.empty() || val == EXECUTOR_NULL_VALUE) continue;
                            auto cit = std::find_if(schema->columns.begin(), schema->columns.end(),
                                                     [&](const ColumnDef& c) { return c.name == col_name; });
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

                    for (auto& [col, val] : new_vals) new_row[col] = val;

                    if (auto* schema = s.catalog.get_table(table)) {
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
                    row["_xmax"] = std::to_string(my_id);

                    nlohmann::json new_j = new_row;
                    attempt_undo_entries.push_back({key, old_json, new_j.dump()});
                    new_versions.push_back(std::move(new_row));
                }
                rows.insert(rows.end(), std::make_move_iterator(new_versions.begin()), std::make_move_iterator(new_versions.end()));
                // Row-level-concurrency Stage 4/5 correctness fix (found via concurrent-
                // reader monotonicity stress testing): invalidate the query cache HERE,
                // still holding table_data_locks EXCLUSIVE -- execute_sql's own
                // post-execute invalidate_table call runs too late (after this lock has
                // already released), leaving a real window for a reader to hit a stale
                // cache entry. See exec_insert_inner's identical comment.
                s.query_cache.invalidate_table(table);
            }
        } // table_lock (EXCLUSIVE) released here -- probe + mutate + insert as one atomic unit.

        if (!conflict) {
            count = attempt_undo_entries.size();
            undo_entries = std::move(attempt_undo_entries);
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
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            auto new_pk_it = new_row.find(pk_col);
            std::string new_pk = new_pk_it != new_row.end() ? new_pk_it->second : u.key;
            nlohmann::json j = new_row;
            // Row-level-concurrency Stage 4/5 correctness fix (found via concurrent-
            // reader monotonicity stress testing -- root cause of the "0 rows returned"
            // phantom, not a cache or Row-field race): remove(key) then insert(key,...)
            // are two SEPARATE calls, each independently locking/releasing BPlusTree's
            // own per-instance mutex (Stage 2) -- between them, the index has NO entry
            // for that key at all. AccessPath::PkPoint (exec_select) deliberately
            // bypasses table_data_locks for this exact index (relying only on its own
            // mutex, since index paths are meant to need no other lock) and can search
            // during exactly that gap. When the PK value is unchanged (the overwhelming
            // common case), a single insert() call already overwrites the existing
            // entry atomically -- skip remove() entirely. Only an actual PK-value change
            // still needs remove(old)+insert(new) (two different keys -- no atomicity
            // is possible or expected there; a reader querying the OLD key value
            // legitimately stops finding this row partway through, same as any DELETE).
            if (new_pk == u.key) {
                idx_it->second.insert(new_pk, j.dump());
            } else {
                idx_it->second.remove(u.key);
                idx_it->second.insert(new_pk, j.dump());
            }
        }
        index_remove_row(s, table, old_row, pk_col);
        index_insert_row(s, table, new_row);
        for (auto& k : comp_keys) {
            // Same atomicity fix as the PK B+Tree above: CompositeIndex::insert_row
            // already overwrites an existing key in place (single tree_.insert() call),
            // so when none of this index's columns actually changed value, skip
            // remove_row() entirely -- calling it first would open the exact same
            // "key temporarily absent" gap for a concurrent CompositeIndexPath read.
            auto& ci = s.composite_indexes.at(k);
            if (ci.key_from_row(old_row) == ci.key_from_row(new_row)) {
                ci.insert_row(new_row);
            } else {
                ci.remove_row(old_row);
                ci.insert_row(new_row);
            }
        }
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
            nlohmann::json j = new_row;
            idx_it->second.insert(key, j.dump());
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
        std::vector<Row> rc = s.tables.at(table);
        s.buffer_pool.write_page(table, rc);
        s.buffer_pool.flush_page(table, s.disk);
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
