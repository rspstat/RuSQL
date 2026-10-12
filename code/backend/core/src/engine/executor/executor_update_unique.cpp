// PRIMARY KEY / UNIQUE enforcement for UPDATE.
//
// INSERT has always rejected a duplicate PRIMARY KEY or UNIQUE value, but UPDATE never checked
// anything: `UPDATE t SET id = 2 WHERE id = 1` happily produced two live rows with id 2, as did
// `SET email = <someone else's>` on a UNIQUE column and `SET id = 9 WHERE id >= 2` over several
// rows. A table with two live rows for one key breaks every "one row per pk" assumption
// downstream (the pk -> position cache, PK B+Tree, index-assisted UPDATE/DELETE).

#include <unordered_set>

#include <algorithm>

#include "engine/executor/executor.hpp"
#include "engine/storage/numeric_key.hpp"

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
        // 2) a changed value must not be held by a live row this statement does not rewrite. A value
        //    that one of the rewritten rows holds right now is fine (that holder is being replaced);
        //    otherwise any live holder is, by construction, a different row.
        for (std::size_t i = 0; i < news.size(); i++) {
            std::string ov = get(*olds[i], col.name), nv = get(news[i], col.name);
            if (collation::equal(ov, nv) || skipped(nv) || held_by_targets.count(collation::key(nv))) continue;
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
        for (std::size_t i = 0; i < news.size(); i++) { // 1) the statement's own result must not repeat a key
            auto nk = encode(news[i], shown);
            if (!nk) continue;
            if (!final_keys.insert(*nk).second) return "Duplicate entry '" + shown + "' for key '" + spec.label + "'";
            if (!held_by_targets.count(*nk)) wanted.insert(*nk);
        }
        if (wanted.empty()) continue;
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
