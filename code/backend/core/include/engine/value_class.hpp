#pragma once

// What kind of value an operand of a comparison is, and how two of them compare.
//
// MySQL compares by the TYPE of the operands, not by what the text looks like: two strings compare as strings (so '10' < '9', and '007' and
// '7' are different), a number and anything else compare as numbers (a string is read by the number it starts with: 'abc' = 0, '12abc' = 12).
// Values are stored as text, so the type has to come from outside: a column's declared type, a literal's quotes, an expression's operators.
// ValueClass::Unknown is an operand whose type cannot be told (a derived column, a function with no fixed result type): two such operands
// compare the way the engine always did -- as numbers when both are numbers, else as text.

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include "engine/numeric_text.hpp"

namespace engine {

enum class ValueClass : std::uint8_t { Unknown = 0, Number, Text };

// What a scalar function gives: text or a number. COALESCE, IF, GREATEST ... give what their arguments are: not told here.
inline ValueClass function_result_class(std::string_view name) {
    static const std::unordered_set<std::string> text = {
        "UPPER", "LOWER", "UCASE", "LCASE", "CONCAT", "CONCAT_WS", "SUBSTRING", "SUBSTR", "TRIM", "LTRIM", "RTRIM", "LEFT", "RIGHT", "REPLACE",
        "REVERSE", "REPEAT", "LPAD", "RPAD", "DATE_FORMAT", "MD5", "SHA1", "SHA2", "HEX", "UNHEX", "CHAR", "FORMAT", "MONTHNAME", "DAYNAME",
        "DATABASE", "SCHEMA", "USER", "VERSION", "SPACE", "NOW", "CURDATE", "CURRENT_DATE", "CURRENT_TIMESTAMP", "DATE", "TIME", "DATE_ADD",
        "DATE_SUB", "LAST_DAY", "STR_TO_DATE", "FROM_UNIXTIME", "JSON_UNQUOTE", "REGEXP_REPLACE", "REGEXP_SUBSTR"};
    static const std::unordered_set<std::string> number = {
        "LENGTH", "CHAR_LENGTH", "CHARACTER_LENGTH", "BIT_LENGTH", "ASCII", "ABS", "CEIL", "CEILING", "FLOOR", "ROUND", "TRUNCATE", "MOD", "POW",
        "POWER", "SQRT", "EXP", "LOG", "LOG2", "LOG10", "SIN", "COS", "TAN", "SIGN", "PI", "RAND", "INSTR", "LOCATE", "POSITION", "FIELD",
        "DATEDIFF", "YEAR", "MONTH", "DAY", "DAYOFMONTH", "DAYOFWEEK", "DAYOFYEAR", "WEEKDAY", "WEEK", "HOUR", "MINUTE", "SECOND",
        "UNIX_TIMESTAMP", "TIMESTAMPDIFF", "FIND_IN_SET", "REGEXP_LIKE", "ISNULL", "STRCMP"};
    std::string key(name);
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (text.count(key)) return ValueClass::Text;
    if (number.count(key)) return ValueClass::Number;
    return ValueClass::Unknown;
}

// Three-way comparison of two values that are not NULL: -1, 0 or 1.
inline int compare_classed(ValueClass a, ValueClass b, std::string_view x, std::string_view y) {
    if (a == ValueClass::Number || b == ValueClass::Number) return compare_numbers(x, y);
    if (!(a == ValueClass::Text && b == ValueClass::Text) && parse_number(x) && parse_number(y)) return compare_numbers(x, y);
    const int c = x.compare(y);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

} // namespace engine
