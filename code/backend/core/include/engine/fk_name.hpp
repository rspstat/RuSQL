#pragma once

// The name a foreign key is reported under (SHOW CREATE TABLE, information_schema, ALTER TABLE ... DROP FOREIGN KEY): the one it was given, else MySQL's
// <table>_ibfk_<n>. CREATE TABLE numbers the keys it was not given a name for 1, 2, ... in the order they are written; ALTER TABLE ... ADD FOREIGN KEY takes the number
// after the highest <table>_ibfk_<n> the table has (a dropped key leaves a gap, and the other keys keep their names). The name is kept in the key (ForeignKey::name);
// only a schema written before that has keys with no name, which are numbered by the order of their columns as they used to be.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

#include "engine/catalog/schema.hpp"

namespace engine {

inline std::string fk_constraint_name(const TableSchema& schema, const std::string& bare_table, std::size_t column_index, std::size_t fk_index = 0) {
    const ForeignKey& fk = schema.columns[column_index].foreign_keys[fk_index];
    if (!fk.name.empty()) return fk.name;
    std::size_t n = 0;
    for (std::size_t i = 0; i < column_index; i++) n += schema.columns[i].foreign_keys.size();
    return bare_table + "_ibfk_" + std::to_string(n + fk_index + 1);
}

// n of `<bare_table>_ibfk_<n>` (the table's name in any letter case), 0 for any other name
inline std::size_t fk_auto_number(const std::string& name, const std::string& bare_table) {
    const std::string tail = "_ibfk_";
    if (name.size() <= bare_table.size() + tail.size()) return 0;
    for (std::size_t i = 0; i < bare_table.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(bare_table[i]))) return 0;
    }
    if (name.compare(bare_table.size(), tail.size(), tail) != 0) return 0;
    const std::string digits = name.substr(bare_table.size() + tail.size());
    if (digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return 0;
    return static_cast<std::size_t>(std::stoul(digits));
}

// the keys with no name get the one they are reported under (so that dropping another key does not change it from then on)
inline void fk_materialize_names(TableSchema& schema, const std::string& bare_table) {
    for (std::size_t i = 0; i < schema.columns.size(); i++) {
        for (std::size_t k = 0; k < schema.columns[i].foreign_keys.size(); k++) {
            const std::string name = fk_constraint_name(schema, bare_table, i, k);
            schema.columns[i].foreign_keys[k].name = name;
        }
    }
}

// the name of a key added to a table, when none is given: one more than the highest <table>_ibfk_<n> it has
inline std::string fk_next_name(TableSchema& schema, const std::string& bare_table) {
    fk_materialize_names(schema, bare_table);
    std::size_t highest = 0;
    for (const ColumnDef& c : schema.columns) {
        for (const ForeignKey& fk : c.foreign_keys) highest = std::max(highest, fk_auto_number(fk.name, bare_table));
    }
    return bare_table + "_ibfk_" + std::to_string(highest + 1);
}

// RENAME TABLE: the keys named <old>_ibfk_<n> are named <new>_ibfk_<n> (a key given another name keeps it)
inline void fk_rename_table(TableSchema& schema, const std::string& old_bare, const std::string& new_bare) {
    fk_materialize_names(schema, old_bare);
    for (ColumnDef& c : schema.columns) {
        for (ForeignKey& fk : c.foreign_keys) {
            if (const std::size_t n = fk_auto_number(fk.name, old_bare)) fk.name = new_bare + "_ibfk_" + std::to_string(n);
        }
    }
}

} // namespace engine
