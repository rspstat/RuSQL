// PRIMARY KEY / UNIQUE enforcement for UPDATE.
//
// INSERT has always rejected a duplicate PRIMARY KEY or UNIQUE value, but UPDATE never checked
// anything: `UPDATE t SET id = 2 WHERE id = 1` happily produced two live rows with id 2, as did
// `SET email = <someone else's>` on a UNIQUE column and `SET id = 9 WHERE id >= 2` over several
// rows. A table with two live rows for one key breaks every "one row per pk" assumption
// downstream (the pk -> position cache, PK B+Tree, index-assisted UPDATE/DELETE).

#include <unordered_set>

#include "engine/executor/executor.hpp"

namespace engine {

std::optional<std::string> Executor::update_unique_violation(SharedDatabase& s, const std::string& table, const std::vector<const Row*>& olds,
                                                             const std::vector<Row>& news) {
    const TableSchema* schema = s.catalog.get_table(table);
    auto tit = s.tables.find(table);
    if (!schema || tit == s.tables.end() || olds.size() != news.size()) return std::nullopt;

    std::size_t pk_count = 0;
    for (auto& c : schema->columns) {
        if (c.primary_key) pk_count++;
    }

    auto get = [](const Row& r, const std::string& k) {
        auto it = r.find(k);
        return it != r.end() ? it->second : std::string();
    };
    auto dup = [&](const std::string& val, const std::string& col) { return "Duplicate value '" + val + "' for column '" + col + "'"; };

    for (auto& col : schema->columns) {
        // a single-column PRIMARY KEY, or a single-column UNIQUE; composite keys were never checked here
        const bool is_pk = col.primary_key && pk_count == 1;
        const bool is_unique = col.unique && !col.primary_key;
        if (!is_pk && !is_unique) continue;

        bool any_changed = false;
        std::unordered_set<std::string> held_by_targets; // the values the rows being rewritten hold right now
        for (std::size_t i = 0; i < news.size(); i++) {
            std::string ov = get(*olds[i], col.name), nv = get(news[i], col.name);
            held_by_targets.insert(ov);
            if (ov != nv) any_changed = true;
        }
        if (!any_changed) continue;

        auto skipped = [&](const std::string& v) { return !is_pk && v == EXECUTOR_NULL_VALUE; }; // UNIQUE allows NULLs

        // 1) the statement's own result must not repeat a value
        std::unordered_set<std::string> final_values;
        for (std::size_t i = 0; i < news.size(); i++) {
            std::string nv = get(news[i], col.name);
            if (skipped(nv)) continue;
            if (!final_values.insert(nv).second) return dup(nv, col.name);
        }
        // 2) a changed value must not be held by a live row this statement does not rewrite. A value
        //    that one of the rewritten rows holds right now is fine (that holder is being replaced);
        //    otherwise any live holder is, by construction, a different row.
        for (std::size_t i = 0; i < news.size(); i++) {
            std::string ov = get(*olds[i], col.name), nv = get(news[i], col.name);
            if (ov == nv || skipped(nv) || held_by_targets.count(nv)) continue;
            if (index_or_scan_exists(s, table, tit->second, col.name, nv, [](const Row& r) { return Executor::is_visible(r); })) {
                return dup(nv, col.name);
            }
        }
    }
    return std::nullopt;
}

} // namespace engine
