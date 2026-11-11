// Faithful port of the transaction control statements from
// rusql-core/src/engine/executor.rs (Phase 8d): exec_begin, exec_commit (the simple,
// single-phase version dispatched when execute_with_s is called directly — e.g. from
// inside a trigger/procedure body or a CTE, none of which are ported yet, but the
// function itself is small and self-contained so it's included now), the grouped
// 2-phase commit path used by the public execute() entry point
// (execute_commit_grouped + exec_commit_phase1), apply_rollback/exec_rollback,
// savepoints, and boot-time WAL crash recovery (recover_from_wal).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace engine {

namespace {
std::string isolation_level_debug(IsolationLevel level) {
    switch (level) {
        case IsolationLevel::ReadUncommitted: return "ReadUncommitted";
        case IsolationLevel::ReadCommitted: return "ReadCommitted";
        case IsolationLevel::RepeatableRead: return "RepeatableRead";
        case IsolationLevel::Serializable: return "Serializable";
    }
    return "";
}
} // namespace

StringResult Executor::exec_begin(SharedDatabase& s, bool consistent_snapshot) {
    // the level of the transaction: the one SET TRANSACTION gave it, else the session's (a SET made inside the last transaction takes effect now)
    if (!txn.is_active()) txn.set_isolation_level(next_isolation_ ? *next_isolation_ : session_isolation_);
    supersede_marks_.clear();
    auto res = txn.begin_with_snapshot(*s.active_txn_ids->lock(), consistent_snapshot);
    if (res.is_err()) return StringResult::Err(res.error());
    std::uint64_t txn_id = res.value();
    s.active_txn_ids->lock()->insert(txn_id);
    return StringResult::Ok("Transaction " + std::to_string(txn_id) + " started. (isolation: " + isolation_level_debug(txn.isolation_level()) +
                             ")");
}

// ── REPEATABLE READ / SERIALIZABLE: a row this transaction writes must not be seen twice ───────────────────────────────────────────────────
std::optional<SnapshotCtx> Executor::snapshot_for_hiding() const {
    if (!txn.is_active()) return std::nullopt;
    return txn.frozen_ctx(); // (only the levels that read through one snapshot have it)
}

namespace {
std::uint64_t txn_id_field(const Row& row, const char* name) {
    auto it = row.find(name);
    return it == row.end() ? 0 : std::strtoull(it->second.c_str(), nullptr, 10);
}

// what says two versions are one row: the values of its primary key, joined with a NUL byte
std::string version_key_of(const Row& row, const std::vector<std::string>& key_columns) {
    std::string key;
    for (std::size_t i = 0; i < key_columns.size(); i++) {
        if (i) key += '\0';
        auto it = row.find(key_columns[i]);
        if (it != row.end()) key += it->second;
    }
    return key;
}
} // namespace

void Executor::hide_versions_of(SharedDatabase& s, const std::string& table, const std::unordered_set<std::string>& keys) {
    auto frozen = txn.frozen_ctx();
    const TableSchema* schema = s.catalog.get_table(table);
    auto rows_it = s.tables.find(table);
    if (!frozen || !schema || rows_it == s.tables.end() || keys.empty()) return;
    const std::vector<std::string> key_columns = schema->identity_columns();
    if (key_columns.empty()) return;
    const std::string mine = std::to_string(frozen->self_txn_id);
    for (Row& row : rows_it->second) {
        if (txn_id_field(row, "_xmin") == frozen->self_txn_id) continue; // (its own versions are the ones that stay)
        if (!keys.count(version_key_of(row, key_columns))) continue;
        if (is_visible_for_read(row, *frozen)) row["_xsup"] = mine;
    }
    txn.note_superseding();
}

void Executor::hide_older_versions(SharedDatabase& s, const std::string& table, const std::vector<Row>& unseen, std::size_t undo_before) {
    const TableSchema* schema = s.catalog.get_table(table);
    if (!schema || unseen.empty()) return;
    const std::vector<std::string> key_columns = schema->identity_columns();
    if (key_columns.empty()) return; // (rows of a table without a key cannot be told apart from one version to the next)
    std::unordered_set<std::string> keys;
    for (const Row& row : unseen) {
        std::string key = version_key_of(row, key_columns);
        supersede_marks_.push_back(SupersedeMark{table, key, undo_before});
        keys.insert(std::move(key));
    }
    hide_versions_of(s, table, keys);
}

void Executor::unhide_after_rollback(SharedDatabase& s, std::uint64_t self_id, std::size_t remaining_undo) {
    if (supersede_marks_.empty()) return;
    std::vector<SupersedeMark> kept;
    std::unordered_set<std::string> touched;
    for (auto& mark : supersede_marks_) {
        if (mark.undo_size >= remaining_undo) touched.insert(mark.table);
        else kept.push_back(mark);
    }
    if (touched.empty()) return;
    supersede_marks_ = kept;
    const std::string mine = std::to_string(self_id);
    for (const std::string& table : touched) {
        auto rows_it = s.tables.find(table);
        if (rows_it == s.tables.end()) continue;
        for (Row& row : rows_it->second) {
            auto sup = row.find("_xsup");
            if (sup != row.end() && sup->second == mine) row.erase(sup);
        }
    }
    // ... and what the statements before the savepoint hid stays hidden
    std::unordered_map<std::string, std::unordered_set<std::string>> again;
    for (auto& mark : kept) {
        if (touched.count(mark.table)) again[mark.table].insert(mark.key);
    }
    for (auto& [table, keys] : again) hide_versions_of(s, table, keys);
}

StringResult Executor::exec_commit(SharedDatabase& s) {
    if (auto res = txn.validate_serializable(s.tables, *s.active_txn_ids->lock()); res.is_err()) {
        apply_rollback(s);
        return StringResult::Err(res.error() + " (auto-rolled back)");
    }
    // Predicate lock (SSI phantom detection): validate_serializable above only re-checks
    // rows this transaction actually read; this catches the complementary case -- a
    // brand-new row inserted into a range this transaction scanned (see
    // register_predicate_read's doc comment and the INSERT-side check in executor_dml.cpp).
    // Must run before lock_mgr.release() below, which clears this transaction's flag.
    if (s.lock_mgr.take_predicate_violation(txn.current_txn_id())) {
        apply_rollback(s);
        return StringResult::Err("Serialization failure: a row was inserted into a range this transaction scanned (auto-rolled back)");
    }

    // MVCC Stage 2: writes now land directly in s.tables as they happen (no more
    // session_tables staging to merge in here) -- COMMIT's remaining job is just to
    // persist whatever tables this transaction actually touched, per the undo log.
    auto dirty_tables = txn.dirty_tables();
    for (auto& table : dirty_tables) {
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            s.buffer_pool.write_through(table, it->second, s.disk, txn.current_txn_id());
        }
    }

    std::uint64_t txn_id = txn.current_txn_id();
    if (auto res = txn.commit(); res.is_err()) return StringResult::Err(res.error());
    s.lock_mgr.release(txn_id);
    s.active_txn_ids->lock()->erase(txn_id);
    // Stage 4: per touched table, not a single global call -- COMMIT's own lock set
    // (see Executor::execute()) already covers exactly these tables.
    for (auto& table : dirty_tables) maybe_auto_vacuum(s, table);
    return StringResult::Ok("Transaction committed.");
}

StringResult Executor::exec_commit_phase1(SharedDatabase& s, bool use_redo) {
    if (auto res = txn.validate_serializable(s.tables, *s.active_txn_ids->lock()); res.is_err()) {
        apply_rollback(s);
        return StringResult::Err(res.error() + " (auto-rolled back)");
    }
    // Predicate lock (SSI phantom detection): see exec_commit's identical check above.
    if (s.lock_mgr.take_predicate_violation(txn.current_txn_id())) {
        apply_rollback(s);
        return StringResult::Err("Serialization failure: a row was inserted into a range this transaction scanned (auto-rolled back)");
    }

    for (auto& table : txn.dirty_tables()) {
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            // Redo mode: the table file is NOT rewritten here -- the commit's redo batch
            // (written by execute_commit_grouped) is what makes it durable.
            if (!use_redo) {
                s.buffer_pool.write_through(table, it->second, s.disk, txn.current_txn_id());
                redo_mark_flushed(s, table); // table_locks are held EXCLUSIVE for dirty tables here
            }
            s.query_cache.invalidate_table(table);
        }
    }

    std::uint64_t txn_id = txn.current_txn_id();
    if (!use_redo) {
        if (auto res = txn.commit_write_record(); res.is_err()) return StringResult::Err(res.error());
    }
    s.lock_mgr.release(txn_id);
    // Regression (checkpoint/group-commit TOCTOU): active_txn_ids used to be erased here,
    // right after commit_write_record() writes the COMMIT record WITHOUT an fsync
    // (log_commit_no_sync). exec_checkpoint/maybe_auto_checkpoint treat an empty
    // active_txn_ids as "safe to truncate the WAL" -- erasing this early meant a
    // checkpoint racing in the window between this line and execute_commit_grouped()'s
    // sync_commit()/commit_finalize() (below, outside the write lock) could truncate the
    // WAL before this commit's own record was ever durably fsynced. The erase now happens
    // only once the commit is truly durable -- see execute_commit_grouped().
    return StringResult::Ok("");
}

StringResult Executor::execute_commit_grouped() {
    std::uint64_t txn_id = txn.current_txn_id();
    // Stage 4: dirty_tables() is a pure read of this transaction's own undo log -- safe to
    // capture before touching SharedDatabase at all, and before exec_commit_phase1 (via
    // txn.commit_write_record()) leaves it in a state this call no longer needs to trust.
    // Only these specific tables' locks are needed for the critical section below (instead
    // of the whole database's exclusive lock), so an unrelated table's concurrent statement
    // isn't blocked by this commit.
    auto dirty_tables = txn.dirty_tables();
    std::sort(dirty_tables.begin(), dirty_tables.end());

    // Row-level-concurrency correctness fix (found via same-row explicit-transaction
    // stress testing, see "Two explicit transactions racing the SAME row" in
    // test_concurrency.cpp): table_locks for `dirty_tables` must stay held EXCLUSIVE
    // continuously from phase1 all the way through the active_txn_ids erase below --
    // this used to be two separate critical sections (phase1 in one, the erase+vacuum in
    // a second one re-acquired after sync_commit()/commit_finalize() ran lock-free), which
    // left a real gap where table_locks was released with active_txn_ids still containing
    // this (now-durable) txn_id. A concurrent UPDATE only needs table_locks SHARED, so it
    // could run in that gap and compute its MVCC visibility snapshot while this txn still
    // read as "in progress": its OLD, already-superseded row (xmax = this txn) then looked
    // visible again, and its NEW row (xmin = this txn) looked NOT visible yet -- a version
    // fork. Each recurrence compounds (every future commit re-discovers the resurrected
    // old version as an extra "still visible" match and forks it again), observed directly
    // as literal Fibonacci-shaped unbounded row growth (233, 377, 610, 987, 1597, ...) and
    // multi-second-then-multi-minute commit latency under just two threads. Holding the
    // lock through the erase closes the gap: any reader taking table_locks SHARED either
    // runs before this whole section (sees the pre-commit state, correctly) or waits for
    // it and then sees post-commit state where active_txn_ids has already been updated --
    // never the inconsistent in-between. This does serialize concurrent COMMITs of the
    // SAME table (previously only phase1 was serialized; sync_commit()'s fsync and
    // commit_finalize() ran unlocked) -- group commit's cross-session fsync batching still
    // works fully for commits touching DIFFERENT tables, just not for the same one
    // concurrently, which correctness now requires anyway.
    auto guard = acquire_table_locks(shared->read(), dirty_tables, /*exclusive=*/true);
    // Redo mode unless a statement ran in this transaction whose changes the redo ops don't
    // fully describe (cascade/MERGE/multi-table/...): then fall back to flushing dirty tables.
    const bool use_redo = !txn.redo_incomplete();
    auto phase1 = exec_commit_phase1(const_cast<SharedDatabase&>(*guard.structural), use_redo);
    if (phase1.is_err()) return phase1;
    for (auto& table : dirty_tables) unique_probe_commit(const_cast<SharedDatabase&>(*guard.structural), table); // (the rows that are live do not change at COMMIT)
    auto coord = guard.structural->group_commit_coord;
    auto active_txn_ids = guard.structural->active_txn_ids;

    if (use_redo) {
        // One batch + one (group-committed) fsync makes the whole transaction durable.
        write_redo_batch(const_cast<SharedDatabase&>(*guard.structural), txn.take_redo_ops(std::to_string(txn_id)));
    } else {
        coord->sync_commit();
    }
    txn.commit_finalize();
    // Only now -- once the commit is truly durable (fsync'd via sync_commit(), WAL/undo
    // pruned via commit_finalize()) -- is it safe for a concurrent reader/checkpoint to
    // treat this txn as fully done. Still holding table_locks EXCLUSIVE here (see above).
    active_txn_ids->lock()->erase(txn_id);
    for (auto& table : dirty_tables) maybe_auto_vacuum(const_cast<SharedDatabase&>(*guard.structural), table);

    return StringResult::Ok("Transaction committed.");
}

void Executor::apply_rollback(SharedDatabase& s) {
    std::uint64_t txn_id = txn.current_txn_id();
    std::string txn_id_str = std::to_string(txn_id);
    // dirty_tables() reads the undo log, which txn.abort() below clears -- must capture
    // it first.
    std::vector<std::string> dirty_tables = txn.dirty_tables();
    (void)txn.abort();
    s.lock_mgr.release(txn_id);
    s.active_txn_ids->lock()->erase(txn_id);

    // MVCC Stage 2: writes now live directly in s.tables, tagged with this transaction's
    // id -- undoing them is a single filtered pass per touched table instead of a JSON
    // undo-log replay: physically erase whatever this txn created (_xmin == its id), and
    // revive whatever it soft-deleted or superseded via UPDATE (_xmax == its id resets to
    // the "0" == alive sentinel).
    for (auto& table : dirty_tables) {
        auto tit = s.tables.find(table);
        if (tit == s.tables.end()) continue;
        auto& rows = tit->second;
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                   [&](const Row& r) {
                                       auto it = r.find("_xmin");
                                       return it != r.end() && it->second == txn_id_str;
                                   }),
                   rows.end());
        for (auto& row : rows) {
            auto it = row.find("_xmax");
            if (it != row.end() && it->second == txn_id_str) it->second = "0";
            if (auto sup = row.find("_xsup"); sup != row.end() && sup->second == txn_id_str) row.erase(sup); // (the older version this transaction hid is seen again)
        }

        std::vector<Row> rows_clone = rows;

        std::string pk_col = "id";
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    pk_col = c.name;
                    break;
                }
            }
        }
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) idx_it->second = build_pk_tree(rows_clone, pk_col, idx_it->second.kinds());

        rebuild_secondary_indexes(s, table, rows_clone);

        std::vector<std::string> comp_keys;
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table == table) comp_keys.push_back(k);
        }
        for (auto& k : comp_keys) s.composite_indexes.at(k).rebuild(rows_clone);
    }
}

StringResult Executor::exec_rollback(SharedDatabase& s) {
    apply_rollback(s);
    return StringResult::Ok("Transaction rolled back.");
}

StringResult Executor::exec_savepoint(const std::string& name) {
    // outside a transaction MySQL takes the statement and keeps nothing (a ROLLBACK TO that name finds no savepoint)
    if (!txn.is_active()) return StringResult::Ok("Savepoint '" + name + "' created.");
    if (auto res = txn.create_savepoint(name); res.is_err()) return StringResult::Err(res.error());
    return StringResult::Ok("Savepoint '" + name + "' created.");
}

StringResult Executor::exec_release_savepoint(const std::string& name) {
    if (!txn.is_active()) return StringResult::Err("ERROR 1305 (42000): SAVEPOINT " + name + " does not exist");
    if (auto res = txn.release_savepoint(name); res.is_err()) return StringResult::Err("ERROR 1305 (42000): SAVEPOINT " + name + " does not exist");
    return StringResult::Ok("Savepoint '" + name + "' released.");
}

StringResult Executor::exec_rollback_to(SharedDatabase& s, const std::string& name) {
    if (!txn.is_active()) return StringResult::Err("ERROR 1305 (42000): SAVEPOINT " + name + " does not exist");
    auto res = txn.rollback_to_savepoint(name);
    if (res.is_err()) return StringResult::Err("ERROR 1305 (42000): SAVEPOINT " + name + " does not exist");

    std::string txn_id_str = std::to_string(txn.current_txn_id());
    std::unordered_set<std::string> touched_tables;

    for (auto& entry : res.value()) {
        touched_tables.insert(entry.table);
        std::string pk_col = "id";
        if (auto* sc = s.catalog.get_table(entry.table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    pk_col = c.name;
                    break;
                }
            }
        }
        auto tit = s.tables.find(entry.table);
        if (tit == s.tables.end()) continue;
        auto& rows = tit->second;

        if (entry.operation == "INSERT") {
            // Only the live version this INSERT made goes. The entries come newest first, so whatever happened to it later in the transaction is undone by now and it is live
            // again; an older version of the same key that this transaction made BEFORE the savepoint (and a later REPLACE / DELETE ended: _xmax is this transaction's id) must
            // stay, or the DELETE entry that follows finds nothing to bring back and the row is gone for good.
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                       [&](const Row& r) {
                                           auto pit = r.find(pk_col);
                                           auto xnit = r.find("_xmin");
                                           auto xxit = r.find("_xmax");
                                           return pit != r.end() && pit->second == entry.key && xnit != r.end() &&
                                                  xnit->second == txn_id_str && (xxit == r.end() || xxit->second == "0");
                                       }),
                       rows.end());
        } else if (entry.operation == "UPDATE") {
            if (entry.old_data) {
                try {
                    Row old_row = row_from_json(*entry.old_data);
                    // MVCC: this UPDATE created exactly one new "tip" version (tagged
                    // with this txn's id, still live) and marked exactly one earlier
                    // version dead (_xmax == this txn's id) -- remove the tip, then
                    // revive the specific earlier version by content match against
                    // old_data (more than one row can carry _xmax == this txn's id if the
                    // same key was updated more than once in this transaction, so a bare
                    // key+_xmax match would be ambiguous).
                    const std::string& tip_key = entry.new_key ? *entry.new_key : entry.key; // (a primary key that the UPDATE changed)
                    rows.erase(std::remove_if(rows.begin(), rows.end(),
                                               [&](const Row& r) {
                                                   auto pit = r.find(pk_col);
                                                   if (pit == r.end() || pit->second != tip_key) return false;
                                                   auto xnit = r.find("_xmin");
                                                   auto xxit = r.find("_xmax");
                                                   return xnit != r.end() && xnit->second == txn_id_str && xxit != r.end() &&
                                                          xxit->second == "0";
                                               }),
                               rows.end());
                    for (auto& row : rows) {
                        auto pit = row.find(pk_col);
                        if (pit == row.end() || pit->second != entry.key) continue;
                        auto xxit = row.find("_xmax");
                        if (xxit == row.end() || xxit->second != txn_id_str) continue;
                        bool matches = std::all_of(old_row.begin(), old_row.end(), [&](auto& kv) {
                            if (kv.first == "_xmax") return true;
                            auto it2 = row.find(kv.first);
                            return it2 != row.end() && it2->second == kv.second;
                        });
                        if (matches) {
                            row["_xmax"] = "0";
                            break;
                        }
                    }
                } catch (...) {
                }
            }
        } else if (entry.operation == "DELETE") {
            // Only the version this DELETE ended comes back: an earlier UPDATE of the same row in the transaction has ended another one (it carries the same key and the same
            // _xmax), and reviving that one too left the row twice.
            std::optional<Row> deleted;
            if (entry.old_data) {
                try {
                    deleted = row_from_json(*entry.old_data);
                } catch (...) {
                }
            }
            for (auto& row : rows) {
                auto pit = row.find(pk_col);
                auto xxit = row.find("_xmax");
                if (pit == row.end() || pit->second != entry.key || xxit == row.end() || xxit->second != txn_id_str) continue;
                if (deleted && !std::all_of(deleted->begin(), deleted->end(), [&](auto& kv) {
                        if (kv.first == "_xmax") return true;
                        auto it2 = row.find(kv.first);
                        return it2 != row.end() && it2->second == kv.second;
                    })) {
                    continue;
                }
                row["_xmax"] = "0";
                break;
            }
        }
    }

    // Indexes were already mutated live by the DML this savepoint is undoing (Stage 2
    // writes hit s.tables/indexes directly, no more session_tables staging) -- rebuild
    // them for every table this rollback touched so they reflect the restored rows.
    for (auto& table : touched_tables) {
        auto tit = s.tables.find(table);
        if (tit == s.tables.end()) continue;
        std::vector<Row> rows_clone = tit->second;

        std::string pk_col = "id";
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    pk_col = c.name;
                    break;
                }
            }
        }
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) idx_it->second = build_pk_tree(rows_clone, pk_col, idx_it->second.kinds());
        rebuild_secondary_indexes(s, table, rows_clone);
        std::vector<std::string> comp_keys;
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table == table) comp_keys.push_back(k);
        }
        for (auto& k : comp_keys) s.composite_indexes.at(k).rebuild(rows_clone);
    }

    unhide_after_rollback(s, txn.current_txn_id(), txn.undo_size());
    return StringResult::Ok("Rolled back to savepoint '" + name + "'.");
}

void Executor::recover_from_wal() {
    auto sw = shared->write();
    auto records = txn.wal_records();
    if (records.empty()) return;

    std::size_t start_idx = 0;
    for (std::size_t i = records.size(); i-- > 0;) {
        if (records[i].op == WalOp::Checkpoint) {
            start_idx = i + 1;
            break;
        }
    }

    std::vector<WalRecord> replay_records(records.begin() + static_cast<std::ptrdiff_t>(start_idx), records.end());
    if (replay_records.empty()) {
        txn.wal_clear();
        txn.clear_undo_log_file();
        return;
    }

    std::vector<std::uint64_t> order;
    std::unordered_map<std::uint64_t, std::vector<const WalRecord*>> groups;
    std::unordered_set<std::uint64_t> committed;
    for (auto& r : replay_records) {
        if (r.op == WalOp::Insert || r.op == WalOp::Update || r.op == WalOp::Delete) {
            if (!groups.count(r.txn_id)) order.push_back(r.txn_id);
            groups[r.txn_id].push_back(&r);
        } else if (r.op == WalOp::Commit) {
            committed.insert(r.txn_id);
        }
    }
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    for (auto txn_id : order) (committed.count(txn_id) ? sw->recovery.wal_committed : sw->recovery.wal_undone)++;

    std::unordered_map<std::uint64_t, std::vector<UndoEntry>> undo_by_txn;
    for (auto& e : txn.read_undo_log_file()) undo_by_txn[e.txn_id].push_back(e);

    // Faithful port of a real Rust bug: replay below only ever patches sw->tables (and,
    // for a REDO INSERT only, the PK B+Tree) -- secondary/hash/composite indexes, and the
    // PK B+Tree for every other operation kind, are never touched. After a crash-recovery
    // replay, any such index on a table this loop modified is stale relative to the
    // recovered rows. Fixed here (not in the per-record branches above) by rebuilding
    // every index for each touched table once, after the whole replay finishes.
    std::unordered_set<std::string> touched_tables;

    for (auto txn_id : order) {
        auto git = groups.find(txn_id);
        if (git == groups.end()) continue;

        if (committed.count(txn_id)) {
            for (auto* record : git->second) {
                const std::string& table = record->table_name;
                touched_tables.insert(table);
                std::string pk_col = "id";
                if (auto* sc = sw->catalog.get_table(table)) {
                    for (auto& c : sc->columns) {
                        if (c.primary_key) {
                            pk_col = c.name;
                            break;
                        }
                    }
                }
                if (record->op == WalOp::Insert) {
                    try {
                        Row row = row_from_json(record->data);
                        auto tit = sw->tables.find(table);
                        if (tit != sw->tables.end()) {
                            auto it = row.find(pk_col);
                            std::string key = it != row.end() ? it->second : std::string();
                            bool exists = std::any_of(tit->second.begin(), tit->second.end(), [&](const Row& r) {
                                auto rit = r.find(pk_col);
                                return rit != r.end() && rit->second == key;
                            });
                            if (!exists) {
                                tit->second.push_back(row);
                                if (auto idx_it = sw->indexes.find(table); idx_it != sw->indexes.end()) idx_it->second.insert(key, row_to_json(row));
                                sw->disk.save_table(table, tit->second);
                            }
                        }
                    } catch (...) {
                    }
                } else if (record->op == WalOp::Update) {
                    try {
                        Row new_row = row_from_json(record->data);
                        auto tit = sw->tables.find(table);
                        if (tit != sw->tables.end()) {
                            auto nit = new_row.find(pk_col);
                            for (auto& row : tit->second) {
                                auto rit = row.find(pk_col);
                                bool same = (rit == row.end() && nit == new_row.end()) ||
                                            (rit != row.end() && nit != new_row.end() && rit->second == nit->second);
                                if (same) {
                                    row = new_row;
                                    break;
                                }
                            }
                            sw->disk.save_table(table, tit->second);
                        }
                    } catch (...) {
                    }
                } else if (record->op == WalOp::Delete) {
                    auto tit = sw->tables.find(table);
                    if (tit != sw->tables.end()) {
                        auto& rows = tit->second;
                        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                                   [&](const Row& r) {
                                                       auto rit = r.find(pk_col);
                                                       return rit != r.end() && rit->second == record->key;
                                                   }),
                                   rows.end());
                        sw->disk.save_table(table, rows);
                    }
                }
            }
        } else {
            // Uncommitted at crash time. Its row versions are tagged with its transaction id
            // (_xmin = created by it, _xmax = killed by it), so undoing it is: drop what it
            // created, revive what it killed -- the same MVCC rule apply_rollback() uses at
            // runtime. This is idempotent and does not care whether any of its changes ever
            // reached the table files. That matters: since COMMIT stopped rewriting tables,
            // an open transaction's changes usually are NOT on disk, and the old entry-by-
            // entry undo (re-insert the old image of a deleted row, overwrite "the" row with
            // an old image) manufactured row versions that never existed there.
            auto uit = undo_by_txn.find(txn_id);
            if (uit != undo_by_txn.end()) {
                const std::string id_str = std::to_string(txn_id);
                std::unordered_set<std::string> tables_of_txn;
                for (auto& entry : uit->second) tables_of_txn.insert(entry.table);
                for (auto& table : tables_of_txn) {
                    auto tit = sw->tables.find(table);
                    if (tit == sw->tables.end()) continue;
                    auto& rows = tit->second;
                    touched_tables.insert(table);
                    rows.erase(std::remove_if(rows.begin(), rows.end(),
                                               [&](const Row& r) {
                                                   auto it = r.find("_xmin");
                                                   return it != r.end() && it->second == id_str;
                                               }),
                               rows.end());
                    for (auto& row : rows) {
                        auto it = row.find("_xmax");
                        if (it != row.end() && it->second == id_str) it->second = "0";
                    }
                    sw->disk.save_table(table, rows);
                }
            }
        }
    }

    for (auto& table : touched_tables) {
        auto tit = sw->tables.find(table);
        if (tit == sw->tables.end()) continue;
        const std::vector<Row>& rows = tit->second;

        std::string pk_col = "id";
        if (auto* sc = sw->catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    pk_col = c.name;
                    break;
                }
            }
        }
        if (auto idx_it = sw->indexes.find(table); idx_it != sw->indexes.end()) idx_it->second = build_pk_tree(rows, pk_col, idx_it->second.kinds());
        rebuild_secondary_indexes(*sw, table, rows);
        std::vector<std::string> comp_keys;
        for (auto& [k, ci] : sw->composite_indexes) {
            if (ci.table == table) comp_keys.push_back(k);
        }
        for (auto& k : comp_keys) sw->composite_indexes.at(k).rebuild(rows);
    }

    txn.wal_clear();
    txn.clear_undo_log_file();
}

namespace {
std::string wal_op_debug(WalOp op) {
    switch (op) {
        case WalOp::Insert: return "Insert";
        case WalOp::Update: return "Update";
        case WalOp::Delete: return "Delete";
        case WalOp::Commit: return "Commit";
        case WalOp::Rollback: return "Rollback";
        case WalOp::Checkpoint: return "Checkpoint";
    }
    return "";
}

std::string pad_right(const std::string& v, std::size_t width) {
    std::string out = v;
    if (out.size() < width) out.append(width - out.size(), ' ');
    return out;
}

std::string pad_left(const std::string& v, std::size_t width) {
    std::string out = v;
    if (out.size() < width) out = std::string(width - out.size(), ' ') + out;
    return out;
}
} // namespace

StringResult Executor::exec_show_buffer_pool(const SharedDatabase& s) const {
    std::ostringstream out;
    std::string sep = "+----------------------+---------+";
    out << sep << "\n";
    out << "| 항목                 | 값      |\n";
    out << sep << "\n";
    out << "| 캐시 사용량          | " << pad_left(std::to_string(s.buffer_pool.usage()), 7) << " |\n";
    out << "| 최대 용량            | " << pad_left(std::to_string(s.buffer_pool.capacity), 7) << " |\n";
    out << "| 캐시 히트            | " << pad_left(std::to_string(s.buffer_pool.hit_count), 7) << " |\n";
    out << "| 캐시 미스            | " << pad_left(std::to_string(s.buffer_pool.miss_count), 7) << " |\n";
    out << "| 적중률               | " << std::fixed << std::setprecision(1) << std::setw(6) << s.buffer_pool.hit_rate() << "% |\n";
    out << sep;
    return StringResult::Ok(out.str());
}

StringResult Executor::exec_show_wal() const {
    auto records = txn.wal_records();
    std::uint64_t size = txn.wal_size();
    std::ostringstream out;
    std::string sep = "+------------+----------+----------+";
    out << "WAL 파일 크기: " << size << " bytes\n";
    out << sep << "\n";
    out << "| op         | table    | key      |\n";
    out << sep << "\n";
    for (auto& r : records) {
        std::string table = r.table_name.substr(0, std::min<std::size_t>(8, r.table_name.size()));
        std::string key = r.key.substr(0, std::min<std::size_t>(8, r.key.size()));
        out << "| " << pad_right(wal_op_debug(r.op), 10) << " | " << pad_right(table, 8) << " | " << pad_right(key, 8) << " |\n";
    }
    out << sep;
    return StringResult::Ok(out.str());
}

// SET [SESSION] TRANSACTION ISOLATION LEVEL x [, READ ONLY | READ WRITE] (and SET transaction_isolation = ..., SET transaction_read_only = ...): the characteristics of the
// session. A change made while a transaction is open is for the next one. SET TRANSACTION (no SESSION) is for the next transaction only, and cannot be made in one.
StringResult Executor::exec_set_transaction(const Statement::SetIsolationLevel& v) {
    if (v.next_only && txn.is_active()) {
        return StringResult::Err("ERROR 1568 (25001): Transaction characteristics can't be changed while a transaction is in progress");
    }
    std::string message;
    if (v.has_level) {
        std::string name, variable;
        switch (v.level) {
            case IsolationLevel::ReadUncommitted: name = "READ UNCOMMITTED"; variable = "READ-UNCOMMITTED"; break;
            case IsolationLevel::ReadCommitted: name = "READ COMMITTED"; variable = "READ-COMMITTED"; break;
            case IsolationLevel::RepeatableRead: name = "REPEATABLE READ"; variable = "REPEATABLE-READ"; break;
            case IsolationLevel::Serializable: name = "SERIALIZABLE"; variable = "SERIALIZABLE"; break;
        }
        if (v.next_only) {
            next_isolation_ = v.level;
        } else {
            session_isolation_ = v.level;
            if (!txn.is_active()) txn.set_isolation_level(v.level);
            user_vars["@transaction_isolation"] = user_vars["@tx_isolation"] = variable;
        }
        message = "Isolation level set to " + name + ".";
    }
    if (v.read_only) {
        if (v.next_only) next_read_only_ = *v.read_only;
        else user_vars["@transaction_read_only"] = user_vars["@tx_read_only"] = *v.read_only ? "1" : "0";
        if (message.empty()) message = *v.read_only ? "Access mode set to READ ONLY." : "Access mode set to READ WRITE.";
    }
    return StringResult::Ok(message);
}

bool Executor::read_only_now() const {
    if (txn.is_active()) return txn_read_only_;
    if (next_read_only_) return *next_read_only_;
    auto it = user_vars.find("@transaction_read_only");
    return it != user_vars.end() && it->second == "1";
}

StringResult Executor::exec_show_isolation_level() const {
    std::string name;
    switch (txn.isolation_level()) {
        case IsolationLevel::ReadUncommitted: name = "READ UNCOMMITTED"; break;
        case IsolationLevel::ReadCommitted: name = "READ COMMITTED"; break;
        case IsolationLevel::RepeatableRead: name = "REPEATABLE READ"; break;
        case IsolationLevel::Serializable: name = "SERIALIZABLE"; break;
    }
    return StringResult::Ok("Current isolation level: " + name);
}

StringResult Executor::exec_show_locks(const SharedDatabase& s) const {
    std::ostringstream out;

    auto locks = s.lock_mgr.lock_rows();
    if (locks.empty()) {
        out << "No active row locks.\n";
    } else {
        out << "+------------------+-----+--------+\n";
        out << "| table            | key | txn_id |\n";
        out << "+------------------+-----+--------+\n";
        for (auto& [tbl, key, txn_id] : locks) {
            out << "| " << pad_right(tbl, 16) << " | " << pad_right(key, 3) << " | " << pad_left(std::to_string(txn_id), 6) << " |\n";
        }
        out << "+------------------+-----+--------+\n";
    }

    auto gap_locks = s.lock_mgr.gap_lock_rows();
    if (!gap_locks.empty()) {
        out << "\nGap locks:\n";
        out << "+------------------+----------------------+--------+\n";
        out << "| table            | range                | txn_id |\n";
        out << "+------------------+----------------------+--------+\n";
        for (auto& [tbl, range, txn_id] : gap_locks) {
            out << "| " << pad_right(tbl, 16) << " | " << pad_right(range, 20) << " | " << pad_left(std::to_string(txn_id), 6) << " |\n";
        }
        out << "+------------------+----------------------+--------+\n";
    }

    auto predicate_locks = s.lock_mgr.predicate_lock_rows();
    if (!predicate_locks.empty()) {
        out << "\nPredicate locks (SSI phantom detection, non-blocking):\n";
        out << "+------------------+----------------------+--------+\n";
        out << "| table            | range                | txn_id |\n";
        out << "+------------------+----------------------+--------+\n";
        for (auto& [tbl, range, txn_id] : predicate_locks) {
            out << "| " << pad_right(tbl, 16) << " | " << pad_right(range, 20) << " | " << pad_left(std::to_string(txn_id), 6) << " |\n";
        }
        out << "+------------------+----------------------+--------+\n";
    }

    auto wait_for = s.lock_mgr.wait_for_rows();
    if (!wait_for.empty()) {
        out << "\nWait-for graph:\n";
        for (auto& [waiter, blocker] : wait_for) {
            out << "  txn " << waiter << " waits for txn " << blocker << "\n";
        }
    }

    auto history = s.lock_mgr.deadlock_history();
    if (!history.empty()) {
        out << "\nDeadlock history (this session):\n";
        for (auto& [victim, blocker] : history) {
            out << "  txn " << victim << " deadlocked with txn " << blocker << " (victim: " << victim << ")\n";
        }
    }

    std::string result = out.str();
    std::size_t first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        result = "No active row locks.";
    } else {
        std::size_t last = result.find_last_not_of(" \t\r\n");
        result = result.substr(first, last - first + 1);
    }
    return StringResult::Ok(result);
}

// LOCK TABLES / UNLOCK TABLES (V1 -- no Rust/MySQL-parity original, new C++-native
// addition). Deliberately implemented as an entirely separate, purpose-built registry
// (s.explicit_table_locks) rather than reusing table_locks/table_data_locks/LockManager
// -- those are acquired-and-released within a single statement's execution everywhere
// else in this codebase, and this feature's whole point is holding a lock ACROSS
// multiple separate client round-trips (LOCK TABLES ... arbitrary statements ...
// UNLOCK TABLES). Integrating that into the existing, already-hardened
// deadlock-detection-aware machinery (many past phases were spent making it safe against
// subtle deadlocks under the "short-lived, per-statement" assumption) risked
// destabilizing it in ways not worth this feature's value.
//
// Consequence of this V1 scope: a session that never itself issues LOCK TABLES is NOT
// blocked by another session's LOCK TABLES hold -- ordinary DML/DDL from uncoordinated
// sessions proceeds exactly as before. LOCK TABLES here provides real, blocking,
// timeout-respecting coordination only BETWEEN sessions that both use it, which is a
// smaller (but still real, not fake) guarantee than MySQL's full semantics.
StringResult Executor::exec_lock_tables(SharedDatabase& s, std::vector<std::pair<std::string, bool>> tables) {
    for (auto& [table, exclusive] : tables) {
        (void)exclusive;
        if (!s.catalog.get_table(table)) return StringResult::Err("Table '" + strip_db_prefix(table) + "' not found");
    }

    // MySQL itself implicitly releases a session's previous LOCK TABLES locks the moment
    // it issues a new LOCK TABLES -- mirrored here by releasing first, then acquiring.
    release_explicit_table_locks_impl(s);

    // NOWAIT, not a genuine blocking wait -- discovered via a real hang while testing a
    // first version that DID poll-and-sleep here: execute()'s dispatcher holds
    // shared->write() (the WHOLE database's structural exclusive lock) for this entire
    // call, because LOCK TABLES has no per-table fast-path entry in table_lock_set_for.
    // Sleeping/retrying inside this function while conflicted would therefore hold that
    // database-wide lock the whole time, freezing every session's every statement (not
    // just ones touching these tables) -- confirmed concretely: a conflicting session's
    // own UNLOCK TABLES couldn't even run until the waiting session's poll loop gave up,
    // the opposite of the intended behavior. Properly fixing this would mean threading a
    // release/reacquire-the-outer-guard callback through execute()'s dispatcher
    // specifically for this one statement kind -- exactly the kind of change to the
    // existing, already-hardened locking machinery this feature's V1 scope was chosen to
    // avoid. So: fail immediately with a clear, actionable error on any conflict instead
    // of blocking; a caller that wants retry-with-backoff can implement it at the
    // application level.
    auto guard = s.explicit_table_locks->lock();
    for (auto& [table, exclusive] : tables) {
        auto it = guard->find(table);
        if (it == guard->end()) continue;
        for (auto& holder : it->second) {
            if (exclusive || holder.exclusive) {
                return StringResult::Err("Table '" + strip_db_prefix(table) +
                                          "' is already locked by another session's LOCK TABLES. Retry after it UNLOCKs.");
            }
        }
    }
    for (auto& [table, exclusive] : tables) (*guard)[table].push_back(ExplicitTableLockHolder{session_id, exclusive});
    held_explicit_locks = tables;
    return StringResult::Ok("Tables locked.");
}

StringResult Executor::exec_unlock_tables(SharedDatabase& s) {
    release_explicit_table_locks_impl(s);
    return StringResult::Ok("Tables unlocked.");
}

void Executor::release_explicit_table_locks_impl(const SharedDatabase& s) {
    if (held_explicit_locks.empty()) return;
    auto guard = s.explicit_table_locks->lock();
    for (auto& [table, exclusive] : held_explicit_locks) {
        (void)exclusive;
        auto it = guard->find(table);
        if (it == guard->end()) continue;
        auto& holders = it->second;
        holders.erase(std::remove_if(holders.begin(), holders.end(),
                                      [&](const ExplicitTableLockHolder& h) { return h.session_id == session_id; }),
                      holders.end());
        if (holders.empty()) guard->erase(it);
    }
    held_explicit_locks.clear();
}

void Executor::release_explicit_table_locks() {
    if (held_explicit_locks.empty()) return; // avoid shared->read() entirely when there's nothing to do
    auto s = shared->read();
    release_explicit_table_locks_impl(*s);
}

StringResult Executor::exec_checkpoint(SharedDatabase& s) {
    std::size_t dirty_before = s.buffer_pool.usage();
    s.buffer_pool.flush_all(s.disk);
    bool safe = s.active_txn_ids->lock()->empty();
    txn.do_checkpoint(safe);
    std::string msg = "Checkpoint completed. " + std::to_string(dirty_before) + " dirty page(s) flushed.";
    if (!safe) msg += " (WAL truncation deferred — other transaction(s) still active)";
    return StringResult::Ok(msg);
}

} // namespace engine
