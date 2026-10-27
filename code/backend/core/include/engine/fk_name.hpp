#pragma once

// The name a foreign key is reported under (SHOW CREATE TABLE, information_schema, ALTER TABLE ... DROP FOREIGN KEY): the one it was given, else MySQL's
// <table>_ibfk_<n>, n counting the foreign keys of the table from 1 in the order of their columns (and, within a column, in the order they were declared).

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

} // namespace engine
