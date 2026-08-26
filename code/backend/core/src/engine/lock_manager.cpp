#include "engine/lock_manager.hpp"

#include <algorithm>

namespace engine {

LockManager::LockManager(LockManager&& other) noexcept {
    std::lock_guard<std::mutex> g(other.mutex_);
    row_locks_ = std::move(other.row_locks_);
    wait_for_ = std::move(other.wait_for_);
    deadlock_history_ = std::move(other.deadlock_history_);
    gap_locks_ = std::move(other.gap_locks_);
    predicate_reads_ = std::move(other.predicate_reads_);
    predicate_violations_ = std::move(other.predicate_violations_);
}

LockManager& LockManager::operator=(LockManager&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    row_locks_ = std::move(other.row_locks_);
    wait_for_ = std::move(other.wait_for_);
    deadlock_history_ = std::move(other.deadlock_history_);
    gap_locks_ = std::move(other.gap_locks_);
    predicate_reads_ = std::move(other.predicate_reads_);
    predicate_violations_ = std::move(other.predicate_violations_);
    return *this;
}

std::optional<std::uint64_t> LockManager::try_grant_exclusive_locked(const std::pair<std::string, std::string>& key,
                                                                       std::uint64_t txn_id) {
    auto it = row_locks_.find(key);
    if (it == row_locks_.end()) {
        row_locks_[key] = LockEntry{LockEntry::Exclusive{txn_id}};
        return std::nullopt;
    }

    LockEntry& entry = it->second;
    if (std::holds_alternative<LockEntry::Exclusive>(entry.data)) {
        std::uint64_t holder = std::get<LockEntry::Exclusive>(entry.data).holder;
        if (holder == txn_id) return std::nullopt;
        return holder;
    }

    // Shared
    auto& holders = std::get<LockEntry::Shared>(entry.data).holders;
    if (holders.size() == 1 && holders.count(txn_id)) {
        row_locks_[key] = LockEntry{LockEntry::Exclusive{txn_id}};
        return std::nullopt;
    }
    for (auto h : holders) {
        if (h != txn_id) return h;
    }
    return std::nullopt; // unreachable: holders is non-empty and not solely {txn_id}
}

std::optional<std::uint64_t> LockManager::try_grant_shared_locked(const std::pair<std::string, std::string>& key,
                                                                    std::uint64_t txn_id) {
    auto it = row_locks_.find(key);
    if (it == row_locks_.end()) {
        LockEntry::Shared s;
        s.holders.insert(txn_id);
        row_locks_[key] = LockEntry{std::move(s)};
        return std::nullopt;
    }

    LockEntry& entry = it->second;
    if (std::holds_alternative<LockEntry::Exclusive>(entry.data)) {
        std::uint64_t holder = std::get<LockEntry::Exclusive>(entry.data).holder;
        if (holder == txn_id) return std::nullopt; // 재진입
        return holder;
    }

    std::get<LockEntry::Shared>(entry.data).holders.insert(txn_id);
    return std::nullopt;
}

// Real-blocking-wait stage (see lock_manager.hpp's class-level doc comment): both
// acquire() and acquire_shared() share this exact shape --
//   1. Try to grant instantly (unchanged logic, now factored into try_grant_*_locked).
//   2. On conflict, check creates_cycle() ONCE, at this call's own start, exactly like
//      the pre-blocking version did -- this is deliberately NOT re-checked later while
//      asleep. Reasoning: a cycle that forms *after* this point is always completed by
//      some OTHER transaction's own acquire() call (the one that would create the last
//      edge closing the loop) -- that call detects it via the same creates_cycle() walk
//      over the live wait_for_ graph (this thread's edge, even while asleep, is exactly
//      what makes that walk succeed) and fails immediately as the deadlock "victim". This
//      thread just keeps waiting normally, matching standard victim-selection (the last
//      transaction to complete the cycle aborts, not every member of it).
//   3. If timeout <= 0, return Conflict immediately -- byte-identical to every call site
//      that never passes a timeout.
//   4. Otherwise, sleep on cv_ (notified by release(), the only place a lock ever becomes
//      newly grantable) up to the deadline. On every wake (spurious or real), re-derive
//      the row's CURRENT holder from row_locks_ -- never trust the holder captured before
//      sleeping, since the row can change hands to a different waiter while this thread
//      slept. Grant if now possible; otherwise refresh wait_for_[txn_id] to the current
//      holder (keeping other threads' future cycle checks accurate) and keep waiting,
//      unless the deadline has passed, in which case give up with Kind::Timeout.
LockResult LockManager::acquire(const std::string& table, const std::string& pk, std::uint64_t txn_id,
                                 std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mutex_);
    auto key = std::make_pair(table, pk);

    auto conflict_holder = try_grant_exclusive_locked(key, txn_id);
    if (!conflict_holder) return LockResult::granted();

    if (creates_cycle(txn_id, *conflict_holder)) {
        deadlock_history_.emplace_back(txn_id, *conflict_holder);
        return LockResult::deadlock(*conflict_holder);
    }
    wait_for_[txn_id] = *conflict_holder;

    if (timeout.count() <= 0) return LockResult::conflict(*conflict_holder);

    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        cv_.wait_until(lk, deadline);
        auto retry_holder = try_grant_exclusive_locked(key, txn_id);
        if (!retry_holder) {
            wait_for_.erase(txn_id);
            return LockResult::granted();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            wait_for_.erase(txn_id);
            return LockResult::timed_out(*retry_holder);
        }
        wait_for_[txn_id] = *retry_holder;
    }
}

LockResult LockManager::acquire_shared(const std::string& table, const std::string& pk, std::uint64_t txn_id,
                                        std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mutex_);
    auto key = std::make_pair(table, pk);

    auto conflict_holder = try_grant_shared_locked(key, txn_id);
    if (!conflict_holder) return LockResult::granted();

    if (creates_cycle(txn_id, *conflict_holder)) {
        deadlock_history_.emplace_back(txn_id, *conflict_holder);
        return LockResult::deadlock(*conflict_holder);
    }
    wait_for_[txn_id] = *conflict_holder;

    if (timeout.count() <= 0) return LockResult::conflict(*conflict_holder);

    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        cv_.wait_until(lk, deadline);
        auto retry_holder = try_grant_shared_locked(key, txn_id);
        if (!retry_holder) {
            wait_for_.erase(txn_id);
            return LockResult::granted();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            wait_for_.erase(txn_id);
            return LockResult::timed_out(*retry_holder);
        }
        wait_for_[txn_id] = *retry_holder;
    }
}

void LockManager::release(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(mutex_);
    for (auto it = row_locks_.begin(); it != row_locks_.end();) {
        bool keep = std::visit(
            [&](auto& alt) -> bool {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, LockEntry::Exclusive>) {
                    return alt.holder != txn_id;
                } else {
                    alt.holders.erase(txn_id);
                    return !alt.holders.empty();
                }
            },
            it->second.data);
        if (keep) ++it; else it = row_locks_.erase(it);
    }
    for (auto it = wait_for_.begin(); it != wait_for_.end();) {
        if (it->first == txn_id || it->second == txn_id) it = wait_for_.erase(it);
        else ++it;
    }
    for (auto it = gap_locks_.begin(); it != gap_locks_.end();) {
        auto& rows = it->second;
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const GapLockRow& r) { return r.holder == txn_id; }),
                   rows.end());
        if (rows.empty()) it = gap_locks_.erase(it); else ++it;
    }
    for (auto it = predicate_reads_.begin(); it != predicate_reads_.end();) {
        auto& rows = it->second;
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const GapLockRow& r) { return r.holder == txn_id; }),
                   rows.end());
        if (rows.empty()) it = predicate_reads_.erase(it); else ++it;
    }
    predicate_violations_.erase(txn_id);
    // The only place a row lock ever becomes newly grantable to someone else -- wakes
    // every thread blocked in acquire()/acquire_shared()'s real-wait loop so each can
    // re-check whether the row(s) it wants are now free.
    cv_.notify_all();
}

void LockManager::insert_lock(const std::string& table, const std::string& pk, std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(mutex_);
    row_locks_[std::make_pair(table, pk)] = LockEntry{LockEntry::Exclusive{txn_id}};
}

std::optional<std::uint64_t> LockManager::holder(const std::string& table, const std::string& pk) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = row_locks_.find(std::make_pair(table, pk));
    if (it == row_locks_.end()) return std::nullopt;
    if (std::holds_alternative<LockEntry::Exclusive>(it->second.data)) return std::get<LockEntry::Exclusive>(it->second.data).holder;
    return std::nullopt;
}

std::vector<std::tuple<std::string, std::string, std::uint64_t>> LockManager::lock_rows() const {
    std::lock_guard<std::mutex> g(mutex_);
    std::vector<std::tuple<std::string, std::string, std::uint64_t>> v;
    for (auto& [key, entry] : row_locks_) {
        std::visit(
            [&](const auto& alt) {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, LockEntry::Exclusive>) {
                    v.emplace_back(key.first, key.second, alt.holder);
                } else {
                    for (auto h : alt.holders) v.emplace_back(key.first, key.second, h);
                }
            },
            entry.data);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
        return std::get<1>(a) < std::get<1>(b);
    });
    return v;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> LockManager::wait_for_rows() const {
    std::lock_guard<std::mutex> g(mutex_);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> v(wait_for_.begin(), wait_for_.end());
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> LockManager::deadlock_history() const {
    std::lock_guard<std::mutex> g(mutex_);
    return deadlock_history_;
}

bool LockManager::is_empty() const {
    std::lock_guard<std::mutex> g(mutex_);
    return row_locks_.empty();
}

void LockManager::acquire_gap(const std::string& table, std::optional<std::string> lo, bool lo_inclusive,
                               std::optional<std::string> hi, bool hi_inclusive, std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(mutex_);
    GapLockRow row;
    row.lo = std::move(lo);
    row.hi = std::move(hi);
    row.lo_inclusive = lo_inclusive;
    row.hi_inclusive = hi_inclusive;
    row.holder = txn_id;
    gap_locks_[table].push_back(std::move(row));
}

std::vector<GapLockRow> LockManager::gap_locks_for(const std::string& table) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = gap_locks_.find(table);
    if (it == gap_locks_.end()) return {};
    return it->second;
}

LockResult LockManager::register_gap_conflict(const std::string& table, std::uint64_t txn_id, std::uint64_t holder,
                                                std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mutex_);
    if (creates_cycle(txn_id, holder)) {
        deadlock_history_.emplace_back(txn_id, holder);
        return LockResult::deadlock(holder);
    }
    wait_for_[txn_id] = holder;

    if (timeout.count() <= 0) return LockResult::conflict(holder);

    auto still_held = [&] {
        auto it = gap_locks_.find(table);
        if (it == gap_locks_.end()) return false;
        return std::any_of(it->second.begin(), it->second.end(), [&](const GapLockRow& r) { return r.holder == holder; });
    };
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        cv_.wait_until(lk, deadline);
        if (!still_held()) {
            wait_for_.erase(txn_id);
            return LockResult::granted();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            wait_for_.erase(txn_id);
            return LockResult::timed_out(holder);
        }
    }
}

std::vector<std::tuple<std::string, std::string, std::uint64_t>> LockManager::gap_lock_rows() const {
    std::lock_guard<std::mutex> g(mutex_);
    std::vector<std::tuple<std::string, std::string, std::uint64_t>> v;
    for (auto& [table, rows] : gap_locks_) {
        for (auto& r : rows) {
            std::string range;
            range += r.lo_inclusive ? "[" : "(";
            range += r.lo ? *r.lo : "-inf";
            range += ", ";
            range += r.hi ? *r.hi : "+inf";
            range += r.hi_inclusive ? "]" : ")";
            v.emplace_back(table, range, r.holder);
        }
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
        return std::get<1>(a) < std::get<1>(b);
    });
    return v;
}

void LockManager::register_predicate_read(const std::string& table, std::optional<std::string> lo, bool lo_inclusive,
                                            std::optional<std::string> hi, bool hi_inclusive, std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(mutex_);
    GapLockRow row;
    row.lo = std::move(lo);
    row.hi = std::move(hi);
    row.lo_inclusive = lo_inclusive;
    row.hi_inclusive = hi_inclusive;
    row.holder = txn_id;
    predicate_reads_[table].push_back(std::move(row));
}

std::vector<GapLockRow> LockManager::predicate_reads_for(const std::string& table) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = predicate_reads_.find(table);
    if (it == predicate_reads_.end()) return {};
    return it->second;
}

void LockManager::flag_predicate_violation(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(mutex_);
    predicate_violations_.insert(txn_id);
}

bool LockManager::take_predicate_violation(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = predicate_violations_.find(txn_id);
    if (it == predicate_violations_.end()) return false;
    predicate_violations_.erase(it);
    return true;
}

std::vector<std::tuple<std::string, std::string, std::uint64_t>> LockManager::predicate_lock_rows() const {
    std::lock_guard<std::mutex> g(mutex_);
    std::vector<std::tuple<std::string, std::string, std::uint64_t>> v;
    for (auto& [table, rows] : predicate_reads_) {
        for (auto& r : rows) {
            std::string range;
            range += r.lo_inclusive ? "[" : "(";
            range += r.lo ? *r.lo : "-inf";
            range += ", ";
            range += r.hi ? *r.hi : "+inf";
            range += r.hi_inclusive ? "]" : ")";
            v.emplace_back(table, range, r.holder);
        }
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
        return std::get<1>(a) < std::get<1>(b);
    });
    return v;
}

RowClaimGuard::~RowClaimGuard() {
    if (owns_) lm_->release(txn_id_);
}

// Internal -- called only from acquire()/acquire_shared(), which already hold mutex_.
bool LockManager::creates_cycle(std::uint64_t from, std::uint64_t to) const {
    std::uint64_t current = to;
    std::unordered_set<std::uint64_t> visited;
    for (;;) {
        if (current == from) return true;
        if (!visited.insert(current).second) return false;
        auto it = wait_for_.find(current);
        if (it == wait_for_.end()) return false;
        current = it->second;
    }
}

} // namespace engine
