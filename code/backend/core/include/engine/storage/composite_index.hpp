#pragma once

// Faithful port of rusql-core/src/storage/composite_index.rs, reworked.
// 복합 인덱스: 여러 컬럼을 조합한 B+Tree 인덱스.
//
// Entry key = the indexed columns' values, each numerically normalized (numeric_key.hpp: "7" and "7.00" are one
// value, as in a WHERE clause), then the row's primary key: "val1\x00val2\x00...\x00pk1[\x00pk2...]".
// The primary-key suffix is what keeps a NON-UNIQUE index correct: with the columns alone as the key, two rows that
// agree on them (the normal case) shared one key and the later one silently overwrote the earlier, so
// `WHERE a = 1 AND b = 7` returned one row of three. Now every row has an entry of its own, a lookup is a bounded
// range scan over the entries whose leading columns match, and insert/remove stay plain key operations.

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/storage/btree.hpp"
#include "engine/row.hpp"

namespace engine {

class CompositeIndex {
public:
    CompositeIndex(std::string table, std::vector<std::string> columns, std::vector<std::string> pk_columns = {})
        : table(std::move(table)), columns(std::move(columns)), pk_columns(std::move(pk_columns)) {}

    std::string table;
    std::vector<std::string> columns;
    // The table's primary-key columns. Empty = unknown (no primary key): the entry then ends with the whole row's
    // values instead, so rows are still told apart and an UPDATE of any column moves the entry.
    std::vector<std::string> pk_columns;

    // Plain NUL-joined values (no normalization).
    static std::string make_key(const std::vector<std::string>& values);

    // This row's entry key; nullopt if the row lacks an indexed column.
    std::optional<std::string> key_from_row(const Row& row) const;
    void insert_row(const Row& row);
    void remove_row(const Row& row);

    // Every row whose first values.size() indexed columns equal `values` (numerically, like WHERE), as JSON
    // objects in index order. search_exact: JSON array of them, nullopt if there are none.
    std::vector<std::string> rows_with_prefix(const std::vector<std::string>& values) const;
    std::optional<std::string> search_exact(const std::vector<std::string>& values) const;
    bool matches_conditions(const std::unordered_map<std::string, std::string>& eq_map) const;
    std::optional<std::string> prefix_key_from_eq_map(const std::unordered_map<std::string, std::string>& eq_map) const;

    // Rows matching the longest run of leading indexed columns that `eq_map` fixes; nullopt if it fixes none.
    std::optional<std::vector<Row>> lookup(const std::unordered_map<std::string, std::string>& eq_map) const;

    void rebuild(const std::vector<Row>& rows);

private:
    BPlusTree tree_;
};

} // namespace engine
