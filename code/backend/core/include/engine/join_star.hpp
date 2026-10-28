#pragma once

// The columns `*` stands for over a join, in the order MySQL writes them (the result of the SELECT, and the columns of an empty answer, both go by this).
// A join is read as a tree, ((a JOIN b) JOIN c): the columns of one with USING / NATURAL are the merged ones first (each once), then what is left of its left
// side, then what is left of its right side -- so `a(x, y, a) JOIN b(x, b) USING (x) JOIN c(y, c) USING (y)` shows y, x, a, b, c (the second join puts y in front of
// the x the first one put there). `a RIGHT JOIN b USING (c)` is `b LEFT JOIN a`: the sides change places. A join with ON (or none) just appends its table.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace engine {

struct StarColumn {
    std::size_t table = 0;  // the table the column is read from (a merged column: the first table of the FROM list that has it)
    std::size_t column = 0; // its place among the columns of that table
    bool merged = false;    // one a USING / NATURAL join merged (read by its plain name)
};

// `tables[t]`: the column names of the t-th table of the FROM list (the FROM table first); `joined_using[i]`: what the USING / NATURAL of the join that brings
// in table i + 1 merges (empty: an ordinary join); `swapped[i]`: that join is a RIGHT JOIN ... USING
inline std::vector<StarColumn> star_columns(const std::vector<std::vector<std::string>>& tables, const std::vector<std::vector<std::string>>& joined_using,
                                            const std::vector<char>& swapped) {
    auto same = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char l, char r) { return std::tolower(static_cast<unsigned char>(l)) == std::tolower(static_cast<unsigned char>(r)); });
    };
    std::vector<StarColumn> shown;
    for (std::size_t k = 0; k < tables[0].size(); k++) shown.push_back({0, k, false});
    for (std::size_t i = 0; i < joined_using.size(); i++) {
        std::vector<StarColumn> right;
        for (std::size_t k = 0; k < tables[i + 1].size(); k++) right.push_back({i + 1, k, false});
        const std::vector<std::string>& common = joined_using[i];
        if (common.empty()) {
            shown.insert(shown.end(), right.begin(), right.end());
            continue;
        }
        std::vector<StarColumn> left = std::move(shown);
        if (swapped[i]) std::swap(left, right);
        std::vector<std::string> front; // each merged column once, in the order the left side has them (USING (y, x) over a(x, y) shows x, y)
        for (const std::string& c : common) {
            if (std::none_of(front.begin(), front.end(), [&](const std::string& f) { return same(f, c); })) front.push_back(c);
        }
        auto place_in_left = [&](const std::string& c) {
            auto found = std::find_if(left.begin(), left.end(), [&](const StarColumn& col) { return same(tables[col.table][col.column], c); });
            return static_cast<std::size_t>(found - left.begin());
        };
        std::stable_sort(front.begin(), front.end(), [&](const std::string& a, const std::string& b) { return place_in_left(a) < place_in_left(b); });
        std::vector<StarColumn> next;
        for (const std::string& c : front) {
            for (std::size_t t = 0; t <= i + 1; t++) {
                auto found = std::find_if(tables[t].begin(), tables[t].end(), [&](const std::string& n) { return same(n, c); });
                if (found == tables[t].end()) continue;
                next.push_back({t, static_cast<std::size_t>(found - tables[t].begin()), true});
                break;
            }
        }
        auto kept = [&](const StarColumn& col) {
            return std::none_of(front.begin(), front.end(), [&](const std::string& f) { return same(f, tables[col.table][col.column]); });
        };
        for (const StarColumn& col : left) if (kept(col)) next.push_back(col);
        for (const StarColumn& col : right) if (kept(col)) next.push_back(col);
        shown = std::move(next);
    }
    return shown;
}

} // namespace engine
