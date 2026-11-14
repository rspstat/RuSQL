#pragma once

// The CHECK constraints of a table with the names they have (R83): the checks written on a column come first (in the order of the columns), then the table's own; one that was
// not given a name is called <table>_chk_<n>, counting the unnamed ones (MySQL 8.4); the list is in the order of the names, as SHOW CREATE TABLE and information_schema list them.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "engine/catalog/schema.hpp"

namespace engine {

inline std::vector<std::pair<std::string, std::string>> check_constraints_of(const TableSchema& schema, const std::string& table) {
    std::vector<std::pair<std::string, std::string>> out; // (name, the condition as it was written)
    std::size_t number = 0;
    for (const ColumnDef& col : schema.columns) {
        if (col.check_expr) out.emplace_back(table + "_chk_" + std::to_string(++number), *col.check_expr);
    }
    for (const CheckConstraint& cc : schema.check_constraints) {
        if (cc.name) out.emplace_back(*cc.name, cc.expression);
        else out.emplace_back(table + "_chk_" + std::to_string(++number), cc.expression);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace engine
