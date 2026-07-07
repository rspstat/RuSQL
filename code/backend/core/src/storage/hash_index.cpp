#include "engine/storage/hash_index.hpp"

#include <algorithm>

namespace engine {

namespace {
const std::vector<Row> kEmpty;
}

void HashIndex::rebuild(const std::vector<Row>& rows) {
    data_.clear();
    for (const auto& row : rows) {
        auto it = row.find(column);
        if (it != row.end()) data_[it->second].push_back(row);
    }
}

const std::vector<Row>& HashIndex::get(const std::string& key) const {
    auto it = data_.find(key);
    return it != data_.end() ? it->second : kEmpty;
}

void HashIndex::insert_row(const Row& row) {
    auto it = row.find(column);
    if (it != row.end()) data_[it->second].push_back(row);
}

void HashIndex::remove_row(const std::string& col_val, const std::string& pk_col, const std::string& pk_val) {
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

std::size_t HashIndex::row_count() const {
    std::size_t total = 0;
    for (auto& [k, v] : data_) total += v.size();
    return total;
}

} // namespace engine
