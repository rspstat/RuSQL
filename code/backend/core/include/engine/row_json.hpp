#pragma once

// A row's JSON text without building an intermediate nlohmann::json object.
//
// Every stored row image -- the table file, the PK B+Tree, every secondary-index bucket, the redo/undo/WAL records --
// is `nlohmann::json(row).dump()`. For a 4-column row that is a std::map of json values (about a dozen allocations)
// plus the dump, ~3 us; this writes the same bytes directly. The output is IDENTICAL to nlohmann's: keys sorted the way
// std::map sorts them, no whitespace, the same escapes, and the same exception for a string that is not valid UTF-8
// (anything unusual is handed to nlohmann itself, so the two cannot drift apart).

#include <string>
#include <vector>

#include "engine/row.hpp"

namespace engine {

void append_row_json(std::string& out, const Row& row);
std::string row_to_json(const Row& row);
// nlohmann::json(rows).dump(): a JSON array of the rows' objects
std::string rows_to_json(const std::vector<Row>& rows);

} // namespace engine
