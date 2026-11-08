#pragma once

// The built-in scalar functions, written to MySQL's rules (see sql_value_util.hpp): strings counted by character, exact decimal rounding, calendar
// arithmetic. One call is described by BuiltinCall; a function that is not in the table here is looked up in the older dispatcher
// (Executor::apply_scalar_func), and one that is in neither is an error (MySQL 1305), not a text of its name.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace engine {

struct BuiltinCall {
    std::string name;                                 // upper case
    const std::vector<std::string>& raw;              // the arguments as written (a unit such as MONTH is read from here; a literal string keeps its quotes)
    std::function<std::string(std::size_t)> value;    // the value of argument i ("NULL" for NULL)
    std::string db;                                   // the current database
    // what each argument is when it is known: 'S' a string written in quotes, 'N' a number written out, 'C' a column, 'D' a column MySQL reads as a
    // DOUBLE (FLOAT, DOUBLE, a string) or an expression done in floating point, 'E' the value of another expression (empty: not known, the quotes of
    // `raw` say)
    std::vector<char> kinds;
};

// The result of the call, or nullopt when `name` is not a function of this table. Throws StatementError for a wrong argument count (1582).
std::optional<std::string> call_builtin_function(const BuiltinCall& call);

// Is `name` (upper case) a function this table or the older dispatcher knows?
bool is_known_function_name(const std::string& name);

// Is `name` (upper case) in this table?
bool is_builtin_function(const std::string& name);

// A JSON text in MySQL's form (members ordered, `, ` and `: ` between, numbers as MySQL keeps them), or nullopt when the text is no JSON document;
// `why`, when given, then says why in MySQL's words ("Missing a comma or ']' after an array element." at position 4).
std::optional<std::string> json_canonical_text(const std::string& text, std::string* why = nullptr);

// JSON_ARRAYAGG / JSON_OBJECTAGG: `rows` holds, for every row, the text of JSON_ARRAY(x) (an array) or of JSON_ARRAY(key, value) (an object: a key that comes
// twice keeps the last value; a NULL key is an error).
std::string json_aggregate_text(bool object, const std::vector<std::string>& rows);

} // namespace engine
