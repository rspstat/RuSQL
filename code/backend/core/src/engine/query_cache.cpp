#include "engine/query_cache.hpp"

namespace engine {

QueryResultCache::QueryResultCache(QueryResultCache&& other) noexcept : capacity(other.capacity) {
    std::lock_guard<std::mutex> g(other.mutex_);
    entries_ = std::move(other.entries_);
    table_to_keys_ = std::move(other.table_to_keys_);
    insert_order_ = std::move(other.insert_order_);
    table_generation_ = std::move(other.table_generation_);
    tick_ = other.tick_;
}

QueryResultCache& QueryResultCache::operator=(QueryResultCache&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    capacity = other.capacity;
    entries_ = std::move(other.entries_);
    table_to_keys_ = std::move(other.table_to_keys_);
    insert_order_ = std::move(other.insert_order_);
    table_generation_ = std::move(other.table_generation_);
    tick_ = other.tick_;
    return *this;
}

std::optional<std::string> QueryResultCache::get(const std::string& key) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    for (auto& [table, gen] : it->second.table_gens) {
        auto git = table_generation_.find(table);
        std::uint64_t current = git != table_generation_.end() ? git->second : 0;
        if (current != gen) {
            // Stale relative to a write this entry didn't account for -- evict now
            // (cheap opportunistic cleanup) and report a miss.
            entries_.erase(it);
            insert_order_.erase(key);
            for (auto& [t, keys] : table_to_keys_) {
                (void)t;
                keys.erase(key);
            }
            return std::nullopt;
        }
    }
    return it->second.result;
}

void QueryResultCache::put(std::string key, std::string result, const std::vector<std::pair<std::string, std::uint64_t>>& table_gens) const {
    std::lock_guard<std::mutex> g(mutex_);
    if (entries_.count(key)) return;

    if (entries_.size() >= capacity) {
        std::optional<std::string> oldest_key;
        std::uint64_t oldest_tick = 0;
        for (auto& [k, t] : insert_order_) {
            if (!oldest_key || t < oldest_tick) {
                oldest_key = k;
                oldest_tick = t;
            }
        }
        if (oldest_key) {
            entries_.erase(*oldest_key);
            insert_order_.erase(*oldest_key);
            for (auto& [table, keys] : table_to_keys_) {
                (void)table;
                keys.erase(*oldest_key);
            }
        }
    }

    tick_++;
    for (auto& [table, gen] : table_gens) {
        (void)gen;
        table_to_keys_[table].insert(key);
    }
    insert_order_[key] = tick_;
    entries_[std::move(key)] = CacheEntry{std::move(result), table_gens};
}

void QueryResultCache::invalidate_table(const std::string& table) const {
    std::lock_guard<std::mutex> g(mutex_);
    table_generation_[table]++;
    auto it = table_to_keys_.find(table);
    if (it == table_to_keys_.end()) return;
    for (auto& key : it->second) {
        entries_.erase(key);
        insert_order_.erase(key);
    }
    table_to_keys_.erase(it);
}

void QueryResultCache::clear() const {
    std::lock_guard<std::mutex> g(mutex_);
    entries_.clear();
    table_to_keys_.clear();
    insert_order_.clear();
    tick_ = 0;
}

std::size_t QueryResultCache::len() const {
    std::lock_guard<std::mutex> g(mutex_);
    return entries_.size();
}

std::uint64_t QueryResultCache::table_generation(const std::string& table) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = table_generation_.find(table);
    return it != table_generation_.end() ? it->second : 0;
}

} // namespace engine
