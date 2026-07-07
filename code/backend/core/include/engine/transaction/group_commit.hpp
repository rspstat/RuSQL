#pragma once

// Faithful port of rusql-core/src/transaction/group_commit.rs.
//
// 여러 세션이 동시에 커밋할 때 fsync를 한 번으로 묶는다. 첫 번째 도착 세션(leader)이
// fsync를 수행하고, 그 사이에 도착한 세션(follower)은 leader의 fsync 완료를 기다린다.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "engine/transaction/wal.hpp"

namespace engine {

class GroupCommitCoordinator {
public:
    GroupCommitCoordinator() : GroupCommitCoordinator(std::string(WAL_PATH)) {}
    explicit GroupCommitCoordinator(std::string wal_path) : wal_path_(std::move(wal_path)) {}

    /// COMMIT 레코드를 WAL에 기록한 직후 호출. 함수가 반환되면 COMMIT 레코드가 반드시
    /// 디스크에 영속화되어 있다.
    void sync_commit();

private:
    std::mutex mutex_;
    std::condition_variable cvar_;
    bool flushing_ = false;
    std::uint64_t generation_ = 0;
    std::string wal_path_;
};

} // namespace engine
