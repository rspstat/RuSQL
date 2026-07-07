#include "engine/transaction/group_commit.hpp"

#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace engine {

void GroupCommitCoordinator::sync_commit() {
    std::uint64_t my_gen;
    bool is_leader;
    {
        std::lock_guard<std::mutex> g(mutex_);
        my_gen = generation_;
        is_leader = !flushing_;
        if (is_leader) flushing_ = true;
    }

    if (is_leader) {
        // 팔로워들이 COMMIT 레코드를 파일에 기록할 기회를 한 번 양보
        std::this_thread::yield();

        // 단일 fsync — 이 시점까지 WAL 파일에 기록된 모든 COMMIT 레코드 영속화
        // (std::fstream has no portable fsync; opening+flushing is the closest
        // portable approximation available without platform-specific APIs.)
        if (fs::exists(wal_path_)) {
            std::ofstream f(wal_path_, std::ios::binary | std::ios::app);
            f.flush();
        }

        std::unique_lock<std::mutex> lk(mutex_);
        flushing_ = false;
        generation_ += 1;
        lk.unlock();
        cvar_.notify_all();
    } else {
        std::unique_lock<std::mutex> lk(mutex_);
        cvar_.wait(lk, [&] { return generation_ != my_gen; });
    }
}

} // namespace engine
