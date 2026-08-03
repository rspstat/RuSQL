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

private:
    mutable std::mutex mutex_;
    // (table, pk) key — a std::map (ordered) avoids needing a custom hash for
    // pair<string,string>; row lock tables are small so this has no practical cost.
    std::map<std::pair<std::string, std::string>, LockEntry> row_locks_;
    std::unordered_map<std::uint64_t, std::uint64_t> wait_for_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> deadlock_history_;

    // Internal, called only while mutex_ is already held.
    bool creates_cycle(std::uint64_t from, std::uint64_t to) const;
};

} // namespace engine
