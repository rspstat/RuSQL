#pragma once

// The SELECTs that answer the SHOW statements which read information_schema (R83, R84), and the filter of their LIKE / WHERE. The MySQL protocol layer and the native
// protocol both answer SHOW TRIGGERS, SHOW PROCEDURE STATUS and SHOW FUNCTION STATUS with them, so the two cannot drift apart.

#include <cctype>
#include <string>

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

} // namespace engine::show_sql
