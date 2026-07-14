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

    std::optional<std::vector<std::pair<std::string, std::vector<Row>>>> committed_backups;
    if (txn.is_active()) {
        std::vector<std::pair<std::string, std::vector<Row>>> backups;
        backups.emplace_back(table, session_swap_in(s, table));
        std::vector<std::string> fk_tables;
        for (auto& [tname, tschema] : s.catalog.tables) {
            if (tname == table) continue;
            bool refs = std::any_of(tschema.columns.begin(), tschema.columns.end(), [&](const ColumnDef& c) {
                return c.foreign_key && c.foreign_key->ref_table == table && c.foreign_key->on_delete != FkAction::Restrict;
            });
            if (refs) fk_tables.push_back(tname);
        }
        for (auto& t : fk_tables) backups.emplace_back(t, session_swap_in(s, t));
        committed_backups = std::move(backups);
    }

    if (auto tr = fire_triggers(s, table, "BEFORE", "DELETE"); tr.is_err()) return tr;
    auto result = exec_delete_inner(s, table, condition, returning);
    if (committed_backups) {
        for (auto& [tname, committed] : *committed_backups) session_swap_out(s, tname, std::move(committed));
    }
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

    if (!has_fk_ref && !condition_has_subquery(condition) && !txn.is_active()) {
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
                    s.row_pk_pos.erase(table);
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
                s.row_pk_pos.erase(table);
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
            s.row_pk_pos.erase(table);
        }

        for (auto& del_row : rows_to_delete) index_remove_row(s, table, del_row, pk_col);
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            for (auto& del_row : rows_to_delete) {
                auto it = del_row.find(pk_col);
                idx_it->second.remove(it != del_row.end() ? it->second : std::string());
            }
        }
        std::vector<std::string> comp_keys;
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table == table) comp_keys.push_back(k);
        }
        if (!comp_keys.empty()) {
            std::vector<Row> rows_clone = s.tables.at(table);
            for (auto& k : comp_keys) s.composite_indexes.at(k).rebuild(rows_clone);
        }

        maybe_auto_vacuum(s);
        maybe_auto_analyze(s, table);
        update_stat_rows(s, table, -static_cast<std::int64_t>(deleted));
        maybe_auto_checkpoint(s);

        if (returning) return StringResult::Ok(format_returning_rows(rows_to_delete, *returning));
        return StringResult::Ok(std::to_string(deleted) + " row(s) deleted.");
    }

    // Slow path: FK references, subquery condition, or an active transaction.
    std::vector<Row> rows_to_delete;
    {
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

    for (auto& del_row : rows_to_delete) {
        for (auto& [other_table, cols] : other_tables) {
            for (auto& col : cols) {
                if (col.foreign_key && col.foreign_key->ref_table == table && col.foreign_key->on_delete == FkAction::Restrict) {
                    auto dit = del_row.find(col.foreign_key->ref_column);
                    std::string del_val = dit != del_row.end() ? dit->second : std::string();
                    if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                        bool referenced = std::any_of(oit->second.begin(), oit->second.end(), [&](const Row& r) {
                            if (!is_visible(r)) return false;
                            auto it = r.find(col.name);
                            return it != r.end() && it->second == del_val;
                        });
                        if (referenced) {
                            return StringResult::Err("Foreign key violation: row in '" + table + "' is referenced by '" + other_table + "'.'" +
                                                      col.name + "'");
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
                                    if (it != row.end() && it->second == del_val) row["_xmax"] = txn_id_str;
                                }
                            }
                        } else if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                            auto& rows = oit->second;
                            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                                       [&](const Row& r) {
                                                           if (!is_visible(r)) return false;
                                                           auto it = r.find(col.name);
                                                           return it != r.end() && it->second == del_val;
                                                       }),
                                       rows.end());
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
                                if (it != row.end() && it->second == del_val) row[col.name] = EXECUTOR_NULL_VALUE;
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
                                if (it != row.end() && it->second == del_val) row[col.name] = default_val;
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

    std::string pk_col = "id";
    if (auto* sc = s.catalog.get_table(table)) {
        for (auto& c : sc->columns) {
            if (c.primary_key) {
                pk_col = c.name;
                break;
            }
        }
    }
    std::size_t deleted = 0;

    if (txn.is_active()) {
        std::uint64_t txn_id = txn.current_txn_id();
        std::string txn_id_str = std::to_string(txn_id);
        auto& rows = s.tables.at(table);
        for (auto& row : rows) {
            if (!is_visible(row) || !matches_condexpr(row, condition)) continue;
            auto it = row.find(pk_col);
            std::string key = it != row.end() ? it->second : std::string();

            LockResult lr = s.lock_mgr.acquire(table, key, txn_id);
            if (lr.kind == LockResult::Kind::Conflict) {
                return StringResult::Err("ERROR 1205 (HY000): Lock wait timeout exceeded; row '" + key + "' in '" + table +
                                          "' is held by transaction " + std::to_string(lr.holder) + ". Cannot DELETE.");
            }
            if (lr.kind == LockResult::Kind::Deadlock) {
                return StringResult::Err("Deadlock detected: transaction " + std::to_string(txn_id) + " waits for transaction " +
                                          std::to_string(lr.holder) + " (DELETE '" + table + "'. Transaction " + std::to_string(txn_id) +
                                          " aborted.");
            }

            nlohmann::json old_j = row;
            txn.log_delete(table, key, old_j.dump());
            row["_xmax"] = txn_id_str;
            deleted++;
        }
    } else {
        auto& rows = s.tables.at(table);
        std::size_t before = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](Row& r) { return is_visible(r) && matches_condexpr(r, condition); }), rows.end());
        deleted = before - rows.size();
    }

    if (!txn.is_active()) {
        for (auto& del_row : rows_to_delete) index_remove_row(s, table, del_row, pk_col);
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            for (auto& del_row : rows_to_delete) {
                auto it = del_row.find(pk_col);
                idx_it->second.remove(it != del_row.end() ? it->second : std::string());
            }
        }
        std::vector<std::string> comp_keys;
        for (auto& [k, ci] : s.composite_indexes) {
            if (ci.table == table) comp_keys.push_back(k);
        }
        if (!comp_keys.empty()) {
            std::vector<Row> rows_clone = s.tables.at(table);
            for (auto& k : comp_keys) s.composite_indexes.at(k).rebuild(rows_clone);
        }
        maybe_auto_vacuum(s);
        maybe_auto_analyze(s, table);
    }
    update_stat_rows(s, table, -static_cast<std::int64_t>(deleted));

    maybe_auto_checkpoint(s);
    if (returning) return StringResult::Ok(format_returning_rows(rows_to_delete, *returning));
    return StringResult::Ok(std::to_string(deleted) + " row(s) deleted.");
}

} // namespace engine
