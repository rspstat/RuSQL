#pragma once

// Shared by disk.cpp, wal.cpp, and txn_manager.cpp: writes the full content to a sibling
// ".tmp" file, fsyncs it, then atomically renames it over `path`. Without this, a crash
// mid-write leaves the target file empty/truncated/corrupt with the old contents already
// gone -- std::ofstream has no portable fsync, so this goes through a C stdio FILE* to
// reach the real fd/handle.

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

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
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("파일 교체 실패: " + path + " (" + ec.message() + ")");
}

} // namespace engine
