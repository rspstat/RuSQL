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

StringResult Executor::exec_begin(SharedDatabase& s) {
    auto res = txn.begin_with_snapshot(*s.active_txn_ids->lock());
    if (res.is_err()) return StringResult::Err(res.error());
    std::uint64_t txn_id = res.value();
    s.active_txn_ids->lock()->insert(txn_id);
    return StringResult::Ok("Transaction " + std::to_string(txn_id) + " started. (isolation: " + isolation_level_debug(txn.isolation_level()) +
                             ")");
}

StringResult Executor::exec_commit(SharedDatabase& s) {
    if (auto res = txn.validate_serializable(s.tables, *s.active_txn_ids->lock()); res.is_err()) {
        apply_rollback(s);
        return StringResult::Err(res.error() + " (auto-rolled back)");
    }

    // MVCC Stage 2: writes now land directly in s.tables as they happen (no more
    // session_tables staging to merge in here) -- COMMIT's remaining job is just to
    // persist whatever tables this transaction actually touched, per the undo log.
    auto dirty_tables = txn.dirty_tables();
    for (auto& table : dirty_tables) {
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            s.buffer_pool.write_page(table, it->second);
            s.buffer_pool.flush_page(table, s.disk);
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

StringResult Executor::exec_commit_phase1(SharedDatabase& s) {
    if (auto res = txn.validate_serializable(s.tables, *s.active_txn_ids->lock()); res.is_err()) {
        apply_rollback(s);
        return StringResult::Err(res.error() + " (auto-rolled back)");
    }

    for (auto& table : txn.dirty_tables()) {
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            s.buffer_pool.write_page(table, it->second);
            s.buffer_pool.flush_page(table, s.disk);
            s.query_cache.invalidate_table(table);
        }
    }

    std::uint64_t txn_id = txn.current_txn_id();
    if (auto res = txn.commit_write_record(); res.is_err()) return StringResult::Err(res.error());
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
    // Only these specific tables' locks are needed for both critical sections below
    // (instead of the whole database's exclusive lock), so an unrelated table's
    // concurrent statement isn't blocked by this commit.
    auto dirty_tables = txn.dirty_tables();
    std::sort(dirty_tables.begin(), dirty_tables.end());

    std::shared_ptr<GroupCommitCoordinator> coord;
    std::shared_ptr<Mutex<std::unordered_set<std::uint64_t>>> active_txn_ids;
    {
        auto guard = acquire_table_locks(shared->read(), dirty_tables, /*exclusive=*/true);
        auto phase1 = exec_commit_phase1(const_cast<SharedDatabase&>(*guard.structural));
        if (phase1.is_err()) return phase1;
        coord = guard.structural->group_commit_coord;
        active_txn_ids = guard.structural->active_txn_ids;
    }

    coord->sync_commit();

    txn.commit_finalize();
    // Only now -- once the commit is truly durable (fsync'd via sync_commit(), WAL/undo
    // pruned via commit_finalize()) -- is it safe for a concurrent checkpoint to treat
    // this txn as fully done. active_txn_ids has its own independent mutex, so this
    // doesn't need to re-acquire the big SharedDatabase write lock.
    active_txn_ids->lock()->erase(txn_id);
    {
        auto guard = acquire_table_locks(shared->read(), dirty_tables, /*exclusive=*/true);
        for (auto& table : dirty_tables) maybe_auto_vacuum(const_cast<SharedDatabase&>(*guard.structural), table);
    }

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
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            idx_it->second = BPlusTree();
            for (auto& row : rows_clone) {
                auto it = row.find(pk_col);
                std::string k = it != row.end() ? it->second : std::string();
                nlohmann::json j = row;
                idx_it->second.insert(k, j.dump());
            }
        }

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
    if (auto res = txn.create_savepoint(name); res.is_err()) return StringResult::Err(res.error());
    return StringResult::Ok("Savepoint '" + name + "' created.");
}

StringResult Executor::exec_release_savepoint(const std::string& name) {
    if (auto res = txn.release_savepoint(name); res.is_err()) return StringResult::Err(res.error());
    return StringResult::Ok("Savepoint '" + name + "' released.");
}

StringResult Executor::exec_rollback_to(SharedDatabase& s, const std::string& name) {
    auto res = txn.rollback_to_savepoint(name);
    if (res.is_err()) return StringResult::Err(res.error());

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
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                       [&](const Row& r) {
                                           auto pit = r.find(pk_col);
                                           auto xnit = r.find("_xmin");
                                           return pit != r.end() && pit->second == entry.key && xnit != r.end() &&
                                                  xnit->second == txn_id_str;
                                       }),
                       rows.end());
        } else if (entry.operation == "UPDATE") {
            if (entry.old_data) {
                try {
                    Row old_row = nlohmann::json::parse(*entry.old_data).get<Row>();
                    // MVCC: this UPDATE created exactly one new "tip" version (tagged
                    // with this txn's id, still live) and marked exactly one earlier
                    // version dead (_xmax == this txn's id) -- remove the tip, then
                    // revive the specific earlier version by content match against
                    // old_data (more than one row can carry _xmax == this txn's id if the
                    // same key was updated more than once in this transaction, so a bare
                    // key+_xmax match would be ambiguous).
                    rows.erase(std::remove_if(rows.begin(), rows.end(),
                                               [&](const Row& r) {
                                                   auto pit = r.find(pk_col);
                                                   if (pit == r.end() || pit->second != entry.key) return false;
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
            for (auto& row : rows) {
                auto pit = row.find(pk_col);
                auto xxit = row.find("_xmax");
                if (pit != row.end() && pit->second == entry.key && xxit != row.end() && xxit->second == txn_id_str) {
                    row["_xmax"] = "0";
                }
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
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            idx_it->second = BPlusTree();
            for (auto& row : rows_clone) {
                auto it = row.find(pk_col);
                std::string k = it != row.end() ? it->second : std::string();
                nlohmann::json j = row;
                idx_it->second.insert(k, j.dump());
            }
        }
        rebuild_secondary_indexes(s, table, rows_clone);
        std::vector<std::string> comp_keys;
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table == table) comp_keys.push_back(k);
        }
        for (auto& k : comp_keys) s.composite_indexes.at(k).rebuild(rows_clone);
    }

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
                        Row row = nlohmann::json::parse(record->data).get<Row>();
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
                                nlohmann::json j = row;
                                if (auto idx_it = sw->indexes.find(table); idx_it != sw->indexes.end()) idx_it->second.insert(key, j.dump());
                                sw->disk.save_table(table, tit->second);
                            }
                        }
                    } catch (...) {
                    }
                } else if (record->op == WalOp::Update) {
                    try {
                        Row new_row = nlohmann::json::parse(record->data).get<Row>();
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
            auto uit = undo_by_txn.find(txn_id);
            if (uit != undo_by_txn.end()) {
                for (auto entry_it = uit->second.rbegin(); entry_it != uit->second.rend(); ++entry_it) {
                    auto& entry = *entry_it;
                    touched_tables.insert(entry.table);
                    std::string pk_col = "id";
                    if (auto* sc = sw->catalog.get_table(entry.table)) {
                        for (auto& c : sc->columns) {
                            if (c.primary_key) {
                                pk_col = c.name;
                                break;
                            }
                        }
                    }
                    if (entry.operation == "INSERT") {
                        auto tit = sw->tables.find(entry.table);
                        if (tit != sw->tables.end()) {
                            auto& rows = tit->second;
                            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                                       [&](const Row& r) {
                                                           auto rit = r.find(pk_col);
                                                           return rit != r.end() && rit->second == entry.key;
                                                       }),
                                       rows.end());
                            sw->disk.save_table(entry.table, rows);
                        }
                    } else if (entry.operation == "UPDATE") {
                        if (entry.old_data) {
                            try {
                                Row old_row = nlohmann::json::parse(*entry.old_data).get<Row>();
                                auto tit = sw->tables.find(entry.table);
                                if (tit != sw->tables.end()) {
                                    for (auto& row : tit->second) {
                                        auto rit = row.find(pk_col);
                                        if (rit != row.end() && rit->second == entry.key) {
                                            row = old_row;
                                            break;
                                        }
                                    }
                                    sw->disk.save_table(entry.table, tit->second);
                                }
                            } catch (...) {
                            }
                        }
                    } else if (entry.operation == "DELETE") {
                        if (entry.old_data) {
                            try {
                                Row old_row = nlohmann::json::parse(*entry.old_data).get<Row>();
                                auto tit = sw->tables.find(entry.table);
                                if (tit != sw->tables.end()) {
                                    tit->second.push_back(old_row);
                                    sw->disk.save_table(entry.table, tit->second);
                                }
                            } catch (...) {
                            }
                        }
                    }
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
        if (auto idx_it = sw->indexes.find(table); idx_it != sw->indexes.end()) {
            idx_it->second = BPlusTree();
            for (auto& row : rows) {
                auto it = row.find(pk_col);
                std::string key = it != row.end() ? it->second : std::string();
                nlohmann::json j = row;
                idx_it->second.insert(key, j.dump());
            }
        }
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

StringResult Executor::exec_set_isolation_level(IsolationLevel level) {
    std::string name;
    switch (level) {
        case IsolationLevel::ReadUncommitted: name = "READ UNCOMMITTED"; break;
        case IsolationLevel::ReadCommitted: name = "READ COMMITTED"; break;
        case IsolationLevel::RepeatableRead: name = "REPEATABLE READ"; break;
        case IsolationLevel::Serializable: name = "SERIALIZABLE"; break;
    }
    txn.set_isolation_level(level);
    return StringResult::Ok("Isolation level set to " + name + ".");
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
