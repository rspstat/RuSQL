#pragma once

// Faithful port of rusql-core/src/engine/lock_manager.rs — row-level shared/exclusive
// locks with wait-for-chain deadlock detection.

#include <cstdint>
#include <map>
#include <mutex>
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

// Gap lock (InnoDB-style phantom-read prevention): a range over a table's PK values held
// by a transaction. Unlike row locks, gap locks never conflict with each other at
// acquire time -- only a later INSERT whose PK value falls inside the range conflicts
// (see LockManager::register_gap_conflict). Bound comparison/containment is deliberately
// NOT done here -- LockManager stays SQL-semantics-free; the executor layer (which knows
// the numeric-vs-lexicographic comparison rules used elsewhere for BETWEEN/Gt/Lt/...)
// decides containment and only calls register_gap_conflict once it already knows which
// holder conflicts.
struct GapLockRow {
    std::optional<std::string> lo;
    std::optional<std::string> hi;
    bool lo_inclusive = true;
    bool hi_inclusive = true;
    std::uint64_t holder = 0;
};

// Stage 4 (table-level concurrency): guarded by its own mutex_ so that two sessions
// operating on different tables (each holding only that table's own lock, not a single
// whole-database exclusive lock anymore) can safely call acquire/acquire_shared/release
// concurrently. Semantics are unchanged from before -- still "detect a conflict/deadlock
// and fail immediately," never a real blocking wait; that would be row-level concurrency
// (explicitly out of scope for this stage). All public methods take mutex_ internally;
// deadlock_history()/is_empty() were changed to return by value (a copy) instead of by
// reference, since a reference into row_locks_/deadlock_history_ would no longer be safe
// to read after the method returns and the lock is released.
class LockManager {
public:
    LockManager() = default;
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;
    // mutex_ isn't movable -- manually moves the other members, leaving a fresh mutex_ in
    // the moved-to object (same pattern as BufferPool, which SharedDatabase already relies
    // on for its own move-construction).
    LockManager(LockManager&& other) noexcept;
    LockManager& operator=(LockManager&& other) noexcept;

    LockResult acquire(const std::string& table, const std::string& pk, std::uint64_t txn_id);
    LockResult acquire_shared(const std::string& table, const std::string& pk, std::uint64_t txn_id);
    void release(std::uint64_t txn_id);
    void insert_lock(const std::string& table, const std::string& pk, std::uint64_t txn_id);
    std::optional<std::uint64_t> holder(const std::string& table, const std::string& pk) const;

    std::vector<std::tuple<std::string, std::string, std::uint64_t>> lock_rows() const;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> wait_for_rows() const;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> deadlock_history() const;
    bool is_empty() const;

    // Gap locks: always granted immediately (gap-vs-gap never conflicts). The executor
    // decides range containment and calls register_gap_conflict only when it already
    // knows a specific holder's range collides with a value being inserted.
    void acquire_gap(const std::string& table, std::optional<std::string> lo, bool lo_inclusive,
                      std::optional<std::string> hi, bool hi_inclusive, std::uint64_t txn_id);
    std::vector<GapLockRow> gap_locks_for(const std::string& table) const;
    LockResult register_gap_conflict(std::uint64_t txn_id, std::uint64_t holder);
    std::vector<std::tuple<std::string, std::string, std::uint64_t>> gap_lock_rows() const;

private:
    mutable std::mutex mutex_;
    // (table, pk) key — a std::map (ordered) avoids needing a custom hash for
    // pair<string,string>; row lock tables are small so this has no practical cost.
    std::map<std::pair<std::string, std::string>, LockEntry> row_locks_;
    std::unordered_map<std::uint64_t, std::uint64_t> wait_for_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> deadlock_history_;
    std::unordered_map<std::string, std::vector<GapLockRow>> gap_locks_;

    // Internal, called only while mutex_ is already held.
    bool creates_cycle(std::uint64_t from, std::uint64_t to) const;
};

} // namespace engine
