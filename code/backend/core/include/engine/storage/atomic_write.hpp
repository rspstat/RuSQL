#pragma once

// Shared by disk.cpp, wal.cpp, and txn_manager.cpp: writes the full content to a sibling
// ".tmp" file, fsyncs it, then atomically renames it over `path`. Without this, a crash
// mid-write leaves the target file empty/truncated/corrupt with the old contents already
// gone -- std::ofstream has no portable fsync, so this goes through a C stdio FILE* to
// reach the real fd/handle.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace engine {

inline void write_bytes_atomic(const std::string& path, const void* data, std::size_t size) {
    std::string tmp = path + ".tmp";
    std::FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) throw std::runtime_error("파일 쓰기 실패: " + tmp);
    std::size_t written = std::fwrite(data, 1, size, fp);
    if (written != size) {
        std::fclose(fp);
        throw std::runtime_error("파일 쓰기 실패: " + tmp);
    }
    if (std::fflush(fp) != 0) {
        std::fclose(fp);
        throw std::runtime_error("파일 쓰기 실패: " + tmp);
    }
#ifdef _WIN32
    bool synced = _commit(_fileno(fp)) == 0;
#else
    bool synced = fsync(fileno(fp)) == 0;
#endif
    std::fclose(fp);
    if (!synced) throw std::runtime_error("파일 동기화 실패: " + tmp);

    // Windows-only retry: MoveFileEx (behind std::filesystem::rename) can transiently
    // fail with "Access is denied" when antivirus/indexing briefly holds a handle open
    // on the just-closed .tmp file or the destination -- a well-known OS-level race, not
    // a logic bug. Row-level-concurrency Stage 6 stress testing (many INSERT/UPDATE/
    // DELETE statements against one table in a tight window, each persisting via this
    // function) made this pre-existing flake fire far more often just from raw call
    // volume, so a short retry-with-backoff before giving up is warranted here.
#ifdef _WIN32
    constexpr int kMaxAttempts = 5;
    std::error_code ec;
    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        if (!ec) break;
        if (attempt < kMaxAttempts) std::this_thread::sleep_for(std::chrono::milliseconds(5 * attempt));
    }
#else
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
#endif
    if (ec) throw std::runtime_error("파일 교체 실패: " + path + " (" + ec.message() + ")");
}

} // namespace engine
