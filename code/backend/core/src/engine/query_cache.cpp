#include "engine/query_cache.hpp"

namespace engine {

const std::string* QueryResultCache::get(const std::string& key) const {
    auto it = entries_.find(key);
    return it != entries_.end() ? &it->second : nullptr;
}

void QueryResultCache::put(std::string key, std::string result, const std::vector<std::string>& tables) {
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
    auto it = table_to_keys_.find(table);
    if (it == table_to_keys_.end()) return;
    for (auto& key : it->second) {
        entries_.erase(key);
        insert_order_.erase(key);
    }
    table_to_keys_.erase(it);
}

void QueryResultCache::clear() {
    entries_.clear();
    table_to_keys_.clear();
    insert_order_.clear();
    tick_ = 0;
}

} // namespace engine
