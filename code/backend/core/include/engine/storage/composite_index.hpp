#pragma once

// Faithful port of rusql-core/src/storage/composite_index.rs.
// 복합 인덱스: 여러 컬럼을 조합한 B+Tree 인덱스. 복합 키 형식: "val1\x00val2\x00..."

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/storage/btree.hpp"
#include "engine/row.hpp"

namespace engine {

class CompositeIndex {
public:
    CompositeIndex(std::string table, std::vector<std::string> columns)
        : table(std::move(table)), columns(std::move(columns)) {}

    std::string table;
    std::vector<std::string> columns;

    static std::string make_key(const std::vector<std::string>& values);

    std::optional<std::string> key_from_row(const Row& row) const;
    void insert_row(const Row& row);
    std::optional<std::string> search_exact(const std::vector<std::string>& values) const;
    bool matches_conditions(const std::unordered_map<std::string, std::string>& eq_map) const;
    std::optional<std::string> search_from_eq_map(const std::unordered_map<std::string, std::string>& eq_map) const;
    std::optional<std::string> prefix_key_from_eq_map(const std::unordered_map<std::string, std::string>& eq_map) const;
    std::vector<std::string> prefix_scan(const std::string& prefix) const;
    void rebuild(const std::vector<Row>& rows);

private:
    BPlusTree tree_;
};

} // namespace engine
