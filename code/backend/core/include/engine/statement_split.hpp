#pragma once

// Where the statements of a script or of a connection's input end. A ';' ends a statement unless it is inside a comment, a string, a quoted name or
// the body of a stored procedure / trigger / function (BEGIN ... END), where the statements of the body end with ';' too. The server, the CLI and the
// client all read their input with this one function; they used to carry three copies that had to agree to the byte (a statement the client counted
// differently from the server made it wait for an answer that never came).
//
// BEGIN and END are also words a column may be called (`CREATE TABLE t (begin INT, end INT)`, `SELECT end FROM t`), and END also closes a CASE
// expression (`CASE WHEN x THEN 1 END`), so a BEGIN or END is only a block's when it is not used as a name and, for END, no CASE is open.

#include <cctype>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>

namespace engine {

namespace split_detail {

inline bool word_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
inline bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

inline std::string upper_case(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// the position after the whitespace and comments that start at `i`
inline std::size_t skip_blank(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            i++;
        } else if (s[i] == '#' || (s[i] == '-' && i + 1 < n && s[i + 1] == '-')) {
            while (i < n && s[i] != '\n') i++;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = i + 2 <= n ? i + 2 : n;
        } else {
            break;
        }
    }
    return i;
}

// the word that starts at `i`, in capitals; empty when none does
inline std::string word_at(const std::string& s, std::size_t i) {
    if (i >= s.size() || !word_start(s[i])) return std::string();
    std::size_t j = i;
    while (j < s.size() && word_char(s[j])) j++;
    return upper_case(s.substr(i, j - i));
}

inline bool one_of(const std::string& word, std::initializer_list<const char*> words) {
    for (const char* w : words) {
        if (word == w) return true;
    }
    return false;
}

// BEGIN or END used as the name of a column rather than as the keyword of a block: after a ',' '(' '.' or an operator, after a word that is followed by
// a name (SELECT, WHERE, BY, AS ...), or -- when `look_ahead` -- before what follows a name (',' ')' '.' an operator, FROM, AS, a type, ...)
inline bool used_as_name(char prev, const std::string& prev_word, const std::string& s, std::size_t after, bool look_ahead = true) {
    if (prev_word.empty() && prev != '\0' && std::strchr(",(.=<>!+-*/%", prev)) return true;
    if (one_of(prev_word, {"SELECT", "DISTINCT", "BY", "WHERE", "AND", "OR", "NOT", "SET", "ON", "HAVING", "WHEN", "AS", "BETWEEN", "IN", "LIKE", "IS"})) return true;
    if (!look_ahead) return false;
    const std::size_t j = skip_blank(s, after);
    if (j >= s.size()) return false;
    if (s[j] != '\0' && std::strchr(",).=<>!+-*/%", s[j])) return true;
    return one_of(word_at(s, j), {"FROM", "AS", "ASC", "DESC", "IS", "IN", "BETWEEN", "LIKE", "AND", "OR", "NOT", "THEN", "INT", "INTEGER", "BIGINT", "SMALLINT",
                                  "TINYINT", "MEDIUMINT", "VARCHAR", "CHAR", "TEXT", "DATE", "DATETIME", "TIMESTAMP", "TIME", "YEAR", "DECIMAL", "NUMERIC",
                                  "FLOAT", "DOUBLE", "REAL", "BOOLEAN", "BOOL", "ENUM", "JSON", "BLOB"});
}

// Calls `on_end(index)` for every ';' that ends a statement, in order, until it returns true.
template <class OnEnd>
inline void scan(const std::string& s, OnEnd&& on_end) {
    const std::size_t n = s.size();
    int begin_depth = 0, case_depth = 0;
    char prev = '\0';        // the last character that was not blank or a comment
    std::string prev_word;   // ... and the word it ended, in capitals ("" when it was not part of a word)
    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            i++;
            continue;
        }
        if (c == '#' || (c == '-' && i + 1 < n && s[i + 1] == '-') || (c == '/' && i + 1 < n && s[i + 1] == '*')) {
            i = skip_blank(s, i);
            continue;
        }
        if (c == '\'') {
            i++;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    i += 2;
                } else if (s[i] == '\'') {
                    i++;
                    break;
                } else {
                    i++;
                }
            }
            prev = '\'';
            prev_word.clear();
            continue;
        }
        if (c == '"' || c == '`') {
            i++;
            while (i < n && s[i] != c) i++;
            if (i < n) i++;
            prev = c;
            prev_word.clear();
            continue;
        }
        if (word_start(c)) {
            const std::size_t start = i;
            while (i < n && word_char(s[i])) i++;
            const std::string word = upper_case(s.substr(start, i - start));
            if (word == "BEGIN") {
                if (!used_as_name(prev, prev_word, s, i)) {
                    // A transaction `BEGIN;` / `BEGIN WORK;` has no matching END -- only the BEGIN of a procedure / trigger body has.
                    const std::size_t j = skip_blank(s, i);
                    const bool transaction = j >= n || s[j] == ';' || word_at(s, j) == "WORK";
                    if (!transaction) begin_depth++;
                }
            } else if (word == "CASE") {
                if (prev_word != "END") case_depth++; // (the CASE of END CASE closes the one that was opened)
            } else if (word == "END") {
                // (in a CASE what follows an END -- ',' ')' an operator -- is no sign of a name: `CASE ... END, x`; and an END right after THEN or ELSE
                // is one: a branch needs something to give)
                if (prev_word != "THEN" && prev_word != "ELSE" && !used_as_name(prev, prev_word, s, i, case_depth == 0)) {
                    const std::string next = word_at(s, skip_blank(s, i));
                    if (next == "CASE") {
                        if (case_depth > 0) case_depth--;
                    } else if (one_of(next, {"IF", "WHILE", "LOOP", "REPEAT"})) {
                        // (the end of a statement inside the block)
                    } else if (case_depth > 0) {
                        case_depth--; // the END of a CASE expression
                    } else if (begin_depth > 0) {
                        begin_depth--;
                    }
                }
            }
            prev = s[i - 1];
            prev_word = word;
            continue;
        }
        if (c == ';' && begin_depth == 0) {
            case_depth = 0;
            if (on_end(i)) return;
        }
        prev = c;
        prev_word.clear();
        i++;
    }
}

} // namespace split_detail

// The byte offset of the first ';' that ends a statement; nothing when the input holds no complete statement yet (an unclosed BEGIN ... END body spanning
// several lines: the caller keeps reading before it asks again).
inline std::optional<std::size_t> find_statement_end(const std::string& input) {
    std::optional<std::size_t> end;
    split_detail::scan(input, [&](std::size_t at) {
        end = at;
        return true;
    });
    return end;
}

// How many statements end in the input (the ';' that end them).
inline std::size_t count_statement_ends(const std::string& input) {
    std::size_t count = 0;
    split_detail::scan(input, [&](std::size_t) {
        count++;
        return false;
    });
    return count;
}

} // namespace engine
