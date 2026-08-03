#include "engine/query_cache.hpp"

namespace engine {

QueryResultCache::QueryResultCache(QueryResultCache&& other) noexcept : capacity(other.capacity) {
    std::lock_guard<std::mutex> g(other.mutex_);
    entries_ = std::move(other.entries_);
    table_to_keys_ = std::move(other.table_to_keys_);
    insert_order_ = std::move(other.insert_order_);
    tick_ = other.tick_;
}

QueryResultCache& QueryResultCache::operator=(QueryResultCache&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    capacity = other.capacity;
    entries_ = std::move(other.entries_);
    table_to_keys_ = std::move(other.table_to_keys_);
    insert_order_ = std::move(other.insert_order_);
    tick_ = other.tick_;
    return *this;
}

std::optional<std::string> QueryResultCache::get(const std::string& key) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

void QueryResultCache::put(std::string key, std::string result, const std::vector<std::string>& tables) {
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
    for (auto& table : tables) table_to_keys_[table].insert(key);
    insert_order_[key] = tick_;
    entries_[std::move(key)] = std::move(result);
}

void QueryResultCache::invalidate_table(const std::string& table) {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = table_to_keys_.find(table);
    if (it == table_to_keys_.end()) return;
    for (auto& key : it->second) {
        entries_.erase(key);
        insert_order_.erase(key);
    }
    table_to_keys_.erase(it);
}

void QueryResultCache::clear() {
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

} // namespace engine
