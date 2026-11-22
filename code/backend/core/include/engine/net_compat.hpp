#pragma once

// Sockets for Windows (Winsock) and POSIX (Linux, macOS) behind one small surface. The Windows build calls what it called before (the same functions in the same
// order, only through these names); the POSIX build gets the matching ones. The code that uses it keeps the Winsock names SOCKET and INVALID_SOCKET.

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <thread>

namespace net {

using socklen = int;

inline bool startup() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
inline void cleanup() { WSACleanup(); }
inline int last_error() { return WSAGetLastError(); }
// accept() on a listener that is not waiting: try again (Windows: only the non-blocking listener of the native protocol gets here)
inline bool retry_accept(int error) { return error == WSAEWOULDBLOCK; }
inline void set_blocking(SOCKET s, bool blocking) {
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
}
inline void set_no_delay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}
inline void close_socket(SOCKET s) { closesocket(s); }
inline void spawn_detached(std::function<void()> work) { std::thread(std::move(work)).detach(); }

} // namespace net

#else // POSIX

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <thread>

using SOCKET = int;
inline constexpr SOCKET INVALID_SOCKET = -1;

namespace net {

using socklen = socklen_t;

inline bool startup() {
    // a write to a connection the other side has already closed returns an error instead of ending the whole process with SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}
inline void cleanup() {}
inline int last_error() { return errno; }
// accept() was interrupted by a signal, found nothing to take (a non-blocking listener) or found a connection the client had already dropped: try again
inline bool retry_accept(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ECONNABORTED; }
inline void set_blocking(SOCKET s, bool blocking) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}
inline void set_no_delay(SOCKET s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}
inline void close_socket(SOCKET s) { ::close(s); }

// A connection runs the parser and the executor, which recurse (nested expressions, 16 levels of trigger, ...). A thread has 1 MB of stack on Windows (what the
// engine was tested with), 8 MB on Linux, but only 512 KB on macOS: so the connection threads get 8 MB here (it is address space; only the part that is used is held).
inline void spawn_detached(std::function<void()> work) {
    auto* job = new std::function<void()>(std::move(work));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<std::size_t>(8) << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int rc = pthread_create(
        &thread, &attr,
        [](void* arg) -> void* {
            std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(arg));
            (*owned)();
            return nullptr;
        },
        job);
    pthread_attr_destroy(&attr);
    if (rc != 0) { // (no room for a big stack: an ordinary thread, so that the connection is still served)
        std::thread([job] {
            std::unique_ptr<std::function<void()>> owned(job);
            (*owned)();
        }).detach();
    }
}

} // namespace net

#endif
