#pragma once

// Faithful port of rusql-core/src/engine/lock_manager.rs — row-level shared/exclusive
// locks with wait-for-chain deadlock detection.

#include <chrono>
#include <condition_variable>
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
    // Timeout is distinct from Conflict: Conflict is the instant (timeout==0) "no, right
    // now" answer this class has always given; Timeout means a real blocking wait (see
    // acquire()/acquire_shared()'s `timeout` parameter) ran for the full requested
    // duration without ever becoming grantable. Existing callers that never pass a
    // timeout only ever see Conflict, unchanged.
    enum class Kind { Granted, Conflict, Deadlock, Timeout };
    Kind kind;
    std::uint64_t holder = 0; // meaningful for Conflict/Deadlock/Timeout

    static LockResult granted() { return LockResult{Kind::Granted, 0}; }
    static LockResult conflict(std::uint64_t h) { return LockResult{Kind::Conflict, h}; }
    static LockResult deadlock(std::uint64_t h) { return LockResult{Kind::Deadlock, h}; }
    static LockResult timed_out(std::uint64_t h) { return LockResult{Kind::Timeout, h}; }
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
// concurrently. All public methods take mutex_ internally; deadlock_history()/is_empty()
// were changed to return by value (a copy) instead of by reference, since a reference
// into row_locks_/deadlock_history_ would no longer be safe to read after the method
// returns and the lock is released.
//
// Real-blocking-wait stage: acquire()/acquire_shared() gained an optional `timeout`
// parameter (default 0 = the original instant "Conflict, no wait" answer, byte-identical
// to every pre-existing call site). When timeout > 0 and the row is contended, the
// calling thread genuinely sleeps on cv_ (notified by release()) up to that duration
// before giving up with Kind::Timeout -- see acquire()'s doc comment in lock_manager.cpp
// for the full reasoning (why deadlock-cycle detection only needs to run at each
// acquire() call's own start, never while a thread sleeps). register_gap_conflict() gained
// the identical `timeout` parameter in a later pass (INSERT-vs-gap-lock blocking) -- see
// its own doc comment below. acquire_gap() itself was NOT extended: gap-vs-gap never
// conflicts, so it's always granted instantly by design, not just by omission.
class LockManager {
public:
    LockManager() = default;
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;
    // mutex_/cv_ aren't movable -- manually moves the other members, leaving a fresh
    // mutex_/cv_ in the moved-to object (same pattern as BufferPool, which SharedDatabase
    // already relies on for its own move-construction).
    LockManager(LockManager&& other) noexcept;
    LockManager& operator=(LockManager&& other) noexcept;

    LockResult acquire(const std::string& table, const std::string& pk, std::uint64_t txn_id,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    LockResult acquire_shared(const std::string& table, const std::string& pk, std::uint64_t txn_id,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
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
    // Real-blocking-wait stage, extended to INSERT-vs-Gap-Lock: `timeout` defaults to 0 --
    // the original instant "Conflict, no wait" answer, byte-identical to every pre-existing
    // call site. When timeout > 0 and `holder` still holds a gap lock on `table`, the
    // calling thread genuinely sleeps on cv_ (notified by release(), which already sweeps
    // gap_locks_ for the releasing txn -- see release()'s doc comment) until `holder` has
    // no gap lock left on `table`, or the deadline passes with Kind::Timeout. Unlike a row
    // lock, a gap lock's holder never changes hands mid-wait (release() only ever removes
    // entries, acquire_gap() never conflicts with an existing one) -- so, unlike
    // acquire()/acquire_shared(), there's no "current holder" to re-derive on each wake.
    LockResult register_gap_conflict(const std::string& table, std::uint64_t txn_id, std::uint64_t holder,
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    std::vector<std::tuple<std::string, std::string, std::uint64_t>> gap_lock_rows() const;

private:
    mutable std::mutex mutex_;
    // Notified (notify_all) from release() -- the only place a row lock ever becomes
    // newly grantable to someone else. Waiters in acquire()/acquire_shared()'s blocking
    // loop wake, re-derive the row's CURRENT holder from row_locks_ (never trust a
    // pre-sleep snapshot), and either succeed or go back to sleep.
    std::condition_variable cv_;
    // (table, pk) key — a std::map (ordered) avoids needing a custom hash for
    // pair<string,string>; row lock tables are small so this has no practical cost.
    std::map<std::pair<std::string, std::string>, LockEntry> row_locks_;
    std::unordered_map<std::uint64_t, std::uint64_t> wait_for_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> deadlock_history_;
    std::unordered_map<std::string, std::vector<GapLockRow>> gap_locks_;

    // Internal, called only while mutex_ is already held. Returns nullopt if `txn_id` was
    // granted the lock (row_locks_ already updated to reflect it); otherwise returns the
    // txn_id of a representative blocking holder (for Exclusive: the sole holder; for
    // Shared: any one holder other than txn_id itself).
    std::optional<std::uint64_t> try_grant_exclusive_locked(const std::pair<std::string, std::string>& key, std::uint64_t txn_id);
    std::optional<std::uint64_t> try_grant_shared_locked(const std::pair<std::string, std::string>& key, std::uint64_t txn_id);

    // Internal, called only while mutex_ is already held.
    bool creates_cycle(std::uint64_t from, std::uint64_t to) const;
};

// Row-level-concurrency prep: today, LockManager's row claims are only ever taken inside
// an explicit transaction (`if (cur_txn != 0)`/`txn.is_active()` gates at each call site)
// -- release() only ever runs at COMMIT/ROLLBACK, which exist precisely because an
// explicit transaction's claims must survive across multiple statements. Extending claims
// to autocommit statements (so two autocommit writers touching different rows of the same
// table can be told apart from two writers racing the same row) has no such natural
// release point -- an autocommit statement has no COMMIT/ROLLBACK of its own. RowClaimGuard
// is that release point: constructed with `owns=true` for a fresh, statement-scoped
// one-off id (autocommit), it calls release() in its destructor on every exit path
// (including an early `return StringResult::Err(...)` from a constraint violation, lock
// conflict, etc.) so a claim can never leak. Constructed with `owns=false` when reusing an
// explicit transaction's own long-lived id, it does nothing on destruction -- that
// transaction's claims must keep living until its real COMMIT/ROLLBACK calls release()
// itself, not get dropped after just one statement.
class RowClaimGuard {
public:
    RowClaimGuard(LockManager& lm, std::uint64_t txn_id, bool owns) : lm_(&lm), txn_id_(txn_id), owns_(owns) {}
    ~RowClaimGuard();

    RowClaimGuard(const RowClaimGuard&) = delete;
    RowClaimGuard& operator=(const RowClaimGuard&) = delete;
    RowClaimGuard(RowClaimGuard&& other) noexcept : lm_(other.lm_), txn_id_(other.txn_id_), owns_(other.owns_) {
        other.owns_ = false;
    }
    RowClaimGuard& operator=(RowClaimGuard&&) = delete;

private:
    LockManager* lm_;
    std::uint64_t txn_id_;
    bool owns_;
};

} // namespace engine
