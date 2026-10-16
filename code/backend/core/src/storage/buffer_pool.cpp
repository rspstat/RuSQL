#include "engine/storage/buffer_pool.hpp"

#include <algorithm>
#include <cstdlib>

namespace engine {

BufferPool::BufferPool(BufferPool&& other) noexcept
    : capacity(other.capacity),
      hit_count(other.hit_count.load()),
      miss_count(other.miss_count.load()),
      cache_(std::move(other.cache_)),
      tick_(other.tick_),
      open_transactions_(std::move(other.open_transactions_)) {}

BufferPool& BufferPool::operator=(BufferPool&& other) noexcept {
    if (this == &other) return *this;
    capacity = other.capacity;
    hit_count.store(other.hit_count.load());
    miss_count.store(other.miss_count.load());
    cache_ = std::move(other.cache_);
    tick_ = other.tick_;
    open_transactions_ = std::move(other.open_transactions_);
    return *this;
}

std::vector<Row> BufferPool::get_page(const std::string& table_name, const DiskManager& disk) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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

namespace {
// The transaction id a row's _xmin / _xmax names (0: none -- "0" is the live sentinel of _xmax).
std::uint64_t txn_id_of(const Row& row, const char* field) {
    auto it = row.find(field);
    if (it == row.end() || it->second == "0") return 0;
    return std::strtoull(it->second.c_str(), nullptr, 10);
}
} // namespace

// Writes `rows` as the table's file, leaving out what the transactions that are still open have done to them. Called with mutex_ held.
void BufferPool::save_committed(const DiskManager& disk, const std::string& table_name, const std::vector<Row>& rows, std::uint64_t keep_txn) {
    std::unordered_set<std::uint64_t> open;
    if (open_transactions_) open = open_transactions_();
    open.erase(keep_txn);
    const bool affected = !open.empty() && std::any_of(rows.begin(), rows.end(), [&](const Row& r) {
        return open.count(txn_id_of(r, "_xmin")) || open.count(txn_id_of(r, "_xmax"));
    });
    if (!affected) {
        disk.save_table(table_name, rows);
        return;
    }
    std::vector<Row> committed;
    committed.reserve(rows.size());
    for (const Row& r : rows) {
        if (open.count(txn_id_of(r, "_xmin"))) continue; // created by a transaction that has not committed
        committed.push_back(r);
        if (open.count(txn_id_of(r, "_xmax"))) committed.back()["_xmax"] = "0"; // ended by one
    }
    disk.save_table(table_name, committed);
}

void BufferPool::write_through(const std::string& table_name, const std::vector<Row>& rows, const DiskManager& disk, std::uint64_t keep_txn) {
    std::lock_guard<std::mutex> lock(mutex_);
    save_committed(disk, table_name, rows, keep_txn);
    cache_.erase(table_name);
}

void BufferPool::flush_all(const DiskManager& disk) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> dirty;
    for (auto& [name, entry] : cache_) {
        if (entry.first.is_dirty) dirty.push_back(name);
    }
    for (auto& name : dirty) {
        auto it = cache_.find(name);
        if (it != cache_.end()) {
            save_committed(disk, name, it->second.first.rows, 0);
            it->second.first.is_dirty = false;
        }
    }
}

void BufferPool::invalidate(const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(table_name);
}

std::size_t BufferPool::usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

double BufferPool::hit_rate() const {
    std::uint64_t h = hit_count.load();
    std::uint64_t m = miss_count.load();
    std::uint64_t total = h + m;
    if (total == 0) return 0.0;
    return (static_cast<double>(h) / static_cast<double>(total)) * 100.0;
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
                    save_committed(*disk, evict_key, it->second.first.rows, 0);
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
