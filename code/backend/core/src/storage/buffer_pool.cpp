#include "engine/storage/buffer_pool.hpp"

namespace engine {

std::vector<Row> BufferPool::get_page(const std::string& table_name, const DiskManager& disk) {
    auto it = cache_.find(table_name);
    if (it != cache_.end()) {
        hit_count++;
        tick_++;
        it->second.second = tick_;
        return it->second.first.rows;
    }

    miss_count++;
    std::vector<Row> rows = disk.load_table(table_name);
    insert_page(table_name, rows, false, &disk);
    return rows;
}

void BufferPool::write_page(const std::string& table_name, std::vector<Row> rows) {
    auto it = cache_.find(table_name);
    if (it != cache_.end()) {
        tick_++;
        it->second.first.rows = std::move(rows);
        it->second.first.is_dirty = true;
        it->second.second = tick_;
    } else {
        insert_page(table_name, std::move(rows), true, nullptr);
    }
}

void BufferPool::flush_page(const std::string& table_name, const DiskManager& disk) {
    auto it = cache_.find(table_name);
    if (it != cache_.end() && it->second.first.is_dirty) {
        disk.save_table(table_name, it->second.first.rows);
        it->second.first.is_dirty = false;
    }
}

void BufferPool::flush_all(const DiskManager& disk) {
    std::vector<std::string> dirty;
    for (auto& [name, entry] : cache_) {
        if (entry.first.is_dirty) dirty.push_back(name);
    }
    for (auto& name : dirty) {
        auto it = cache_.find(name);
        if (it != cache_.end()) {
            disk.save_table(name, it->second.first.rows);
            it->second.first.is_dirty = false;
        }
    }
}

void BufferPool::invalidate(const std::string& table_name) {
    cache_.erase(table_name);
}

double BufferPool::hit_rate() const {
    std::uint64_t total = hit_count + miss_count;
    if (total == 0) return 0.0;
    return (static_cast<double>(hit_count) / static_cast<double>(total)) * 100.0;
}

void BufferPool::insert_page(std::string table_name, std::vector<Row> rows, bool is_dirty, const DiskManager* disk) {
    if (cache_.size() >= capacity) {
        // LRU: 가장 오래 전에 접근된 항목(최소 tick) 교체
        std::string evict_key;
        std::uint64_t min_tick = 0;
        bool found = false;
        for (auto& [name, entry] : cache_) {
            if (!found || entry.second < min_tick) {
                found = true;
                min_tick = entry.second;
                evict_key = name;
            }
        }
        if (found) {
            auto it = cache_.find(evict_key);
            if (it != cache_.end()) {
                if (it->second.first.is_dirty && disk) {
                    disk->save_table(evict_key, it->second.first.rows);
                }
                cache_.erase(it);
            }
        }
    }
    tick_++;
    Page page{table_name, std::move(rows), is_dirty};
    cache_.emplace(std::move(table_name), std::make_pair(std::move(page), tick_));
}

} // namespace engine
