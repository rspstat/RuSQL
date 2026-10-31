#pragma once

// The columns of a view as information_schema.columns lists them (executor_result_types.cpp, where a select list's column types are worked out)

#include "engine/executor/executor.hpp"

#include <string>
#include <vector>

namespace engine {

std::vector<ColumnDef> view_column_defs(const SharedDatabase& s, const std::string& view_name, const Statement& body);

} // namespace engine
