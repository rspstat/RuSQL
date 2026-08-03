// Faithful port of the DDL portion of rusql-core/src/engine/executor.rs (Phase 8a):
// exec_create, exec_drop, exec_truncate, exec_alter, exec_create_database,
// exec_drop_database, exec_create_index, exec_drop_index, exec_create_view,
// exec_drop_view, and the shared index-maintenance helpers (persist_views_for_db,
// index_insert_row, index_remove_row, rebuild_secondary_indexes, persist_index_meta),
// plus exec_use/exec_show_tables.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {
std::uint64_t parse_txn_id(const Row& row, const char* col) {
    auto it = row.find(col);
    if (it == row.end()) return 0;
    try {
        return std::stoull(it->second);
    } catch (...) {
        return 0;
    }
}
} // namespace

bool Executor::is_visible(const Row& row) {
    auto it = row.find("_xmax");
    return it == row.end() || it->second == "0";
}

bool Executor::is_visible_for_read(const Row& row, const SnapshotCtx& ctx) {
    auto committed_as_of = [&](std::uint64_t id) { return id == 0 || (id < ctx.cutoff && !ctx.in_progress.count(id)); };

    std::uint64_t xmin = parse_txn_id(row, "_xmin");
    if (xmin != ctx.self_txn_id && !committed_as_of(xmin)) return false; // didn't exist yet, to this reader

    auto xmax_it = row.find("_xmax");
    if (xmax_it == row.end() || xmax_it->second == "0") return true;
    std::uint64_t xmax = parse_txn_id(row, "_xmax");
    if (xmax == ctx.self_txn_id) return false;       // deleted/superseded by this same transaction
    if (committed_as_of(xmax)) return false;          // deleted by a transaction already committed as of this snapshot
    return true;                                       // deleted by a transaction not (yet) committed as of this snapshot
}

SnapshotCtx Executor::current_read_ctx(SharedDatabase& s) const {
    if (txn.isolation_level() == IsolationLevel::ReadUncommitted) {
        return SnapshotCtx{txn.current_txn_id(), std::numeric_limits<std::uint64_t>::max(), {}};
    }
    if (txn.is_active()) {
        if (auto frozen = txn.frozen_ctx()) return *frozen;
    }
    return SnapshotCtx{txn.current_txn_id(), s.txn_io->peek_next_id(), *s.active_txn_ids->lock()};
}

std::uint64_t Executor::tagging_txn_id(SharedDatabase& s) const {
    return txn.is_active() ? txn.current_txn_id() : s.txn_io->next_id();
}

std::uint64_t Executor::oldest_active_txn_id(SharedDatabase& s) {
    auto active = s.active_txn_ids->lock();
    if (active->empty()) return s.txn_io->peek_next_id();
    return *std::min_element(active->begin(), active->end());
}

bool Executor::is_vacuumable(const Row& row, std::uint64_t oldest_active_txn_id) {
    auto it = row.find("_xmax");
    if (it == row.end() || it->second == "0") return false; // still live
    std::uint64_t xmax = parse_txn_id(row, "_xmax");
    return xmax != 0 && xmax < oldest_active_txn_id;
}

namespace {

bool parses_as_i64(const std::string& s) {
    if (s.empty()) return false;
    long long val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

bool parses_as_f64(const std::string& s) {
    if (s.empty()) return false;
    double val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string trim_copy(const std::string& s) {
    auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

// Approximates Rust's `{:?}` Debug format for DataType (tag name only — this is only
// used inside a human-readable error message, not compared against structurally).
std::string debug_data_type(const DataType& dt) {
    return std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int>) return "Int";
            else if constexpr (std::is_same_v<T, DataType::BigInt>) return "BigInt";
            else if constexpr (std::is_same_v<T, DataType::SmallInt>) return "SmallInt";
            else if constexpr (std::is_same_v<T, DataType::TinyInt>) return "TinyInt";
            else if constexpr (std::is_same_v<T, DataType::Text>) return "Text";
            else if constexpr (std::is_same_v<T, DataType::Float>) return "Float";
            else if constexpr (std::is_same_v<T, DataType::Boolean>) return "Boolean";
            else if constexpr (std::is_same_v<T, DataType::Varchar>) return "Varchar(" + std::to_string(alt.length) + ")";
            else if constexpr (std::is_same_v<T, DataType::Date>) return "Date";
            else if constexpr (std::is_same_v<T, DataType::DateTime>) return "DateTime";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>) return "Timestamp";
            else if constexpr (std::is_same_v<T, DataType::Decimal>) return "Decimal";
            else if constexpr (std::is_same_v<T, DataType::Double>) return "Double";
            else if constexpr (std::is_same_v<T, DataType::Time>) return "Time";
            else if constexpr (std::is_same_v<T, DataType::Year>) return "Year";
            else if constexpr (std::is_same_v<T, DataType::Enum>) return "Enum";
            else if constexpr (std::is_same_v<T, DataType::Set>) return "Set";
            else if constexpr (std::is_same_v<T, DataType::Blob>) return "Blob";
            else if constexpr (std::is_same_v<T, DataType::Json>) return "Json";
            else return "Unknown";
        },
        dt.data);
}

bool value_matches_data_type(const DataType& dt, const std::string& val) {
    return std::visit(
        [&val](const auto& alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int> || std::is_same_v<T, DataType::SmallInt> ||
                          std::is_same_v<T, DataType::TinyInt> || std::is_same_v<T, DataType::BigInt>)
                return parses_as_i64(val);
            else if constexpr (std::is_same_v<T, DataType::Float> || std::is_same_v<T, DataType::Decimal> ||
                                std::is_same_v<T, DataType::Double>)
                return parses_as_f64(val);
            else if constexpr (std::is_same_v<T, DataType::Boolean>) {
                std::string lower = to_lower_copy(val);
                return lower == "true" || lower == "false" || lower == "1" || lower == "0";
            } else if constexpr (std::is_same_v<T, DataType::Enum>) {
                return std::find(alt.values.begin(), alt.values.end(), val) != alt.values.end();
            } else if constexpr (std::is_same_v<T, DataType::Set>) {
                std::size_t start = 0;
                while (true) {
                    auto comma = val.find(',', start);
                    std::string part = trim_copy(val.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                    bool part_ok = part.empty() || std::find(alt.values.begin(), alt.values.end(), part) != alt.values.end();
                    if (!part_ok) return false;
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                return true;
            } else {
                // Text/Varchar/Date/DateTime/Timestamp/Time/Year/Blob/Json/Unknown: no validation.
                return true;
            }
        },
        dt.data);
}

} // namespace

StringResult Executor::exec_create(SharedDatabase& s, std::string name, std::vector<ColumnDef> columns, bool if_not_exists,
                                    std::vector<std::string> primary_key_columns,
                                    std::vector<std::pair<std::optional<std::string>, std::string>> check_constraints) {
    if (if_not_exists && s.tables.count(name)) return StringResult::Ok("Table '" + name + "' already exists, skipped.");

    std::vector<CheckConstraint> schema_checks;
    schema_checks.reserve(check_constraints.size());
    for (auto& [cname, expr] : check_constraints) schema_checks.push_back(CheckConstraint{cname, expr});

    auto res = s.catalog.create_table_full(name, std::move(columns), std::move(primary_key_columns), std::move(schema_checks));
    if (res.is_err()) return StringResult::Err(res.error());

    s.tables[name] = {};
    s.indexes[name] = BPlusTree();
    s.table_locks[name] = std::make_shared<std::shared_mutex>();
    // Stage 4: pre-populate every other table-keyed map too. Under per-table locking, a
    // DML statement on this table only ever holds THIS table's own lock -- if its first
    // touch to one of these maps used operator[] to insert a brand-new key, that could
    // race with a concurrent DML statement on a DIFFERENT table doing the same to the
    // same shared unordered_map, and an insert-triggered rehash would invalidate a
    // reference/iterator the other thread is mid-use of. Creating the entry now, under
    // the structural exclusive lock DDL always holds, means later DML only ever finds an
    // existing key (safe to access concurrently across different tables).
    s.table_stats[name] = TableStats{};
    s.dml_since_vacuum[name] = 0;
    s.dml_since_analyze[name] = 0;
    s.row_pk_pos[name] = {};
    s.disk.save_schema(name, *s.catalog.get_table(name));
    return StringResult::Ok("Table '" + name + "' created.");
}

StringResult Executor::exec_drop(SharedDatabase& s, const std::string& name, bool if_exists) {
    if (if_exists && !s.tables.count(name)) return StringResult::Ok("Table '" + name + "' does not exist, skipped.");
    auto res = s.catalog.drop_table(name);
    if (res.is_err()) return StringResult::Err(res.error());
    s.tables.erase(name);
    s.indexes.erase(name);
    s.table_locks.erase(name);
    s.table_stats.erase(name);
    s.dml_since_vacuum.erase(name);
    s.dml_since_analyze.erase(name);
    s.row_pk_pos.erase(name);
    s.buffer_pool.invalidate(name);
    s.disk.delete_table(name);
    return StringResult::Ok("Table '" + name + "' dropped.");
}

StringResult Executor::exec_truncate(SharedDatabase& s, const std::string& name) {
    auto it = s.tables.find(name);
    if (it == s.tables.end()) return StringResult::Err("Table '" + name + "' not found");
    it->second.clear();
    s.row_pk_pos.erase(name);
    if (auto idx_it = s.indexes.find(name); idx_it != s.indexes.end()) idx_it->second = BPlusTree();
    if (auto* schema = s.catalog.get_table_mut(name)) schema->auto_increment_counters.clear();
    s.buffer_pool.invalidate(name);
    s.disk.save_table(name, {});
    return StringResult::Ok("Table '" + name + "' truncated.");
}

StringResult Executor::exec_alter(SharedDatabase& s, const std::string& table, AlterAction action) {
    if (auto* v = std::get_if<AlterAction::AddColumn>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");

        ColumnDef new_col;
        new_col.name = v->column.name;
        new_col.data_type = v->column.data_type;
        new_col.primary_key = false;
        new_col.not_null = v->column.not_null;
        new_col.unique = v->column.unique;
        new_col.unique_constraint_name = v->column.unique_constraint_name;
        new_col.auto_increment = false;
        new_col.default_value = v->column.default_value;
        new_col.foreign_key = std::nullopt;
        new_col.check_expr = v->column.check_expr;
        schema->columns.push_back(new_col);

        std::string fill_val = (v->column.default_value && *v->column.default_value != NULL_DEFAULT) ? *v->column.default_value
                                                                                                       : EXECUTOR_NULL_VALUE;
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) row[new_col.name] = fill_val;
        }
        s.disk.save_schema(table, *s.catalog.get_table(table));
        s.disk.save_table(table, s.tables.at(table));
        return StringResult::Ok("Column '" + new_col.name + "' added to '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::DropColumn>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        auto& cols = schema->columns;
        cols.erase(std::remove_if(cols.begin(), cols.end(), [&](const ColumnDef& c) { return c.name == v->name; }), cols.end());
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) row.erase(v->name);
        }
        s.disk.save_schema(table, *s.catalog.get_table(table));
        s.disk.save_table(table, s.tables.at(table));
        return StringResult::Ok("Column '" + v->name + "' dropped from '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::RenameColumn>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        for (auto& col : schema->columns) {
            if (col.name == v->from) col.name = v->to;
        }
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) {
                auto rit = row.find(v->from);
                if (rit != row.end()) {
                    std::string val = std::move(rit->second);
                    row.erase(rit);
                    row[v->to] = std::move(val);
                }
            }
        }

        // PLAN.md P0 fix: every index caches a serialized copy of each row (or, for
        // composite/hash indexes, tracks the renamed column by name) — none of that
        // was ever refreshed here, so an index-based read (PK point lookup,
        // secondary/hash/composite lookup) kept returning the renamed column as
        // missing even though the schema and s.tables rows above were already
        // correctly updated. Rebuild every index for this table, same as VACUUM's
        // rebuild pattern (exec_vacuum).
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            const std::vector<Row>& rows = it->second;
            if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
                idx_it->second = BPlusTree();
                std::string pk_col_name;
                for (auto& c : schema->columns) {
                    if (c.primary_key) { pk_col_name = c.name; break; }
                }
                if (pk_col_name.empty() && !schema->columns.empty()) pk_col_name = schema->columns.front().name;
                for (auto& row : rows) {
                    auto rit = row.find(pk_col_name);
                    std::string key = rit != row.end() ? rit->second : std::string();
                    nlohmann::json j = row;
                    idx_it->second.insert(key, j.dump());
                }
            }
            for (auto& [name, meta] : s.index_meta) {
                if (meta.first == table && meta.second == v->from) meta.second = v->to;
            }
            for (auto& [name, meta] : s.hash_index_meta) {
                if (meta.first == table && meta.second == v->from) meta.second = v->to;
            }
            rebuild_secondary_indexes(s, table, rows);
            for (auto& [key, ci] : s.composite_indexes) {
                if (ci.table != table) continue;
                for (auto& col : ci.columns) {
                    if (col == v->from) col = v->to;
                }
                ci.rebuild(rows);
            }
            persist_index_meta(s);
        }

        s.disk.save_schema(table, *s.catalog.get_table(table));
        s.disk.save_table(table, s.tables.at(table));
        s.buffer_pool.write_page(table, s.tables.at(table));
        return StringResult::Ok("Column '" + v->from + "' renamed to '" + v->to + "' in '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::ModifyColumn>(&action.data)) {
        const TableSchema* schema_check = s.catalog.get_table(table);
        if (!schema_check) return StringResult::Err("Table '" + table + "' not found");
        bool exists = std::any_of(schema_check->columns.begin(), schema_check->columns.end(),
                                   [&](const ColumnDef& c) { return c.name == v->column.name; });
        if (!exists) return StringResult::Err("Column '" + v->column.name + "' not found in '" + table + "'");

        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) {
                if (!is_visible(row)) continue;
                auto vit = row.find(v->column.name);
                if (vit == row.end()) continue;
                const std::string& val = vit->second;
                if (val == EXECUTOR_NULL_VALUE || val.empty()) continue;
                if (!value_matches_data_type(v->column.data_type, val)) {
                    return StringResult::Err("Cannot convert value '" + val + "' in column '" + v->column.name + "' to " +
                                              debug_data_type(v->column.data_type));
                }
            }
        }

        auto* schema = s.catalog.get_table_mut(table);
        for (auto& c : schema->columns) {
            if (c.name == v->column.name) {
                c.data_type = v->column.data_type;
                c.not_null = v->column.not_null;
                c.unique = v->column.unique;
                c.unique_constraint_name = v->column.unique_constraint_name;
                c.auto_increment = v->column.auto_increment;
                c.default_value = v->column.default_value;
                // primary_key cannot be changed via MODIFY (ignored, matching Rust).
                break;
            }
        }
        s.disk.save_schema(table, *s.catalog.get_table(table));
        return StringResult::Ok("Column '" + v->column.name + "' in '" + table + "' modified.");
    }

    if (auto* v = std::get_if<AlterAction::RenameTable>(&action.data)) {
        if (!s.catalog.tables.count(table)) return StringResult::Err("Table '" + table + "' not found");
        if (s.catalog.tables.count(v->to)) return StringResult::Err("Table '" + v->to + "' already exists");

        auto node = s.catalog.tables.extract(table);
        node.key() = v->to;
        s.catalog.tables.insert(std::move(node));

        if (auto it = s.tables.find(table); it != s.tables.end()) {
            auto rows = std::move(it->second);
            s.tables.erase(it);
            s.tables.insert({v->to, std::move(rows)});
        }
        if (auto it = s.indexes.find(table); it != s.indexes.end()) {
            auto tree = std::move(it->second);
            s.indexes.erase(it);
            s.indexes.insert({v->to, std::move(tree)});
        }
        if (auto it = s.table_locks.find(table); it != s.table_locks.end()) {
            auto lock = std::move(it->second);
            s.table_locks.erase(it);
            s.table_locks.insert({v->to, std::move(lock)});
        }
        if (auto it = s.table_stats.find(table); it != s.table_stats.end()) {
            auto st = std::move(it->second);
            s.table_stats.erase(it);
            s.table_stats.insert({v->to, std::move(st)});
        }
        if (auto it = s.dml_since_vacuum.find(table); it != s.dml_since_vacuum.end()) {
            auto v2 = it->second;
            s.dml_since_vacuum.erase(it);
            s.dml_since_vacuum.insert({v->to, v2});
        }
        if (auto it = s.dml_since_analyze.find(table); it != s.dml_since_analyze.end()) {
            auto v2 = it->second;
            s.dml_since_analyze.erase(it);
            s.dml_since_analyze.insert({v->to, v2});
        }
        if (auto it = s.row_pk_pos.find(table); it != s.row_pk_pos.end()) {
            auto pm = std::move(it->second);
            s.row_pk_pos.erase(it);
            s.row_pk_pos.insert({v->to, std::move(pm)});
        }
        for (auto& [name, meta] : s.index_meta) {
            if (meta.first == table) meta.first = v->to;
        }
        std::string prefix = table + "_";
        std::vector<std::string> sec_keys;
        for (auto& [k, _] : s.indexes) {
            if (k.compare(0, prefix.size(), prefix) == 0) sec_keys.push_back(k);
        }
        for (auto& old_key : sec_keys) {
            std::string suffix = old_key.substr(table.size());
            std::string new_key = v->to + suffix;
            auto it = s.indexes.find(old_key);
            auto tree = std::move(it->second);
            s.indexes.erase(it);
            s.indexes.insert({new_key, std::move(tree)});
        }
        for (auto& [name, ci] : s.composite_indexes) {
            if (ci.table == table) ci.table = v->to;
        }

        s.disk.save_schema(v->to, *s.catalog.get_table(v->to));
        if (auto it = s.tables.find(v->to); it != s.tables.end()) s.disk.save_table(v->to, it->second);
        s.disk.delete_table(table);

        return StringResult::Ok("Table '" + table + "' renamed to '" + v->to + "'.");
    }

    if (auto* v = std::get_if<AlterAction::AddForeignKey>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        auto col_it = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == v->column; });
        if (col_it == schema->columns.end()) return StringResult::Err("Column '" + v->column + "' not found in '" + table + "'");
        ForeignKey fk;
        fk.column = v->column;
        fk.ref_table = v->ref_table;
        fk.ref_column = v->ref_column;
        fk.on_delete = v->on_delete;
        fk.on_update = v->on_update;
        col_it->foreign_key = fk;
        s.disk.save_schema(table, *s.catalog.get_table(table));
        std::string constraint_label = v->name.value_or("fk_" + v->column);
        return StringResult::Ok("Foreign key '" + constraint_label + "' added to '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::DropForeignKey>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        bool found = false;
        for (auto& col : schema->columns) {
            if (col.foreign_key) {
                bool matches = col.foreign_key->column == v->name || ("fk_" + col.foreign_key->column) == v->name;
                if (matches) {
                    col.foreign_key = std::nullopt;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return StringResult::Err("Foreign key '" + v->name + "' not found in '" + table + "'");
        s.disk.save_schema(table, *s.catalog.get_table(table));
        return StringResult::Ok("Foreign key '" + v->name + "' dropped from '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::AddUniqueConstraint>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        auto col_it = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == v->column; });
        if (col_it == schema->columns.end()) return StringResult::Err("Column '" + v->column + "' not found in '" + table + "'");
        col_it->unique = true;
        col_it->unique_constraint_name = v->name;
        s.disk.save_schema(table, *s.catalog.get_table(table));
        std::string constraint_label = v->name.value_or("uq_" + v->column);
        return StringResult::Ok("Unique constraint '" + constraint_label + "' added to '" + table + "'.'" + v->column + "'.");
    }

    if (auto* v = std::get_if<AlterAction::AddCheckConstraint>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        schema->check_constraints.push_back(CheckConstraint{v->name, v->expr});
        s.disk.save_schema(table, *s.catalog.get_table(table));
        std::string constraint_label = v->name.value_or(v->expr);
        return StringResult::Ok("Check constraint '" + constraint_label + "' added to '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::DropConstraint>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        std::size_t before = schema->check_constraints.size();
        auto& checks = schema->check_constraints;
        checks.erase(std::remove_if(checks.begin(), checks.end(),
                                     [&](const CheckConstraint& c) { return c.name == std::optional<std::string>(v->name) || c.expression == v->name; }),
                     checks.end());
        bool removed_check = checks.size() < before;
        if (!removed_check) {
            for (auto& col : schema->columns) {
                if (col.unique_constraint_name == std::optional<std::string>(v->name)) {
                    col.unique = false;
                    col.unique_constraint_name = std::nullopt;
                    s.disk.save_schema(table, *s.catalog.get_table(table));
                    return StringResult::Ok("Constraint '" + v->name + "' dropped from '" + table + "'.");
                }
            }
            return StringResult::Err("Constraint '" + v->name + "' not found in '" + table + "'");
        }
        s.disk.save_schema(table, *s.catalog.get_table(table));
        return StringResult::Ok("Constraint '" + v->name + "' dropped from '" + table + "'.");
    }

    return StringResult::Err("Unknown ALTER TABLE action");
}

StringResult Executor::exec_create_database(SharedDatabase& s, const std::string& name, bool if_not_exists) {
    std::string key = to_lower_copy(name);
    if (s.databases.count(key)) {
        if (if_not_exists) return StringResult::Ok("Database '" + name + "' already exists (skipped).");
        return StringResult::Err("Database '" + name + "' already exists.");
    }
    s.disk.create_db_dir(key);
    s.databases.insert(key);
    current_db = key;
    return StringResult::Ok("Database '" + key + "' created.");
}

StringResult Executor::exec_drop_database(SharedDatabase& s, const std::string& name, bool if_exists) {
    std::string key = to_lower_copy(name);
    if (!s.databases.count(key)) {
        if (if_exists) return StringResult::Ok("Database '" + name + "' does not exist (skipped).");
        return StringResult::Err("Database '" + name + "' does not exist.");
    }
    std::string prefix = key + ".";

    std::vector<std::string> table_keys;
    for (auto& [k, _] : s.tables) {
        if (k.compare(0, prefix.size(), prefix) == 0) table_keys.push_back(k);
    }
    for (auto& t : table_keys) {
        s.catalog.tables.erase(t);
        s.tables.erase(t);
        s.indexes.erase(t);
        s.table_locks.erase(t);
        s.table_stats.erase(t);
        s.dml_since_vacuum.erase(t);
        s.dml_since_analyze.erase(t);
        s.row_pk_pos.erase(t);
        s.buffer_pool.invalidate(t);
        s.disk.delete_table(t);
    }

    std::vector<std::string> sec_keys;
    for (auto& [k, _] : s.indexes) {
        if (k.compare(0, prefix.size(), prefix) == 0) sec_keys.push_back(k);
    }
    for (auto& k : sec_keys) {
        s.buffer_pool.invalidate(k);
        s.indexes.erase(k);
    }

    for (auto it = s.index_meta.begin(); it != s.index_meta.end();) {
        if (it->second.first.compare(0, prefix.size(), prefix) == 0) it = s.index_meta.erase(it);
        else ++it;
    }
    for (auto it = s.composite_indexes.begin(); it != s.composite_indexes.end();) {
        if (it->second.table.compare(0, prefix.size(), prefix) == 0) it = s.composite_indexes.erase(it);
        else ++it;
    }
    for (auto it = s.views.begin(); it != s.views.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) it = s.views.erase(it);
        else ++it;
    }

    s.disk.drop_db_dir(key);
    s.databases.erase(key);

    if (current_db == key) {
        current_db = s.databases.empty() ? std::string() : *s.databases.begin();
    }

    return StringResult::Ok("Database '" + key + "' dropped.");
}

StringResult Executor::exec_create_index(SharedDatabase& s, const std::string& index_name, const std::string& table,
                                          const std::vector<std::string>& columns, bool using_hash) {
    if (!s.tables.count(table)) return StringResult::Err("Table '" + table + "' not found");

    if (using_hash) {
        if (columns.size() != 1) return StringResult::Err("Hash index supports only single-column indexes.");
        const std::string& column = columns.front();
        HashIndex hi(table, column);
        if (auto it = s.tables.find(table); it != s.tables.end()) hi.rebuild(it->second);
        s.hash_indexes.insert({table + "_" + index_name, std::move(hi)});
        s.hash_index_meta.insert({index_name, {table, column}});
        persist_index_meta(s);
        return StringResult::Ok("Hash index '" + index_name + "' created on '" + table + "'.'" + column + "'.");
    }

    if (columns.size() == 1) {
        const std::string& column = columns.front();
        std::unordered_map<std::string, std::vector<Row>> bucket;
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) {
                if (auto vit = row.find(column); vit != row.end()) bucket[vit->second].push_back(row);
            }
        }
        BPlusTree tree;
        for (auto& [key, rows] : bucket) {
            nlohmann::json j = rows;
            tree.insert(key, j.dump());
        }
        std::string idx_key = table + "_" + index_name;
        s.disk.save_btree_index(idx_key, tree);
        s.indexes.insert({idx_key, std::move(tree)});
        s.index_meta.insert({index_name, {table, column}});
        persist_index_meta(s);
        return StringResult::Ok("Index '" + index_name + "' created on '" + table + "'.'" + column + "'.");
    }

    CompositeIndex comp(table, columns);
    if (auto it = s.tables.find(table); it != s.tables.end()) comp.rebuild(it->second);
    s.composite_indexes.insert({index_name, std::move(comp)});
    persist_index_meta(s);
    std::string cols_joined;
    for (std::size_t i = 0; i < columns.size(); i++) {
        if (i) cols_joined += ", ";
        cols_joined += columns[i];
    }
    return StringResult::Ok("Composite index '" + index_name + "' created on '" + table + "' (" + cols_joined + ").");
}

StringResult Executor::exec_drop_index(SharedDatabase& s, const std::string& index_name) {
    if (auto it = s.index_meta.find(index_name); it != s.index_meta.end()) {
        std::string table = it->second.first;
        s.index_meta.erase(it);
        std::string idx_key = table + "_" + index_name;
        s.indexes.erase(idx_key);
        s.disk.delete_btree_index(idx_key);
        persist_index_meta(s);
        return StringResult::Ok("Index '" + index_name + "' dropped.");
    }
    if (auto it = s.hash_index_meta.find(index_name); it != s.hash_index_meta.end()) {
        std::string table = it->second.first;
        s.hash_index_meta.erase(it);
        s.hash_indexes.erase(table + "_" + index_name);
        persist_index_meta(s);
        return StringResult::Ok("Hash index '" + index_name + "' dropped.");
    }
    if (s.composite_indexes.erase(index_name)) {
        persist_index_meta(s);
        return StringResult::Ok("Composite index '" + index_name + "' dropped.");
    }
    return StringResult::Ok("Index '" + index_name + "' does not exist, skipped.");
}

StringResult Executor::exec_create_view(SharedDatabase& s, const std::string& name, Statement query, const std::string& raw_sql) {
    if (auto* sel = std::get_if<Statement::Select>(&query.data)) {
        if (!s.tables.count(sel->table)) return StringResult::Err("Table '" + sel->table + "' not found");
    }
    s.views[name] = std::move(query);
    if (!raw_sql.empty()) s.view_raw_sql[name] = raw_sql;
    persist_views_for_db(s, current_db);
    return StringResult::Ok("View '" + name + "' created.");
}

StringResult Executor::exec_drop_view(SharedDatabase& s, const std::string& name) {
    if (s.views.erase(name)) {
        s.view_raw_sql.erase(name);
        persist_views_for_db(s, current_db);
        return StringResult::Ok("View '" + name + "' dropped.");
    }
    return StringResult::Ok("View '" + name + "' does not exist, skipped.");
}

void Executor::persist_views_for_db(const SharedDatabase& s, const std::string& db) const {
    std::string prefix = db + ".";
    std::unordered_map<std::string, Statement> db_views;
    for (auto& [k, v] : s.views) {
        if (k.compare(0, prefix.size(), prefix) == 0) db_views.insert({k, v});
    }
    s.disk.save_views(db, db_views);

    std::unordered_map<std::string, std::string> db_view_sql;
    for (auto& [k, v] : s.view_raw_sql) {
        if (k.compare(0, prefix.size(), prefix) == 0) db_view_sql.insert({k, v});
    }
    s.disk.save_view_raw_sql(db, db_view_sql);
}

void Executor::index_insert_row(SharedDatabase& s, const std::string& table, const Row& row) {
    std::vector<std::pair<std::string, std::string>> sec;
    for (auto& [name, meta] : s.index_meta) {
        if (meta.first == table) sec.emplace_back(table + "_" + name, meta.second);
    }
    for (auto& [key, col] : sec) {
        auto vit = row.find(col);
        if (vit == row.end()) continue;
        std::vector<Row> bucket;
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) {
            if (auto j = tit->second.search(vit->second)) {
                try {
                    bucket = nlohmann::json::parse(*j).get<std::vector<Row>>();
                } catch (...) {
                }
            }
        }
        bucket.push_back(row);
        nlohmann::json j = bucket;
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) tit->second.insert(vit->second, j.dump());
    }

    std::vector<std::string> hsec;
    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first == table) hsec.push_back(table + "_" + name);
    }
    for (auto& key : hsec) {
        if (auto it = s.hash_indexes.find(key); it != s.hash_indexes.end()) it->second.insert_row(row);
    }
}

void Executor::index_remove_row(SharedDatabase& s, const std::string& table, const Row& row, const std::string& pk_col) {
    auto pk_it = row.find(pk_col);
    if (pk_it == row.end()) return;
    const std::string& pk_val = pk_it->second;

    std::vector<std::pair<std::string, std::string>> sec;
    for (auto& [name, meta] : s.index_meta) {
        if (meta.first == table) sec.emplace_back(table + "_" + name, meta.second);
    }
    for (auto& [key, col] : sec) {
        auto vit = row.find(col);
        if (vit == row.end()) continue;
        std::vector<Row> bucket;
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) {
            if (auto j = tit->second.search(vit->second)) {
                try {
                    bucket = nlohmann::json::parse(*j).get<std::vector<Row>>();
                } catch (...) {
                }
            }
        }
        std::vector<Row> new_bucket;
        for (auto& r : bucket) {
            auto it = r.find(pk_col);
            if (it == r.end() || it->second != pk_val) new_bucket.push_back(r);
        }
        nlohmann::json j = new_bucket;
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) tit->second.insert(vit->second, j.dump());
    }

    std::vector<std::pair<std::string, std::string>> hsec;
    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first == table) hsec.emplace_back(table + "_" + name, meta.second);
    }
    for (auto& [key, col] : hsec) {
        auto vit = row.find(col);
        if (vit == row.end()) continue;
        if (auto it = s.hash_indexes.find(key); it != s.hash_indexes.end()) it->second.remove_row(vit->second, pk_col, pk_val);
    }
}

void Executor::rebuild_secondary_indexes(SharedDatabase& s, const std::string& table, const std::vector<Row>& rows) {
    std::vector<std::pair<std::string, std::string>> sec;
    for (auto& [name, meta] : s.index_meta) {
        if (meta.first == table) sec.emplace_back(name, meta.second);
    }
    for (auto& [idx_name, col] : sec) {
        std::unordered_map<std::string, std::vector<Row>> bucket;
        for (auto& row : rows) {
            if (auto vit = row.find(col); vit != row.end()) bucket[vit->second].push_back(row);
        }
        BPlusTree tree;
        for (auto& [key, bucket_rows] : bucket) {
            nlohmann::json j = bucket_rows;
            tree.insert(key, j.dump());
        }
        s.indexes.insert_or_assign(table + "_" + idx_name, std::move(tree));
    }

    std::vector<std::pair<std::string, std::string>> hsec;
    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first == table) hsec.emplace_back(name, meta.second);
    }
    for (auto& [idx_name, col] : hsec) {
        HashIndex hi(table, col);
        hi.rebuild(rows);
        s.hash_indexes.insert_or_assign(table + "_" + idx_name, std::move(hi));
    }
}

void Executor::persist_index_meta(const SharedDatabase& s) const {
    std::vector<IndexMeta> meta_list;
    for (auto& [name, meta] : s.index_meta) {
        meta_list.push_back(IndexMeta{name, meta.first, {meta.second}, "btree"});
    }
    for (auto& [name, meta] : s.hash_index_meta) {
        meta_list.push_back(IndexMeta{name, meta.first, {meta.second}, "hash"});
    }
    for (auto& [name, comp] : s.composite_indexes) {
        meta_list.push_back(IndexMeta{name, comp.table, comp.columns, "btree"});
    }

    std::unordered_map<std::string, std::vector<IndexMeta>> per_db;
    for (auto& m : meta_list) {
        auto [db, tbl] = split_key(m.table);
        (void)tbl;
        per_db[db].push_back(m);
    }
    for (auto& [db, mlist] : per_db) s.disk.save_index_meta(db, mlist);
    if (per_db.empty()) s.disk.save_index_meta(current_db, {});
}

StringResult Executor::exec_use(SharedDatabase& s, const std::string& database) {
    std::string key = to_lower_copy(database);
    if (!s.databases.count(key)) return StringResult::Err("Unknown database '" + database + "'.");
    current_db = key;
    return StringResult::Ok("Database changed to '" + key + "'.");
}

StringResult Executor::exec_show_tables(const SharedDatabase& s) const {
    std::string prefix = current_db + ".";
    std::vector<std::string> tables;
    for (auto& [k, _] : s.catalog.tables) {
        if (k.compare(0, prefix.size(), prefix) == 0) tables.push_back(k.substr(prefix.size()));
    }
    if (tables.empty()) return StringResult::Ok("No tables found in database '" + current_db + "'.");
    std::sort(tables.begin(), tables.end());

    // Matches Rust's `{:width$}` semantics: a MINIMUM width, not a fixed/max one — if
    // the content (e.g. the "Tables" header, 6 chars) is longer than max_len (whose
    // floor is 5), Rust prints it at its natural length with no padding, no panic.
    // The naive `width - text.size()` here previously underflowed (both are unsigned)
    // whenever text.size() > width, which is exactly what "Tables" being the longest
    // cell triggers when every actual table name is shorter than 5 chars.
    std::size_t max_len = 5;
    for (auto& t : tables) max_len = std::max(max_len, t.size());
    std::string sep = "+" + std::string(max_len + 2, '-') + "+\n";

    auto pad_line = [&](const std::string& text) {
        std::size_t pad = text.size() < max_len ? max_len - text.size() : 0;
        return "| " + text + std::string(pad, ' ') + " |\n";
    };

    std::string output = sep + pad_line("Tables") + sep;
    for (auto& t : tables) output += pad_line(t);
    output += sep.substr(0, sep.size() - 1); // no trailing newline after the final separator, matching Rust's output.push_str(&sep)
    return StringResult::Ok(output);
}

} // namespace engine
