#pragma once

// A row's JSON text without building an intermediate nlohmann::json object.
//
// Every stored row image -- the table file, the PK B+Tree, every secondary-index bucket, the redo/undo/WAL records --
// is `nlohmann::json(row).dump()`. For a 4-column row that is a std::map of json values (about a dozen allocations)
// plus the dump, ~3 us; this writes the same bytes directly. The output is IDENTICAL to nlohmann's: keys sorted the way
// std::map sorts them, no whitespace, the same escapes, and the same exception for a string that is not valid UTF-8
// (anything unusual is handed to nlohmann itself, so the two cannot drift apart).
//
// The other direction: `nlohmann::json::parse(text).get<Row>()` builds a json tree (a std::map of json values, the same
// dozen allocations) only to copy it into a Row -- ~10 us per 4-column row, the bulk of loading a table file at startup.

#include <string>
#include <string_view>
#include <vector>

#include "engine/row.hpp"

namespace engine {

void append_row_json(std::string& out, const Row& row);
std::string row_to_json(const Row& row);
// nlohmann::json(rows).dump(): a JSON array of the rows' objects
std::string rows_to_json(const std::vector<Row>& rows);

// Same result as nlohmann::json::parse(text).get<Row>() (and the same exceptions for text that is not an object of
// strings): a flat object of strings with strictly increasing keys, which is all this engine writes, is read directly;
// everything else -- other value types, unsorted or repeated keys, an escape or UTF-8 sequence nlohmann would reject --
// goes to nlohmann itself, so the two cannot drift apart.
Row row_from_json(std::string_view text);
// ... and nlohmann::json::parse(text).get<std::vector<Row>>()
std::vector<Row> rows_from_json(std::string_view text);

} // namespace engine
