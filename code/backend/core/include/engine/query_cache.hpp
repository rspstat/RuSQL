#pragma once

// Faithful port of rusql-core/src/engine/query_cache.rs — SELECT result LRU-ish cache.

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

constexpr std::size_t QUERY_CACHE_DEFAULT_CAPACITY = 512;

// Stage 4 (table-level concurrency): guarded by its own mutex_ so that autocommit
// statements against different tables, running concurrently under their own table
// locks (no longer a single whole-database exclusive lock), can safely hit this shared
// cache at the same time. get() returns a copy (std::optional<std::string>) rather than
// a raw pointer into entries_ -- a pointer would dangle the instant the lock is released,
// since a concurrent put() on an unrelated key can rehash the underlying unordered_map.
//
// put()/invalidate_table()/clear() are `const` (with entries_/table_to_keys_/
// insert_order_/tick_ all `mutable`, alongside mutex_) so callers can invoke them
// through a `const SharedDatabase&` -- i.e. while holding the outer RwLock<SharedDatabase>
// only shared, not exclusive. The cache's own mutex_ is what actually protects these
// members; there's no reason a cache-only operation should need the whole database
// locked exclusively.
class QueryResultCache {
public:
    QueryResultCache() : QueryResultCache(QUERY_CACHE_DEFAULT_CAPACITY) {}
    explicit QueryResultCache(std::size_t capacity) : capacity(capacity) {}

    QueryResultCache(const QueryResultCache&) = delete;
    QueryResultCache& operator=(const QueryResultCache&) = delete;
    QueryResultCache(QueryResultCache&& other) noexcept;
    QueryResultCache& operator=(QueryResultCache&& other) noexcept;

    std::optional<std::string> get(const std::string& key) const;
    // `table_gens` is the exact set of (table, generation-at-capture-time) pairs the
    // caller captured via table_generation() -- see that method's doc comment for the
    // capture-before/capture-after protocol this is built for. Storing them lets get()
    // detect a table modified since this entry was cached even if invalidate_table()
    // itself already ran and removed this exact key (a NEW put() racing right after an
    // invalidate would otherwise silently resurrect stale data -- see table_generation's
    // comment for why invalidate_table's own key-removal isn't sufficient on its own).
    void put(std::string key, std::string result, const std::vector<std::pair<std::string, std::uint64_t>>& table_gens) const;
    void invalidate_table(const std::string& table) const;
    void clear() const;
    std::size_t len() const;

    // Row-level-concurrency Stage 4/5 correctness fix: current generation counter for
    // `table` (0 if it has never been invalidated). invalidate_table() increments this
    // every time it runs. Callers (execute_sql) capture this for every table a SELECT
    // touches BOTH immediately before and immediately after running it; if any table's
    // generation changed across that window, a concurrent write's invalidate_table()
    // ran DURING the read and the result must not be cached at all (it may already be
    // stale, or a plain key-removal-based invalidate could otherwise be raced by this
    // exact put() landing a moment later -- table_to_keys_-based removal alone cannot
    // prevent that, since it only clears keys that exist AT THE TIME invalidate_table()
    // runs, not ones inserted afterward). get() independently re-checks the stamped
    // generations on every lookup, so even a successful put() stamped with stale-but-
    // matching generations is still caught once a LATER invalidate_table() actually runs.
    std::uint64_t table_generation(const std::string& table) const;

    std::size_t capacity;

private:
    struct CacheEntry {
        std::string result;
        std::vector<std::pair<std::string, std::uint64_t>> table_gens;
    };
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, CacheEntry> entries_;
    mutable std::unordered_map<std::string, std::unordered_set<std::string>> table_to_keys_;
    mutable std::unordered_map<std::string, std::uint64_t> insert_order_;
    mutable std::unordered_map<std::string, std::uint64_t> table_generation_;
    mutable std::uint64_t tick_ = 0;
};

} // namespace engine
