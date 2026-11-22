#pragma once

// KILL CONNECTION n: the session of the connection is blocked reading its next command, so the connection is shut down and the read that waits is cancelled
// (shutdown alone does not wake a recv that is already waiting on Windows; the shutdown makes a recv that comes later return at once). On Linux and macOS a
// shutdown wakes a recv that is waiting, so it is all there is to do.

#include "engine/net_compat.hpp"

inline void wake_connection(SOCKET s) {
#ifdef _WIN32
    shutdown(s, SD_BOTH);
    CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
#else
    shutdown(s, SHUT_RDWR);
#endif
}
