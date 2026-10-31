#pragma once

// The slow query log (R59): a statement that ran for at least long_query_time seconds is counted (SHOW STATUS Slow_queries, whether the log is on or not, as in MySQL) and,
// when slow_query_log is ON, written down: in a list in memory that information_schema.slow_log shows (the last kMaxEntries), and as a block of text at the end of
// <data dir>/slow_query.log in the shape MySQL's slow log file has. The switches belong to the server, not to a session: SET GLOBAL slow_query_log = ON and SET GLOBAL
// long_query_time = 0.5 are read by every statement of every session through two atomics, so a statement that is not slow costs one load and one comparison.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace engine {

struct SlowLogEntry {
    std::string start_time;  // "YYYY-MM-DD HH:MM:SS.ffffff", when the statement began
    std::string user_host;   // "user[user] @ host [host]"
    double query_seconds = 0;
    std::uint64_t rows_sent = 0;
    std::string db;
    std::string sql;
    std::size_t thread_id = 0;
};

class SlowLog {
public:
    static constexpr std::size_t kMaxEntries = 1000; // (the oldest go first)
    static constexpr std::size_t kMaxSqlBytes = 65536;

    std::atomic<bool> enabled{false};
    std::atomic<std::int64_t> threshold_us{10'000'000}; // long_query_time, 10 seconds as in MySQL
    std::atomic<std::uint64_t> version{0};               // bumped when a switch changes: a session re-reads @@slow_query_log and @@long_query_time then

    void set_directory(const std::string& data_dir) { file_path_ = data_dir.empty() ? std::string() : data_dir + "/slow_query.log"; }

    void record(SlowLogEntry entry) {
        if (entry.sql.size() > kMaxSqlBytes) entry.sql.resize(kMaxSqlBytes);
        std::lock_guard<std::mutex> guard(mutex_);
        if (!file_path_.empty()) append_to_file(entry);
        entries_.push_back(std::move(entry));
        if (entries_.size() > kMaxEntries) entries_.pop_front();
    }

    std::vector<SlowLogEntry> entries() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return std::vector<SlowLogEntry>(entries_.begin(), entries_.end());
    }

private:
    // # Time: ...  /  # User@Host: ...  /  # Query_time: 1.234567  Rows_sent: 3  /  use db;  /  SET timestamp=...;  /  the statement;
    void append_to_file(const SlowLogEntry& e) const {
        std::ofstream out(file_path_, std::ios::app | std::ios::binary);
        if (!out) return;
        char seconds[32];
        std::snprintf(seconds, sizeof seconds, "%.6f", e.query_seconds);
        out << "# Time: " << e.start_time << "\n# User@Host: " << e.user_host << "  Id: " << e.thread_id << "\n# Query_time: " << seconds << "  Rows_sent: " << e.rows_sent << "\n";
        if (!e.db.empty()) out << "use " << e.db << ";\n";
        out << e.sql << (e.sql.empty() || e.sql.back() != ';' ? ";" : "") << "\n";
    }

    mutable std::mutex mutex_;
    std::deque<SlowLogEntry> entries_;
    std::string file_path_;
};

} // namespace engine
