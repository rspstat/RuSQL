#pragma once

// The text of a result table, as the engine writes it and everything that reads it (the engine itself for a derived table or INSERT ... SELECT, the MySQL
// protocol layer, the desktop client, the MCP server) takes it apart again:
//
//     +------+--------+
//     | id   | name   |
//     +------+--------+
//     | 1    | \sx\s  |
//
// A cell is written after one space and followed by the padding that makes the column as wide as its widest cell, so a space or tab at either END of a
// value cannot be told from that padding: such a character is written as \s or \t. The others that would break the row are written as \\ (a backslash),
// \n (a newline) and \| (a bar). Only these five escapes exist; anything else after a backslash stands for itself.

#include <cstddef>
#include <string>
#include <vector>

namespace engine::result_text {

inline std::string escape_cell(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t");
    const std::size_t last = first == std::string::npos ? std::string::npos : value.find_last_not_of(" \t");
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); i++) {
        const char c = value[i];
        const bool at_an_end = first == std::string::npos || i < first || i > last;
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '|') out += "\\|";
        else if (c == ' ' && at_an_end) out += "\\s";
        else if (c == '\t' && at_an_end) out += "\\t";
        else out += c;
    }
    return out;
}

// The text of a SELECT that has columns but no rows: the header between its bars, nothing under it, and the line every empty answer ends with. (The readers of a table
// take it for a table of no rows: the desktop client shows the headers, the MySQL protocol sends the columns of an empty result set.)
inline std::string empty_table(const std::vector<std::string>& columns) {
    std::string bar, header;
    for (const std::string& column : columns) {
        const std::string cell = escape_cell(column);
        bar += "+" + std::string(cell.size() + 2, '-');
        header += "| " + cell + " ";
    }
    bar += "+";
    header += "|";
    return bar + "\n" + header + "\n" + bar + "\n" + bar + "\n0 rows returned.";
}

// Reverses escape_cell on one cell (already cut out of its row, padding removed). A lone backslash cannot come from escape_cell and stands for itself.
inline std::string unescape_cell(const std::string& cell) {
    std::string out;
    out.reserve(cell.size());
    for (std::size_t i = 0; i < cell.size(); i++) {
        if (cell[i] == '\\' && i + 1 < cell.size()) {
            const char next = cell[i + 1];
            if (next == '\\' || next == '|') { out += next; i++; continue; }
            if (next == 'n') { out += '\n'; i++; continue; }
            if (next == 's') { out += ' '; i++; continue; }
            if (next == 't') { out += '\t'; i++; continue; }
        }
        out += cell[i];
    }
    return out;
}

// The next '|' at or after `start` that is not the second character of an escape (so \| is never a cell boundary), or npos.
inline std::size_t find_unescaped_bar(const std::string& line, std::size_t start) {
    for (std::size_t i = start; i < line.size(); i++) {
        if (line[i] == '\\' && i + 1 < line.size()) { i++; continue; }
        if (line[i] == '|') return i;
    }
    return std::string::npos;
}

// The cells of one row line ("| a   | b |"), unescaped, padding removed. The pieces before the first bar and after the last are not cells; a piece
// of nothing but padding is an empty cell.
inline std::vector<std::string> split_row(const std::string& line) {
    std::vector<std::string> cells;
    std::size_t start = 0;
    for (;;) {
        const std::size_t bar = find_unescaped_bar(line, start);
        const std::string piece = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (!piece.empty()) {
            const std::size_t from = piece.find_first_not_of(" \t");
            if (from == std::string::npos) cells.emplace_back();
            else cells.push_back(unescape_cell(piece.substr(from, piece.find_last_not_of(" \t") - from + 1)));
        }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return cells;
}

} // namespace engine::result_text
