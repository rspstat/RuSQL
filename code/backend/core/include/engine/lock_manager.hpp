#pragma once

// Faithful port of rusql-core/src/engine/lock_manager.rs — row-level shared/exclusive
// locks with wait-for-chain deadlock detection.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace engine {

struct LockEntry {
    struct Exclusive { std::uint64_t holder; };
    struct Shared { std::unordered_set<std::uint64_t> holders; };
    std::variant<Exclusive, Shared> data;
};

struct LockResult {
    enum class Kind { Granted, Conflict, Deadlock };
    Kind kind;
    std::uint64_t holder = 0; // meaningful for Conflict/Deadlock

    static LockResult granted() { return LockResult{Kind::Granted, 0}; }
    static LockResult conflict(std::uint64_t h) { return LockResult{Kind::Conflict, h}; }
    static LockResult deadlock(std::uint64_t h) { return LockResult{Kind::Deadlock, h}; }
};

// Not internally synchronized — row_locks_/wait_for_/deadlock_history_ are plain
// containers with no mutex of their own. This is safe ONLY because every mutating touch
// (acquire/acquire_shared/release/insert_lock, reached from DML, SELECT ... FOR UPDATE/
// FOR SHARE, and COMMIT/ROLLBACK) is classified write-required by
// Executor::is_pure_read_only() and therefore always runs under SharedDatabase's
// exclusive shared->write() lock, fully serialized against every other statement
// including concurrent readers. The only read-classified access (SHOW LOCKS, via
// lock_rows()/wait_for_rows()/deadlock_history()) never mutates. If FOR UPDATE/FOR SHARE
// or any DML/transaction-control statement is ever reclassified as read-only, this class
// needs its own mutex added first — don't assume that invariant holds without re-checking
// is_pure_read_only().
class LockManager {
public:
    LockManager() = default;

    LockResult acquire(const std::string& table, const std::string& pk, std::uint64_t txn_id);
    LockResult acquire_shared(const std::string& table, const std::string& pk, std::uint64_t txn_id);
    void release(std::uint64_t txn_id);
    void insert_lock(const std::string& table, const std::string& pk, std::uint64_t txn_id);
    std::optional<std::uint64_t> holder(const std::string& table, const std::string& pk) const;

    std::vector<std::tuple<std::string, std::string, std::uint64_t>> lock_rows() const;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> wait_for_rows() const;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& deadlock_history() const { return deadlock_history_; }
    bool is_empty() const { return row_locks_.empty(); }

private:
    // (table, pk) key — a std::map (ordered) avoids needing a custom hash for
    // pair<string,string>; row lock tables are small so this has no practical cost.
    std::map<std::pair<std::string, std::string>, LockEntry> row_locks_;
    std::unordered_map<std::uint64_t, std::uint64_t> wait_for_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> deadlock_history_;

    bool creates_cycle(std::uint64_t from, std::uint64_t to) const;
};

} // namespace engine
