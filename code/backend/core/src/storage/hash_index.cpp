#include "engine/storage/hash_index.hpp"

#include <algorithm>

namespace engine {

HashIndex::HashIndex(const HashIndex& other) : table(other.table), column(other.column) {
    std::lock_guard<std::mutex> g(other.mutex_);
    data_ = other.data_;
}

HashIndex& HashIndex::operator=(const HashIndex& other) {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        table = other.table;
        column = other.column;
        data_ = other.data_;
    }
    return *this;
}

// mutex_ isn't movable -- manually moves the other members, leaving a fresh mutex_ in
// the moved-to object (same pattern LockManager/BPlusTree/QueryResultCache already use).
HashIndex::HashIndex(HashIndex&& other) noexcept : table(std::move(other.table)), column(std::move(other.column)) {
    std::lock_guard<std::mutex> g(other.mutex_);
    data_ = std::move(other.data_);
}

HashIndex& HashIndex::operator=(HashIndex&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    table = std::move(other.table);
    column = std::move(other.column);
    data_ = std::move(other.data_);
    return *this;
}

void HashIndex::rebuild(const std::vector<Row>& rows) {
    std::lock_guard<std::mutex> g(mutex_);
    data_.clear();
    for (const auto& row : rows) {
        auto it = row.find(column);
        if (it != row.end()) data_[it->second].push_back(row);
    }
}

std::vector<Row> HashIndex::get(const std::string& key) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = data_.find(key);
    return it != data_.end() ? it->second : std::vector<Row>{};
}

void HashIndex::insert_row(const Row& row) {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = row.find(column);
    if (it != row.end()) data_[it->second].push_back(row);
}

void HashIndex::remove_row(const std::string& col_val, const std::string& pk_col, const std::string& pk_val) {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = data_.find(col_val);
    if (it == data_.end()) return;
    auto& bucket = it->second;
    bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                 [&](const Row& r) {
                                     auto pit = r.find(pk_col);
                                     return pit != r.end() && pit->second == pk_val;
                                 }),
                 bucket.end());
    if (bucket.empty()) data_.erase(it);
}

std::size_t HashIndex::bucket_count() const {
    std::lock_guard<std::mutex> g(mutex_);
    return data_.size();
}

std::size_t HashIndex::row_count() const {
    std::lock_guard<std::mutex> g(mutex_);
    std::size_t total = 0;
    for (auto& [k, v] : data_) total += v.size();
    return total;
}

} // namespace engine
