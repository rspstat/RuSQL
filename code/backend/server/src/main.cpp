// Faithful port of rusql-server/src/main.rs — the native TCP server (thread-per-
// connection, line-based protocol matching rusql-client's LineReader/AUTH handshake).
// The MySQL wire protocol listener (mysql.rs, start_mysql_listener) is ported
// separately; this file covers the native-protocol half only.

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/executor/executor.hpp"
#include "mysql.hpp"

#pragma comment(lib, "ws2_32.lib")

using namespace engine;

namespace {

std::string timestamp() {
    auto secs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", static_cast<unsigned long long>((secs % 86400) / 3600),
                  static_cast<unsigned long long>((secs % 3600) / 60), static_cast<unsigned long long>(secs % 60));
    return buf;
}

std::mutex g_log_mutex;
void log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "[" << timestamp() << "] " << msg << "\n";
    std::cout.flush();
}

std::string to_upper_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}

bool ieq(const std::string& a, const char* b) {
    if (a.size() != std::char_traits<char>::length(b)) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string trim_copy(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Returns the byte offset of the first ';' at BEGIN...END depth 0, skipping
// --/#/block comments and string literals. std::nullopt means the buffer so
// far holds no complete top-level statement yet (e.g. an unclosed BEGIN body
// spanning multiple lines) -- the caller must keep accumulating more input
// before trying again, mirroring the CLI's find_stmt_end (cli/src/main.cpp).
// Unlike a naive "does buf contain any ';'" check, this is depth-aware, so a
// ';' inside an open BEGIN block never causes a premature, partial split.
std::optional<std::size_t> find_stmt_end(const std::string& input) {
    int begin_depth = 0;
    std::size_t i = 0;
    std::size_t len = input.size();

    while (i < len) {
        if (input[i] == '-' && i + 1 < len && input[i + 1] == '-') {
            while (i < len && input[i] != '\n') i++;
            continue;
        }
        if (input[i] == '#') {
            while (i < len && input[i] != '\n') i++;
            continue;
        }
        if (input[i] == '/' && i + 1 < len && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < len) {
                if (input[i] == '*' && input[i + 1] == '/') {
                    i += 2;
                    break;
                }
                i++;
            }
            continue;
        }
        if (input[i] == '\'') {
            i++;
            while (i < len) {
                char c = input[i];
                i++;
                if (c == '\'') break;
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(input[i])) || input[i] == '_') {
            std::size_t start = i;
            while (i < len && (std::isalnum(static_cast<unsigned char>(input[i])) || input[i] == '_')) i++;
            std::string word = input.substr(start, i - start);
            std::string upper = to_upper_str(word);
            if (upper == "BEGIN") {
                // A transaction `BEGIN;` / `BEGIN WORK;` has no matching END -- only a
                // procedure/trigger body's BEGIN does. Without this check, a bare
                // transaction BEGIN made begin_depth stick above 0 for the rest of the
                // input, silently fusing every following statement into one chunk (this
                // was masked for a long time by a separate parser bug that ignored
                // trailing tokens after a valid statement; now that that's fixed, the
                // fused chunk fails outright with "Unexpected token(s) after end of
                // statement"). Mirrors the CLI's find_stmt_end (cli/src/main.cpp).
                std::size_t j = i;
                while (j < len && std::isspace(static_cast<unsigned char>(input[j]))) j++;
                bool is_transaction;
                if (j >= len || input[j] == ';') {
                    is_transaction = true;
                } else if (std::isalpha(static_cast<unsigned char>(input[j]))) {
                    std::size_t s2 = j, k = j;
                    while (k < len && (std::isalnum(static_cast<unsigned char>(input[k])) || input[k] == '_')) k++;
                    is_transaction = to_upper_str(input.substr(s2, k - s2)) == "WORK";
                } else {
                    is_transaction = false;
                }
                if (!is_transaction) begin_depth++;
            } else if (upper == "END") {
                std::size_t j = i;
                while (j < len && std::isspace(static_cast<unsigned char>(input[j]))) j++;
                bool next_is_sub = false;
                if (j < len && (std::isalpha(static_cast<unsigned char>(input[j])) || input[j] == '_')) {
                    std::size_t s2 = j, k = j;
                    while (k < len && (std::isalnum(static_cast<unsigned char>(input[k])) || input[k] == '_')) k++;
                    std::string nw = to_upper_str(input.substr(s2, k - s2));
                    next_is_sub = (nw == "IF" || nw == "WHILE" || nw == "LOOP" || nw == "REPEAT" || nw == "CASE");
                }
                if (!next_is_sub && begin_depth > 0) begin_depth--;
            }
            continue;
        }
        if (input[i] == ';' && begin_depth == 0) return i;
        i++;
    }
    return std::nullopt;
}

std::optional<std::string> handle_builtin(const std::string& cmd, std::size_t client_count,
                                           const std::chrono::steady_clock::time_point& uptime) {
    std::string lower = trim_copy(cmd);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower == "\\help" || lower == "help") {
        return "RuSQL Server v2.3.0\n"
               "Commands:\n"
               "  \\help           - this help\n"
               "  \\status         - server status\n"
               "  exit | quit     - disconnect\n"
               "SQL:\n"
               "  SHOW TABLES;    - table list\n"
               "  DESCRIBE <t>;   - table structure\n"
               "  SHOW BUFFER POOL; SHOW WAL; SHOW LOCKS;";
    }
    if (lower == "\\status") {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - uptime).count();
        std::ostringstream oss;
        oss << "Status: RUNNING\nUptime: " << (elapsed / 3600) << "h " << ((elapsed % 3600) / 60) << "m " << (elapsed % 60)
            << "s\nConnections: " << client_count;
        return oss.str();
    }
    return std::nullopt;
}

bool send_line_raw(SOCKET sock, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        int n = send(sock, text.data() + sent, static_cast<int>(text.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool send_response(SOCKET sock, const std::string& status, const std::string& body, double elapsed_secs) {
    std::ostringstream oss;
    oss << status << "\n" << body << "\n";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.3f sec)\n", elapsed_secs);
    oss << buf << "---END---\n";
    return send_line_raw(sock, oss.str());
}

// Buffered line reader over a raw socket, mirroring BufReader<TcpStream>::lines().
class LineReader {
public:
    explicit LineReader(SOCKET sock) : sock_(sock) {}

    bool read_line(std::string& out) {
        for (;;) {
            std::size_t pos = buffer_.find('\n');
            if (pos != std::string::npos) {
                out = buffer_.substr(0, pos);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                buffer_.erase(0, pos + 1);
                return true;
            }
            char chunk[4096];
            int n = recv(sock_, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            buffer_.append(chunk, static_cast<std::size_t>(n));
        }
    }

private:
    SOCKET sock_;
    std::string buffer_;
};

void handle_client(SOCKET sock, std::shared_ptr<RwLock<SharedDatabase>> shared, std::shared_ptr<std::atomic<std::size_t>> client_count,
                    std::chrono::steady_clock::time_point server_start) {
    sockaddr_in peer_addr{};
    int addr_len = sizeof(peer_addr);
    std::string peer = "unknown";
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len) == 0) {
        char ip_buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf))) {
            peer = std::string(ip_buf) + ":" + std::to_string(ntohs(peer_addr.sin_port));
        }
    }
    std::size_t count = client_count->fetch_add(1) + 1;
    log("Client connected: " + peer + " (total: " + std::to_string(count) + ")");

    struct SocketGuard {
        SOCKET s;
        ~SocketGuard() { closesocket(s); }
    } socket_guard{sock};

    auto cleanup_and_return = [&]() { client_count->fetch_sub(1); };

    // 1. banner
    {
        std::ostringstream oss;
        oss << "+-----------------------------------------+\n";
        oss << "|   RuSQL Server v2.3.0                  |\n";
        std::size_t pad = peer.size() >= 23 ? 0 : 23 - peer.size();
        oss << "|   Connected: " << peer << std::string(pad, ' ') << "|\n";
        oss << "+-----------------------------------------+\n";
        oss << "---END---\n";
        if (!send_line_raw(sock, oss.str())) {
            cleanup_and_return();
            return;
        }
    }

    LineReader reader(sock);

    // 2. AUTH handshake
    std::string auth_line;
    if (!reader.read_line(auth_line)) {
        cleanup_and_return();
        return;
    }

    std::vector<std::string> parts;
    {
        std::size_t start = 0;
        for (int i = 0; i < 2 && start <= auth_line.size(); i++) {
            auto sp = auth_line.find(' ', start);
            if (sp == std::string::npos) break;
            parts.push_back(auth_line.substr(start, sp - start));
            start = sp + 1;
        }
        parts.push_back(auth_line.substr(std::min(start, auth_line.size())));
    }
    std::string cmd = parts.size() > 0 ? parts[0] : "";
    std::string auth_user = parts.size() > 1 ? trim_copy(parts[1]) : "";
    std::string auth_pass = parts.size() > 2 ? trim_copy(parts[2]) : "";

    if (!ieq(cmd, "auth") || auth_user.empty()) {
        send_line_raw(sock, "ERR expected: AUTH <user> <password>\n---END---\n");
        cleanup_and_return();
        return;
    }

    bool ok = shared->read()->validate_credentials(auth_user, auth_pass);
    if (!ok) {
        log("[" + peer + "] AUTH failed: '" + auth_user + "'");
        send_line_raw(sock, "ERR Access denied for user '" + auth_user + "'\n---END---\n");
        cleanup_and_return();
        return;
    }
    shared->write()->migrate_mysql_hash(auth_user, auth_pass);

    log("[" + peer + "] Authenticated as '" + auth_user + "'");
    send_line_raw(sock, "OK authenticated as '" + auth_user + "'\n---END---\n");

    // 3. query session
    Executor exec = Executor::new_session(shared);
    exec.register_process(auth_user, peer);
    std::string buf;

    std::string line;
    while (reader.read_line(line)) {
        std::string trimmed = trim_copy(line);

        if (ieq(trimmed, "exit") || ieq(trimmed, "quit")) {
            send_line_raw(sock, "Bye!\n---END---\n");
            break;
        }

        if ((!trimmed.empty() && trimmed.front() == '\\') || ieq(trimmed, "help")) {
            std::size_t cnt = client_count->load();
            if (auto resp = handle_builtin(trimmed, cnt, server_start)) {
                send_line_raw(sock, *resp + "\n---END---\n");
            }
            continue;
        }

        buf += line;
        buf += "\n";

        // Extract one complete (BEGIN...END depth-0-terminated) statement at a time.
        // A naive "does buf contain any ';'" check here would misfire on a ';' inside
        // a still-open, multi-line BEGIN body (e.g. a CREATE TRIGGER whose body spans
        // several lines): it would call the old whole-buffer splitter early, which
        // always flushed its trailing partial content as if it were a real statement,
        // then clear buf -- discarding the "still inside BEGIN" context and causing the
        // trigger/procedure body's own inner statements to leak out as separate
        // top-level queries on subsequent lines.
        for (;;) {
            auto pos = find_stmt_end(buf);
            if (!pos) break;
            std::string q = trim_copy(buf.substr(0, *pos));
            buf = buf.substr(*pos + 1);
            if (q.empty()) {
                send_line_raw(sock, "---END---\n");
                continue;
            }

            std::string preview = q.size() > 60 ? q.substr(0, 60) + "..." : q;
            log("[" + auth_user + "@" + peer + "] " + preview);

            exec.update_process_command("Query", q);
            auto t0 = std::chrono::steady_clock::now();
            auto result = exec.execute_sql(q);
            std::string status = result.is_ok() ? "OK" : "ERR";
            std::string output = result.is_ok() ? result.value() : result.error();
            exec.update_process_command("Sleep", "");
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!send_response(sock, status, output, elapsed)) {
                exec.deregister_process();
                cleanup_and_return();
                return;
            }
        }
    }

    exec.deregister_process();
    std::size_t remaining = client_count->fetch_sub(1) - 1;
    log("Client disconnected: " + peer + " (remaining: " + std::to_string(remaining) + ")");
}

std::optional<std::string> find_arg(const std::vector<std::string>& args, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == flag) return args[i + 1];
    }
    return std::nullopt;
}

std::atomic<bool> g_running{true};

BOOL WINAPI ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        std::cout << "\n[!] Ctrl+C received. Shutting down...\n";
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);

    int port = 7878;
    if (auto v = find_arg(args, "--port")) {
        try {
            port = std::stoi(*v);
        } catch (...) {
        }
    } else if (args.size() > 1) {
        try {
            port = std::stoi(args[1]);
        } catch (...) {
        }
    }

    bool no_mysql = std::find(args.begin(), args.end(), "--no-mysql") != args.end();
    int mysql_port = 3306;
    if (auto v = find_arg(args, "--mysql-port")) {
        try {
            mysql_port = std::stoi(*v);
        } catch (...) {
        }
    }
    std::size_t buffer_pool_size = 64;
    if (auto v = find_arg(args, "--buffer-pool-size")) {
        try {
            buffer_pool_size = std::stoull(*v);
        } catch (...) {
        }
    }
    std::string data_dir = "data";
    if (auto v = find_arg(args, "--data-dir")) {
        data_dir = *v;
    }

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "Cannot create socket\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "Failed to bind 127.0.0.1:" << port << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    if (listen(listener, SOMAXCONN) != 0) {
        std::cerr << "Failed to listen on 127.0.0.1:" << port << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    u_long nonblocking = 1;
    ioctlsocket(listener, FIONBIO, &nonblocking);

    Executor boot(data_dir, buffer_pool_size);
    auto shared = boot.get_shared();

    if (shared->write()->ensure_default_user()) {
        log("No users found. Created default user 'root'@'%' with password 'root'.");
    }

    auto client_count = std::make_shared<std::atomic<std::size_t>>(0);
    auto server_start = std::chrono::steady_clock::now();

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    std::cout << "+-----------------------------------------+\n";
    std::cout << "|   RuSQL Server v2.3.0                  |\n";
    {
        // Matches Rust's `println!("...127.0.0.1:{:<9}|", port)` — only the port
        // number is padded, not the "host:port" string as a whole.
        std::string s = std::to_string(port);
        std::cout << "|   Native protocol on 127.0.0.1:" << s << std::string(s.size() >= 9 ? 0 : 9 - s.size(), ' ') << "|\n";
    }
    if (!no_mysql) {
        std::string s = std::to_string(mysql_port);
        std::cout << "|   MySQL protocol on 0.0.0.0:" << s << std::string(s.size() >= 12 ? 0 : 12 - s.size(), ' ') << "|\n";
    }
    {
        std::string s = std::to_string(buffer_pool_size);
        std::cout << "|   Buffer pool size: " << s << std::string(s.size() >= 20 ? 0 : 20 - s.size(), ' ') << "|\n";
    }
    std::cout << "|   Press Ctrl+C to stop                  |\n";
    std::cout << "+-----------------------------------------+\n";

    if (!no_mysql) {
        start_mysql_listener(mysql_port, shared);
    }

    log("Server started.");

    while (g_running.load()) {
        SOCKET client_sock = accept(listener, nullptr, nullptr);
        if (client_sock == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::cerr << "Accept error: " << err << "\n";
            break;
        }
        u_long blocking = 0;
        ioctlsocket(client_sock, FIONBIO, &blocking);
        BOOL nodelay = TRUE;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        std::thread(handle_client, client_sock, shared, client_count, server_start).detach();
    }

    auto uptime_secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - server_start).count();
    log("Server stopped. Total uptime: " + std::to_string(uptime_secs) + "s");
    closesocket(listener);
    WSACleanup();
    return 0;
}
