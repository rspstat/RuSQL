#pragma once

// Faithful port of rusql-core/src/engine/query_cache.rs — SELECT result LRU-ish cache.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

constexpr std::size_t QUERY_CACHE_DEFAULT_CAPACITY = 512;

class QueryResultCache {
public:
    QueryResultCache() : QueryResultCache(QUERY_CACHE_DEFAULT_CAPACITY) {}
    explicit QueryResultCache(std::size_t capacity) : capacity(capacity) {}

    const std::string* get(const std::string& key) const;
    void put(std::string key, std::string result, const std::vector<std::string>& tables);
    void invalidate_table(const std::string& table);
    void clear();
    std::size_t len() const { return entries_.size(); }

    std::size_t capacity;

private:
    std::unordered_map<std::string, std::string> entries_;
    std::unordered_map<std::string, std::unordered_set<std::string>> table_to_keys_;
    std::unordered_map<std::string, std::uint64_t> insert_order_;
    std::uint64_t tick_ = 0;
};

} // namespace engine
