// Faithful port of rusql-client/src/main.rs — a pure TCP client for RuSQL's custom
// line-based protocol (AUTH handshake, queries terminated by a `---END---` sentinel).
// Deliberately has no dependency on engine_core, mirroring the Rust original.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sha1/sha1.hpp>

#pragma comment(lib, "ws2_32.lib")

namespace {

// ANSI 색상 코드 (matches the Rust original's constants)
constexpr const char* RESET = "\x1b[0m";
constexpr const char* RED   = "\x1b[31m";
constexpr const char* GREEN = "\x1b[32m";
constexpr const char* CYAN  = "\x1b[36m";
constexpr const char* BOLD  = "\x1b[1m";
constexpr const char* DIM   = "\x1b[2m";

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool ieq(const std::string& a, const char* b) {
    if (a.size() != std::char_traits<char>::length(b)) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::optional<std::string> get_arg(const std::vector<std::string>& args, const std::string& flag) {
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == flag) return args[i + 1];
    }
    return std::nullopt;
}

// Buffered line reader over a raw socket, mirroring BufReader<TcpStream>::read_line.
class LineReader {
public:
    explicit LineReader(SOCKET sock) : sock_(sock) {}

    // Returns false only when there is nothing left to read at all (mirrors
    // `Ok(0) | Err(_) => break` in the original read_response()).
    bool read_line(std::string& out) {
        for (;;) {
            size_t pos = buffer_.find('\n');
            if (pos != std::string::npos) {
                out = buffer_.substr(0, pos + 1);
                buffer_.erase(0, pos + 1);
                return true;
            }
            char chunk[4096];
            int n = recv(sock_, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                if (buffer_.empty()) return false;
                out = buffer_;
                buffer_.clear();
                return true;
            }
            buffer_.append(chunk, static_cast<size_t>(n));
        }
    }

private:
    SOCKET sock_;
    std::string buffer_;
};

// 서버 응답을 ---END--- 가 올 때까지 읽어 줄 목록 반환
std::vector<std::string> read_response(LineReader& reader) {
    std::vector<std::string> lines;
    for (;;) {
        std::string line;
        if (!reader.read_line(line)) break;
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "---END---") break;
        lines.push_back(std::move(line));
    }
    return lines;
}

// 입력에서 세미콜론 개수 카운트 (주석·문자열 안 제외)
size_t count_semicolons(const std::string& input) {
    size_t count = 0;
    size_t i = 0;
    const size_t n = input.size();
    while (i < n) {
        char c = input[i];
        if (c == '-' && i + 1 < n && input[i + 1] == '-') {
            while (i < n && input[i] != '\n') i++;
        } else if (c == '#') {
            while (i < n && input[i] != '\n') i++;
        } else if (c == '\'') {
            i++;
            while (i < n && input[i] != '\'') i++;
            if (i < n) i++;
        } else if (c == '/' && i + 1 < n && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(input[i] == '*' && input[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
        } else if (c == ';') {
            count++;
            i++;
        } else {
            i++;
        }
    }
    return count;
}

// 서버 쿼리 응답 출력. 형식: [OK|ERR]\n<출력>\n(x.xxx sec)\n---END---
void display_response(const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    const std::string& status = lines[0];

    if (status == "ERR") {
        std::cerr << RED << BOLD;
        for (size_t i = 1; i < lines.size(); i++) std::cerr << lines[i] << "\n";
        std::cerr << RESET;
    } else {
        for (size_t i = 1; i < lines.size(); i++) std::cout << lines[i] << "\n";
    }
}

void print_help() {
    std::cout << BOLD << "Commands:" << RESET << "\n";
    std::cout << "  SQL query" << GREEN << "; " << RESET << "    — Execute SQL (end with semicolon)\n";
    std::cout << "  " << CYAN << "\\status" << RESET << "         — Show server status\n";
    std::cout << "  " << DIM << "exit | quit" << RESET << "     — Disconnect\n";
    std::cout << "  " << DIM << "\\help" << RESET << "           — This help\n";
}

// ---------------------------------------------------------------------------
// mysql_native_password-style challenge-response (same scheme the MySQL wire
// protocol already uses, engine-side implementation in
// SharedDatabase::verify_mysql_native_password). Computes the token the server
// expects, so the plaintext password itself is never sent over the wire.
// ---------------------------------------------------------------------------
std::string sha1_hex(const std::string& data) {
    SHA1 hasher;
    hasher.update(data);
    return hasher.final();
}

std::vector<std::uint8_t> hex_decode(const std::string& hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string hex_encode(const std::vector<std::uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out.push_back(digits[(b >> 4) & 0xf]);
        out.push_back(digits[b & 0xf]);
    }
    return out;
}

// token = SHA1(password) XOR SHA1(nonce || SHA1(SHA1(password)))
std::string compute_native_password_token(const std::string& password, const std::vector<std::uint8_t>& nonce) {
    auto stage1 = hex_decode(sha1_hex(password));
    std::string stage1_str(stage1.begin(), stage1.end());
    auto stage2 = hex_decode(sha1_hex(stage1_str));

    std::string concat(nonce.begin(), nonce.end());
    concat.append(stage2.begin(), stage2.end());
    auto xor_key = hex_decode(sha1_hex(concat));

    std::vector<std::uint8_t> token(20);
    for (size_t i = 0; i < 20; i++) token[i] = stage1[i] ^ xor_key[i];
    return hex_encode(token);
}

// Extracts the 20-byte nonce from a "NONCE <hex>" line in the server's banner.
std::optional<std::vector<std::uint8_t>> extract_nonce(const std::vector<std::string>& banner_lines) {
    for (const auto& line : banner_lines) {
        if (line.rfind("NONCE ", 0) == 0) {
            auto bytes = hex_decode(line.substr(6));
            if (bytes.size() == 20) return bytes;
        }
    }
    return std::nullopt;
}

bool send_line(SOCKET sock, const std::string& text) {
    std::string out = text + "\n";
    size_t sent = 0;
    while (sent < out.size()) {
        int n = send(sock, out.data() + sent, static_cast<int>(out.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);

    if (std::find(args.begin(), args.end(), "--help") != args.end()) {
        std::cout << "Usage: rusql-client [-u user] [-p password] [-h host] [-P port]\n";
        std::cout << "  -u  username  (default: root)\n";
        std::cout << "  -p  password  (default: root)\n";
        std::cout << "  -h  host      (default: 127.0.0.1)\n";
        std::cout << "  -P  port      (default: 7878)\n";
        return 0;
    }

    const std::string user = get_arg(args, "-u").value_or("root");
    const std::string pass = get_arg(args, "-p").value_or("root");
    const std::string host = get_arg(args, "-h").value_or("127.0.0.1");
    int port = 7878;
    if (auto p = get_arg(args, "-P")) {
        try { port = std::stoi(*p); } catch (...) { port = 7878; }
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << RED << BOLD << "WSAStartup failed" << RESET << "\n";
        return 1;
    }

    // ── 연결 ──
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << RED << BOLD << "Cannot create socket" << RESET << "\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Fall back to DNS resolution for non-literal hosts (e.g. "localhost").
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || !result) {
            std::cerr << RED << BOLD << "Cannot connect to " << host << ":" << port << RESET << "\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }
        addr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
        freeaddrinfo(result);
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << RED << BOLD << "Cannot connect to " << host << ":" << port << RESET << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    LineReader reader(sock);

    // ── 배너 수신 (NONCE 라인 포함 — challenge-response 인증에 필요) ──
    auto banner_lines = read_response(reader);
    auto nonce = extract_nonce(banner_lines);
    if (!nonce) {
        std::cerr << RED << BOLD << "Server did not send an auth challenge (NONCE)." << RESET << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // ── AUTH 송신 (평문 비밀번호 대신 mysql_native_password 방식 challenge-response) ──
    std::string token = compute_native_password_token(pass, *nonce);
    if (!send_line(sock, "AUTH " + user + " " + token)) {
        std::cerr << RED << "Connection lost during auth." << RESET << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // ── AUTH 응답 수신 ──
    auto auth_resp = read_response(reader);
    const std::string status = auth_resp.empty() ? "" : auth_resp[0];
    if (status.rfind("OK", 0) != 0) {
        std::string msg = auth_resp.size() > 1 ? auth_resp[1] : "authentication failed";
        std::cerr << RED << BOLD << "ERROR: " << msg << RESET << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // ── 세션 시작 메시지 ──
    std::cout << BOLD << CYAN << "RuSQL" << RESET << " "
              << GREEN << "[" << user << "@" << host << ":" << port << "]" << RESET << "\n";
    std::cout << DIM << "Type SQL queries ending with ';'. \\help for commands." << RESET << "\n\n";

    // ── REPL ──
    std::string buf;

    for (;;) {
        if (trim(buf).empty()) {
            std::cout << BOLD << "rusql" << RESET << GREEN << ">" << RESET << " ";
        } else {
            std::cout << "       " << GREEN << ">" << RESET << " ";
        }
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) break;

        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        if (trim(buf).empty()) {
            if (ieq(trimmed, "exit") || ieq(trimmed, "quit")) {
                if (send_line(sock, "exit")) { /* flushed by send_line */ }
                std::cout << "Bye!\n";
                break;
            }
            if (trimmed == "\\help" || ieq(trimmed, "help")) {
                print_help();
                continue;
            }
            if (trimmed == "\\status") {
                if (!send_line(sock, "\\status")) {
                    std::cerr << RED << "Connection lost." << RESET << "\n";
                    break;
                }
                auto resp = read_response(reader);
                for (const auto& l : resp) std::cout << l << "\n";
                continue;
            }
        }

        buf += line;
        buf += "\n";

        if (buf.find(';') == std::string::npos) continue;

        size_t n = std::max<size_t>(count_semicolons(buf), 1);
        if (!send_line(sock, trim(buf))) {
            std::cerr << RED << "Connection lost." << RESET << "\n";
            break;
        }
        buf.clear();

        for (size_t i = 0; i < n; i++) {
            auto resp = read_response(reader);
            if (resp.empty()) {
                std::cerr << RED << "Connection closed by server." << RESET << "\n";
                closesocket(sock);
                WSACleanup();
                return 0;
            }
            display_response(resp);
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
