// PRIMARY KEY / UNIQUE enforcement for UPDATE.
//
// INSERT has always rejected a duplicate PRIMARY KEY or UNIQUE value, but UPDATE never checked
// anything: `UPDATE t SET id = 2 WHERE id = 1` happily produced two live rows with id 2, as did
// `SET email = <someone else's>` on a UNIQUE column and `SET id = 9 WHERE id >= 2` over several
// rows. A table with two live rows for one key breaks every "one row per pk" assumption
// downstream (the pk -> position cache, PK B+Tree, index-assisted UPDATE/DELETE).

#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <numeric>

#include "engine/executor/executor.hpp"
#include "engine/storage/numeric_key.hpp"

namespace engine {

namespace {
// Whether Executor::index_or_scan_exists answers "no" for a value of `column` by scanning the table. Only a single-column primary key with its tree and a
// column with a hash index know every holder of a value (a secondary B+Tree keeps one row per key: it can confirm a value, not rule it out).
bool lookup_scans(const SharedDatabase& s, const std::string& table, const std::string& column) {
    std::string pk_column;
    std::size_t pk_count = 0;
    if (const auto* schema = s.catalog.get_table(table)) {
        for (auto& c : schema->columns) {
            if (!c.primary_key) continue;
            if (pk_count == 0) pk_column = c.name;
            pk_count++;
        }
    }
    if (pk_count == 1 && pk_column == column && s.indexes.count(table)) return false;
    for (auto& [index_key, meta] : s.hash_index_meta) {
        if (meta.first == table && meta.second == column && s.hash_indexes.count(index_key)) return false;
    }
    return true;
}

// Whether rewriting the rows one after the other, in `order`, ever gives a row a key that another row still holds. InnoDB checks every row as it writes it, so
// `SET u = u + 1` over u = 1, 2, 3 fails on the first row, and over u = 3, 2, 1 (or with ORDER BY u DESC) it does not. `old_keys` / `new_keys` are the key of every row
// before and after (nullopt: a NULL in a UNIQUE key, which never clashes). Returns the row that clashes.
std::optional<std::size_t> first_clash(const std::vector<std::optional<std::string>>& old_keys, const std::vector<std::optional<std::string>>& new_keys,
                                       const std::vector<std::size_t>& order) {
    std::unordered_map<std::string, std::size_t> held;
    held.reserve(order.size() * 2);
    for (std::size_t i = 0; i < old_keys.size(); i++) {
        if (old_keys[i]) held.emplace(*old_keys[i], i);
    }
    for (std::size_t i : order) {
        if (old_keys[i]) held.erase(*old_keys[i]);
        if (new_keys[i] && !held.emplace(*new_keys[i], i).second) return i;
    }
    return std::nullopt;
}
} // namespace

std::optional<std::string> Executor::update_unique_violation(SharedDatabase& s, const std::string& table, const std::vector<const Row*>& olds,
                                                             const std::vector<Row>& news, const std::vector<std::string>* by_order,
                                                             const std::vector<std::string>& where_columns) {
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

    // The orders the rows may be written in: the one ORDER BY gave; else the order of the PRIMARY KEY (how InnoDB reads the table), or, when the WHERE compares a
    // column that has an index to a value, the order of that index (the key, then the primary key). Which one MySQL's plan takes is not told here, so a clash is
    // reported only when it happens in all of them: a shift that fits in one of them is let through, never refused wrongly.
    std::vector<std::vector<std::size_t>> orders;
    bool orders_made = false;
    auto plausible_orders = [&]() -> const std::vector<std::vector<std::size_t>>& {
        if (orders_made) return orders;
        orders_made = true;
        if (by_order && !by_order->empty()) {
            std::unordered_map<std::string, std::size_t> rank;
            for (std::size_t k = 0; k < by_order->size(); k++) rank.emplace((*by_order)[k], k);
            const std::vector<std::string> identity = schema->identity_columns();
            std::vector<std::size_t> rank_of(olds.size(), by_order->size());
            for (std::size_t i = 0; i < olds.size(); i++) {
                std::string key;
                for (std::size_t c = 0; c < identity.size(); c++) key += (c ? std::string(1, '\x00') : std::string()) + get(*olds[i], identity[c]);
                if (auto it = rank.find(key); it != rank.end()) rank_of[i] = it->second;
            }
            std::vector<std::size_t> given(olds.size());
            std::iota(given.begin(), given.end(), std::size_t{0});
            std::stable_sort(given.begin(), given.end(), [&](std::size_t a, std::size_t b) { return rank_of[a] < rank_of[b]; });
            orders.push_back(std::move(given));
            return orders;
        }
        orders.push_back(rows_in_key_order(schema, olds, {}));
        std::vector<std::string> tried;
        for (const std::string& column : where_columns) {
            if (std::find(tried.begin(), tried.end(), column) != tried.end()) continue;
            tried.push_back(column);
            bool indexed = false;
            for (auto& c : schema->columns) {
                if (c.name == column) indexed = c.unique && !c.primary_key; // (the primary key is the order the rows are read in anyway)
            }
            for (auto& [name, meta] : s.index_meta) indexed = indexed || (meta.first == table && meta.second == column);
            for (auto& [name, meta] : s.hash_index_meta) indexed = indexed || (meta.first == table && meta.second == column);
            for (auto& [name, index] : s.composite_indexes) indexed = indexed || (index.table == table && !index.columns.empty() && index.columns.front() == column);
            for (auto& key : schema->unique_keys) indexed = indexed || (!key.columns.empty() && key.columns.front() == column);
            if (indexed) orders.push_back(rows_in_key_order(schema, olds, {column}));
        }
        return orders;
    };

    for (auto& col : schema->columns) {
        // a single-column PRIMARY KEY, or a single-column UNIQUE; composite keys were never checked here
        const bool is_pk = col.primary_key && pk_count == 1;
        const bool is_unique = col.unique && !col.primary_key;
        if (!is_pk && !is_unique) continue;

        bool any_changed = false;
        std::unordered_set<std::string> held_by_targets; // the values the rows being rewritten hold right now
        for (std::size_t i = 0; i < news.size(); i++) {
            std::string ov = get(*olds[i], col.name), nv = get(news[i], col.name);
            held_by_targets.insert(collation::key(ov));
            if (!collation::equal(ov, nv)) any_changed = true;
        }
        if (!any_changed) continue;

        auto skipped = [&](const std::string& v) { return !is_pk && v == EXECUTOR_NULL_VALUE; }; // UNIQUE allows NULLs

        // 1) the statement's own result must not repeat a value
        std::unordered_set<std::string> final_values;
        for (std::size_t i = 0; i < news.size(); i++) {
            std::string nv = get(news[i], col.name);
            if (skipped(nv)) continue;
            if (!final_values.insert(collation::key(nv)).second) return dup(nv, col.name);
        }
        // 1b) the rows are written one after the other: a row that takes the value another row still holds clashes (a shift: SET u = u + 1)
        bool chained = false;
        for (std::size_t i = 0; i < news.size() && !chained; i++) {
            const std::string ov = get(*olds[i], col.name), nv = get(news[i], col.name);
            chained = !collation::equal(ov, nv) && !skipped(nv) && held_by_targets.count(collation::key(nv)) > 0;
        }
        if (chained) {
            std::vector<std::optional<std::string>> old_keys(news.size()), new_keys(news.size());
            for (std::size_t i = 0; i < news.size(); i++) {
                const std::string ov = get(*olds[i], col.name), nv = get(news[i], col.name);
                if (!skipped(ov)) old_keys[i] = collation::key(ov);
                if (!skipped(nv)) new_keys[i] = collation::key(nv);
            }
            std::optional<std::size_t> clash;
            bool every_order = true;
            for (const auto& order : plausible_orders()) {
                auto found = first_clash(old_keys, new_keys, order);
                if (!found) {
                    every_order = false;
                    break;
                }
                if (!clash) clash = found;
            }
            if (every_order && clash) return dup(get(news[*clash], col.name), col.name);
        }
        // 2) a changed value must not be held by a live row this statement does not rewrite. A value
        //    that one of the rewritten rows holds right now is fine (that holder is being replaced);
        //    otherwise any live holder is, by construction, a different row.
        std::vector<std::size_t> wanted_rows; // the rows whose new value has to be looked for in the table
        for (std::size_t i = 0; i < news.size(); i++) {
            std::string ov = get(*olds[i], col.name), nv = get(news[i], col.name);
            if (collation::equal(ov, nv) || skipped(nv) || held_by_targets.count(collation::key(nv))) continue;
            wanted_rows.push_back(i);
        }
        // The cache of UNIQUE values (it is current when no other statement has locked the table since it was made): when it says that no live row holds any of the new
        // values, there is nothing to look for.
        if (!wanted_rows.empty()) {
            bool all_absent = true;
            for (std::size_t i : wanted_rows) {
                const Row& new_row = news[i];
                const auto value_of = [&new_row](const std::string& c) -> const std::string* {
                    auto it = new_row.find(c);
                    return it == new_row.end() ? nullptr : &it->second;
                };
                if (!unique_probe_proves_absent(s, table, *schema, {col.name}, /*tuple=*/false, /*skip_null=*/true, value_of, ProbeUse::InsideExclusive)) {
                    all_absent = false;
                    break;
                }
            }
            if (all_absent) continue;
        }
        if (wanted_rows.size() > 1 && lookup_scans(s, table, col.name)) {
            // no index answers "is this value there": one pass over the table for all the values (a lookup each scanned the table once per row)
            std::unordered_set<std::string> wanted_keys, held;
            for (std::size_t i : wanted_rows) wanted_keys.insert(collation::key(get(news[i], col.name)));
            for (auto& live : tit->second) {
                if (!is_visible(live)) continue;
                auto it = live.find(col.name);
                if (it == live.end()) continue;
                std::string key = collation::key(it->second);
                if (wanted_keys.count(key)) held.insert(std::move(key));
            }
            for (std::size_t i : wanted_rows) {
                const std::string nv = get(news[i], col.name);
                if (held.count(collation::key(nv))) return dup(nv, col.name);
            }
            continue;
        }
        for (std::size_t i : wanted_rows) {
            const std::string nv = get(news[i], col.name);
            if (index_or_scan_exists(s, table, tit->second, col.name, nv, [](const Row& r) { return Executor::is_visible(r); })) {
                return dup(nv, col.name);
            }
        }
    }

    // the keys over several columns: the composite PRIMARY KEY (it was never checked here) and every UNIQUE (a, b)
    struct KeySpec { std::vector<std::string> columns; std::string label; bool primary; };
    std::vector<KeySpec> specs;
    const std::string bare_table = table.substr(table.rfind('.') == std::string::npos ? 0 : table.rfind('.') + 1);
    if (schema->primary_key_columns.size() > 1) specs.push_back({schema->primary_key_columns, "PRIMARY", true});
    for (auto& key : schema->unique_keys) specs.push_back({key.columns, bare_table + "." + key.name.value_or(key.columns.front()), false});
    for (auto& spec : specs) {
        auto encode = [&](const Row& r, std::string& shown) -> std::optional<std::string> {
            std::string encoded;
            shown.clear();
            for (std::size_t k = 0; k < spec.columns.size(); k++) {
                const std::string v = get(r, spec.columns[k]);
                if (v == EXECUTOR_NULL_VALUE && !spec.primary) return std::nullopt; // (a NULL in a UNIQUE key never conflicts)
                const std::string part = collation::key(v);
                encoded += std::to_string(part.size()) + ":" + part;
                shown += (k ? "-" : "") + v;
            }
            return encoded;
        };
        std::string shown;
        bool any_changed = false;
        std::unordered_set<std::string> held_by_targets;
        for (std::size_t i = 0; i < news.size(); i++) {
            auto ok = encode(*olds[i], shown), nk = encode(news[i], shown);
            if (ok) held_by_targets.insert(*ok);
            if (ok != nk) any_changed = true;
        }
        if (!any_changed) continue;
        std::unordered_set<std::string> final_keys, wanted;
        std::vector<std::size_t> wanted_rows; // the rows whose new key has to be looked for
        bool chained = false;                 // a row takes the key another row holds (a shift: SET b = b + 1)
        for (std::size_t i = 0; i < news.size(); i++) { // 1) the statement's own result must not repeat a key
            auto nk = encode(news[i], shown);
            if (!nk) continue;
            if (!final_keys.insert(*nk).second) return "Duplicate entry '" + shown + "' for key '" + spec.label + "'";
            if (!held_by_targets.count(*nk)) {
                wanted.insert(*nk);
                wanted_rows.push_back(i);
            } else if (!chained) {
                std::string before_shown;
                chained = encode(*olds[i], before_shown) != nk;
            }
        }
        if (chained) { // 1b) the rows are written one after the other
            std::vector<std::optional<std::string>> old_keys(news.size()), new_keys(news.size());
            for (std::size_t i = 0; i < news.size(); i++) {
                old_keys[i] = encode(*olds[i], shown);
                new_keys[i] = encode(news[i], shown);
            }
            std::optional<std::size_t> clash;
            bool every_order = true;
            for (const auto& order : plausible_orders()) {
                auto found = first_clash(old_keys, new_keys, order);
                if (!found) {
                    every_order = false;
                    break;
                }
                if (!clash) clash = found;
            }
            if (every_order && clash) {
                encode(news[*clash], shown);
                return "Duplicate entry '" + shown + "' for key '" + spec.label + "'";
            }
        }
        if (wanted.empty()) continue;
        {   // the cache of UNIQUE values: no live row holds any of these keys
            bool all_absent = true;
            for (std::size_t i : wanted_rows) {
                const Row& new_row = news[i];
                const auto value_of = [&new_row](const std::string& c) -> const std::string* {
                    auto it = new_row.find(c);
                    return it == new_row.end() ? nullptr : &it->second;
                };
                if (!unique_probe_proves_absent(s, table, *schema, spec.columns, /*tuple=*/true, /*skip_null=*/!spec.primary, value_of, ProbeUse::InsideExclusive)) {
                    all_absent = false;
                    break;
                }
            }
            if (all_absent) continue;
        }
        std::unordered_set<const Row*> rewritten(olds.begin(), olds.end());
        for (auto& live : tit->second) { // 2) a key that is new must not be held by a live row this statement does not rewrite
            if (!is_visible(live) || rewritten.count(&live)) continue;
            auto lk = encode(live, shown);
            if (lk && wanted.count(*lk)) return "Duplicate entry '" + shown + "' for key '" + spec.label + "'";
        }
    }
    return std::nullopt;
}

namespace {
std::string text_of(const Row& r, const std::string& column) {
    auto it = r.find(column);
    return it != r.end() ? it->second : std::string(EXECUTOR_NULL_VALUE);
}
} // namespace

std::vector<Executor::KeyClaim> Executor::unique_key_claims(const TableSchema& schema, const std::string& table, const Row& row, const Row* old_row) {
    std::vector<KeyClaim> claims;
    const std::string bare_table = table.substr(table.rfind('.') == std::string::npos ? 0 : table.rfind('.') + 1);
    // the key of `columns` in `r` as the lock manager and the comparison see it; nullopt when a column is NULL (a NULL never repeats)
    auto key_of = [](const Row& r, const std::vector<std::string>& columns, std::string& shown) -> std::optional<std::string> {
        std::string encoded;
        shown.clear();
        for (std::size_t k = 0; k < columns.size(); k++) {
            const std::string v = text_of(r, columns[k]);
            if (v == EXECUTOR_NULL_VALUE) return std::nullopt;
            const std::string part = normalize_numeric_key(v);
            encoded += std::to_string(part.size()) + ":" + part;
            shown += (k ? "-" : "") + v;
        }
        return encoded;
    };
    auto add = [&](std::string lock_table, const std::vector<std::string>& columns, bool composite, std::string label) {
        std::string shown, old_shown;
        auto now = key_of(row, columns, shown);
        if (!now) return;
        if (old_row) {
            auto before = key_of(*old_row, columns, old_shown);
            if (before && *before == *now) return; // the key is not new
        }
        KeyClaim claim;
        claim.lock_table = std::move(lock_table);
        claim.lock_key = composite ? *now : text_of(row, columns.front());
        claim.comparable = *now;
        claim.message = composite ? "Duplicate entry '" + shown + "' for key '" + label + "'" : "Duplicate value '" + shown + "' for column '" + label + "'";
        claim.columns = columns;
        claims.push_back(std::move(claim));
    };
    for (auto& col : schema.columns) {
        if (col.unique && !col.primary_key) add(table + "#uk:" + col.name, {col.name}, false, col.name);
    }
    if (schema.primary_key_columns.size() > 1) add(table + "#ck:pk", schema.primary_key_columns, true, "PRIMARY");
    for (std::size_t k = 0; k < schema.unique_keys.size(); k++) {
        auto& key = schema.unique_keys[k];
        add(table + "#ck:u" + std::to_string(k), key.columns, true, bare_table + "." + key.name.value_or(key.columns.front()));
    }
    return claims;
}

std::vector<std::string> Executor::referenced_columns(const SharedDatabase& s, const std::string& table) {
    std::vector<std::string> out;
    for (auto& [name, schema] : s.catalog.tables) {
        for (auto& col : schema.columns) {
            if (col.foreign_key && col.foreign_key->ref_table == table && std::find(out.begin(), out.end(), col.foreign_key->ref_column) == out.end()) {
                out.push_back(col.foreign_key->ref_column);
            }
        }
    }
    return out;
}

std::vector<Executor::KeyClaim> Executor::fk_claims(const SharedDatabase& s, const TableSchema& schema, const std::string& table, const Row& row, const Row& old_row) {
    std::vector<KeyClaim> claims;
    for (auto& col : schema.columns) {
        if (!col.foreign_key) continue;
        const std::string v = text_of(row, col.name);
        if (v == EXECUTOR_NULL_VALUE || collation::equal(v, text_of(old_row, col.name))) continue;
        KeyClaim claim;
        claim.lock_table = fk_claim_table(col.foreign_key->ref_table, col.foreign_key->ref_column);
        claim.lock_key = v;
        claim.shared = true;
        claims.push_back(std::move(claim));
    }
    for (auto& column : referenced_columns(s, table)) {
        const std::string before = text_of(old_row, column);
        if (before == EXECUTOR_NULL_VALUE || collation::equal(before, text_of(row, column))) continue;
        KeyClaim claim;
        claim.lock_table = fk_claim_table(table, column);
        claim.lock_key = before;
        claims.push_back(std::move(claim));
    }
    return claims;
}

bool Executor::row_holds_claim(const KeyClaim& claim, const Row& row) {
    std::string encoded;
    for (auto& column : claim.columns) {
        const std::string v = text_of(row, column);
        if (v == EXECUTOR_NULL_VALUE) return false;
        const std::string part = normalize_numeric_key(v);
        encoded += std::to_string(part.size()) + ":" + part;
    }
    return encoded == claim.comparable;
}

} // namespace engine
