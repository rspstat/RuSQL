#pragma once

// A cache, per table, of the UNIQUE values and composite keys that live rows hold. A one-row INSERT asks it "can a row hold this value?" instead of scanning the table
// for it: when the answer is "no" the scan is skipped, when it is "maybe" the scan runs as it always did. So the cache can only make a duplicate invisible by missing
// a live key -- it is therefore never kept up to date by guessing: it is stamped with how many times the locks that protect the table have been taken exclusively
// (every writer takes one of them that way), and it is current only while the stamp is. The one writer that updates it in place is the INSERT, which knows what it
// adds; anything else (UPDATE, DELETE, ROLLBACK, COMMIT, VACUUM, DDL ...) moves a count, the cache is no longer current and is made again from the rows when it is next
// wanted. See Executor::unique_probe_* (executor_unique_probe.cpp).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct UniqueProbeKeySpec {
    std::vector<std::string> columns;
    bool tuple = false;     // the key is the values of several columns (a composite primary key, UNIQUE (a, b)); otherwise one UNIQUE column
    bool skip_null = true;  // a NULL in the key never repeats (UNIQUE); a primary key has none
};

struct UniqueProbe {
    std::vector<UniqueProbeKeySpec> specs;
    std::string signature; // what the specs were made from: a changed schema makes new specs
    std::vector<std::unordered_map<std::string, std::uint32_t>> counts; // per spec: key -> number of live rows that hold it

    bool built = false;
    // the stamp of the locks when `counts` was last known to be right
    std::uint64_t locks_id = 0, data_id = 0, sum = 0;

    // when to build: not on the first lookups after the table was changed by something else than INSERTs (a table that is changed and inserted into in turn would
    // build it for every INSERT), but once the INSERTs come in a row
    std::uint64_t insert_tails = 0; // INSERT statements that have written to the table
    bool seen = false;
    std::uint64_t seen_locks_id = 0, seen_data_id = 0, seen_sum = 0, seen_insert_tails = 0;
    unsigned streak = 0;
};

struct UniqueProbes {
    std::mutex mutex; // guards `by_table` and every probe in it (lookups run under a shared data lock, so several can meet)
    std::unordered_map<std::string, UniqueProbe> by_table;
    bool enabled = true;      // RUSQL_UNIQUE_PROBE=0 turns the cache off
    bool check = false;       // RUSQL_PROBE_CHECK=1: every use compares the cache with a fresh count of the rows (for the test suite)
    unsigned streak_needed = 2; // RUSQL_PROBE_STREAK=n
    std::atomic<std::uint64_t> builds{0};  // how often a cache was made from the rows, and how many scans an answer of "absent" saved (for the tests)
    std::atomic<std::uint64_t> skipped{0};
    UniqueProbes();
};

} // namespace engine
