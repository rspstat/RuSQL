// Faithful port of the DDL portion of rusql-core/src/engine/executor.rs (Phase 8a):
// exec_create, exec_drop, exec_truncate, exec_alter, exec_create_database,
// exec_drop_database, exec_create_index, exec_drop_index, exec_create_view,
// exec_drop_view, and the shared index-maintenance helpers (persist_views_for_db,
// index_insert_row, index_remove_row, rebuild_secondary_indexes, persist_index_meta),
// plus exec_use/exec_show_tables.

#include "engine/executor/executor.hpp"
#include "engine/fk_name.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <unordered_set>

#include "engine/storage/numeric_key.hpp"
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

// FIRST / AFTER c: the column `column` of the schema goes there ("" when it does; the message of the error when `after` names no column)
std::string place_column(TableSchema& schema, const std::string& column, const ColumnPosition& position) {
    auto find = [&](const std::string& name) {
        return std::find_if(schema.columns.begin(), schema.columns.end(), [&](const ColumnDef& c) { return c.name == name; });
    };
    auto moving = find(column);
    if (moving == schema.columns.end()) return "Column '" + column + "' not found";
    if (position.after && *position.after == column) return "";
    if (position.after && find(*position.after) == schema.columns.end()) return "Unknown column '" + *position.after + "' in '" + schema.name + "'";
    ColumnDef def = std::move(*moving);
    schema.columns.erase(moving);
    if (position.first) schema.columns.insert(schema.columns.begin(), std::move(def));
    else if (position.after) schema.columns.insert(find(*position.after) + 1, std::move(def));
    else schema.columns.push_back(std::move(def));
    return "";
}

// two names are one when they differ in case only (a column, an index)
bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

} // namespace

// ALTER TABLE with several actions is all or nothing: the columns the actions name are followed through a copy of the names, so the error of a
// later action is found before an earlier one has changed the table ("" when the actions can be done). As in MySQL, DROP / CHANGE / MODIFY /
// RENAME COLUMN and ALTER COLUMN name a column of the table as it was before the statement (one that an earlier action added is not there for them),
// while the new names must not meet a name that is taken by then and an index may name a column that was just added.
std::string alter_actions_error(const TableSchema& schema, const std::string& table, const std::vector<const AlterAction*>& actions) {
    std::vector<std::string> columns;
    for (const ColumnDef& c : schema.columns) columns.push_back(c.name);
    const std::vector<std::string> original = columns;
    auto find = [&](const std::string& name) {
        return std::find_if(columns.begin(), columns.end(), [&](const std::string& c) { return same_name(c, name); });
    };
    auto existing = [&](const std::string& name) { // a column of the table as it was, still there
        return find(name) != columns.end() &&
               std::any_of(original.begin(), original.end(), [&](const std::string& c) { return same_name(c, name); });
    };
    auto missing_after = [&](const std::optional<ColumnPosition>& position) {
        return position && position->after && find(*position->after) == columns.end();
    };
    for (const AlterAction* action : actions) {
        if (auto* v = std::get_if<AlterAction::AddColumn>(&action->data)) {
            if (find(v->column.name) != columns.end()) return "Duplicate column name '" + v->column.name + "'";
            if (missing_after(v->position)) return "Unknown column '" + *v->position->after + "' in '" + schema.name + "'";
            columns.push_back(v->column.name);
        } else if (auto* v = std::get_if<AlterAction::DropColumn>(&action->data)) {
            if (!existing(v->name)) return "Can't DROP '" + v->name + "'; check that column/key exists";
            columns.erase(find(v->name));
        } else if (auto* v = std::get_if<AlterAction::RenameColumn>(&action->data)) {
            if (!existing(v->from)) return "Unknown column '" + v->from + "' in '" + table + "'";
            if (!same_name(v->from, v->to) && find(v->to) != columns.end()) return "Duplicate column name '" + v->to + "'";
            *find(v->from) = v->to;
        } else if (auto* v = std::get_if<AlterAction::ChangeColumn>(&action->data)) {
            if (!existing(v->from)) return "Unknown column '" + v->from + "' in '" + table + "'";
            if (!same_name(v->from, v->column.name) && find(v->column.name) != columns.end()) return "Duplicate column name '" + v->column.name + "'";
            if (missing_after(v->position)) return "Unknown column '" + *v->position->after + "' in '" + schema.name + "'";
            *find(v->from) = v->column.name;
        } else if (auto* v = std::get_if<AlterAction::ModifyColumn>(&action->data)) {
            if (!existing(v->column.name)) return "Column '" + v->column.name + "' not found in '" + table + "'";
            if (missing_after(v->position)) return "Unknown column '" + *v->position->after + "' in '" + schema.name + "'";
        } else if (auto* v = std::get_if<AlterAction::SetDefault>(&action->data)) {
            if (!existing(v->column)) return "Unknown column '" + v->column + "' in '" + table + "'";
        } else if (auto* v = std::get_if<AlterAction::AddIndex>(&action->data)) {
            for (const std::string& column : v->columns) {
                if (find(column) == columns.end()) return "Key column '" + column + "' doesn't exist in table";
            }
        }
    }
    return "";
}

StringResult Executor::exec_create(SharedDatabase& s, std::string name, std::vector<ColumnDef> columns, bool if_not_exists,
                                    std::vector<std::string> primary_key_columns,
                                    std::vector<std::pair<std::optional<std::string>, std::string>> check_constraints,
                                    std::optional<PartitionBy> partition_by, std::vector<UniqueKey> unique_keys) {
    if (if_not_exists && s.tables.count(name)) return StringResult::Ok("Table '" + name + "' already exists, skipped.");
    for (std::size_t i = 0; i < columns.size(); i++) {
        for (std::size_t j = 0; j < i; j++) {
            if (same_name(columns[i].name, columns[j].name)) return StringResult::Err("Duplicate column name '" + columns[i].name + "'");
        }
    }
    for (auto& key : unique_keys) {
        for (auto& key_column : key.columns) {
            if (std::none_of(columns.begin(), columns.end(), [&](const ColumnDef& c) { return c.name == key_column; })) {
                return StringResult::Err("Key column '" + key_column + "' doesn't exist in table");
            }
        }
    }

    // Table partitioning (V1): expand HASH into an explicit partition list and validate
    // RANGE's MAXVALUE placement BEFORE creating anything, so a bad PARTITION BY clause
    // fails cleanly with nothing half-created. Child table names are `name__pname` (a
    // separator this parser's identifiers can never produce on their own).
    if (partition_by) {
        auto col_it = std::find_if(columns.begin(), columns.end(), [&](const ColumnDef& c) { return c.name == partition_by->column; });
        if (col_it == columns.end()) return StringResult::Err("PARTITION BY column '" + partition_by->column + "' not defined");
        // V1 correctness restriction: the partition column can't be AUTO_INCREMENT --
        // routing a row to a child (RANGE/LIST) needs the partition-key value BEFORE the
        // row is inserted, but an auto-increment value is only assigned INSIDE the
        // per-child exec_insert_inner call the router recurses into (chicken-and-egg).
        if (col_it->auto_increment) return StringResult::Err("PARTITION BY column cannot be AUTO_INCREMENT");
        // V1 correctness restriction: if the table has a primary key at all, the
        // partition column must be part of it (matches MySQL's own "every unique key
        // must include all partitioning columns" rule) -- each child table only enforces
        // PK uniqueness within ITSELF, so without this, two different children could
        // independently accept the same PK value with nothing to catch the collision.
        // When the partition column IS part of the PK, two rows sharing a PK necessarily
        // share the same partition-key value too, so they're always routed to the SAME
        // child, where its own PK uniqueness check catches the duplicate correctly.
        bool has_explicit_pk = !primary_key_columns.empty();
        bool has_inline_pk = std::any_of(columns.begin(), columns.end(), [](const ColumnDef& c) { return c.primary_key; });
        if (has_explicit_pk || has_inline_pk) {
            bool col_in_pk = has_explicit_pk
                                  ? std::find(primary_key_columns.begin(), primary_key_columns.end(), partition_by->column) != primary_key_columns.end()
                                  : col_it->primary_key;
            if (!col_in_pk) return StringResult::Err("PARTITION BY column '" + partition_by->column + "' must be part of the PRIMARY KEY");
        }
        if (partition_by->kind == PartitionKind::Hash) {
            if (partition_by->hash_partitions < 1) return StringResult::Err("PARTITION BY HASH requires at least 1 partition");
            partition_by->partitions.clear();
            for (int i = 0; i < partition_by->hash_partitions; i++) {
                PartitionDef def;
                def.name = "p" + std::to_string(i);
                partition_by->partitions.push_back(std::move(def));
            }
        } else {
            if (partition_by->partitions.empty()) return StringResult::Err("PARTITION BY requires at least one partition");
            for (std::size_t i = 0; i < partition_by->partitions.size(); i++) {
                if (partition_by->partitions[i].range_is_maxvalue && i + 1 != partition_by->partitions.size()) {
                    return StringResult::Err("MAXVALUE partition must be the last one in PARTITION BY RANGE");
                }
            }
        }
        for (auto& def : partition_by->partitions) def.child_table = name + "__" + def.name;
    }

    std::vector<CheckConstraint> schema_checks;
    schema_checks.reserve(check_constraints.size());
    for (auto& [cname, expr] : check_constraints) schema_checks.push_back(CheckConstraint{cname, expr});

    // `columns`/`primary_key_columns`/`check_constraints` are deliberately NOT moved into
    // create_table_full below (unlike before partitioning existed) -- the partitioned
    // branch further down needs to reuse them unchanged, once per child table.
    auto res = s.catalog.create_table_full(name, columns, primary_key_columns, schema_checks);
    if (res.is_err()) return StringResult::Err(res.error());
    s.catalog.get_table_mut(name)->unique_keys = unique_keys;

    s.tables[name] = {};
    {
        std::string pk_name;
        for (auto& c : columns) {
            if (c.primary_key) { pk_name = c.name; break; }
        }
        std::vector<KeyKind> kinds;
        for (auto& c : columns) {
            if (c.name == pk_name) kinds.push_back(key_kind_of(class_of_type(c.data_type)));
        }
        s.indexes[name] = BPlusTree(std::move(kinds));
    }
    s.table_locks[name] = std::make_shared<FairSharedMutex>();
    s.table_data_locks[name] = std::make_shared<FairSharedMutex>();
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

    // Table partitioning (V1): `s.tables[name]` above stays a permanently-empty phantom --
    // every child gets the exact same schema via a plain (non-partitioned) recursive
    // exec_create call, reusing every line above unchanged for each one. The partition
    // router (executor_partition.cpp) is what actually redirects INSERT/UPDATE/DELETE/
    // SELECT on `name` to these children; nothing else in this function needs to know
    // partitioning exists.
    if (partition_by) {
        for (auto& def : partition_by->partitions) {
            auto child_res = exec_create(s, def.child_table, columns, /*if_not_exists=*/false, primary_key_columns, check_constraints, std::nullopt, unique_keys);
            if (child_res.is_err()) {
                // Best-effort cleanup so a failed CREATE TABLE ... PARTITION BY doesn't
                // leave a half-created table behind for this session to trip over later.
                for (auto& cleanup : partition_by->partitions) {
                    if (cleanup.child_table == def.child_table) break;
                    exec_drop(s, cleanup.child_table, /*if_exists=*/true);
                }
                exec_drop(s, name, /*if_exists=*/true);
                return child_res;
            }
        }
        s.catalog.get_table_mut(name)->partition_info = partition_by;
    }

    s.disk.save_schema(name, *s.catalog.get_table(name));
    return StringResult::Ok("Table '" + name + "' created.");
}

StringResult Executor::exec_drop(SharedDatabase& s, const std::string& name, bool if_exists) {
    if (if_exists && !s.tables.count(name)) return StringResult::Ok("Table '" + name + "' does not exist, skipped.");
    // a table that another table's foreign key refers to cannot go (MySQL 3730) -- except while a RESTORE replays a dump, which drops and
    // recreates the tables one after the other (a dump of mysqldump sets FOREIGN_KEY_CHECKS = 0 for the same reason)
    for (auto& [other_name, other_schema] : s.catalog.tables) {
        if (skip_fk_checks) break;
        if (other_name == name) continue;
        for (auto& col : other_schema.columns) {
            if (col.foreign_key && col.foreign_key->ref_table == name) {
                std::string bare = other_name.substr(other_name.rfind('.') == std::string::npos ? 0 : other_name.rfind('.') + 1);
                std::string bare_name = name.substr(name.rfind('.') == std::string::npos ? 0 : name.rfind('.') + 1);
                return StringResult::Err("Cannot drop table '" + bare_name + "' referenced by a foreign key constraint on table '" + bare + "'.");
            }
        }
    }
    // Table partitioning: capture the child list BEFORE drop_table erases the catalog
    // entry (and so partition_info with it) below.
    std::optional<PartitionBy> partition_by;
    if (auto* schema = s.catalog.get_table(name)) partition_by = schema->partition_info;
    auto res = s.catalog.drop_table(name);
    if (res.is_err()) return StringResult::Err(res.error());
    s.tables.erase(name);
    s.indexes.erase(name);
    s.table_locks.erase(name);
    s.table_data_locks.erase(name);
    s.table_stats.erase(name);
    s.dml_since_vacuum.erase(name);
    s.dml_since_analyze.erase(name);
    s.row_pk_pos.erase(name);
    s.buffer_pool.invalidate(name);
    s.disk.delete_table(name);
    // The indexes of the table go with it. They stayed, and a table made later under the same name was read through them: a query found rows of the dropped table,
    // and an INSERT was refused as a duplicate of a value the old table had held.
    bool dropped_index = false;
    for (auto it = s.index_meta.begin(); it != s.index_meta.end();) {
        if (it->second.first != name) {
            ++it;
            continue;
        }
        s.indexes.erase(it->first);
        it = s.index_meta.erase(it);
        dropped_index = true;
    }
    for (auto it = s.hash_index_meta.begin(); it != s.hash_index_meta.end();) {
        if (it->second.first != name) {
            ++it;
            continue;
        }
        s.hash_indexes.erase(it->first);
        it = s.hash_index_meta.erase(it);
        dropped_index = true;
    }
    for (auto it = s.composite_indexes.begin(); it != s.composite_indexes.end();) {
        if (it->second.table != name) {
            ++it;
            continue;
        }
        it = s.composite_indexes.erase(it);
        dropped_index = true;
    }
    if (dropped_index) persist_index_meta(s);
    {
        std::lock_guard<std::mutex> guard(s.unique_probes->mutex);
        s.unique_probes->by_table.erase(name);
    }
    if (partition_by) {
        for (auto& def : partition_by->partitions) exec_drop(s, def.child_table, /*if_exists=*/true);
    }
    return StringResult::Ok("Table '" + name + "' dropped.");
}

StringResult Executor::exec_truncate(SharedDatabase& s, const std::string& name) {
    auto it = s.tables.find(name);
    if (it == s.tables.end()) return StringResult::Err("Table '" + name + "' not found");
    // Table partitioning: the logical table's own `s.tables[name]` is a permanently-empty
    // phantom (see exec_create) -- truncate every real child instead. The clear()/erase()/
    // BPlusTree-reset below still run for `name` itself too (harmless no-ops on an
    // already-empty phantom), so a non-partitioned TRUNCATE is completely unaffected.
    if (auto* schema = s.catalog.get_table(name); schema && schema->partition_info) {
        for (auto& def : schema->partition_info->partitions) {
            auto child_res = exec_truncate(s, def.child_table);
            if (child_res.is_err()) return child_res;
        }
    }
    it->second.clear();
    s.row_pk_pos.erase(name);
    if (auto idx_it = s.indexes.find(name); idx_it != s.indexes.end()) idx_it->second = BPlusTree(idx_it->second.kinds());
    if (auto* schema = s.catalog.get_table_mut(name)) schema->auto_increment_counters.clear();
    s.buffer_pool.invalidate(name);
    s.disk.save_table(name, {});
    return StringResult::Ok("Table '" + name + "' truncated.");
}

// CREATE TABLE t LIKE s: the columns, keys, checks and indexes of s, no rows (and no foreign keys)
StringResult Executor::exec_create_like(SharedDatabase& s, const std::string& name, const std::string& source, bool if_not_exists) {
    const TableSchema* from = s.catalog.get_table(source);
    if (!from) return StringResult::Err("Table '" + source + "' not found");
    if (if_not_exists && s.catalog.tables.count(name)) return StringResult::Ok("Table '" + name + "' already exists, skipped.");
    std::vector<ColumnDef> columns = from->columns;
    for (ColumnDef& c : columns) c.foreign_key = std::nullopt;
    std::vector<std::pair<std::optional<std::string>, std::string>> checks;
    for (const CheckConstraint& c : from->check_constraints) checks.emplace_back(c.name, c.expression);
    const std::vector<std::string> primary_key_columns = from->primary_key_columns;
    const std::vector<UniqueKey> unique_keys = from->unique_keys;
    // (the indexes made by CREATE INDEX are kept as "<table>_<name>")
    struct Secondary { std::string name; std::vector<std::string> columns; bool hashed; };
    std::vector<Secondary> secondary;
    const std::string key_prefix = source + "_";
    auto bare = [&](const std::string& key) { return key.compare(0, key_prefix.size(), key_prefix) == 0 ? key.substr(key_prefix.size()) : key; };
    for (auto& [key, meta] : s.index_meta) {
        if (meta.first == source) secondary.push_back({bare(key), {meta.second}, false});
    }
    for (auto& [key, meta] : s.hash_index_meta) {
        if (meta.first == source) secondary.push_back({bare(key), {meta.second}, true});
    }
    for (auto& [key, comp] : s.composite_indexes) {
        if (comp.table == source) secondary.push_back({bare(key), comp.columns, false});
    }
    auto created = exec_create(s, name, std::move(columns), false, primary_key_columns, std::move(checks), std::nullopt, unique_keys);
    if (created.is_err()) return created;
    for (const Secondary& index : secondary) {
        auto made = exec_create_index(s, index.name, name, index.columns, index.hashed);
        if (made.is_err()) return made;
    }
    return created;
}

// CREATE TABLE t [(columns)] [AS] SELECT ...: the table is made from the columns of the query (a column of a table the query reads keeps its type; any other gets
// one from the values it holds), and the rows of the query go in
StringResult Executor::exec_create_as(SharedDatabase& s, const std::string& name, bool if_not_exists, std::vector<ColumnDef> columns, Statement query) {
    if (s.catalog.tables.count(name) || s.views.count(name)) {
        if (if_not_exists) return StringResult::Ok("Table '" + name + "' already exists, skipped.");
        return StringResult::Err("Table '" + name + "' already exists");
    }
    const Statement query_copy = query; // (the names of an empty answer's columns come from the statement)
    auto out = execute_with_s(s, std::move(query));
    if (out.is_err()) return out;
    auto [names, rows] = parse_table_output(out.value());
    if (names.empty()) names = derived_column_names(s, query_copy);
    if (names.empty()) return StringResult::Err("The query of CREATE TABLE ... SELECT has no columns");

    // the tables the query reads, for the types of its plain columns
    std::vector<const TableSchema*> sources;
    {
        const Statement* q = &query_copy;
        while (q) { // (a set operation: the left side names the columns)
            if (auto* u = std::get_if<Statement::Union>(&q->data)) q = u->left.get();
            else if (auto* i = std::get_if<Statement::Intersect>(&q->data)) q = i->left.get();
            else if (auto* e = std::get_if<Statement::Except>(&q->data)) q = e->left.get();
            else break;
        }
        if (q) {
            if (auto* sel = std::get_if<Statement::Select>(&q->data)) {
                if (auto* t = s.catalog.get_table(sel->table)) sources.push_back(t);
                for (const Join& j : sel->joins) {
                    if (auto* t = s.catalog.get_table(j.table)) sources.push_back(t);
                }
            }
        }
    }
    auto same = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    };
    auto type_of_values = [&](const std::string& column) {
        bool any = false, whole = true, number = true;
        std::size_t scale = 0;
        for (const Row& r : rows) {
            auto v = r.find(column);
            if (v == r.end() || v->second == EXECUTOR_NULL_VALUE) continue;
            any = true;
            const std::string& text = v->second;
            if (!parse_number(text)) {
                number = false;
                break;
            }
            if (text.find_first_of(".eE") != std::string::npos) {
                whole = false;
                auto dot = text.find('.');
                if (dot != std::string::npos && text.find_first_of("eE") == std::string::npos) scale = std::max(scale, text.size() - dot - 1);
            }
        }
        ColumnDef def;
        def.name = column;
        def.data_type = DataType(DataType::Text{});
        if (!any) return def;
        if (number && whole) def.data_type = DataType(DataType::BigInt{});
        else if (number) def.data_type = DataType(DataType::Decimal{30, static_cast<std::uint8_t>(std::min<std::size_t>(scale, 30))});
        return def;
    };

    std::vector<ColumnDef> made = columns; // (the columns the statement declares come first)
    for (const std::string& column : names) {
        auto declared = std::find_if(made.begin(), made.end(), [&](const ColumnDef& c) { return same(c.name, column); });
        if (declared != made.end()) continue;
        ColumnDef def;
        bool found = false;
        for (const TableSchema* t : sources) {
            auto source_column = std::find_if(t->columns.begin(), t->columns.end(), [&](const ColumnDef& c) { return same(c.name, column); });
            if (source_column == t->columns.end()) continue;
            def.name = column;
            def.data_type = source_column->data_type;
            def.is_unsigned = source_column->is_unsigned;
            def.not_null = source_column->not_null && !source_column->primary_key;
            found = true;
            break;
        }
        made.push_back(found ? def : type_of_values(column));
    }

    auto created = exec_create(s, name, made, false, {}, {}, std::nullopt, {});
    if (created.is_err()) return created;
    if (rows.empty()) return created;

    Statement::Insert insert;
    insert.table = name;
    insert.columns = names;
    insert.on_conflict = InsertConflict(InsertConflict::Abort{});
    for (const Row& r : rows) {
        std::vector<std::string> values;
        for (const std::string& column : names) {
            auto v = r.find(column);
            values.push_back(v == r.end() ? EXECUTOR_NULL_VALUE : v->second);
        }
        insert.values.push_back(std::move(values));
    }
    auto inserted = execute_with_s(s, Statement(std::move(insert)));
    if (inserted.is_err()) {
        exec_drop(s, name, true); // (nothing half-made)
        return inserted;
    }
    return StringResult::Ok("Table '" + name + "' created with " + std::to_string(rows.size()) + " row(s).");
}

StringResult Executor::exec_alter(SharedDatabase& s, const std::string& table, AlterAction action) {
    // Table partitioning (V1): every action except ADD/DROP PARTITION is blocked on a
    // partitioned table -- a schema change (ADD/DROP/MODIFY/RENAME COLUMN, FK/constraint
    // changes) would need to propagate identically to every child, which this version
    // doesn't implement. Rejecting with a clear error here is far safer than silently
    // altering only the permanently-empty logical parent.
    bool is_partition_action =
        std::holds_alternative<AlterAction::AddPartition>(action.data) || std::holds_alternative<AlterAction::DropPartition>(action.data);
    if (!is_partition_action) {
        if (auto* schema = s.catalog.get_table(table); schema && schema->partition_info) {
            return StringResult::Err("ALTER TABLE only supports ADD PARTITION/DROP PARTITION on a partitioned table in this version");
        }
    }

    if (std::holds_alternative<AlterAction::NoOp>(action.data)) { // DISABLE KEYS / ENABLE KEYS
        if (!s.catalog.get_table(table)) return StringResult::Err("Table '" + table + "' doesn't exist");
        return StringResult::Ok("Table '" + table + "' altered.");
    }

    // Every index keeps a copy of the rows it points to (a lookup by key hands that copy back), so after the columns of the rows changed -- a column added with
    // its default, one dropped, the values of one converted -- the indexes have to be built again, or a lookup shows the row as it was (the new column empty).
    auto rebuild_indexes = [&]() {
        const TableSchema* schema = s.catalog.get_table(table);
        auto rows_it = s.tables.find(table);
        if (!schema || rows_it == s.tables.end()) return;
        const std::vector<Row>& rows = rows_it->second;
        if (auto idx_it = s.indexes.find(table); idx_it != s.indexes.end()) {
            std::string pk_col_name;
            for (auto& c : schema->columns) {
                if (c.primary_key) { pk_col_name = c.name; break; }
            }
            if (pk_col_name.empty() && !schema->columns.empty()) pk_col_name = schema->columns.front().name;
            idx_it->second = build_pk_tree(rows, pk_col_name, key_kinds_of(schema, {pk_col_name})); // (the column may have a new type, and so a new kind of key)
        }
        rebuild_secondary_indexes(s, table, rows);
        for (auto& [key, ci] : s.composite_indexes) {
            if (ci.table == table) ci.rebuild(rows);
        }
    };

    // AUTO_INCREMENT = n: the next number is n, or the one after the largest the table holds when that is more (the counter holds the last number given)
    if (auto* v = std::get_if<AlterAction::SetAutoIncrement>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' doesn't exist");
        for (const ColumnDef& col : schema->columns) {
            if (!col.auto_increment) continue;
            std::int64_t largest = 0;
            if (auto rows = s.tables.find(table); rows != s.tables.end()) {
                for (const Row& r : rows->second) {
                    auto cell = r.find(col.name);
                    if (cell == r.end() || !is_visible(r)) continue;
                    std::int64_t number = 0;
                    const char* end = cell->second.data() + cell->second.size();
                    if (std::from_chars(cell->second.data(), end, number).ptr == end) largest = std::max(largest, number);
                }
            }
            schema->auto_increment_counters[col.name] = std::max<std::int64_t>(v->next - 1, largest);
            s.disk.save_schema(table, *schema);
            break;
        }
        return StringResult::Ok("Table '" + table + "' altered.");
    }

    if (auto* v = std::get_if<AlterAction::AddPartition>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        if (!schema->partition_info) return StringResult::Err("Table '" + table + "' is not partitioned");
        PartitionBy& info = *schema->partition_info;
        if (info.kind == PartitionKind::Hash) return StringResult::Err("Cannot ADD PARTITION on a HASH-partitioned table");
        // The parser detects RANGE vs LIST from LESS-THAN vs IN, so a mismatch here means
        // e.g. `ADD PARTITION (... VALUES IN (...))` was used on a RANGE table.
        if (info.kind == PartitionKind::Range && !v->def.list_values.empty())
            return StringResult::Err("Table '" + table + "' is RANGE-partitioned -- use VALUES LESS THAN");
        if (info.kind == PartitionKind::List && (v->def.range_upper_bound || v->def.range_is_maxvalue))
            return StringResult::Err("Table '" + table + "' is LIST-partitioned -- use VALUES IN");
        if (std::any_of(info.partitions.begin(), info.partitions.end(), [&](const PartitionDef& d) { return d.name == v->def.name; })) {
            return StringResult::Err("Partition '" + v->def.name + "' already exists");
        }
        if (info.kind == PartitionKind::Range && !info.partitions.empty() && info.partitions.back().range_is_maxvalue) {
            return StringResult::Err("Cannot ADD PARTITION after a MAXVALUE partition");
        }
        PartitionDef new_def = v->def;
        new_def.child_table = table + "__" + new_def.name;
        // exec_create's check_constraints parameter is the raw (name, expr) pair form;
        // TableSchema only stores the already-converted CheckConstraint form -- convert back.
        std::vector<std::pair<std::optional<std::string>, std::string>> raw_checks;
        raw_checks.reserve(schema->check_constraints.size());
        for (auto& c : schema->check_constraints) raw_checks.emplace_back(c.name, c.expression);
        auto create_res =
            exec_create(s, new_def.child_table, schema->columns, /*if_not_exists=*/false, schema->primary_key_columns, raw_checks, std::nullopt, schema->unique_keys);
        if (create_res.is_err()) return create_res;
        schema = s.catalog.get_table_mut(table); // exec_create above may have invalidated schema (map rehash)
        schema->partition_info->partitions.push_back(new_def);
        s.disk.save_schema(table, *schema);
        return StringResult::Ok("Partition '" + new_def.name + "' added to '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::DropPartition>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        if (!schema->partition_info) return StringResult::Err("Table '" + table + "' is not partitioned");
        PartitionBy& info = *schema->partition_info;
        auto it = std::find_if(info.partitions.begin(), info.partitions.end(), [&](const PartitionDef& d) { return d.name == v->name; });
        if (it == info.partitions.end()) return StringResult::Err("Partition '" + v->name + "' not found in '" + table + "'");
        std::string child_table = it->child_table;
        info.partitions.erase(it);
        s.disk.save_schema(table, *schema);
        return exec_drop(s, child_table, /*if_exists=*/true).is_err()
                   ? StringResult::Err("Partition '" + v->name + "' metadata removed but dropping its data failed")
                   : StringResult::Ok("Partition '" + v->name + "' dropped from '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::AddColumn>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        for (const ColumnDef& c : schema->columns) {
            if (same_name(c.name, v->column.name)) return StringResult::Err("Duplicate column name '" + v->column.name + "'");
        }

        ColumnDef new_col;
        new_col.name = v->column.name;
        new_col.data_type = v->column.data_type;
        new_col.is_unsigned = v->column.is_unsigned;
        new_col.primary_key = false;
        new_col.not_null = v->column.not_null;
        new_col.unique = v->column.unique;
        new_col.unique_constraint_name = v->column.unique_constraint_name;
        new_col.auto_increment = false;
        new_col.default_value = v->column.default_value;
        new_col.foreign_key = std::nullopt;
        new_col.check_expr = v->column.check_expr;
        schema->columns.push_back(new_col);
        if (v->position) {
            if (auto error = place_column(*schema, new_col.name, *v->position); !error.empty()) {
                schema->columns.pop_back();
                return StringResult::Err(error);
            }
        }

        std::string fill_val = (v->column.default_value && *v->column.default_value != NULL_DEFAULT) ? *v->column.default_value
                                                                                                       : EXECUTOR_NULL_VALUE;
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) row[new_col.name] = fill_val;
        }
        rebuild_indexes();
        s.disk.save_schema(table, *s.catalog.get_table(table));
        s.buffer_pool.write_through(table, s.tables.at(table), s.disk); // (committed rows only: see BufferPool::write_through)
        return StringResult::Ok("Column '" + new_col.name + "' added to '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::DropColumn>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        auto& cols = schema->columns;
        if (std::none_of(cols.begin(), cols.end(), [&](const ColumnDef& c) { return c.name == v->name; })) {
            return StringResult::Err("Can't DROP '" + v->name + "'; check that column/key exists");
        }
        cols.erase(std::remove_if(cols.begin(), cols.end(), [&](const ColumnDef& c) { return c.name == v->name; }), cols.end());
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) row.erase(v->name);
        }
        rebuild_indexes();
        s.disk.save_schema(table, *s.catalog.get_table(table));
        s.buffer_pool.write_through(table, s.tables.at(table), s.disk); // (committed rows only: see BufferPool::write_through)
        return StringResult::Ok("Column '" + v->name + "' dropped from '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::RenameColumn>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        if (std::none_of(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == v->from; })) {
            return StringResult::Err("Unknown column '" + v->from + "' in '" + table + "'");
        }
        for (const ColumnDef& c : schema->columns) {
            if (c.name != v->from && same_name(c.name, v->to)) return StringResult::Err("Duplicate column name '" + v->to + "'");
        }
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
                std::string pk_col_name;
                for (auto& c : schema->columns) {
                    if (c.primary_key) { pk_col_name = c.name; break; }
                }
                if (pk_col_name.empty() && !schema->columns.empty()) pk_col_name = schema->columns.front().name;
                idx_it->second = build_pk_tree(rows, pk_col_name, idx_it->second.kinds());
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
        s.buffer_pool.write_through(table, s.tables.at(table), s.disk); // (committed rows only: see BufferPool::write_through)
        s.buffer_pool.write_page(table, s.tables.at(table));
        return StringResult::Ok("Column '" + v->from + "' renamed to '" + v->to + "' in '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::ModifyColumn>(&action.data)) {
        const TableSchema* schema_check = s.catalog.get_table(table);
        if (!schema_check) return StringResult::Err("Table '" + table + "' not found");
        bool exists = std::any_of(schema_check->columns.begin(), schema_check->columns.end(),
                                   [&](const ColumnDef& c) { return c.name == v->column.name; });
        if (!exists) return StringResult::Err("Column '" + v->column.name + "' not found in '" + table + "'");
        if (v->position && v->position->after && v->position->after != v->column.name &&
            std::none_of(schema_check->columns.begin(), schema_check->columns.end(), [&](const ColumnDef& c) { return c.name == *v->position->after; })) {
            return StringResult::Err("Unknown column '" + *v->position->after + "' in '" + schema_check->name + "'"); // (before anything is changed)
        }

        // The values the column holds are converted to the new definition, or the statement fails and nothing changes (MySQL strict mode: 300 does not fit a
        // TINYINT, 99999.99 a DECIMAL(5,1), 'long text' a VARCHAR(5); a DECIMAL(10,4) writes 1.5 as 1.5000, a DATETIME a date with 00:00:00).
        std::vector<std::pair<Row*, std::string>> converted;
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            ColumnDef target = *std::find_if(schema_check->columns.begin(), schema_check->columns.end(), [&](const ColumnDef& c) { return c.name == v->column.name; });
            target.data_type = v->column.data_type;
            target.is_unsigned = v->column.is_unsigned;
            std::size_t row_number = 0;
            for (auto& row : it->second) {
                const bool visible = is_visible(row);
                if (visible) row_number++;
                auto vit = row.find(v->column.name);
                if (vit == row.end()) continue;
                std::string val = vit->second;
                if (val == EXECUTOR_NULL_VALUE) {
                    if (visible && v->column.not_null) return StringResult::Err("Invalid use of NULL value");
                    continue;
                }
                if (auto error = coerce_column_value(target, val, row_number)) {
                    if (visible) return StringResult::Err(*error);
                    continue; // (an older version of a row that is gone: left as it is)
                }
                if (visible && !value_matches_data_type(v->column.data_type, val)) { // (an ENUM or SET the value is not one of)
                    return StringResult::Err("Cannot convert value '" + val + "' in column '" + v->column.name + "' to " +
                                              debug_data_type(v->column.data_type));
                }
                if (val != vit->second) converted.emplace_back(&row, std::move(val));
            }
        }

        auto* schema = s.catalog.get_table_mut(table);
        for (auto& c : schema->columns) {
            if (c.name == v->column.name) {
                c.data_type = v->column.data_type;
                c.is_unsigned = v->column.is_unsigned;
                c.not_null = v->column.not_null;
                if (v->column.unique) { // (a UNIQUE KEY the column has is an index, which the new definition does not remove)
                    c.unique = true;
                    c.unique_constraint_name = v->column.unique_constraint_name;
                }
                c.auto_increment = v->column.auto_increment;
                c.default_value = v->column.default_value;
                // primary_key cannot be changed via MODIFY (ignored, matching Rust).
                break;
            }
        }
        if (v->position) {
            if (auto error = place_column(*schema, v->column.name, *v->position); !error.empty()) return StringResult::Err(error);
        }
        if (!converted.empty()) {
            for (auto& [row, text] : converted) (*row)[v->column.name] = std::move(text);
            rebuild_indexes();
            s.buffer_pool.write_through(table, s.tables.at(table), s.disk); // (committed rows only: see BufferPool::write_through)
            s.buffer_pool.write_page(table, s.tables.at(table));
        }
        s.disk.save_schema(table, *s.catalog.get_table(table));
        return StringResult::Ok("Column '" + v->column.name + "' in '" + table + "' modified.");
    }

    // CHANGE [COLUMN] old new definition: the column is renamed, then given the definition
    if (auto* v = std::get_if<AlterAction::ChangeColumn>(&action.data)) {
        if (!s.catalog.get_table(table)) return StringResult::Err("Table '" + table + "' not found");
        if (v->from != v->column.name) {
            auto renamed = exec_alter(s, table, AlterAction(AlterAction::RenameColumn{v->from, v->column.name}));
            if (renamed.is_err()) return renamed;
        }
        return exec_alter(s, table, AlterAction(AlterAction::ModifyColumn{v->column, v->position}));
    }

    // ADD INDEX|KEY [name] (a, b)
    if (auto* v = std::get_if<AlterAction::AddIndex>(&action.data)) {
        if (v->columns.empty()) return StringResult::Err("An index needs at least one column");
        return exec_create_index(s, v->name.value_or(v->columns.front()), table, v->columns, false);
    }

    // ALTER COLUMN c SET DEFAULT v / DROP DEFAULT
    if (auto* v = std::get_if<AlterAction::SetDefault>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        auto column = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == v->column; });
        if (column == schema->columns.end()) return StringResult::Err("Unknown column '" + v->column + "' in '" + table + "'");
        column->default_value = v->value;
        s.disk.save_schema(table, *schema);
        return StringResult::Ok("Default of column '" + v->column + "' in '" + table + "' changed.");
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
        if (auto it = s.table_data_locks.find(table); it != s.table_data_locks.end()) {
            auto lock = std::move(it->second);
            s.table_data_locks.erase(it);
            s.table_data_locks.insert({v->to, std::move(lock)});
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
        // index_meta/hash_index_meta/composite_indexes keys are qualified as "<table>_<name>"
        // (so the same index name can be reused across tables/databases) -- rebuild both the
        // key and the .first/.table value together so they stay in sync with the renamed
        // s.indexes/s.hash_indexes storage keys below.
        {
            std::vector<std::string> old_keys;
            for (auto& [k, meta] : s.index_meta) {
                if (meta.first == table) old_keys.push_back(k);
            }
            for (auto& old_key : old_keys) {
                auto meta_node = s.index_meta.extract(old_key);
                meta_node.key() = v->to + old_key.substr(table.size());
                meta_node.mapped().first = v->to;
                s.index_meta.insert(std::move(meta_node));
            }
        }
        {
            std::vector<std::string> old_keys;
            for (auto& [k, meta] : s.hash_index_meta) {
                if (meta.first == table) old_keys.push_back(k);
            }
            for (auto& old_key : old_keys) {
                auto meta_node = s.hash_index_meta.extract(old_key);
                meta_node.key() = v->to + old_key.substr(table.size());
                meta_node.mapped().first = v->to;
                s.hash_index_meta.insert(std::move(meta_node));
            }
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
        std::vector<std::string> hash_keys;
        for (auto& [k, _] : s.hash_indexes) {
            if (k.compare(0, prefix.size(), prefix) == 0) hash_keys.push_back(k);
        }
        for (auto& old_key : hash_keys) {
            std::string suffix = old_key.substr(table.size());
            std::string new_key = v->to + suffix;
            auto it = s.hash_indexes.find(old_key);
            auto hi = std::move(it->second);
            s.hash_indexes.erase(it);
            hi.table = v->to;
            s.hash_indexes.insert({new_key, std::move(hi)});
        }
        {
            std::vector<std::string> old_keys;
            for (auto& [k, ci] : s.composite_indexes) {
                if (ci.table == table) old_keys.push_back(k);
            }
            for (auto& old_key : old_keys) {
                auto ci_node = s.composite_indexes.extract(old_key);
                ci_node.key() = v->to + old_key.substr(table.size());
                ci_node.mapped().table = v->to;
                s.composite_indexes.insert(std::move(ci_node));
            }
        }
        persist_index_meta(s);

        s.disk.save_schema(v->to, *s.catalog.get_table(v->to));
        if (auto it = s.tables.find(v->to); it != s.tables.end()) s.buffer_pool.write_through(v->to, it->second, s.disk);
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
        // (CREATE TABLE keeps the parent under its database too: an unqualified name is the parent in the database of this table)
        if (fk.ref_table.find('.') == std::string::npos && table.find('.') != std::string::npos) fk.ref_table = table.substr(0, table.find('.')) + "." + fk.ref_table;
        fk.ref_column = v->ref_column;
        fk.on_delete = v->on_delete;
        fk.on_update = v->on_update;
        fk.name = v->name.value_or("");
        if (!fk.name.empty()) {
            for (auto& other : schema->columns) {
                if (other.foreign_key && same_name(other.foreign_key->name, fk.name)) return StringResult::Err("Duplicate foreign key constraint name '" + fk.name + "'");
            }
        }
        col_it->foreign_key = fk;
        s.disk.save_schema(table, *s.catalog.get_table(table));
        std::string constraint_label = v->name.value_or("fk_" + v->column);
        return StringResult::Ok("Foreign key '" + constraint_label + "' added to '" + table + "'.");
    }

    if (auto* v = std::get_if<AlterAction::DropForeignKey>(&action.data)) {
        auto* schema = s.catalog.get_table_mut(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        bool found = false;
        const std::string bare_table = table.substr(table.rfind('.') == std::string::npos ? 0 : table.rfind('.') + 1);
        for (std::size_t i = 0; i < schema->columns.size(); i++) {
            auto& col = schema->columns[i];
            if (col.foreign_key) {
                // by its name (the one it was given, or the <table>_ibfk_<n> it is reported under); the column name and fk_<column> are the older spellings
                bool matches = same_name(fk_constraint_name(*schema, bare_table, i), v->name) || col.foreign_key->column == v->name || ("fk_" + col.foreign_key->column) == v->name;
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
        // the rows the table holds already must not repeat the key (MySQL 1062); a NULL in it repeats nothing
        auto repeated_key = [&](const std::vector<std::string>& key_columns) -> std::optional<std::string> {
            auto tit = s.tables.find(table);
            if (tit == s.tables.end()) return std::nullopt;
            std::unordered_set<std::string> seen;
            for (auto& row : tit->second) {
                if (!is_visible(row)) continue;
                std::string encoded, shown;
                bool has_null = false;
                for (std::size_t k = 0; k < key_columns.size(); k++) {
                    auto it = row.find(key_columns[k]);
                    const std::string value = it != row.end() ? it->second : std::string(EXECUTOR_NULL_VALUE);
                    if (value == EXECUTOR_NULL_VALUE) has_null = true;
                    const std::string part = collation::key(value);
                    encoded += std::to_string(part.size()) + ":" + part;
                    shown += (k ? "-" : "") + value;
                }
                if (has_null) continue;
                if (!seen.insert(encoded).second) return "Duplicate entry '" + shown + "' for key '" + v->name.value_or(key_columns.front()) + "'";
            }
            return std::nullopt;
        };
        if (v->columns.size() > 1) { // a key over several columns
            for (auto& key_column : v->columns) {
                if (std::none_of(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == key_column; })) {
                    return StringResult::Err("Key column '" + key_column + "' doesn't exist in table");
                }
            }
            if (auto repeated = repeated_key(v->columns)) return StringResult::Err(*repeated);
            schema->unique_keys.push_back(UniqueKey{v->name, v->columns});
            s.disk.save_schema(table, *s.catalog.get_table(table));
            return StringResult::Ok("Unique constraint '" + v->name.value_or(v->columns.front()) + "' added to '" + table + "'.");
        }
        auto col_it = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == v->column; });
        if (col_it == schema->columns.end()) return StringResult::Err("Column '" + v->column + "' not found in '" + table + "'");
        if (auto repeated = repeated_key({v->column})) return StringResult::Err(*repeated);
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
            auto& keys = schema->unique_keys;
            const std::size_t keys_before = keys.size();
            keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const UniqueKey& k) { return k.name == std::optional<std::string>(v->name); }), keys.end());
            if (keys.size() < keys_before) {
                s.disk.save_schema(table, *s.catalog.get_table(table));
                return StringResult::Ok("Constraint '" + v->name + "' dropped from '" + table + "'.");
            }
            for (auto& col : schema->columns) {
                if (col.unique_constraint_name == std::optional<std::string>(v->name)) {
                    col.unique = false;
                    col.unique_constraint_name = std::nullopt;
                    s.disk.save_schema(table, *s.catalog.get_table(table));
                    return StringResult::Ok("Constraint '" + v->name + "' dropped from '" + table + "'.");
                }
            }
            // (ALTER TABLE ... DROP INDEX name: an index of that name goes; DROP INDEX itself answers "skipped" for one that does not exist)
            if (auto dropped = exec_drop_index(s, v->name); dropped.is_ok() && dropped.value().find(" dropped.") != std::string::npos) return dropped;
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
        s.table_data_locks.erase(t);
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

    // PLAN.md-tracked bug fix: hash indexes were never cleaned up here at all (unlike
    // the secondary B+Tree / composite index loops just above/below), leaking every
    // dropped database's hash indexes in memory permanently.
    std::vector<std::string> hash_keys;
    for (auto& [k, _] : s.hash_indexes) {
        if (k.compare(0, prefix.size(), prefix) == 0) hash_keys.push_back(k);
    }
    for (auto& k : hash_keys) s.hash_indexes.erase(k);

    {
        std::lock_guard<std::mutex> guard(s.unique_probes->mutex);
        for (auto it = s.unique_probes->by_table.begin(); it != s.unique_probes->by_table.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) it = s.unique_probes->by_table.erase(it);
            else ++it;
        }
    }
    for (auto it = s.hash_index_meta.begin(); it != s.hash_index_meta.end();) {
        if (it->second.first.compare(0, prefix.size(), prefix) == 0) it = s.hash_index_meta.erase(it);
        else ++it;
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
    {
        // an index name is used once on a table, whatever the case (MySQL error 1061)
        const std::string wanted = table + "_" + index_name;
        auto taken = [&](const auto& indexes, auto table_of) {
            return std::any_of(indexes.begin(), indexes.end(), [&](const auto& entry) { return table_of(entry.second) == table && same_name(entry.first, wanted); });
        };
        if (taken(s.index_meta, [](const auto& meta) { return meta.first; }) || taken(s.hash_index_meta, [](const auto& meta) { return meta.first; }) ||
            taken(s.composite_indexes, [](const auto& index) { return index.table; })) {
            return StringResult::Err("Duplicate key name '" + index_name + "'");
        }
    }

    if (using_hash) {
        if (columns.size() != 1) return StringResult::Err("Hash index supports only single-column indexes.");
        const std::string& column = columns.front();
        HashIndex hi(table, column);
        if (auto it = s.tables.find(table); it != s.tables.end()) hi.rebuild(it->second);
        s.hash_indexes.insert({table + "_" + index_name, std::move(hi)});
        s.hash_index_meta.insert({table + "_" + index_name, {table, column}});
        persist_index_meta(s);
        return StringResult::Ok("Hash index '" + index_name + "' created on '" + table + "'.'" + column + "'.");
    }

    if (columns.size() == 1) {
        const std::string& column = columns.front();
        BPlusTree tree(key_kinds_of(s.catalog.get_table(table), {column}));
        IndexBuckets bucket(tree.kinds());
        if (auto it = s.tables.find(table); it != s.tables.end()) {
            for (auto& row : it->second) {
                if (auto vit = row.find(column); vit != row.end()) bucket.add(vit->second, row);
            }
        }
        bucket.fill(tree);
        std::string idx_key = table + "_" + index_name;
        s.indexes.insert({idx_key, std::move(tree)});
        s.index_meta.insert({idx_key, {table, column}});
        persist_index_meta(s);
        return StringResult::Ok("Index '" + index_name + "' created on '" + table + "'.'" + column + "'.");
    }

    CompositeIndex comp(table, columns);
    if (const TableSchema* sch = s.catalog.get_table(table)) comp.pk_columns = sch->identity_columns();
    if (auto it = s.tables.find(table); it != s.tables.end()) comp.rebuild(it->second);
    s.composite_indexes.insert({table + "_" + index_name, std::move(comp)});
    persist_index_meta(s);
    std::string cols_joined;
    for (std::size_t i = 0; i < columns.size(); i++) {
        if (i) cols_joined += ", ";
        cols_joined += columns[i];
    }
    return StringResult::Ok("Composite index '" + index_name + "' created on '" + table + "' (" + cols_joined + ").");
}

StringResult Executor::exec_drop_index(SharedDatabase& s, const std::string& index_name, const std::optional<std::string>& table) {
    // Index names are stored as "<table>_<index_name>" (to allow the same index name to be
    // reused across different tables/databases). DROP INDEX name ON table says which table;
    // without it we match by suffix and drop the first match found. An index name does not
    // depend on case (MySQL), so the match ignores it.
    std::string suffix = "_" + index_name;
    auto same_case_free = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };
    auto has_suffix = [&](const std::string& key) {
        if (key.size() < suffix.size()) return false;
        if (!same_case_free(key.substr(key.size() - suffix.size()), suffix)) return false;
        if (!table) return true;
        const std::string wanted = *table + "_" + index_name;
        return same_case_free(key, wanted);
    };

    for (auto it = s.index_meta.begin(); it != s.index_meta.end(); ++it) {
        if (!has_suffix(it->first)) continue;
        std::string idx_key = it->first;
        s.index_meta.erase(it);
        s.indexes.erase(idx_key);
        s.disk.delete_btree_index(idx_key);
        persist_index_meta(s);
        return StringResult::Ok("Index '" + index_name + "' dropped.");
    }
    for (auto it = s.hash_index_meta.begin(); it != s.hash_index_meta.end(); ++it) {
        if (!has_suffix(it->first)) continue;
        std::string idx_key = it->first;
        s.hash_index_meta.erase(it);
        s.hash_indexes.erase(idx_key);
        persist_index_meta(s);
        return StringResult::Ok("Hash index '" + index_name + "' dropped.");
    }
    for (auto it = s.composite_indexes.begin(); it != s.composite_indexes.end(); ++it) {
        if (!has_suffix(it->first)) continue;
        s.composite_indexes.erase(it);
        persist_index_meta(s);
        return StringResult::Ok("Composite index '" + index_name + "' dropped.");
    }
    return StringResult::Ok("Index '" + index_name + "' does not exist, skipped.");
}

StringResult Executor::exec_create_view(SharedDatabase& s, const std::string& name, Statement query, const std::string& raw_sql) {
    if (auto* sel = std::get_if<Statement::Select>(&query.data)) {
        // (a SELECT with no FROM, `SELECT 1 AS a`, reads the one-row table _dual_; a derived table is no table either: a dump makes each view first as `SELECT 1 AS a, 1 AS b`)
        const bool no_table = sel->subquery || sel->table == "_dual_" || (sel->table.size() > 7 && sel->table.compare(sel->table.size() - 7, 7, "._dual_") == 0);
        if (!no_table && !s.tables.count(sel->table) && !s.views.count(sel->table)) return StringResult::Err("Table '" + sel->table + "' not found"); // (a view may read a view)
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
        if (meta.first == table) sec.emplace_back(name, meta.second);
    }
    for (auto& [key, col] : sec) {
        auto vit = row.find(col);
        if (vit == row.end()) continue;
        std::vector<Row> bucket;
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) {
            if (auto j = tit->second.search(vit->second)) {
                try {
                    bucket = rows_from_json(*j);
                } catch (...) {
                }
            }
        }
        bucket.push_back(row);
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) tit->second.insert(vit->second, rows_to_json(bucket));
    }

    std::vector<std::string> hsec;
    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first == table) hsec.push_back(name);
    }
    for (auto& key : hsec) {
        if (auto it = s.hash_indexes.find(key); it != s.hash_indexes.end()) it->second.insert_row(row);
    }
}

void Executor::index_remove_row(SharedDatabase& s, const std::string& table, const Row& row, const std::string& pk_col) {
    auto pk_it = row.find(pk_col);
    if (pk_it == row.end()) return;

    // Composite PRIMARY KEY (a, b): callers hand over only the FIRST key column as `pk_col`, and
    // matching bucket entries on it alone purged every sibling row sharing that column -- one
    // UPDATE/DELETE of (2,4) emptied the secondary-index bucket of (2,1)..(2,5) too, so
    // `WHERE v = ..` silently returned too few rows. Identify the row by its WHOLE key.
    std::vector<std::pair<std::string, std::string>> pk_parts{{pk_col, pk_it->second}};
    if (const TableSchema* sch = s.catalog.get_table(table); sch && sch->primary_key_columns.size() > 1) {
        pk_parts.clear();
        for (auto& c : sch->primary_key_columns) {
            auto cit = row.find(c);
            if (cit == row.end()) return;
            pk_parts.emplace_back(c, cit->second);
        }
    }
    auto same_pk = [&pk_parts](const Row& r) {
        for (auto& [c, v] : pk_parts) {
            auto it = r.find(c);
            if (it == r.end() || it->second != v) return false;
        }
        return true;
    };

    std::vector<std::pair<std::string, std::string>> sec;
    for (auto& [name, meta] : s.index_meta) {
        if (meta.first == table) sec.emplace_back(name, meta.second);
    }
    for (auto& [key, col] : sec) {
        auto vit = row.find(col);
        if (vit == row.end()) continue;
        std::vector<Row> bucket;
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) {
            if (auto j = tit->second.search(vit->second)) {
                try {
                    bucket = rows_from_json(*j);
                } catch (...) {
                }
            }
        }
        std::vector<Row> new_bucket;
        for (auto& r : bucket) {
            if (!same_pk(r)) new_bucket.push_back(r);
        }
        if (auto tit = s.indexes.find(key); tit != s.indexes.end()) tit->second.insert(vit->second, rows_to_json(new_bucket));
    }

    std::vector<std::pair<std::string, std::string>> hsec;
    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first == table) hsec.emplace_back(name, meta.second);
    }
    for (auto& [key, col] : hsec) {
        auto vit = row.find(col);
        if (vit == row.end()) continue;
        if (auto it = s.hash_indexes.find(key); it != s.hash_indexes.end()) it->second.remove_row(vit->second, pk_parts);
    }
}

// Moves a whole statement's worth of rows through the secondary and hash indexes in one go. A
// secondary B+Tree stores each bucket (all rows sharing an indexed value) as ONE JSON array of
// full rows, so the per-row index_remove_row + index_insert_row made a statement touching k rows of
// one bucket parse and rewrite that bucket 2k times -- quadratic (a 400-row UPDATE through a
// 50-value column took 1.2 s at 20,000 rows). Here every affected bucket is parsed once, loses
// all its leaving rows, gains all its entering rows and is written back once. Because removals
// and additions are applied together, a row that moves into a key vacated by another row of the
// same statement (SET id = id + 1) cannot be confused with the row it replaces either.
void Executor::index_replace_rows(SharedDatabase& s, const std::string& table, const std::vector<Row>& olds, const std::vector<Row>& news,
                                  const std::string& pk_col) {
    if (olds.empty() && news.empty()) return;
    const TableSchema* sch = s.catalog.get_table(table);
    // a row's identity inside a bucket: its whole primary key (see index_remove_row)
    std::vector<std::string> id_cols{pk_col};
    if (sch && sch->primary_key_columns.size() > 1) id_cols = sch->primary_key_columns;
    auto identity = [&id_cols](const Row& r, std::string& out) {
        out.clear();
        for (auto& c : id_cols) {
            auto it = r.find(c);
            if (it == r.end()) return false;
            out += it->second;
            out += '\x00';
        }
        return true;
    };
    std::vector<std::string> old_ids(olds.size());
    std::vector<char> old_ok(olds.size(), 0);
    for (std::size_t i = 0; i < olds.size(); i++) old_ok[i] = identity(olds[i], old_ids[i]) ? 1 : 0;

    for (auto& [name, meta] : s.index_meta) {
        if (meta.first != table) continue;
        auto tit = s.indexes.find(name);
        if (tit == s.indexes.end()) continue;
        const std::string& col = meta.second;

        struct Touch {
            std::unordered_set<std::string> leaving;
            std::vector<const Row*> entering;
        };
        std::unordered_map<std::string, Touch> touched;
        for (std::size_t i = 0; i < olds.size(); i++) {
            if (!old_ok[i]) continue;
            auto v = olds[i].find(col);
            if (v != olds[i].end()) touched[v->second].leaving.insert(old_ids[i]);
        }
        for (auto& r : news) {
            auto v = r.find(col);
            if (v != r.end()) touched[v->second].entering.push_back(&r);
        }
        for (auto& [key, t] : touched) {
            std::vector<Row> bucket;
            if (auto j = tit->second.search(key)) {
                try {
                    bucket = rows_from_json(*j);
                } catch (...) {
                }
            }
            if (!t.leaving.empty()) {
                std::vector<Row> kept;
                kept.reserve(bucket.size());
                std::string id;
                for (auto& r : bucket) {
                    if (identity(r, id) && t.leaving.count(id)) continue;
                    kept.push_back(std::move(r));
                }
                bucket = std::move(kept);
            }
            for (const Row* r : t.entering) bucket.push_back(*r);
            tit->second.insert(key, rows_to_json(bucket));
        }
    }

    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first != table) continue;
        auto hit = s.hash_indexes.find(name);
        if (hit == s.hash_indexes.end()) continue;
        for (std::size_t i = 0; i < olds.size(); i++) {
            if (!old_ok[i]) continue;
            auto v = olds[i].find(meta.second);
            if (v == olds[i].end()) continue;
            std::vector<std::pair<std::string, std::string>> parts;
            for (auto& c : id_cols) parts.emplace_back(c, olds[i].find(c)->second);
            hit->second.remove_row(v->second, parts);
        }
        for (auto& r : news) hit->second.insert_row(r);
    }
}

// A table can hold several physical versions of one row (the dead ones stay until VACUUM) and VACUUM moves rows
// around, so a dead version can sit AFTER the live one. Inserting rows in physical order with "the last one wins" then
// left the tree pointing at the dead copy, and every lookup of that row answered "not found" (found by the SELECT
// differential fuzz: after a ROLLBACK, `WHERE id = 84` returned nothing while the scan returned the row). The dead
// versions go in first, then the live ones overwrite them.
std::vector<KeyKind> Executor::key_kinds_of(const TableSchema* schema, const std::vector<std::string>& columns) {
    std::vector<KeyKind> kinds;
    for (auto& name : columns) {
        KeyKind kind = KeyKind::Mixed;
        if (schema) {
            for (auto& c : schema->columns) {
                if (c.name == name) kind = key_kind_of(class_of_type(c.data_type));
            }
        }
        kinds.push_back(kind);
    }
    return kinds;
}

BPlusTree Executor::build_pk_tree(const std::vector<Row>& rows, const std::string& pk_col, std::vector<KeyKind> kinds) {
    BPlusTree tree(std::move(kinds));
    if (pk_col.empty()) return tree;
    for (bool live_pass : {false, true}) {
        for (auto& row : rows) {
            auto x = row.find("_xmax");
            bool live = x == row.end() || x->second == "0";
            if (live != live_pass) continue;
            auto it = row.find(pk_col);
            if (it == row.end()) continue;
            tree.insert(it->second, row_to_json(row));
        }
    }
    return tree;
}

void Executor::rebuild_secondary_indexes(SharedDatabase& s, const std::string& table, const std::vector<Row>& rows) {
    std::vector<std::pair<std::string, std::string>> sec;
    for (auto& [name, meta] : s.index_meta) {
        if (meta.first == table) sec.emplace_back(name, meta.second);
    }
    for (auto& [idx_name, col] : sec) {
        BPlusTree tree(key_kinds_of(s.catalog.get_table(table), {col}));
        IndexBuckets bucket(tree.kinds());
        for (auto& row : rows) {
            if (auto vit = row.find(col); vit != row.end()) bucket.add(vit->second, row);
        }
        bucket.fill(tree);
        s.indexes.insert_or_assign(idx_name, std::move(tree));
    }

    std::vector<std::pair<std::string, std::string>> hsec;
    for (auto& [name, meta] : s.hash_index_meta) {
        if (meta.first == table) hsec.emplace_back(name, meta.second);
    }
    for (auto& [idx_name, col] : hsec) {
        HashIndex hi(table, col);
        hi.rebuild(rows);
        s.hash_indexes.insert_or_assign(idx_name, std::move(hi));
    }
}

void Executor::persist_index_meta(const SharedDatabase& s) const {
    // Live maps are keyed by "<table>_<index_name>" (to allow the same bare index name to be
    // reused across tables/databases), but the on-disk IndexMeta record stores the bare name
    // (files are already split per-database, so no qualification is needed there).
    auto bare_name = [](const std::string& key, const std::string& table) {
        std::string prefix = table + "_";
        return (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0) ? key.substr(prefix.size())
                                                                                           : key;
    };
    std::vector<IndexMeta> meta_list;
    for (auto& [key, meta] : s.index_meta) {
        meta_list.push_back(IndexMeta{bare_name(key, meta.first), meta.first, {meta.second}, "btree"});
    }
    for (auto& [key, meta] : s.hash_index_meta) {
        meta_list.push_back(IndexMeta{bare_name(key, meta.first), meta.first, {meta.second}, "hash"});
    }
    for (auto& [key, comp] : s.composite_indexes) {
        meta_list.push_back(IndexMeta{bare_name(key, comp.table), comp.table, comp.columns, "btree"});
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

StringResult Executor::exec_show_tables(const SharedDatabase& s, const std::optional<std::string>& database, const std::optional<std::string>& like) const {
    const std::string db = database.value_or(current_db);
    std::string prefix = db + ".";
    std::vector<std::string> tables;
    for (auto& [k, _] : s.catalog.tables) {
        if (k.compare(0, prefix.size(), prefix) != 0) continue;
        std::string name = k.substr(prefix.size());
        if (like && !collation::like(name, *like)) continue;
        tables.push_back(std::move(name));
    }
    if (tables.empty() && !like) return StringResult::Ok("No tables found in database '" + db + "'.");
    std::sort(tables.begin(), tables.end());

    // Matches Rust's `{:width$}` semantics: a MINIMUM width, not a fixed/max one — if
    // the content (e.g. the "Tables" header, 6 chars) is longer than max_len (whose
    // floor is 5), Rust prints it at its natural length with no padding, no panic.
    // The naive `width - text.size()` here previously underflowed (both are unsigned)
    // whenever text.size() > width, which is exactly what "Tables" being the longest
    // cell triggers when every actual table name is shorter than 5 chars.
    const std::string header = "Tables_in_" + db;
    std::size_t max_len = header.size();
    for (auto& t : tables) max_len = std::max(max_len, t.size());
    std::string sep = "+" + std::string(max_len + 2, '-') + "+\n";

    auto pad_line = [&](const std::string& text) {
        std::size_t pad = text.size() < max_len ? max_len - text.size() : 0;
        return "| " + text + std::string(pad, ' ') + " |\n";
    };

    std::string output = sep + pad_line(header) + sep;
    for (auto& t : tables) output += pad_line(t);
    output += sep.substr(0, sep.size() - 1); // no trailing newline after the final separator, matching Rust's output.push_str(&sep)
    return StringResult::Ok(output);
}

} // namespace engine
