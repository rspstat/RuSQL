#pragma once

// KILL CONNECTION n: the session of the connection is blocked reading its next command, so the connection is shut down and the read that waits is cancelled
// (shutdown alone does not wake a recv that is already waiting on Windows; the shutdown makes a recv that comes later return at once).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

inline void wake_connection(SOCKET s) {
    shutdown(s, SD_BOTH);
    CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
}
