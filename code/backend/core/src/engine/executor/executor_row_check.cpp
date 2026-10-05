// Constraints a row has to satisfy besides PRIMARY KEY / UNIQUE (executor_update_unique.cpp) when a statement REWRITES it
// (UPDATE, INSERT ... ON DUPLICATE KEY UPDATE, MERGE, multi-table UPDATE): NOT NULL and the child side of a FOREIGN KEY.
// INSERT has always checked both; UPDATE checked neither, so `UPDATE t SET not_null_col = NULL` and
// `UPDATE child SET parent_id = 99` (no such parent) were accepted and left rows no INSERT could have created.

#include <algorithm>
#include <map>

#include "engine/executor/executor.hpp"

namespace engine {

std::vector<std::string> Executor::fk_parent_tables(const SharedDatabase& s, const TableSchema& schema, const std::vector<std::string>& cols) {
    std::vector<std::string> out;
    for (auto& col : schema.columns) {
        if (!col.foreign_key) continue;
        if (!cols.empty() && std::find(cols.begin(), cols.end(), col.name) == cols.end()) continue;
        // Table partitioning: the rows of a partitioned parent live in its children (its own entry is an empty phantom)
        if (auto part_info = partition_info_for(s, col.foreign_key->ref_table)) {
            for (auto& def : part_info->partitions) out.push_back(def.child_table);
        } else {
            out.push_back(col.foreign_key->ref_table);
        }
    }
    return out;
}

std::optional<std::string> Executor::fk_child_violation(SharedDatabase& s, const ColumnDef& col, const std::string& val) {
    if (!col.foreign_key || val == EXECUTOR_NULL_VALUE) return std::nullopt;
    std::vector<std::pair<std::string, const std::vector<Row>*>> ref_row_sets;
    if (auto part_info = partition_info_for(s, col.foreign_key->ref_table)) {
        for (auto& def : part_info->partitions) {
            if (auto child_it = s.tables.find(def.child_table); child_it != s.tables.end()) ref_row_sets.emplace_back(def.child_table, &child_it->second);
        }
    } else {
        auto ref_it = s.tables.find(col.foreign_key->ref_table);
        if (ref_it == s.tables.end()) return "Referenced table '" + col.foreign_key->ref_table + "' not found";
        ref_row_sets.emplace_back(col.foreign_key->ref_table, &ref_it->second);
    }
    auto live = [](const Row& r) { return Executor::is_visible(r); }; // a deleted parent row is no parent
    bool exists = std::any_of(ref_row_sets.begin(), ref_row_sets.end(), [&](const auto& named_rows) {
        return index_or_scan_exists(s, named_rows.first, *named_rows.second, col.foreign_key->ref_column, val, live);
    });
    if (exists) return std::nullopt;
    return "Foreign key violation: '" + val + "' not found in '" + col.foreign_key->ref_table + "'.'" + col.foreign_key->ref_column + "'";
}

std::optional<std::string> Executor::rewritten_row_violation(SharedDatabase& s, const TableSchema& schema, const Row& row,
                                                             const std::vector<std::string>& changed_cols, const Row* old_row) {
    for (auto& name : changed_cols) {
        auto cit = std::find_if(schema.columns.begin(), schema.columns.end(), [&](const ColumnDef& c) { return c.name == name; });
        if (cit == schema.columns.end()) continue;
        auto vit = row.find(name);
        const std::string val = vit != row.end() ? vit->second : std::string(EXECUTOR_NULL_VALUE);
        if (val == EXECUTOR_NULL_VALUE && (cit->not_null || cit->primary_key || cit->auto_increment)) return "Column '" + name + "' cannot be NULL";
    }
    for (auto& name : changed_cols) {
        auto cit = std::find_if(schema.columns.begin(), schema.columns.end(), [&](const ColumnDef& c) { return c.name == name; });
        if (cit == schema.columns.end() || !cit->foreign_key) continue;
        auto vit = row.find(name);
        if (vit == row.end()) continue;
        if (old_row) { // a value the statement leaves as it was is not checked again
            auto oit = old_row->find(name);
            if (oit != old_row->end() && oit->second == vit->second) continue;
        }
        if (auto violation = fk_child_violation(s, *cit, vit->second)) return violation;
    }
    return std::nullopt;
}

std::optional<std::string> Executor::delete_restrict_violation(SharedDatabase& s, const std::string& table, const Row& row) {
    for (auto& [other_table, schema] : s.catalog.tables) {
        if (other_table == table) continue;
        for (auto& col : schema.columns) {
            if (!col.foreign_key || col.foreign_key->ref_table != table || col.foreign_key->on_delete != FkAction::Restrict) continue;
            auto dit = row.find(col.foreign_key->ref_column);
            const std::string del_val = dit != row.end() ? dit->second : std::string();
            if (auto oit = s.tables.find(other_table); oit != s.tables.end()) {
                if (index_or_scan_exists(s, other_table, oit->second, col.name, del_val, [](const Row& r) { return Executor::is_visible(r); })) {
                    return "Foreign key violation: row in '" + table + "' is referenced by '" + other_table + "'.'" + col.name + "'";
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> Executor::fk_restrict_children(const SharedDatabase& s, const std::string& table, const std::vector<std::string>& changed_cols) {
    std::vector<std::string> out;
    for (auto& [other_table, schema] : s.catalog.tables) {
        for (auto& col : schema.columns) {
            if (!col.foreign_key || col.foreign_key->ref_table != table || col.foreign_key->on_update != FkAction::Restrict) continue;
            if (std::find(changed_cols.begin(), changed_cols.end(), col.foreign_key->ref_column) != changed_cols.end()) {
                out.push_back(other_table);
                break;
            }
        }
    }
    return out;
}

std::optional<std::string> Executor::update_restrict_violation(SharedDatabase& s, const std::string& table, const Row& old_row, const Row& new_row,
                                                               const std::vector<std::string>& changed_cols) {
    for (auto& [other_table, schema] : s.catalog.tables) {
        for (auto& col : schema.columns) {
            if (!col.foreign_key || col.foreign_key->ref_table != table || col.foreign_key->on_update != FkAction::Restrict) continue;
            const std::string& ref_col = col.foreign_key->ref_column;
            if (std::find(changed_cols.begin(), changed_cols.end(), ref_col) == changed_cols.end()) continue;
            auto oit = old_row.find(ref_col);
            auto nit = new_row.find(ref_col);
            const std::string old_val = oit != old_row.end() ? oit->second : std::string();
            const std::string new_val = nit != new_row.end() ? nit->second : std::string();
            if (old_val == new_val) continue;
            if (auto tit = s.tables.find(other_table); tit != s.tables.end()) {
                if (index_or_scan_exists(s, other_table, tit->second, col.name, old_val, [](const Row& r) { return Executor::is_visible(r); })) {
                    return "Foreign key violation (ON UPDATE RESTRICT): '" + ref_col + "' is referenced by '" + other_table + "'.'" + col.name + "'";
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> Executor::fk_child_tables(const SharedDatabase& s, const std::string& table) {
    std::vector<std::string> out;
    for (auto& [other_table, schema] : s.catalog.tables) {
        if (other_table == table) continue;
        for (auto& col : schema.columns) {
            if (col.foreign_key && col.foreign_key->ref_table == table) {
                out.push_back(other_table);
                break;
            }
        }
    }
    return out;
}

Executor::DataLockGuard Executor::acquire_table_data_locks_mixed(SharedDatabase& s, const std::vector<std::string>& exclusive,
                                                                 const std::vector<std::string>& shared_tables) {
    std::map<std::string, bool> mode; // table -> exclusive, in the sorted order every other acquisition uses
    for (auto& t : shared_tables) mode.emplace(t, false);
    for (auto& t : exclusive) mode[t] = true;
    DataLockGuard guard;
    for (auto& [t, ex] : mode) {
        auto it = s.table_data_locks.find(t);
        if (it == s.table_data_locks.end()) continue;
        FairSharedMutex& m = *it->second;
        if (ex) guard.exclusive_locks.emplace_back(m);
        else guard.shared_locks.emplace_back(m);
    }
    return guard;
}

} // namespace engine
