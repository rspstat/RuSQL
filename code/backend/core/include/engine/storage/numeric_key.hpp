#pragma once

// Numeric equivalence of index keys.
//
// A WHERE clause (executor_eval.cpp: eval_single/cmp_num) treats two values as equal when both parse
// as numbers and are equal as doubles -- `price = 7` matches "7", "7.0", "7.00" and "007" alike -- and
// falls back to comparing the text when either side is not a number. Column values are stored as the
// text the user typed ("007" in an INT column stays "007"), so an index that looks keys up by their exact
// text misses rows the scan finds. The two helpers here let an index answer the same question the scan does:
//   * normalize_numeric_key: one spelling per number, for hash buckets (exact);
//   * widen_numeric_bound:   a bound one ulp wider, for B+Tree range scans (superset, re-check afterwards).

#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace engine {

// Same notion of "is a number" as executor_eval.cpp's parse_f64: the WHOLE string must parse.
inline bool parse_number_key(const std::string& s, double& out) {
    if (s.empty()) return false;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

// Numerically equal texts ("7", "7.0", "007", "7e0") map to the same string; anything that is not a number
// is returned unchanged. NaN is never equal to anything, so it keeps its own text.
inline std::string normalize_numeric_key(const std::string& s) {
    double v;
    if (!parse_number_key(s, v) || std::isnan(v)) return s;
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    if (v == 0) return "0"; // also folds "-0"
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

// B+Tree keys order numerically-equal strings by their text ("07" < "7" < "7.0"), while a WHERE clause treats
// them as equal -- so `price = 7` must also find a row stored as "7.00", and `qty >= 7` one stored as "07". A
// numeric bound is therefore widened by one ulp in the direction it needs to cover (`lower`: toward -inf, else
// toward +inf); the entries found are re-checked against the real condition afterwards, so the widening can only
// add rows that get filtered out again. A non-numeric bound is used as it is. nullopt = "cannot express this
// bound safely" (infinity, or a value whose one-ulp neighbour does not survive a round trip through text) -> the
// caller must not use an index path.
inline std::optional<std::string> widen_numeric_bound(const std::string& key, bool lower) {
    double v;
    if (!parse_number_key(key, v)) return key;
    if (!std::isfinite(v)) return std::nullopt;
    double w = std::nextafter(v, lower ? -INFINITY : INFINITY);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", w);
    double back;
    if (!parse_number_key(buf, back) || back != w) return std::nullopt;
    return std::string(buf);
}

} // namespace engine
