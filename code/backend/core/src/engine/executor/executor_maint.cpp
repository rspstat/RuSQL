// Faithful port of VACUUM and ANALYZE TABLE from rusql-core/src/engine/executor.rs
// (Phase 8f): exec_vacuum, exec_analyze_table.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace engine {

namespace {
std::optional<double> try_parse_f64(const std::string& s) { return parse_number(s); }
} // namespace

StringResult Executor::exec_vacuum(SharedDatabase& s, std::optional<std::string> table) {
    std::vector<std::string> targets;
    if (table) {
        if (!s.tables.count(*table)) return StringResult::Err("Table '" + *table + "' not found");
        targets.push_back(*table);
    } else {
        for (auto& [k, _] : s.tables) targets.push_back(k);
    }

    std::uint64_t horizon = oldest_active_txn_id(s);
    std::size_t total_removed = 0;
    for (auto& t : targets) {
        auto& rows = s.tables.at(t);
        std::size_t before = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return is_vacuumable(r, horizon); }), rows.end());
        std::size_t removed = before - rows.size();
        total_removed += removed;

        if (removed > 0) {
            std::vector<Row> rows_clone = s.tables.at(t);
            if (auto idx_it = s.indexes.find(t); idx_it != s.indexes.end()) {
                // PLAN.md P0 fix: the Rust original used `row.values().next()` here — an
                // arbitrary (HashMap-iteration-order-dependent) value, not necessarily the
                // PK, silently corrupting the rebuilt index. Resolve the real PK column
                // from the schema instead (same pattern as the insert/update reindex paths
                // below in executor_dml.cpp).
                std::string pk_col_name;
                if (auto* schema = s.catalog.get_table(t)) {
                    for (auto& c : schema->columns) {
                        if (c.primary_key) { pk_col_name = c.name; break; }
                    }
                    if (pk_col_name.empty() && !schema->columns.empty()) pk_col_name = schema->columns.front().name;
                }
                idx_it->second = build_pk_tree(rows_clone, pk_col_name, idx_it->second.kinds());
            }
            std::vector<std::string> comp_keys;
            for (auto& [k, ci] : s.composite_indexes) {
                if (ci.table == t) comp_keys.push_back(k);
            }
            for (auto& k : comp_keys) s.composite_indexes.at(k).rebuild(rows_clone);

            s.buffer_pool.write_through(t, rows_clone, s.disk);
            redo_mark_flushed(s, t); // the rows that were removed may still be described by the redo log: its older ops for this table must not be replayed (the log is not emptied for a VACUUM)
        }
    }

    return StringResult::Ok("VACUUM complete. " + std::to_string(total_removed) + " dead row(s) removed.");
}

StringResult Executor::exec_analyze_table(SharedDatabase& s, const std::string& table) const {
    auto tit = s.tables.find(table);
    if (tit == s.tables.end()) return StringResult::Err("Table '" + display_name(table) + "' not found");

    // MVCC: always a fresh "right now" ReadCommitted-style ctx, regardless of the calling
    // transaction's own isolation level -- statistics should reflect the current committed
    // state, not stale data, and must not count another session's uncommitted rows.
    SnapshotCtx now_ctx{txn.current_txn_id(), s.txn_io->peek_next_id(), *s.active_txn_ids->lock()};
    std::vector<Row> rows;
    for (auto& r : tit->second) {
        if (is_visible_for_read(r, now_ctx)) rows.push_back(r);
    }

    std::size_t total = rows.size();
    std::unordered_map<std::string, std::unordered_set<std::string>> distinct;
    std::unordered_map<std::string, std::size_t> null_cnt;
    std::unordered_map<std::string, std::string> min_map;
    std::unordered_map<std::string, std::string> max_map;

    auto col_name_of = [](const std::string& key) -> std::string {
        auto pos = key.rfind('.');
        return pos == std::string::npos ? key : key.substr(pos + 1);
    };

    for (auto& row : rows) {
        for (auto& [key, val] : row) {
            if (!key.empty() && key.front() == '_') continue; // skip _xmin/_xmax
            std::string col = col_name_of(key);
            if (val == EXECUTOR_NULL_VALUE) {
                null_cnt[col] += 1;
            } else {
                distinct[col].insert(val);
                auto val_f = try_parse_f64(val);

                bool new_lt;
                auto min_it = min_map.find(col);
                if (min_it == min_map.end()) new_lt = true;
                else if (val_f) {
                    auto cur_f = try_parse_f64(min_it->second);
                    new_lt = cur_f ? (*val_f < *cur_f) : true;
                } else {
                    new_lt = val < min_it->second;
                }
                if (new_lt) min_map[col] = val;

                bool new_gt;
                auto max_it = max_map.find(col);
                if (max_it == max_map.end()) new_gt = true;
                else if (val_f) {
                    auto cur_f = try_parse_f64(max_it->second);
                    new_gt = cur_f ? (*val_f > *cur_f) : true;
                } else {
                    new_gt = val > max_it->second;
                }
                if (new_gt) max_map[col] = val;
            }
        }
    }

    std::unordered_map<std::string, ColumnStats> col_stats;
    for (auto& [col, set] : distinct) {
        ColumnStats cs;
        cs.distinct_count = set.size();
        cs.null_count = null_cnt.count(col) ? null_cnt.at(col) : 0;
        if (auto it = min_map.find(col); it != min_map.end()) cs.min_val = it->second;
        if (auto it = max_map.find(col); it != max_map.end()) cs.max_val = it->second;
        col_stats[col] = std::move(cs);
    }
    for (auto& [col, cnt] : null_cnt) {
        if (!col_stats.count(col)) {
            ColumnStats cs;
            cs.null_count = cnt;
            col_stats[col] = std::move(cs);
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> col_all_values;
    for (auto& row : rows) {
        for (auto& [key, val] : row) {
            if (!key.empty() && key.front() == '_') continue;
            std::string col = col_name_of(key);
            if (val != EXECUTOR_NULL_VALUE && !val.empty()) col_all_values[col].push_back(val);
        }
    }
    for (auto& [col, vals0] : col_all_values) {
        std::unordered_map<std::string, std::size_t> freq;
        for (auto& v : vals0) freq[v]++;
        std::vector<std::pair<std::string, std::size_t>> mcv;
        for (auto& [v, cnt] : freq) {
            if (cnt > 1) mcv.emplace_back(v, cnt);
        }
        std::sort(mcv.begin(), mcv.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (mcv.size() > NUM_MCV_ENTRIES) mcv.resize(NUM_MCV_ENTRIES);
        if (auto it = col_stats.find(col); it != col_stats.end()) it->second.mcv = std::move(mcv);

        std::vector<std::string> vals = vals0;
        if (vals.size() < 2) continue;
        bool is_numeric = true;
        for (std::size_t i = 0; i < std::min<std::size_t>(20, vals.size()); i++) {
            if (!try_parse_f64(vals[i])) {
                is_numeric = false;
                break;
            }
        }
        if (is_numeric) {
            std::sort(vals.begin(), vals.end(), [](const std::string& a, const std::string& b) {
                double fa = try_parse_f64(a).value_or(0.0);
                double fb = try_parse_f64(b).value_or(0.0);
                return fa < fb;
            });
        } else {
            std::sort(vals.begin(), vals.end());
        }
        std::size_t n_buckets = std::min(NUM_HISTOGRAM_BUCKETS, vals.size());
        std::size_t bucket_size = vals.size() / n_buckets;
        std::vector<std::string> boundaries;
        for (std::size_t i = 1; i <= n_buckets; i++) {
            std::size_t idx = std::min(i * bucket_size - 1, vals.size() - 1);
            boundaries.push_back(vals[idx]);
        }
        if (!boundaries.empty()) boundaries.back() = vals.back();
        if (auto it = col_stats.find(col); it != col_stats.end()) it->second.histogram = boundaries;
    }

    std::string table_display = display_name(table);
    s.table_stats[table] = TableStats{total, col_stats};

    std::vector<std::string> col_order;
    if (const auto* sc = s.catalog.get_table(table)) {
        for (auto& c : sc->columns) col_order.push_back(c.name);
    } else {
        for (auto& [k, _] : col_stats) col_order.push_back(k);
        std::sort(col_order.begin(), col_order.end());
    }

    std::string col_bar = "+" + std::string(14, '-') + "+" + std::string(10, '-') + "+" + std::string(10, '-') + "+" + std::string(12, '-') +
                           "+" + std::string(12, '-') + "+" + std::string(12, '-') + "+" + std::string(12, '-') + "+" + std::string(12, '-') + "+";
    std::size_t inner_width = col_bar.size() - 2;
    std::string bar = "+" + std::string(inner_width, '-') + "+";
    std::string title = "ANALYZE: " + table_display + " (" + std::to_string(total) + " rows)";
    // Matches Rust's `{:<width$}` minimum-width (non-truncating) semantics: if title is
    // longer than inner_width - 1 (e.g. a very long table name), don't underflow the
    // (unsigned) pad count — just emit it at its natural length, as Rust would.
    std::size_t title_pad = title.size() < inner_width - 1 ? inner_width - 1 - title.size() : 0;
    std::string header = "| " + title + std::string(title_pad, ' ') + "|";

    auto pad_left = [](const std::string& v, std::size_t w) {
        return v.size() >= w ? v : std::string(w - v.size(), ' ') + v;
    };
    auto pad_right = [](const std::string& v, std::size_t w) {
        return v.size() >= w ? v : v + std::string(w - v.size(), ' ');
    };

    std::string col_header = "| " + pad_right("column", 12) + " | " + pad_left("distinct", 8) + " | " + pad_left("nulls", 8) + " | " +
                              pad_right("min", 10) + " | " + pad_right("max", 10) + " | " + pad_right("p25", 10) + " | " + pad_right("p50", 10) +
                              " | " + pad_right("p75", 10) + " |";

    std::vector<std::string> lines = {bar, header, bar, col_header, col_bar};
    for (auto& col : col_order) {
        auto it = col_stats.find(col);
        if (it == col_stats.end()) continue;
        auto& cs = it->second;
        std::string min_s = cs.min_val.value_or("NULL");
        std::string max_s = cs.max_val.value_or("NULL");
        std::string min_t = min_s.size() > 10 ? min_s.substr(0, 10) : min_s;
        std::string max_t = max_s.size() > 10 ? max_s.substr(0, 10) : max_s;

        auto p_bucket = [&](std::size_t pct) -> std::string {
            auto& hist = cs.histogram;
            if (hist.empty()) return "-";
            std::size_t raw = hist.size() * pct / 100;
            std::size_t idx = std::min(raw == 0 ? 0 : raw - 1, hist.size() - 1);
            const std::string& val = hist[idx];
            return val.size() > 10 ? val.substr(0, 10) : val;
        };

        std::string col_t = col.size() > 12 ? col.substr(0, 12) : col;
        lines.push_back("| " + pad_right(col_t, 12) + " | " + pad_left(std::to_string(cs.distinct_count), 8) + " | " +
                         pad_left(std::to_string(cs.null_count), 8) + " | " + pad_right(min_t, 10) + " | " + pad_right(max_t, 10) + " | " +
                         pad_right(p_bucket(25), 10) + " | " + pad_right(p_bucket(50), 10) + " | " + pad_right(p_bucket(75), 10) + " |");
    }
    lines.push_back(col_bar);

    bool any_mcv = false;
    for (auto& col : col_order) {
        auto it = col_stats.find(col);
        if (it != col_stats.end() && !it->second.mcv.empty()) { any_mcv = true; break; }
    }
    if (any_mcv) {
        lines.push_back("Most common values:");
        for (auto& col : col_order) {
            auto it = col_stats.find(col);
            if (it == col_stats.end() || it->second.mcv.empty()) continue;
            std::string entry = "  " + col + ": ";
            for (std::size_t i = 0; i < it->second.mcv.size(); i++) {
                if (i) entry += ", ";
                auto& [val, cnt] = it->second.mcv[i];
                entry += val + " (" + std::to_string(cnt) + ")";
            }
            lines.push_back(entry);
        }
    }

    std::string out;
    for (std::size_t i = 0; i < lines.size(); i++) {
        if (i) out += "\n";
        out += lines[i];
    }
    return StringResult::Ok(out);
}

// OPTIMIZE TABLE t [, t2 ...] and CHECK TABLE t [, t2 ...] [option] (the text of the statement): MySQL's answer, a row (Table, Op, Msg_type, Msg_text) for each message.
// OPTIMIZE makes the table what MySQL's "recreate + analyze" does for InnoDB: the dead row versions go (VACUUM) and the statistics are taken again (ANALYZE TABLE).
// CHECK reads every row: a NOT NULL column that holds NULL, a primary key or a UNIQUE key that two rows share, a foreign key whose value has no parent row.
StringResult Executor::exec_table_maintenance(const std::string& sql, const std::string& op) {
    std::string upper = sql;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto keyword = upper.find("TABLE");
    if (keyword == std::string::npos) {
        return StringResult::Err("ERROR 1064 (42000): You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near '" + sql + "'");
    }
    // the table names: the first word of each comma-separated part (what follows the last one is an option such as QUICK or FOR UPGRADE)
    std::vector<std::string> names;
    {
        const std::string rest = sql.substr(keyword + 5);
        std::size_t start = 0;
        while (start <= rest.size()) {
            const auto comma = rest.find(',', start);
            std::string part = rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const auto first = part.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                part = part.substr(first);
                part = part.substr(0, part.find_first_of(" \t\r\n;"));
                part.erase(std::remove(part.begin(), part.end(), '`'), part.end());
                if (!part.empty()) names.push_back(part);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (names.empty()) {
        return StringResult::Err("ERROR 1064 (42000): You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near ''");
    }

    std::vector<Row> rows;
    auto say = [&](const std::string& table, const char* type, const std::string& text) {
        Row r;
        r["Table"] = table;
        r["Op"] = op;
        r["Msg_type"] = type;
        r["Msg_text"] = text;
        rows.push_back(std::move(r));
    };
    for (const std::string& written : names) {
        Statement named = Statement(Statement::Describe{written});
        resolve_names(named);
        const std::string table = qualify_name(std::get<Statement::Describe>(named.data).table);
        std::vector<std::string> lock_set;
        bool is_view = false, exists = false;
        {
            auto s = shared->read();
            is_view = s->views.count(table) > 0;
            if (const TableSchema* schema = s->catalog.get_table(table); schema && s->tables.count(table)) {
                exists = true;
                lock_set.push_back(table);
                for (const std::string& parent : fk_parent_tables(*s, *schema, {})) {
                    if (s->tables.count(parent)) lock_set.push_back(parent);
                }
            }
        }
        if (is_view) {
            say(table, "Error", "'" + table + "' is not BASE TABLE");
            say(table, "status", "Operation failed");
            continue;
        }
        if (!exists) {
            say(table, "Error", "Table '" + table + "' doesn't exist");
            say(table, "status", "Operation failed");
            continue;
        }
        if (op == "optimize") {
            auto done = execute(Statement(Statement::Vacuum{table}));
            if (done.is_ok()) done = execute(Statement(Statement::AnalyzeTable{table}));
            if (done.is_err()) {
                say(table, "Error", done.error());
                say(table, "status", "Operation failed");
                continue;
            }
            say(table, "note", "Table does not support optimize, doing recreate + analyze instead");
            say(table, "status", "OK");
            continue;
        }
        // CHECK TABLE
        std::sort(lock_set.begin(), lock_set.end());
        lock_set.erase(std::unique(lock_set.begin(), lock_set.end()), lock_set.end());
        std::vector<std::string> problems;
        {
            auto guard = acquire_table_locks(shared->read(), lock_set, /*exclusive=*/false);
            SharedDatabase& s = const_cast<SharedDatabase&>(*guard.structural);
            auto data = acquire_table_data_locks(s, lock_set, /*exclusive=*/false);
            const TableSchema* schema = s.catalog.get_table(table);
            auto rows_it = s.tables.find(table);
            if (schema && rows_it != s.tables.end()) {
                const SnapshotCtx now{txn.current_txn_id(), s.txn_io->peek_next_id(), *s.active_txn_ids->lock()};
                std::vector<const Row*> live;
                for (const Row& r : rows_it->second) {
                    if (is_visible_for_read(r, now)) live.push_back(&r);
                }
                auto value_of = [](const Row& r, const std::string& column) {
                    auto it = r.find(column);
                    return it == r.end() ? std::string(EXECUTOR_NULL_VALUE) : it->second;
                };
                auto note = [&](std::string text) {
                    if (problems.size() < 20) problems.push_back(std::move(text));
                };
                // NOT NULL
                for (const ColumnDef& col : schema->columns) {
                    if (!col.not_null && !col.primary_key) continue;
                    for (const Row* r : live) {
                        if (value_of(*r, col.name) == EXECUTOR_NULL_VALUE) {
                            note("Column '" + col.name + "' is NOT NULL but a row holds NULL");
                            break;
                        }
                    }
                }
                // keys that no two rows may share: the primary key, a UNIQUE column, a UNIQUE key over several columns (a NULL is no value to share)
                auto shared_key = [&](const std::vector<std::string>& columns, const std::string& key_name) {
                    std::unordered_set<std::string> seen;
                    for (const Row* r : live) {
                        std::string key;
                        bool has_null = false;
                        for (std::size_t i = 0; i < columns.size(); i++) {
                            const std::string v = value_of(*r, columns[i]);
                            if (v == EXECUTOR_NULL_VALUE) has_null = true;
                            if (i) key += '-';
                            key += v;
                        }
                        if (has_null) continue;
                        if (!seen.insert(key).second) {
                            note("Duplicate entry '" + key + "' for key '" + key_name + "'");
                            break;
                        }
                    }
                };
                const std::vector<std::string> identity = schema->identity_columns();
                if (!identity.empty()) shared_key(identity, "PRIMARY");
                for (const ColumnDef& col : schema->columns) {
                    if (col.unique && !col.primary_key) shared_key({col.name}, col.unique_constraint_name.value_or(col.name));
                }
                for (const UniqueKey& key : schema->unique_keys) shared_key(key.columns, key.name.value_or(key.columns.empty() ? std::string() : key.columns.front()));
                // foreign keys: every value has a parent row
                for (const ColumnDef& col : schema->columns) {
                    for (const ForeignKey& fk : col.foreign_keys) {
                        for (const Row* r : live) {
                            const std::string v = value_of(*r, col.name);
                            if (v == EXECUTOR_NULL_VALUE) continue;
                            if (fk_child_violation(s, fk, v)) {
                                note("Foreign key '" + col.name + "' holds " + v + ", which " + display_name(fk.ref_table) + "(" + fk.ref_column + ") does not have");
                                break;
                            }
                        }
                    }
                }
            }
        }
        for (const std::string& problem : problems) say(table, "error", problem);
        say(table, "status", problems.empty() ? "OK" : "Corrupt");
    }
    return StringResult::Ok(format_set_result({"Table", "Op", "Msg_type", "Msg_text"}, rows));
}

} // namespace engine
