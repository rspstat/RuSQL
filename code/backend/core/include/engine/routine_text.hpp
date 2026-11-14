#pragma once

// What a procedure or function was written as (R83): the text after its name, taken apart again. SHOW CREATE PROCEDURE / FUNCTION lays it out the way MySQL does (the
// parameters, RETURNS, one characteristic to a line, the body) and information_schema.routines gives the body and the characteristics.

#include <cctype>
#include <string>
#include <vector>

namespace engine {

struct RoutineParts {
    std::string params;                       // what stands between the brackets of the parameter list, as written
    std::string returns;                      // a function: the type after RETURNS, as written
    std::vector<std::string> characteristics; // DETERMINISTIC, NOT DETERMINISTIC, NO SQL, READS SQL DATA, COMMENT '...', SQL SECURITY INVOKER ... each as written
    std::string body;                         // BEGIN ... END, RETURN <expression> or the one statement, as written
};

inline RoutineParts split_routine_source(const std::string& text, bool is_function) {
    RoutineParts parts;
    const std::size_t n = text.size();
    std::size_t i = 0;
    auto is_word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    auto skip_space = [&] {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) i++;
    };
    // the end (one past the closing quote) of the quoted text that starts at `at`
    auto skip_quoted = [&](std::size_t at) {
        const char quote = text[at];
        std::size_t j = at + 1;
        while (j < n) {
            if (text[j] == '\\' && quote != '`' && j + 1 < n) j += 2;
            else if (text[j] == quote) {
                if (j + 1 < n && text[j + 1] == quote) j += 2;
                else return j + 1;
            } else j++;
        }
        return n;
    };
    auto upper_word_at = [&](std::size_t at) {
        std::string w;
        while (at < n && is_word_char(text[at])) w.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(text[at++]))));
        return w;
    };
    auto after_word = [&](std::size_t at) {
        while (at < n && is_word_char(text[at])) at++;
        return at;
    };

    skip_space();
    if (i < n && text[i] == '(') { // the parameter list
        int depth = 0;
        std::size_t j = i;
        for (; j < n; j++) {
            if (text[j] == '\'' || text[j] == '"' || text[j] == '`') { j = skip_quoted(j) - 1; continue; }
            if (text[j] == '(') depth++;
            else if (text[j] == ')' && --depth == 0) break;
        }
        parts.params = text.substr(i + 1, j > i ? j - i - 1 : 0);
        i = j < n ? j + 1 : n;
    }
    auto characteristic_at = [&](std::size_t at) -> std::size_t { // the end of the characteristic that starts at `at`, or `at` when none does
        const std::string w1 = upper_word_at(at);
        if (w1.empty()) return at;
        std::size_t e1 = after_word(at);
        auto next_word = [&](std::size_t from, std::string& word) {
            std::size_t j = from;
            while (j < n && std::isspace(static_cast<unsigned char>(text[j]))) j++;
            word = upper_word_at(j);
            return word.empty() ? from : after_word(j);
        };
        std::string w2, w3;
        if (w1 == "DETERMINISTIC") return e1;
        if (w1 == "NOT") { std::size_t e2 = next_word(e1, w2); return w2 == "DETERMINISTIC" ? e2 : at; }
        if (w1 == "NO" || w1 == "CONTAINS") { std::size_t e2 = next_word(e1, w2); return w2 == "SQL" ? e2 : at; }
        if (w1 == "READS" || w1 == "MODIFIES") {
            std::size_t e2 = next_word(e1, w2);
            if (w2 != "SQL") return at;
            std::size_t e3 = next_word(e2, w3);
            return w3 == "DATA" ? e3 : at;
        }
        if (w1 == "LANGUAGE") { std::size_t e2 = next_word(e1, w2); return w2 == "SQL" ? e2 : at; }
        if (w1 == "SQL") {
            std::size_t e2 = next_word(e1, w2);
            if (w2 != "SECURITY") return at;
            std::size_t e3 = next_word(e2, w3);
            return w3.empty() ? at : e3;
        }
        if (w1 == "COMMENT") {
            std::size_t j = e1;
            while (j < n && std::isspace(static_cast<unsigned char>(text[j]))) j++;
            return j < n && text[j] == '\'' ? skip_quoted(j) : at;
        }
        return at;
    };

    skip_space();
    if (is_function && upper_word_at(i) == "RETURNS") { // RETURNS type: up to the first characteristic or the body
        i = after_word(i);
        skip_space();
        const std::size_t type_start = i;
        int depth = 0;
        while (i < n) {
            if (text[i] == '\'' || text[i] == '"' || text[i] == '`') { i = skip_quoted(i); continue; }
            if (text[i] == '(') depth++;
            else if (text[i] == ')') depth--;
            if (depth == 0 && is_word_char(text[i]) && (i == type_start || !is_word_char(text[i - 1]))) {
                const std::string w = upper_word_at(i);
                if (i > type_start && (w == "RETURN" || w == "BEGIN" || characteristic_at(i) != i)) break;
            }
            i++;
        }
        std::size_t end = i;
        while (end > type_start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        parts.returns = text.substr(type_start, end - type_start);
    }
    for (;;) {
        skip_space();
        const std::size_t end = characteristic_at(i);
        if (end == i) break;
        parts.characteristics.push_back(text.substr(i, end - i));
        i = end;
    }
    skip_space();
    std::size_t end = n;
    while (end > i && (std::isspace(static_cast<unsigned char>(text[end - 1])) || text[end - 1] == ';')) end--;
    parts.body = text.substr(i, end - i);
    return parts;
}

// The characteristics MySQL prints, in the order it prints them: the SQL access (not CONTAINS SQL), DETERMINISTIC, SQL SECURITY INVOKER, COMMENT '...'
// (NOT DETERMINISTIC, CONTAINS SQL, LANGUAGE SQL and SQL SECURITY DEFINER are what a routine is anyway and are left out; the last one written of a kind counts)
inline std::vector<std::string> canonical_characteristics(const std::vector<std::string>& written) {
    std::string access, comment;
    bool deterministic = false, invoker = false;
    for (const std::string& c : written) {
        std::string up;
        for (char ch : c) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        if (up == "DETERMINISTIC") deterministic = true;
        else if (up.rfind("NOT", 0) == 0) deterministic = false;
        else if (up.rfind("NO ", 0) == 0) access = "NO SQL";
        else if (up.rfind("READS", 0) == 0) access = "READS SQL DATA";
        else if (up.rfind("MODIFIES", 0) == 0) access = "MODIFIES SQL DATA";
        else if (up.rfind("CONTAINS", 0) == 0) access.clear();
        else if (up.rfind("SQL", 0) == 0) invoker = up.find("INVOKER") != std::string::npos;
        else if (up.rfind("COMMENT", 0) == 0 && c.find('\'') != std::string::npos) comment = c.substr(c.find('\''));
    }
    std::vector<std::string> out;
    if (!access.empty()) out.push_back(access);
    if (deterministic) out.push_back("DETERMINISTIC");
    if (invoker) out.push_back("SQL SECURITY INVOKER");
    if (!comment.empty()) out.push_back("COMMENT " + comment);
    return out;
}

// CREATE DEFINER=... PROCEDURE `name`(params) [RETURNS type] [characteristic ...] body, as SHOW CREATE writes it: one characteristic to a line, the body on a line of its own.
// `returns`: the type of a function as MySQL writes it ("int", "varchar(30) CHARSET utf8mb4"); empty: as it was typed
inline std::string routine_create_text(const std::string& definer, const char* kind, const std::string& name, const std::string& source, const std::string& returns = "") {
    const bool is_function = std::string(kind) == "FUNCTION";
    const RoutineParts parts = split_routine_source(source, is_function);
    std::string out = "CREATE DEFINER=" + definer + " " + kind + " `" + name + "`(" + parts.params + ")";
    if (is_function && !parts.returns.empty()) out += " RETURNS " + (returns.empty() ? parts.returns : returns);
    for (const std::string& c : canonical_characteristics(parts.characteristics)) out += "\n    " + c;
    out += "\n" + parts.body;
    return out;
}

} // namespace engine
