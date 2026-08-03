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
class QueryResultCache {
public:
    QueryResultCache() : QueryResultCache(QUERY_CACHE_DEFAULT_CAPACITY) {}
    explicit QueryResultCache(std::size_t capacity) : capacity(capacity) {}

    QueryResultCache(const QueryResultCache&) = delete;
    QueryResultCache& operator=(const QueryResultCache&) = delete;
    QueryResultCache(QueryResultCache&& other) noexcept;
    QueryResultCache& operator=(QueryResultCache&& other) noexcept;

    std::optional<std::string> get(const std::string& key) const;
    void put(std::string key, std::string result, const std::vector<std::string>& tables);
    void invalidate_table(const std::string& table);
    void clear();
    std::size_t len() const;

    std::size_t capacity;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> entries_;
    std::unordered_map<std::string, std::unordered_set<std::string>> table_to_keys_;
    std::unordered_map<std::string, std::uint64_t> insert_order_;
    std::uint64_t tick_ = 0;
};

} // namespace engine
