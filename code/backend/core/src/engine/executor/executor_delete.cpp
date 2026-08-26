// Faithful port of the DELETE path from rusql-core/src/engine/executor.rs (Phase 8b):
// exec_delete, exec_delete_inner (fast path with PK-position-index optimizations for
// equality/BETWEEN predicates, slow path with FK RESTRICT/CASCADE/SET NULL/SET DEFAULT
// handling and MVCC logical delete inside transactions).
//
// The fast path is only ever taken when condition_has_subquery(condition) is false, so
// its plain matches_condexpr calls are already exactly right. The slow path's
// rows_to_delete candidate collection branches on condition_has_subquery to use
// matches_condition_with_subquery (Phase 8c) when needed — matching the Rust original,
// which then applies the *plain* matches_condexpr again for the actual deletion pass
// (transaction MVCC mark, or the non-transaction retain) even along the slow path; this
// mixed behavior is preserved faithfully rather than "fixed" to be fully subquery-aware.

#include "engine/executor/executor.hpp"

#include <algorithm>

namespace engine {

bool Executor::condition_has_subquery(const std::optional<CondExpr>& condition) {
    struct Walker {
        static bool check(const CondExpr& e) {
            if (auto* v = std::get_if<CondExpr::And>(&e.data)) return check(*v->lhs) || check(*v->rhs);
            if (auto* v = std::get_if<CondExpr::Or>(&e.data)) return check(*v->lhs) || check(*v->rhs);
            if (auto* v = std::get_if<CondExpr::Not>(&e.data)) return check(*v->inner);
            if (auto* v = std::get_if<CondExpr::Leaf>(&e.data)) return std::holds_alternative<ConditionValue::Subquery>(v->condition.value.data);
            return false;
        }
    };
    return condition && Walker::check(*condition);
}

std::optional<std::string> Executor::extract_pk_eq_value(const std::optional<CondExpr>& condition, const std::string& pk_col) {
    if (!condition) return std::nullopt;
    auto* leaf = std::get_if<CondExpr::Leaf>(&condition->data);
    if (!leaf || leaf->condition.op != Operator::Eq) return std::nullopt;
    auto* col = std::get_if<ArithExpr::Col>(&leaf->condition.left.data);
    auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data);
    if (col && lit && col->name == pk_col) return lit->value;
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> Executor::extract_pk_between_value(const std::optional<CondExpr>& condition,
                                                                                         const std::string& pk_col) {
    if (!condition) return std::nullopt;
    auto* leaf = std::get_if<CondExpr::Leaf>(&condition->data);
    if (!leaf || leaf->condition.op != Operator::Between) return std::nullopt;
    auto* col = std::get_if<ArithExpr::Col>(&leaf->condition.left.data);
    auto* between = std::get_if<ConditionValue::Between>(&leaf->condition.value.data);
    if (col && between && col->name == pk_col) return std::make_pair(between->lo, between->hi);
    return std::nullopt;
}

StringResult Executor::exec_delete(SharedDatabase& s, const std::string& table, std::optional<CondExpr> condition,
                                    std::optional<std::vector<SelectColumn>> returning) {
    if (s.views.count(table)) {
        if (auto resolved = resolve_updatable_view(s, table)) {
            auto merged_cond = merge_conditions(resolved->second, condition);
            return exec_delete(s, resolved->first, merged_cond, returning);
        }
        return StringResult::Err("View '" + strip_db_prefix(table) + "' is not updatable");
    }

    if (auto tr = fire_triggers(s, table, "BEFORE", "DELETE"); tr.is_err()) return tr;
    auto result = exec_delete_inner(s, table, condition, returning);
    if (result.is_ok()) {
        if (auto tr = fire_triggers(s, table, "AFTER", "DELETE"); tr.is_err()) return tr;
    }
    return result;
}

StringResult Executor::exec_delete_inner(SharedDatabase& s, const std::string& table, const std::optional<CondExpr>& condition,
                                          const std::optional<std::vector<SelectColumn>>& returning) {
    bool has_fk_ref = std::any_of(s.catalog.tables.begin(), s.catalog.tables.end(), [&](auto& kv) {
        return std::any_of(kv.second.columns.begin(), kv.second.columns.end(),
                            [&](const ColumnDef& c) { return c.foreign_key && c.foreign_key->ref_table == table; });
    });

    // MVCC: a physical (hard) delete is only safe when no session anywhere has an open
    // snapshot that might still need to see the pre-delete row -- otherwise it must be a
    // soft delete (mark _xmax, keep the row physically present) exactly like an
    // in-progress transaction's own DELETE already does below, or another session's
    // still-open REPEATABLE READ/SERIALIZABLE snapshot would lose a row it's entitled to see.
    bool globally_quiescent = s.active_txn_ids->lock()->empty();

    // Row-level-concurrency Stage 4: reconstruct the equivalent Statement::Delete to
    // reuse table_lock_set_for's table-closure computation (FK child neighbors + any
    // condition-subquery tables) -- same technique as exec_update_inner, and for the
    // same reason (deterministic re-derivation of the SAME set already used for
    // table_locks, far simpler than re-walking CondExpr subqueries by hand).
    Statement lock_probe_stmt;
    lock_probe_stmt.data = Statement::Delete{table, condition, returning};
    std::vector<std::string> neighbor_tables;
    if (auto extra = table_lock_set_for(s, lock_probe_stmt)) {
        for (auto& t : *extra) {
            if (t != table) neighbor_tables.push_back(t);
        }
    }

    // One claim-id for this whole statement (see exec_insert_inner/exec_update_inner for
    // the identical pattern + rationale): reuses the active explicit transaction's id, or
    // a fresh one-off id (released by RowClaimGuard at statement end) for autocommit.
    std::uint64_t claim_txn_id_setup = txn.current_txn_id();
    bool autocommit_claim = (claim_txn_id_setup == 0);
    std::uint64_t claim_txn_id = autocommit_claim ? s.txn_io->next_id() : claim_txn_id_setup;
    RowClaimGuard row_claim_guard(s.lock_mgr, claim_txn_id, /*owns=*/autocommit_claim);
    // Real-blocking-wait stage: one deadline for the WHOLE statement -- see
    // exec_insert_inner's identical field for the rationale.
    auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(lock_wait_timeout_ms);

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
    // FK cascade mutated/erased s.tables[other_table] rows but never touched that table's
    // PK B+Tree (s.indexes[other_table], keyed by bare table name), secondary/hash indexes
    // (index_insert_row/index_remove_row), or composite indexes -- a PK-indexed point
    // lookup on the cascaded table kept serving stale data indefinitely. Mirrors the
    // refresh pattern this same function's own refresh_indexes_for_soft_delete uses below
    // for the primary table, generalized to take table/pk_col as parameters since cascade
    // operates on `other_table`. A cascade never changes the cascaded row's OWN pk_col
    // value (SetNull/SetDefault only ever touch the FK column pointing at `table`), so the
    // PK B+Tree side only ever needs the single-insert overwrite path.
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
    // Row-level-concurrency Stage 4: FK cascade previously took no LockManager claim on
    // the child table's affected rows at all (safe only because the whole child table
    // was held exclusively via table_locks for the statement's duration) -- claim each
    // affected row (single-column PK only, V1 scope, same limitation as everywhere else)
    // before mutating/removing it, mirroring exec_update_inner's cascade claim.
    //
    // Real-blocking-wait stage: claim_cascade_row() itself never blocks -- it only probes
    // (timeout=0) and reports the outcome. See exec_update_inner's identical CascadeClaim/
    // CascadeAttemptResult/try_cascade_once pattern (executor_update.cpp) for the full
    // reasoning -- restarting the whole cascade computation from scratch after a blocked
    // retry is safe/idempotent here too: the `it->second == del_val` match check
    // naturally excludes rows already soft-deleted (is_visible() now false) or physically
    // erased (no longer present) by an earlier attempt.
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
    // Shared boilerplate for all 3 mutating FkActions below (Cascade/SetNull/SetDefault):
    // claim the row, translate a Deadlock/Conflict LockResult into the matching
    // CascadeAttemptResult, or fall through (nullopt) to let the caller proceed with the
    // actual mutation. `action_desc` only affects the error message text.
    auto claim_or_signal = [&](const std::string& other_table, const Row& row,
                                const char* action_desc) -> std::optional<CascadeAttemptResult> {
        auto claim = claim_cascade_row(other_table, row);
        if (!claim) return std::nullopt;
        if (claim->result.kind == LockResult::Kind::Deadlock) {
            return CascadeAttemptResult::hard_error(StringResult::Err(
                "Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                std::to_string(claim->result.holder) + " (DELETE cascade " + action_desc + " on '" + other_table + "'). Transaction " +
                std::to_string(claim_txn_id) + " aborted."));
        }
        if (claim->result.kind == LockResult::Kind::Conflict) {
            return CascadeAttemptResult::needs_retry(other_table, claim->pk_val);
        }
        return std::nullopt;
    };

    if (!has_fk_ref && !condition_has_subquery(condition) && !txn.is_active() && globally_quiescent) {
        // Row-level-concurrency Stage 4: EXCLUSIVE for this whole fast path -- every
        // branch below is scan-and-mutate/swap-remove on `table`'s own vector in one
        // pass (can't be split into a SHARED read phase + brief EXCLUSIVE write phase
        // the way INSERT/UPDATE can), and has_fk_ref is false here so `table` is the
        // ONLY table this path ever touches.
        auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        std::string pk_col = "id";
        if (auto* sc = s.catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    pk_col = c.name;
                    break;
                }
            }
        }
        std::vector<Row> rows_to_delete;
        std::size_t deleted = 0;

        bool used_pos_idx = false;
        if (auto pk_val = extract_pk_eq_value(condition, pk_col)) {
            std::optional<std::size_t> pos_opt;
            if (auto mit = s.row_pk_pos.find(table); mit != s.row_pk_pos.end()) {
                if (auto pit = mit->second.find(*pk_val); pit != mit->second.end()) pos_opt = pit->second;
            }
            if (pos_opt) {
                auto tit = s.tables.find(table);
                if (tit == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");
                auto& rows = tit->second;
                bool valid = *pos_opt < rows.size();
                if (valid) {
                    auto it = rows[*pos_opt].find(pk_col);
                    valid = it != rows[*pos_opt].end() && it->second == *pk_val && is_visible(rows[*pos_opt]);
                }
                if (valid) {
                    Row del_row = std::move(rows[*pos_opt]);
                    rows[*pos_opt] = std::move(rows.back());
                    rows.pop_back();
                    auto& pos_map = s.row_pk_pos[table];
                    if (*pos_opt < rows.size()) {
                        auto sit = rows[*pos_opt].find(pk_col);
                        std::string swapped_pk = sit != rows[*pos_opt].end() ? sit->second : std::string();
                        pos_map[swapped_pk] = *pos_opt;
                        pos_map.erase(*pk_val);
                    } else {
                        pos_map.erase(*pk_val);
                    }
                    rows_to_delete.push_back(std::move(del_row));
                    used_pos_idx = true;
                } else {
                    // Stage 4: clear the entry's VALUE in place rather than erasing the
                    // top-level key -- under per-table locking, only DDL (CREATE/DROP
                    // TABLE, structural exclusive) may add/remove table-name keys from
                    // this map; a DML-triggered erase-then-later-reinsert on a shared,
                    // table-keyed unordered_map would race with a different table's
                    // concurrent DML doing the same.
                    if (auto it = s.row_pk_pos.find(table); it != s.row_pk_pos.end()) it->second.clear();
                }
            }
        }

        if (used_pos_idx) {
            deleted = rows_to_delete.size();
        } else if (auto range = extract_pk_between_value(condition, pk_col)) {
            std::vector<std::string> pks_to_delete;
            if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) pks_to_delete = idx_it->second.range_keys(range->first, range->second);

            std::vector<std::pair<std::size_t, std::string>> pos_map_snapshot;
            if (auto mit = s.row_pk_pos.find(table); mit != s.row_pk_pos.end()) {
                for (auto& pk : pks_to_delete) {
                    if (auto pit = mit->second.find(pk); pit != mit->second.end()) pos_map_snapshot.emplace_back(pit->second, pk);
                }
            }

            if (!pks_to_delete.empty() && pos_map_snapshot.size() == pks_to_delete.size()) {
                std::sort(pos_map_snapshot.begin(), pos_map_snapshot.end(), [](auto& a, auto& b) { return a.first > b.first; });
                auto tit = s.tables.find(table);
                if (tit == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");
                auto& rows = tit->second;
                auto& pos_map = s.row_pk_pos[table];
                for (auto& [pos, pk_val] : pos_map_snapshot) {
                    Row del_row = std::move(rows[pos]);
                    rows[pos] = std::move(rows.back());
                    rows.pop_back();
                    if (pos < rows.size()) {
                        auto sit = rows[pos].find(pk_col);
                        std::string swapped_pk = sit != rows[pos].end() ? sit->second : std::string();
                        pos_map[swapped_pk] = pos;
                        pos_map.erase(pk_val);
                    } else {
                        pos_map.erase(pk_val);
                    }
                    rows_to_delete.push_back(std::move(del_row));
                }
                deleted = rows_to_delete.size();
            } else {
                auto tit = s.tables.find(table);
                if (tit == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");
                auto& rows = tit->second;
                std::size_t before = rows.size();
                rows.erase(std::remove_if(rows.begin(), rows.end(),
                                           [&](Row& r) {
                                               if (is_visible(r) && matches_condexpr(r, condition)) {
                                                   rows_to_delete.push_back(r);
                                                   return true;
                                               }
                                               return false;
                                           }),
                           rows.end());
                deleted = before - rows.size();
                if (auto it = s.row_pk_pos.find(table); it != s.row_pk_pos.end()) it->second.clear();
            }
        } else {
            auto tit = s.tables.find(table);
            if (tit == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");
            auto& rows = tit->second;
            std::size_t before = rows.size();
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                       [&](Row& r) {
                                           if (is_visible(r) && matches_condexpr(r, condition)) {
                                               rows_to_delete.push_back(r);
                                               return true;
                                           }
                                           return false;
                                       }),
                       rows.end());
            deleted = before - rows.size();
            if (auto it = s.row_pk_pos.find(table); it != s.row_pk_pos.end()) it->second.clear();
        }

        for (auto& del_row : rows_to_delete) index_remove_row(s, table, del_row, pk_col);
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            for (auto& del_row : rows_to_delete) {
                auto it = del_row.find(pk_col);
                idx_it->second.remove(it != del_row.end() ? it->second : std::string());
            }
        }
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table != table) continue;
            for (auto& del_row : rows_to_delete) ci.remove_row(del_row);
        }

        maybe_auto_vacuum(s, table);
        maybe_auto_analyze(s, table);
        update_stat_rows(s, table, -static_cast<std::int64_t>(deleted));
        // Row-level-concurrency Stage 4/5 correctness fix (found via concurrent-reader
        // monotonicity stress testing): invalidate the query cache HERE, still holding
        // table_data_locks EXCLUSIVE -- see exec_insert_inner's identical comment for why
        // execute_sql's own later invalidate_table call is too late to be load-bearing.
        s.query_cache.invalidate_table(table);
        maybe_auto_checkpoint(s);

        if (returning) return StringResult::Ok(format_returning_rows(rows_to_delete, *returning));
        return StringResult::Ok(std::to_string(deleted) + " row(s) deleted.");
    }

    // Slow path: FK references, subquery condition, or an active transaction.
    std::vector<Row> rows_to_delete;
    {
        // Row-level-concurrency Stage 4: SHARED -- this block only ever reads
        // s.tables[table] (candidate collection, possibly via matches_condition_with_
        // subquery's exec_select recursion, which is itself lock-free by design -- see
        // execute()'s dispatch comment), never resizes it.
        auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/false);
        auto tit = s.tables.find(table);
        if (tit == s.tables.end()) return StringResult::Err("Table '" + table + "' not found");
        if (condition_has_subquery(condition)) {
            std::vector<Row> candidates;
            for (auto& r : tit->second) {
                if (is_visible(r)) candidates.push_back(r);
            }
            for (auto& r : candidates) {
                if (matches_condition_with_subquery(s, r, condition)) rows_to_delete.push_back(r);
            }
        } else {
            for (auto& r : tit->second) {
                if (is_visible(r) && matches_condexpr(r, condition)) rows_to_delete.push_back(r);
            }
        }
    }

    std::vector<std::pair<std::string, std::vector<ColumnDef>>> other_tables;
    for (auto& [name, schema] : s.catalog.tables) {
        if (name != table) other_tables.emplace_back(name, schema.columns);
    }

    // Row-level-concurrency Stage 4: EXCLUSIVE on every FK-child table touched below --
    // Cascade's non-txn branch does a real erase (shape change) on `other_table`, so
    // EXCLUSIVE (not SHARED) is required there; applying it uniformly for the whole
    // Restrict-check + Cascade/SetNull/SetDefault section (rather than conditionally per
    // txn-state/action) keeps this tractable and is always a safe superset of what a
    // read or in-place mutation alone would need.
    // Real-blocking-wait stage: the nested loop below (rows_to_delete -> other_tables ->
    // cols -> per-FkAction row scan, 4 levels deep, plus a 5th for Cascade's separate
    // claim-then-erase passes) is wrapped in try_cascade_once() so any conflict can
    // `return` straight out of every level via the lambda boundary -- see
    // exec_update_inner's identical pattern for the full reasoning.
    auto try_cascade_once = [&]() -> CascadeAttemptResult {
    auto neighbor_write_lock = acquire_table_data_locks(s, neighbor_tables, /*exclusive=*/true);

    for (auto& del_row : rows_to_delete) {
        for (auto& [other_table, cols] : other_tables) {
            for (auto& col : cols) {
                if (col.foreign_key && col.foreign_key->ref_table == table && col.foreign_key->on_delete == FkAction::Restrict) {
                    auto dit = del_row.find(col.foreign_key->ref_column);
                    std::string del_val = dit != del_row.end() ? dit->second : std::string();
                    if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                        bool referenced = index_or_scan_exists(s, other_table, oit->second, col.name, del_val, is_visible);
                        if (referenced) {
                            return CascadeAttemptResult::hard_error(StringResult::Err(
                                "Foreign key violation: row in '" + table + "' is referenced by '" + other_table + "'.'" + col.name + "'"));
                        }
                    }
                }
            }
        }
    }

    for (auto& del_row : rows_to_delete) {
        for (auto& [other_table, cols] : other_tables) {
            for (auto& col : cols) {
                if (!col.foreign_key || col.foreign_key->ref_table != table) continue;
                auto dit = del_row.find(col.foreign_key->ref_column);
                std::string del_val = dit != del_row.end() ? dit->second : std::string();

                switch (col.foreign_key->on_delete) {
                    case FkAction::Restrict:
                        break; // already checked above
                    case FkAction::Cascade: {
                        if (txn.is_active()) {
                            std::string txn_id_str = std::to_string(txn.current_txn_id());
                            if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                                for (auto& row : oit->second) {
                                    if (!is_visible(row)) continue;
                                    auto it = row.find(col.name);
                                    if (it != row.end() && it->second == del_val) {
                                        if (auto sig = claim_or_signal(other_table, row, "Cascade")) return *sig;
                                        Row cascade_old_row = row;
                                        row["_xmax"] = txn_id_str;
                                        if (auto opc = single_pk_col(other_table)) refresh_cascade_indexes(other_table, *opc, cascade_old_row, row);
                                    }
                                }
                            }
                        } else if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                            auto& rows = oit->second;
                            std::vector<Row> cascade_erased;
                            for (auto& r : rows) {
                                if (!is_visible(r)) continue;
                                auto it = r.find(col.name);
                                if (it != r.end() && it->second == del_val) {
                                    if (auto sig = claim_or_signal(other_table, r, "Cascade")) return *sig;
                                    cascade_erased.push_back(r);
                                }
                            }
                            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                                       [&](const Row& r) {
                                                           if (!is_visible(r)) return false;
                                                           auto it = r.find(col.name);
                                                           return it != r.end() && it->second == del_val;
                                                       }),
                                       rows.end());
                            if (auto opc = single_pk_col(other_table)) {
                                auto idx_it = s.indexes.find(other_table);
                                for (auto& er : cascade_erased) {
                                    index_remove_row(s, other_table, er, *opc);
                                    if (idx_it != s.indexes.end()) {
                                        auto pkv = er.find(*opc);
                                        idx_it->second.remove(pkv != er.end() ? pkv->second : std::string());
                                    }
                                }
                                for (auto& [k, ci] : s.composite_indexes) {
                                    if (ci.table != other_table) continue;
                                    for (auto& er : cascade_erased) ci.remove_row(er);
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
                        if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                            for (auto& row : oit->second) {
                                if (!is_visible(row)) continue;
                                auto it = row.find(col.name);
                                if (it != row.end() && it->second == del_val) {
                                    if (auto sig = claim_or_signal(other_table, row, "SetNull")) return *sig;
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
                            auto cit = std::find_if(osc->columns.begin(), osc->columns.end(), [&](const ColumnDef& c2) { return c2.name == col.name; });
                            if (cit != osc->columns.end() && cit->default_value) default_val = *cit->default_value;
                        }
                        if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                            for (auto& row : oit->second) {
                                if (!is_visible(row)) continue;
                                auto it = row.find(col.name);
                                if (it != row.end() && it->second == del_val) {
                                    if (auto sig = claim_or_signal(other_table, row, "SetDefault")) return *sig;
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
    // Row-level-concurrency Stage 4/5 correctness fix: invalidate the cache for every
    // FK-cascade-touched table HERE, still holding neighbor_write_lock EXCLUSIVE.
    for (auto& t : neighbor_tables) s.query_cache.invalidate_table(t);
    return CascadeAttemptResult::done();
    }; // neighbor_write_lock (EXCLUSIVE, a local inside this lambda) released on every return above

    for (;;) {
        auto attempt = try_cascade_once();
        if (attempt.outcome == CascadeAttemptResult::Outcome::Done) break;
        if (attempt.outcome == CascadeAttemptResult::Outcome::HardError) return attempt.err;
        // NeedsRetry: release table_locks (neighbor_write_lock is already released, by
        // ordinary scope rules, since try_cascade_once() already returned) and block on
        // just the one contested row, then restart the whole cascade computation.
        release_table_locks_for_block();
        auto lr2 = block_on_row(s.lock_mgr, attempt.conflict_table, attempt.conflict_key, claim_txn_id, /*exclusive=*/true, lock_deadline);
        reacquire_table_locks_after_block();
        if (lr2.kind == LockResult::Kind::Deadlock) {
            return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                      std::to_string(lr2.holder) + " (DELETE cascade on '" + attempt.conflict_table + "'). Transaction " +
                                      std::to_string(claim_txn_id) + " aborted.");
        }
        if (lr2.kind != LockResult::Kind::Granted) {
            return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + attempt.conflict_key + "' in '" +
                                      attempt.conflict_table + "' is held by transaction " + std::to_string(lr2.holder) +
                                      ". Cannot cascade DELETE.");
        }
        // else: granted -- loop back and retry try_cascade_once() from scratch.
    }

    std::string pk_col = "id";
    std::size_t pk_col_count = 0;
    if (auto* sc = s.catalog.get_table(table)) {
        for (auto& c : sc->columns) {
            if (c.primary_key) {
                if (pk_col_count == 0) pk_col = c.name;
                pk_col_count++;
            }
        }
    }

    std::size_t deleted = 0;
    bool hard_deleted = false;

    // MVCC: a soft delete only flips _xmax on the Row object living in s.tables -- the PK
    // B+Tree and every secondary/hash index hold their own separate JSON copy of that same
    // row's data (inserted once at INSERT/UPDATE time), which this does NOT touch. Left
    // alone, index-based fast-path reads (AccessPath::PkPoint et al.) would keep serving
    // the stale pre-delete copy (_xmax still "0") forever, even to a session with no
    // reason to still see it -- re-upsert the PK entry and refresh secondary/hash indexes
    // via the same remove-then-insert pattern UPDATE already uses for its two-row swap.
    auto refresh_indexes_for_soft_delete = [&](const Row& old_row, const Row& new_row, const std::string& key) {
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            nlohmann::json j = new_row;
            idx_it->second.insert(key, j.dump());
        }
        index_remove_row(s, table, old_row, pk_col);
        index_insert_row(s, table, new_row);
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table != table) continue;
            ci.remove_row(old_row);
            ci.insert_row(new_row);
        }
    };

    if (txn.is_active()) {
        // Row-level-concurrency Stage 4 correctness fix: EXCLUSIVE, not SHARED. In-place
        // field mutation (`row["_xmax"] = ...` below) is NOT safe under SHARED alone --
        // the per-row LockManager claim only keeps a SECOND WRITER from racing the same
        // row, but a plain autocommit SELECT's scan/copy takes NO claim at all and can
        // still hold table_data_locks SHARED at the exact same moment, racing this
        // mutation at the raw std::map level (undefined behavior, found via concurrent-
        // reader stress testing: manifested as a monotonicity violation from a torn
        // read). EXCLUSIVE is required whenever an EXISTING row's fields are mutated in
        // place, matching exec_update_inner's mutation phase.
        std::uint64_t txn_id = txn.current_txn_id();
        std::string txn_id_str = std::to_string(txn_id);
        // Gap lock: only under RR/Serializable and only for single-column PK tables (V1
        // scope), matching the same gating used at the FOR UPDATE/FOR SHARE call sites.
        // Registered once, before the retry loop below -- acquire_gap has no dedup, so
        // calling it again on every retry attempt would leave duplicate gap-lock entries.
        if (pk_col_count == 1 &&
            (txn.isolation_level() == IsolationLevel::RepeatableRead || txn.isolation_level() == IsolationLevel::Serializable)) {
            GapRange range = extract_pk_gap_range(condition, pk_col);
            s.lock_mgr.acquire_gap(table, range.lo, range.lo_inclusive, range.hi, range.hi_inclusive, txn_id);
        }
        // Real-blocking-wait stage: on a row-lock conflict, release table_lock (must not
        // block while holding it), block on just that one row, then restart the WHOLE
        // scan from scratch. This is safe/idempotent: is_visible() treats ANY nonzero
        // _xmax -- including one this same loop already stamped in an earlier attempt --
        // as invisible, so already-soft-deleted rows are silently skipped on retry, and
        // LockManager grants a re-request for an already-held claim for free (re-entrant).
        for (;;) {
            auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
            bool conflict = false;
            std::string conflict_key;
            auto& rows = s.tables.at(table);
            for (auto& row : rows) {
                if (!is_visible(row) || !matches_condexpr(row, condition)) continue;
                auto it = row.find(pk_col);
                std::string key = it != row.end() ? it->second : std::string();

                LockResult lr = s.lock_mgr.acquire(table, key, txn_id);
                if (lr.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                              std::to_string(lr.holder) + " (DELETE '" + table + "'. Transaction " + std::to_string(txn_id) +
                                              " aborted.");
                }
                if (lr.kind == LockResult::Kind::Conflict) {
                    conflict = true;
                    conflict_key = key;
                    break;
                }

                nlohmann::json old_j = row;
                txn.log_delete(table, key, old_j.dump());
                row["_xmax"] = txn_id_str;
                refresh_indexes_for_soft_delete(old_j.get<Row>(), row, key);
                deleted++;
            }
            if (!conflict) {
                // Row-level-concurrency Stage 4/5 correctness fix: invalidate the cache
                // HERE, still holding table_lock EXCLUSIVE -- see exec_insert_inner's
                // comment.
                if (deleted > 0) s.query_cache.invalidate_table(table);
                break;
            }
            table_lock = DataLockGuard{}; // release -- must not block while holding it
            // Real-blocking-wait stage, second correctness fix: also release
            // table_locks[table] before blocking -- see release_table_locks_for_block's
            // doc comment in executor.hpp.
            release_table_locks_for_block();
            auto lr2 = block_on_row(s.lock_mgr, table, conflict_key, txn_id, /*exclusive=*/true, lock_deadline);
            reacquire_table_locks_after_block();
            if (lr2.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                          std::to_string(lr2.holder) + " (DELETE '" + table + "'. Transaction " + std::to_string(txn_id) +
                                          " aborted.");
            }
            if (lr2.kind != LockResult::Kind::Granted) {
                return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + conflict_key + "' in '" + table +
                                          "' is held by transaction " + std::to_string(lr2.holder) + ". Cannot DELETE.");
            }
            // else: granted -- loop back, reacquire table_lock, retry the whole scan.
        }
    } else if (globally_quiescent) {
        // Row-level-concurrency Stage 4: EXCLUSIVE -- real erase (shape change).
        auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        auto& rows = s.tables.at(table);
        std::size_t before = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](Row& r) { return is_visible(r) && matches_condexpr(r, condition); }), rows.end());
        deleted = before - rows.size();
        hard_deleted = true;
        if (deleted > 0) s.query_cache.invalidate_table(table);
    } else {
        // Row-level-concurrency Stage 4 correctness fix: EXCLUSIVE, not SHARED -- same
        // reasoning as the txn-active branch above (in-place `row["_xmax"] = ...` is not
        // safe under SHARED against a concurrent plain-SELECT reader). Also adds the
        // per-row LockManager claim this autocommit branch previously had NONE of at all
        // (safe only under the old whole-table-exclusive model): two autocommit DELETEs
        // racing the same row now get told apart instead of silently double-soft-
        // deleting/corrupting index refresh order.
        //
        // MVCC: another session has an open snapshot that may still need to see these
        // rows -- soft-delete instead (same shape as the in-txn branch above), tagged
        // with a fresh global id since autocommit has no txn of its own (mirrors INSERT's
        // tagging_txn_id since Stage 1). Computed once, outside the retry loop below --
        // stable for the whole statement, unlike the per-row LockManager claim_txn_id.
        std::uint64_t my_id = tagging_txn_id(s);
        std::string txn_id_str = std::to_string(my_id);
        // Real-blocking-wait stage: same release-block-restart-scan pattern as the
        // txn-active branch above -- see its comment for why restarting from scratch is
        // safe/idempotent here.
        for (;;) {
            auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
            bool conflict = false;
            std::string conflict_key;
            auto& rows = s.tables.at(table);
            for (auto& row : rows) {
                if (!is_visible(row) || !matches_condexpr(row, condition)) continue;
                auto it = row.find(pk_col);
                std::string key = it != row.end() ? it->second : std::string();

                LockResult lr = s.lock_mgr.acquire(table, key, claim_txn_id);
                if (lr.kind == LockResult::Kind::Deadlock) {
                    return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                              std::to_string(lr.holder) + " (DELETE '" + table + "'. Transaction " + std::to_string(claim_txn_id) +
                                              " aborted.");
                }
                if (lr.kind == LockResult::Kind::Conflict) {
                    conflict = true;
                    conflict_key = key;
                    break;
                }

                Row old_row = row;
                row["_xmax"] = txn_id_str;
                refresh_indexes_for_soft_delete(old_row, row, key);
                deleted++;
            }
            if (!conflict) {
                // Row-level-concurrency Stage 4/5 correctness fix: invalidate the cache
                // HERE, still holding table_lock EXCLUSIVE -- see exec_insert_inner's
                // comment.
                if (deleted > 0) s.query_cache.invalidate_table(table);
                break;
            }
            table_lock = DataLockGuard{}; // release -- must not block while holding it
            // Real-blocking-wait stage, second correctness fix: also release
            // table_locks[table] before blocking -- see release_table_locks_for_block's
            // doc comment in executor.hpp.
            release_table_locks_for_block();
            auto lr2 = block_on_row(s.lock_mgr, table, conflict_key, claim_txn_id, /*exclusive=*/true, lock_deadline);
            reacquire_table_locks_after_block();
            if (lr2.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(claim_txn_id) + " waits for transaction " +
                                          std::to_string(lr2.holder) + " (DELETE '" + table + "'. Transaction " + std::to_string(claim_txn_id) +
                                          " aborted.");
            }
            if (lr2.kind != LockResult::Kind::Granted) {
                return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + conflict_key + "' in '" + table +
                                          "' is held by transaction " + std::to_string(lr2.holder) + ". Cannot DELETE.");
            }
            // else: granted -- loop back, reacquire table_lock, retry the whole scan.
        }
    }

    if (hard_deleted) {
        // Row-level-concurrency Stage 4: EXCLUSIVE -- maybe_auto_vacuum erases rows
        // (shape-changing); wrapping the whole tail uniformly matches exec_insert_inner/
        // exec_update_inner.
        auto table_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        for (auto& del_row : rows_to_delete) index_remove_row(s, table, del_row, pk_col);
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            for (auto& del_row : rows_to_delete) {
                auto it = del_row.find(pk_col);
                idx_it->second.remove(it != del_row.end() ? it->second : std::string());
            }
        }
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table != table) continue;
            for (auto& del_row : rows_to_delete) ci.remove_row(del_row);
        }
        maybe_auto_vacuum(s, table);
        maybe_auto_analyze(s, table);
        s.query_cache.invalidate_table(table);
    }
    update_stat_rows(s, table, -static_cast<std::int64_t>(deleted));

    maybe_auto_checkpoint(s);
    if (returning) return StringResult::Ok(format_returning_rows(rows_to_delete, *returning));
    return StringResult::Ok(std::to_string(deleted) + " row(s) deleted.");
}

} // namespace engine
