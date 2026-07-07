#include "engine/lock_manager.hpp"

#include <algorithm>

namespace engine {

LockResult LockManager::acquire(const std::string& table, const std::string& pk, std::uint64_t txn_id) {
    auto key = std::make_pair(table, pk);
    auto it = row_locks_.find(key);
    if (it == row_locks_.end()) {
        row_locks_[key] = LockEntry{LockEntry::Exclusive{txn_id}};
        return LockResult::granted();
    }

    LockEntry& entry = it->second;
    if (std::holds_alternative<LockEntry::Exclusive>(entry.data)) {
        std::uint64_t holder = std::get<LockEntry::Exclusive>(entry.data).holder;
        if (holder == txn_id) return LockResult::granted();
        if (creates_cycle(txn_id, holder)) {
            deadlock_history_.emplace_back(txn_id, holder);
            return LockResult::deadlock(holder);
        }
        wait_for_[txn_id] = holder;
        return LockResult::conflict(holder);
    }

    // Shared
    auto& holders = std::get<LockEntry::Shared>(entry.data).holders;
    if (holders.size() == 1 && holders.count(txn_id)) {
        row_locks_[key] = LockEntry{LockEntry::Exclusive{txn_id}};
        return LockResult::granted();
    }
    std::uint64_t holder = txn_id;
    for (auto h : holders) {
        if (h != txn_id) { holder = h; break; }
    }
    if (creates_cycle(txn_id, holder)) {
        deadlock_history_.emplace_back(txn_id, holder);
        return LockResult::deadlock(holder);
    }
    wait_for_[txn_id] = holder;
    return LockResult::conflict(holder);
}

LockResult LockManager::acquire_shared(const std::string& table, const std::string& pk, std::uint64_t txn_id) {
    auto key = std::make_pair(table, pk);
    auto it = row_locks_.find(key);
    if (it == row_locks_.end()) {
        LockEntry::Shared s;
        s.holders.insert(txn_id);
        row_locks_[key] = LockEntry{std::move(s)};
        return LockResult::granted();
    }

    LockEntry& entry = it->second;
    if (std::holds_alternative<LockEntry::Exclusive>(entry.data)) {
        std::uint64_t holder = std::get<LockEntry::Exclusive>(entry.data).holder;
        if (holder == txn_id) return LockResult::granted(); // 재진입
        if (creates_cycle(txn_id, holder)) {
            deadlock_history_.emplace_back(txn_id, holder);
            return LockResult::deadlock(holder);
        }
        wait_for_[txn_id] = holder;
        return LockResult::conflict(holder);
    }

    std::get<LockEntry::Shared>(entry.data).holders.insert(txn_id);
    return LockResult::granted();
}

void LockManager::release(std::uint64_t txn_id) {
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
}

void LockManager::insert_lock(const std::string& table, const std::string& pk, std::uint64_t txn_id) {
    row_locks_[std::make_pair(table, pk)] = LockEntry{LockEntry::Exclusive{txn_id}};
}

std::optional<std::uint64_t> LockManager::holder(const std::string& table, const std::string& pk) const {
    auto it = row_locks_.find(std::make_pair(table, pk));
    if (it == row_locks_.end()) return std::nullopt;
    if (std::holds_alternative<LockEntry::Exclusive>(it->second.data)) return std::get<LockEntry::Exclusive>(it->second.data).holder;
    return std::nullopt;
}

std::vector<std::tuple<std::string, std::string, std::uint64_t>> LockManager::lock_rows() const {
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
    std::vector<std::pair<std::uint64_t, std::uint64_t>> v(wait_for_.begin(), wait_for_.end());
    std::sort(v.begin(), v.end());
    return v;
}

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
