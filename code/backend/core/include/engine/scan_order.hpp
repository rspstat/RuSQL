#pragma once

// The order a scan reads the rows of a table that has a PRIMARY KEY in. MySQL (InnoDB) reads such a table in the order of the key; the rows of a table lie here in
// the order they were written, and an UPDATE puts the new version of a row at the end. A scan puts the rows in key order when they are not in it, and to do that
// once per change of the table instead of once per scan, the order is kept: per table, the positions of all its row versions in key order (null when the rows
// already lie in key order, which is the usual case and costs nothing to read). It is stamped like the UNIQUE-value cache (engine/unique_probe.hpp) with how many times the
// locks that protect the table have been taken exclusively -- every writer takes one of them that way -- and it is current only while the stamp is. See
// Executor::scan_key_order (executor_select.cpp).

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct ScanOrder {
    std::uint64_t locks_id = 0, data_id = 0, sum = 0; // the stamp of the table's locks when `order` was made
    std::size_t rows = 0;                             // how many row versions it was made for
    std::shared_ptr<const std::vector<std::uint32_t>> order; // positions in key order; null: the rows lie in key order
};

struct ScanOrders {
    std::mutex mutex; // guards `by_table` (scans run under a shared data lock, so several can meet)
    std::unordered_map<std::string, ScanOrder> by_table;
};

} // namespace engine
