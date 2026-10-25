#pragma once

// What SHOW STATUS counts (R53): statements by kind, connections, how long the server has been up, and the numbers of the buffer pool and the row locks. The counters are
// atomics that a statement or a connection bumps without taking any lock of the database, and SHOW STATUS reads them the same way, so neither waits for a long
// statement that is writing. The buffer pool and the lock manager are the database's own objects: the pointers are set once, when the engine starts.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine {

class BufferPool;
class LockManager;

// The counters that count the statements, one for each kind MySQL's Com_<kind> counts that RuSQL has, and Questions for all of them.
enum class Stat : int {
    Questions, ComSelect, ComInsert, ComUpdate, ComDelete, ComReplace, ComBegin, ComCommit, ComRollback, ComCall,
    ComCreateTable, ComDropTable, ComAlterTable, ComCreateIndex, ComDropIndex, ComKill, Count
};
inline constexpr int kStatCount = static_cast<int>(Stat::Count);

inline const char* stat_name(Stat s) {
    static const char* const names[kStatCount] = {"Questions", "Com_select", "Com_insert", "Com_update", "Com_delete", "Com_replace", "Com_begin", "Com_commit",
                                                   "Com_rollback", "Com_call", "Com_create_table", "Com_drop_table", "Com_alter_table", "Com_create_index",
                                                   "Com_drop_index", "Com_kill"};
    return names[static_cast<int>(s)];
}

// The kind a statement is counted as (besides Questions), from its first word and, for CREATE / DROP / ALTER, the second: both upper case.
inline std::optional<Stat> stat_of_statement(const std::string& first, const std::string& second) {
    if (first == "SELECT" || first == "WITH") return Stat::ComSelect;
    if (first == "INSERT") return Stat::ComInsert;
    if (first == "UPDATE") return Stat::ComUpdate;
    if (first == "DELETE") return Stat::ComDelete;
    if (first == "REPLACE") return Stat::ComReplace;
    if (first == "BEGIN" || first == "START") return Stat::ComBegin;
    if (first == "COMMIT") return Stat::ComCommit;
    if (first == "ROLLBACK") return Stat::ComRollback;
    if (first == "CALL") return Stat::ComCall;
    if (first == "KILL") return Stat::ComKill;
    if (first == "CREATE" && second == "TABLE") return Stat::ComCreateTable;
    if (first == "DROP" && second == "TABLE") return Stat::ComDropTable;
    if (first == "ALTER" && second == "TABLE") return Stat::ComAlterTable;
    if ((first == "CREATE") && (second == "INDEX" || second == "UNIQUE")) return Stat::ComCreateIndex;
    if (first == "DROP" && second == "INDEX") return Stat::ComDropIndex;
    return std::nullopt;
}

struct ServerCounters {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::array<std::atomic<std::uint64_t>, kStatCount> statements{}; // every statement the clients sent, by kind
    std::atomic<std::uint64_t> connections{0};          // connections that signed in
    std::atomic<std::uint64_t> max_used_connections{0}; // the most that were connected at once
    std::atomic<std::uint64_t> aborted_connects{0};     // sign-ins that were refused
    const BufferPool* buffer_pool = nullptr;
    const LockManager* lock_mgr = nullptr;
};

// The rows of SHOW STATUS: (name, value). `session` (the session's own count of each statement kind) is what SESSION STATUS shows for the statement counters; nullptr
// gives the server's own counts (GLOBAL STATUS).
std::vector<std::pair<std::string, std::string>> server_status_values(const ServerCounters& counters, std::size_t threads_connected, std::size_t threads_running,
                                                                      std::uint64_t redo_log_bytes, const std::array<std::uint64_t, kStatCount>* session);

} // namespace engine
