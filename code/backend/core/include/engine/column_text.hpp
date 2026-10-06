#pragma once

// The text of a column reference, and of what an aggregate takes as its argument.

#include <cctype>
#include <string>

namespace engine {

inline bool identifier_char(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// A plain `name`, `table.name` or `db.table.name`; anything else (a call, a JSON path, `@variable`, `*`, a number) is not a column reference.
inline bool plain_reference(const std::string& name) {
    if (name.empty()) return false;
    bool segment_start = true;
    for (unsigned char c : name) {
        if (c == '.') {
            if (segment_start) return false;
            segment_start = true;
            continue;
        }
        if (!identifier_char(c)) return false;
        if (segment_start && std::isdigit(c)) return false;
        segment_start = false;
    }
    return !segment_start;
}

// The argument of an aggregate -- the text the parser keeps in `Agg::col` and inside `SUM(...)` -- is a column (`v`, `t.v`), `*`, the placeholder of a CASE
// (`__case__`, a name like any other), or an expression (`price * qty`, `COALESCE(x, 0)`, `1`) that the executor computes for every row under that text.
inline bool is_expression_argument(const std::string& arg) { return !arg.empty() && arg != "*" && !plain_reference(arg); }

// The argument inside an aggregate reference: `SUM(price * qty)` -> `price * qty`, `COUNT(DISTINCT x)` -> `x`; empty when `ref` is not a call.
inline std::string aggregate_reference_argument(const std::string& ref) {
    const std::size_t lp = ref.find('('), rp = ref.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp <= lp) return std::string();
    std::string arg = ref.substr(lp + 1, rp - lp - 1);
    if (arg.rfind("DISTINCT ", 0) == 0) arg.erase(0, 9);
    return arg;
}

} // namespace engine
