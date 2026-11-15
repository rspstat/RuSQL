#pragma once

// The SELECTs that answer the SHOW statements which read information_schema (R83, R84), and the filter of their LIKE / WHERE. The MySQL protocol layer and the native
// protocol both answer SHOW TRIGGERS, SHOW PROCEDURE STATUS and SHOW FUNCTION STATUS with them, so the two cannot drift apart.

#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace engine::show_sql {

inline std::string quoted(const std::string& text) {
    std::string out = "'";
    for (char c : text) out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}

inline std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// SHOW PROCEDURE STATUS / SHOW FUNCTION STATUS: the routines of every database, with the columns of MySQL's answer
inline std::string routine_status_select(bool function) {
    return std::string("SELECT ROUTINE_SCHEMA AS `Db`, ROUTINE_NAME AS `Name`, ROUTINE_TYPE AS `Type`, EXTERNAL_LANGUAGE AS `Language`, DEFINER AS `Definer`, "
                       "LAST_ALTERED AS `Modified`, CREATED AS `Created`, SECURITY_TYPE AS `Security_type`, ROUTINE_COMMENT AS `Comment`, "
                       "CHARACTER_SET_CLIENT AS `character_set_client`, COLLATION_CONNECTION AS `collation_connection`, DATABASE_COLLATION AS `Database Collation` "
                       "FROM information_schema.routines WHERE ROUTINE_TYPE = ") +
           (function ? "'FUNCTION'" : "'PROCEDURE'");
}

// SHOW TRIGGERS [FROM db]: the triggers of a database (the current one when `database` is empty)
inline std::string triggers_select(const std::string& database) {
    return std::string("SELECT TRIGGER_NAME AS `Trigger`, EVENT_MANIPULATION AS `Event`, EVENT_OBJECT_TABLE AS `Table`, ACTION_STATEMENT AS `Statement`, "
                       "ACTION_TIMING AS `Timing`, CREATED AS `Created`, SQL_MODE AS `sql_mode`, DEFINER AS `Definer`, CHARACTER_SET_CLIENT AS `character_set_client`, "
                       "COLLATION_CONNECTION AS `collation_connection`, DATABASE_COLLATION AS `Database Collation` FROM information_schema.triggers WHERE TRIGGER_SCHEMA = ") +
           (database.empty() ? std::string("DATABASE()") : quoted(database));
}

// SHOW ... [LIKE 'pattern' | WHERE condition]: the SELECT that gives the rows of `select` (written with the column names SHOW has) that what follows the statement's name
// asks for; `like_column` is the column a LIKE is matched against
inline std::string filtered(const std::string& select, const std::string& tail, const std::string& like_column) {
    const std::string up_tail = upper(tail);
    auto trimmed = [](std::string text) {
        const std::size_t from = text.find_first_not_of(" \t\r\n");
        if (from == std::string::npos) return std::string();
        return text.substr(from, text.find_last_not_of(" \t\r\n") - from + 1);
    };
    if (up_tail.rfind("WHERE ", 0) == 0) return "SELECT * FROM (" + select + ") AS shown WHERE " + tail.substr(6);
    if (up_tail.rfind("LIKE ", 0) == 0) return "SELECT * FROM (" + select + ") AS shown WHERE `" + like_column + "` LIKE " + trimmed(tail.substr(5));
    return select;
}

// SHOW DATABASES | SCHEMAS [LIKE 'pattern' | WHERE condition] (R89): the SELECT that answers it, over `names` (the schemas the account may see). LIKE names the column
// `Database (pattern)` as MySQL does and WHERE keeps `Database`; what follows the statement's name must be one string after LIKE or a condition after WHERE (nothing: all the
// names), anything else is not this statement (nullopt: the parser says what is wrong with it).
inline std::optional<std::string> databases_query(const std::vector<std::string>& names, const std::string& tail) {
    // (the rows of information_schema.schemata that are among the names the account may see: a table of the engine, so that a column that is not there is an error in WHERE)
    std::string select = "SELECT schema_name AS `Database` FROM information_schema.schemata WHERE ";
    if (names.empty()) {
        select += "1 = 0";
    } else {
        select += "schema_name IN (";
        for (std::size_t i = 0; i < names.size(); i++) select += (i ? ", " : "") + quoted(names[i]);
        select += ")";
    }
    const char* const blanks = " \t\r\n";
    const std::size_t from = tail.find_first_not_of(blanks);
    if (from == std::string::npos) return select + " ORDER BY `Database`";
    const std::string rest = tail.substr(from, tail.find_last_not_of(blanks) - from + 1);
    const std::string up = upper(rest);
    if (up.rfind("WHERE ", 0) == 0) return "SELECT * FROM (" + select + ") AS shown WHERE " + rest.substr(6) + " ORDER BY `Database`";
    if (up.rfind("LIKE ", 0) != 0) return std::nullopt;
    const std::size_t start = rest.find_first_not_of(" \t", 5);
    if (start == std::string::npos || (rest[start] != '\'' && rest[start] != '"')) return std::nullopt;
    const char quote = rest[start];
    std::string value; // (the string the way MySQL reads it: \% and \_ keep their backslash, the other escapes are the character)
    std::size_t i = start + 1;
    for (; i < rest.size(); i++) {
        const char c = rest[i];
        if (c == quote) {
            if (i + 1 < rest.size() && rest[i + 1] == quote) {
                value += quote;
                i++;
                continue;
            }
            break;
        }
        if (c == '\\' && i + 1 < rest.size()) {
            const char e = rest[++i];
            switch (e) {
                case '0': value += '\0'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'b': value += '\b'; break;
                case 'Z': value += '\x1a'; break;
                case '%':
                case '_': value += '\\'; value += e; break;
                default: value += e;
            }
            continue;
        }
        value += c;
    }
    if (i >= rest.size() || rest.find_first_not_of(" \t", i + 1) != std::string::npos) return std::nullopt; // (no closing quote, or something after the string)
    const std::string heading = "Database (" + value + ")";
    std::string alias;
    for (char c : heading) alias += c == '`' ? std::string("``") : std::string(1, c);
    return "SELECT `Database` AS `" + alias + "` FROM (" + select + ") AS shown WHERE `Database` LIKE " + rest.substr(start, i + 1 - start) + " ORDER BY `Database`";
}

} // namespace engine::show_sql
