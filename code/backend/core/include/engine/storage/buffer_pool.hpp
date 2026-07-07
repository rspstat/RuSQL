#pragma once

// Faithful port of rusql-core/src/storage/buffer_pool.rs — LRU buffer pool using a
// monotonic tick clock (O(1) hit, O(n) eviction scan only when the pool is full).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/storage/disk.hpp"
#include "engine/row.hpp"

namespace engine {

constexpr std::size_t BUFFER_POOL_SIZE = 64;

struct Page {
    std::string table_name;
    std::vector<Row> rows;
    bool is_dirty = false;
};

class BufferPool {
public:
    BufferPool() : BufferPool(BUFFER_POOL_SIZE) {}
    explicit BufferPool(std::size_t capacity) : capacity(capacity) {}

    std::vector<Row> get_page(const std::string& table_name, const DiskManager& disk);
    void write_page(const std::string& table_name, std::vector<Row> rows);
    void flush_page(const std::string& table_name, const DiskManager& disk);
    void flush_all(const DiskManager& disk);
    void invalidate(const std::string& table_name);

    double hit_rate() const;
    std::size_t usage() const { return cache_.size(); }

    std::size_t capacity;
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;

private:
    std::unordered_map<std::string, std::pair<Page, std::uint64_t>> cache_;
    std::uint64_t tick_ = 0;

    void insert_page(std::string table_name, std::vector<Row> rows, bool is_dirty, const DiskManager* disk);
};

} // namespace engine
