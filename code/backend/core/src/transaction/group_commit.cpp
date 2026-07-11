#include "engine/transaction/group_commit.hpp"

#include <cstdio>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

        // 단일 fsync — 이 시점까지 WAL 파일에 기록된 모든 COMMIT 레코드 영속화.
        // Opens the existing file (no truncate/append semantics needed -- this handle
        // is only used to force a real disk sync of whatever earlier writers already
        // flushed to the OS, matching Rust's OpenOptions::write(true).open()+sync_all()).
        if (fs::exists(wal_path_)) {
            std::FILE* fp = std::fopen(wal_path_.c_str(), "r+b");
            if (fp) {
#ifdef _WIN32
                _commit(_fileno(fp));
#else
                fsync(fileno(fp));
#endif
                std::fclose(fp);
            }
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
