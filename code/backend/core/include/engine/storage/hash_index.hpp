#pragma once

// Faithful port of rusql-core/src/storage/hash_index.rs.

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/row.hpp"

namespace engine {

// Row-level-concurrency Stage 2: guarded by its own mutex_, same pattern/reasoning as
// BPlusTree -- one mutex per instance, held only for the duration of a single call.
// get() returns a copy rather than a reference into data_ -- a reference would dangle
// the instant the lock is released, since a concurrent insert_row()/remove_row() on an
// unrelated key can still rehash the underlying unordered_map.
class HashIndex {
public:
    HashIndex(std::string table, std::string column) : table(std::move(table)), column(std::move(column)) {}
    HashIndex(const HashIndex& other);
    HashIndex& operator=(const HashIndex& other);
    HashIndex(HashIndex&& other) noexcept;
    HashIndex& operator=(HashIndex&& other) noexcept;

    std::string table;
    std::string column;

    /// 테이블 전체 행으로 인덱스를 (재)빌드한다.
    void rebuild(const std::vector<Row>& rows);

    /// key에 해당하는 행 슬라이스를 반환한다.
    std::vector<Row> get(const std::string& key) const;

    /// 단일 행을 인덱스에 추가한다 (O(1)).
    void insert_row(const Row& row);

    /// PK 값이 pk_val인 행을 col_val 버킷에서 제거한다 (O(bucket size)).
    void remove_row(const std::string& col_val, const std::string& pk_col, const std::string& pk_val);

    std::size_t bucket_count() const;
    std::size_t row_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Row>> data_;
};

} // namespace engine
