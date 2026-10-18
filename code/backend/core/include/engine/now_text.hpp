#pragma once

// The current time as MySQL's NOW(fsp), CURTIME(fsp), UTC_TIMESTAMP(fsp) write it: the fraction of the second is cut (not rounded) to `fsp` places.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace engine {

// "YYYY-MM-DD HH:MM:SS[.ffffff]" (date_too: false gives "HH:MM:SS[.ffffff]")
inline std::string time_now_text(int fsp, bool utc, bool date_too) {
    if (fsp < 0) fsp = 0;
    if (fsp > 6) fsp = 6;
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    if (utc) gmtime_s(&tm_buf, &t);
    else localtime_s(&tm_buf, &t);
    char buf[32];
    if (date_too) std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    else std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    std::string out = buf;
    if (fsp > 0) {
        const long long micro = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;
        char frac[8];
        std::snprintf(frac, sizeof(frac), "%06lld", micro < 0 ? micro + 1000000 : micro);
        out += "." + std::string(frac).substr(0, static_cast<std::size_t>(fsp));
    }
    return out;
}

} // namespace engine
