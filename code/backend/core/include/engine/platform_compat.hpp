#pragma once

// The few C library calls the engine makes whose Windows (MSVC) form is not the POSIX (Linux, macOS) one. The Windows build calls exactly what it called before;
// the POSIX build gets the matching function. (Sockets: net_compat.hpp.)

#include <cstdlib>
#include <ctime>

namespace engine::platform {

// The local / UTC time of `*t` in `*out`. MSVC: localtime_s(tm*, time_t*) / gmtime_s(tm*, time_t*), POSIX: localtime_r(time_t*, tm*) / gmtime_r(time_t*, tm*). 0 = ok.
inline int local_time(const std::time_t* t, std::tm* out) {
#ifdef _WIN32
    return localtime_s(out, t);
#else
    return localtime_r(t, out) != nullptr ? 0 : 1;
#endif
}

inline int utc_time(const std::time_t* t, std::tm* out) {
#ifdef _WIN32
    return gmtime_s(out, t);
#else
    return gmtime_r(t, out) != nullptr ? 0 : 1;
#endif
}

// Sets an environment variable of this process; an empty value removes it (what _putenv_s does, and what the tests use to unset one).
inline int set_env(const char* name, const char* value) {
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return (value == nullptr || value[0] == '\0') ? unsetenv(name) : setenv(name, value, 1);
#endif
}

// Reads the time zone again after TZ was changed.
inline void reload_time_zone() {
#ifdef _WIN32
    _tzset();
#else
    tzset();
#endif
}

// The seconds since 1970 of a broken-down UTC time (_mkgmtime / timegm).
inline std::time_t utc_seconds(std::tm* t) {
#ifdef _WIN32
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}

} // namespace engine::platform
