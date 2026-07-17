// Faithful port of rusql-server/src/mysql.rs — see mysql.hpp.

#include "mysql.hpp"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace engine;

namespace {

// COM command bytes
constexpr std::uint8_t COM_QUIT = 0x01;
constexpr std::uint8_t COM_INIT_DB = 0x02;
constexpr std::uint8_t COM_QUERY = 0x03;
constexpr std::uint8_t COM_PING = 0x0e;
constexpr std::uint8_t COM_STMT_PREPARE = 0x16;
constexpr std::uint8_t COM_STMT_EXECUTE = 0x17;
constexpr std::uint8_t COM_STMT_CLOSE = 0x19;
constexpr std::uint8_t COM_STMT_RESET = 0x1a;

// Server capability flags advertised to clients
constexpr std::uint32_t CAPS = 0x0001 |       // CLIENT_LONG_PASSWORD
                                0x0004 |       // CLIENT_LONG_FLAG
                                0x0200 |       // CLIENT_PROTOCOL_41
                                0x2000 |       // CLIENT_TRANSACTIONS
                                0x8000 |       // CLIENT_SECURE_CONNECTION
                                0x00080000 |   // CLIENT_PLUGIN_AUTH
                                0x00010000 |   // CLIENT_MULTI_STATEMENTS
                                0x00040000;    // CLIENT_MULTI_RESULTS

struct PreparedStmt {
    std::string query;
    std::size_t num_params;
};
using StmtMap = std::unordered_map<std::uint32_t, PreparedStmt>;

// ── Packet I/O ─────────────────────────────────────────────────

bool read_exact(SOCKET sock, void* buf, std::size_t n) {
    auto* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        int r = recv(sock, p + got, static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool write_all(SOCKET sock, const void* buf, std::size_t n) {
    const auto* p = static_cast<const char*>(buf);
    std::size_t sent = 0;
    while (sent < n) {
        int r = send(sock, p + sent, static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

std::optional<std::pair<std::uint8_t, std::vector<std::uint8_t>>> read_packet(SOCKET sock) {
    std::uint8_t h[4];
    if (!read_exact(sock, h, 4)) return std::nullopt;
    std::size_t len = static_cast<std::size_t>(h[0]) | (static_cast<std::size_t>(h[1]) << 8) | (static_cast<std::size_t>(h[2]) << 16);
    std::uint8_t seq = h[3];
    std::vector<std::uint8_t> buf(len);
    if (len > 0 && !read_exact(sock, buf.data(), len)) return std::nullopt;
    return std::make_pair(seq, std::move(buf));
}

bool write_packet(SOCKET sock, std::uint8_t seq, const std::vector<std::uint8_t>& payload) {
    std::size_t n = payload.size();
    std::uint8_t header[4] = {static_cast<std::uint8_t>(n & 0xff), static_cast<std::uint8_t>((n >> 8) & 0xff),
                               static_cast<std::uint8_t>((n >> 16) & 0xff), seq};
    if (!write_all(sock, header, 4)) return false;
    if (!payload.empty() && !write_all(sock, payload.data(), payload.size())) return false;
    return true;
}

// ── Length-encoded encoding ─────────────────────────────────────

void lenenc(std::vector<std::uint8_t>& buf, std::uint64_t n) {
    if (n < 251) {
        buf.push_back(static_cast<std::uint8_t>(n));
    } else if (n <= 0xffff) {
        buf.push_back(0xfc);
        buf.push_back(static_cast<std::uint8_t>(n & 0xff));
        buf.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
    } else if (n <= 0xffffff) {
        buf.push_back(0xfd);
        buf.push_back(static_cast<std::uint8_t>(n & 0xff));
        buf.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
        buf.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
    } else {
        buf.push_back(0xfe);
        for (int i = 0; i < 8; i++) buf.push_back(static_cast<std::uint8_t>((n >> (i * 8)) & 0xff));
    }
}

void lenstr(std::vector<std::uint8_t>& buf, const std::string& s) {
    lenenc(buf, s.size());
    buf.insert(buf.end(), s.begin(), s.end());
}

void nulstr(std::vector<std::uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

// ── Packet builders ────────────────────────────────────────────

std::array<std::uint8_t, 20> make_nonce(std::uint32_t conn_id) {
    auto t = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::array<std::uint8_t, 20> n{};
    for (int i = 0; i < 8; i++) n[i] = static_cast<std::uint8_t>((t >> (i * 8)) & 0xff);
    for (int i = 0; i < 4; i++) n[8 + i] = static_cast<std::uint8_t>((conn_id >> (i * 8)) & 0xff);
    for (int i = 0; i < 4; i++) n[12 + i] = static_cast<std::uint8_t>(((t >> 32) >> (i * 8)) & 0xff);
    n[16] = 0x52;
    n[17] = 0x44;
    n[18] = 0x42;
    n[19] = 0x21; // "RDB!"
    for (auto& b : n) {
        if (b == 0) b = 0x5a;
    }
    return n;
}

std::vector<std::uint8_t> handshake_pkt(std::uint32_t conn_id, const std::array<std::uint8_t, 20>& nonce) {
    std::vector<std::uint8_t> p;
    p.push_back(10); // protocol version 10
    nulstr(p, "5.7.0-rusql");
    for (int i = 0; i < 4; i++) p.push_back(static_cast<std::uint8_t>((conn_id >> (i * 8)) & 0xff));
    p.insert(p.end(), nonce.begin(), nonce.begin() + 8);
    p.push_back(0x00);
    std::uint16_t caps_lo = static_cast<std::uint16_t>(CAPS & 0xffff);
    p.push_back(static_cast<std::uint8_t>(caps_lo & 0xff));
    p.push_back(static_cast<std::uint8_t>((caps_lo >> 8) & 0xff));
    p.push_back(0x21); // charset: utf8
    p.push_back(0x02);
    p.push_back(0x00); // status: AUTOCOMMIT
    std::uint16_t caps_hi = static_cast<std::uint16_t>((CAPS >> 16) & 0xffff);
    p.push_back(static_cast<std::uint8_t>(caps_hi & 0xff));
    p.push_back(static_cast<std::uint8_t>((caps_hi >> 8) & 0xff));
    p.push_back(21); // auth-plugin-data total len
    for (int i = 0; i < 10; i++) p.push_back(0);
    p.insert(p.end(), nonce.begin() + 8, nonce.end());
    p.push_back(0x00);
    nulstr(p, "mysql_native_password");
    return p;
}

std::vector<std::uint8_t> ok_pkt(std::uint64_t affected) {
    std::vector<std::uint8_t> p{0x00};
    lenenc(p, affected);
    lenenc(p, 0);
    p.push_back(0x02);
    p.push_back(0x00); // status: AUTOCOMMIT
    p.push_back(0x00);
    p.push_back(0x00); // warnings
    return p;
}

std::vector<std::uint8_t> err_pkt(const std::string& msg) {
    std::vector<std::uint8_t> p{0xff};
    p.push_back(static_cast<std::uint8_t>(1064 & 0xff));
    p.push_back(static_cast<std::uint8_t>((1064 >> 8) & 0xff)); // ER_PARSE_ERROR
    p.push_back('#');
    p.insert(p.end(), {'4', '2', '0', '0', '0'}); // SQL state
    p.insert(p.end(), msg.begin(), msg.end());
    return p;
}

std::vector<std::uint8_t> eof_pkt() { return {0xfe, 0x00, 0x00, 0x02, 0x00}; }

std::vector<std::uint8_t> col_def_pkt(const std::string& name) {
    std::vector<std::uint8_t> p;
    lenstr(p, "def");
    lenstr(p, "");
    lenstr(p, "");
    lenstr(p, "");
    lenstr(p, name);
    lenstr(p, name);
    p.push_back(0x0c);
    p.push_back(0x21);
    p.push_back(0x00); // charset: utf8 (33)
    for (int i = 0; i < 4; i++) p.push_back(0xff); // max column length
    p.push_back(0xfd);                             // type: VARSTRING
    p.push_back(0x00);
    p.push_back(0x00); // flags
    p.push_back(0x00); // decimals
    p.push_back(0x00);
    p.push_back(0x00); // filler
    return p;
}

// ── Prepared statement support ─────────────────────────────────

std::vector<std::uint8_t> stmt_prepare_ok(std::uint32_t stmt_id, std::uint16_t num_params, std::uint16_t num_cols) {
    std::vector<std::uint8_t> p{0x00};
    for (int i = 0; i < 4; i++) p.push_back(static_cast<std::uint8_t>((stmt_id >> (i * 8)) & 0xff));
    p.push_back(static_cast<std::uint8_t>(num_cols & 0xff));
    p.push_back(static_cast<std::uint8_t>((num_cols >> 8) & 0xff));
    p.push_back(static_cast<std::uint8_t>(num_params & 0xff));
    p.push_back(static_cast<std::uint8_t>((num_params >> 8) & 0xff));
    p.push_back(0x00); // reserved
    p.push_back(0x00);
    p.push_back(0x00); // warning_count
    return p;
}

std::size_t count_placeholders(const std::string& q) {
    std::size_t n = 0;
    bool in_str = false;
    char prev = ' ';
    for (char c : q) {
        if (c == '\'' && prev != '\\') in_str = !in_str;
        if (c == '?' && !in_str) n++;
        prev = c;
    }
    return n;
}

std::string bind_params(const std::string& query, const std::vector<std::string>& params) {
    std::string result;
    result.reserve(query.size());
    std::size_t param_idx = 0;
    bool in_str = false;
    char prev = ' ';
    for (char c : query) {
        if (c == '\'' && prev != '\\') in_str = !in_str;
        if (c == '?' && !in_str) {
            if (param_idx < params.size()) result += params[param_idx];
            else result += '?';
            param_idx++;
        } else {
            result += c;
        }
        prev = c;
    }
    return result;
}

std::pair<std::size_t, std::size_t> read_lenenc(const std::uint8_t* buf, std::size_t len) {
    if (len == 0) return {0, 1};
    std::uint8_t b = buf[0];
    if (b < 251) return {b, 1};
    if (b == 0xfc && len >= 3) return {static_cast<std::size_t>(buf[1]) | (static_cast<std::size_t>(buf[2]) << 8), 3};
    if (b == 0xfd && len >= 4)
        return {static_cast<std::size_t>(buf[1]) | (static_cast<std::size_t>(buf[2]) << 8) | (static_cast<std::size_t>(buf[3]) << 16), 4};
    return {0, 1};
}

// Matches Rust's `format!("{}", v)` / f64 Display: the shortest decimal representation
// that round-trips exactly. `std::to_string(double)` instead uses fixed 6-decimal-place
// formatting, silently truncating a bound FLOAT/DOUBLE prepared-statement parameter's
// precision before it's substituted into the query text (e.g. 3.141592653589793 -> "3.141593").
std::string fmt_double_param(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed);
    return std::string(buf, res.ptr);
}

std::vector<std::string> parse_execute_params(const std::vector<std::uint8_t>& payload, std::size_t num_params) {
    if (num_params == 0) return {};

    std::size_t pos = 9; // skip stmt_id(4) + flags(1) + iteration_count(4)
    if (pos >= payload.size()) return std::vector<std::string>(num_params, "NULL");

    std::size_t null_bitmap_len = (num_params + 7) / 8;
    if (pos + null_bitmap_len > payload.size()) return std::vector<std::string>(num_params, "NULL");
    const std::uint8_t* null_bitmap = payload.data() + pos;
    pos += null_bitmap_len;

    std::uint8_t new_params_bound = pos < payload.size() ? payload[pos] : 0;
    pos += 1;

    std::vector<std::pair<std::uint8_t, std::uint8_t>> types;
    if (new_params_bound == 1) {
        for (std::size_t i = 0; i < num_params; i++) {
            std::uint8_t ft = pos < payload.size() ? payload[pos] : 0xfd;
            std::uint8_t uf = pos + 1 < payload.size() ? payload[pos + 1] : 0;
            types.emplace_back(ft, uf);
            pos += 2;
        }
    } else {
        types.assign(num_params, {0xfd, 0});
    }

    std::vector<std::string> params;
    params.reserve(num_params);
    for (std::size_t i = 0; i < num_params; i++) {
        bool is_null = ((null_bitmap[i / 8] >> (i % 8)) & 1) == 1;
        if (is_null) {
            params.push_back("NULL");
            continue;
        }
        std::uint8_t ft = types[i].first;
        std::string val;
        switch (ft) {
            case 0x01: { // TINY
                auto v = static_cast<std::int8_t>(pos < payload.size() ? payload[pos] : 0);
                pos += 1;
                val = std::to_string(static_cast<int>(v));
                break;
            }
            case 0x02: { // SHORT
                if (pos + 2 > payload.size()) {
                    params.push_back("NULL");
                    pos = payload.size();
                    continue;
                }
                std::int16_t v = static_cast<std::int16_t>(payload[pos] | (payload[pos + 1] << 8));
                pos += 2;
                val = std::to_string(v);
                break;
            }
            case 0x03:
            case 0x09: { // LONG
                if (pos + 4 > payload.size()) {
                    params.push_back("NULL");
                    pos = payload.size();
                    continue;
                }
                std::int32_t v;
                std::memcpy(&v, payload.data() + pos, 4);
                pos += 4;
                val = std::to_string(v);
                break;
            }
            case 0x08:
            case 0x10: { // LONGLONG
                if (pos + 8 > payload.size()) {
                    params.push_back("NULL");
                    pos = payload.size();
                    continue;
                }
                std::int64_t v;
                std::memcpy(&v, payload.data() + pos, 8);
                pos += 8;
                val = std::to_string(v);
                break;
            }
            case 0x04: { // FLOAT
                if (pos + 4 > payload.size()) {
                    params.push_back("NULL");
                    pos = payload.size();
                    continue;
                }
                float v;
                std::memcpy(&v, payload.data() + pos, 4);
                pos += 4;
                val = fmt_double_param(v);
                break;
            }
            case 0x05: { // DOUBLE
                if (pos + 8 > payload.size()) {
                    params.push_back("NULL");
                    pos = payload.size();
                    continue;
                }
                double v;
                std::memcpy(&v, payload.data() + pos, 8);
                pos += 8;
                val = fmt_double_param(v);
                break;
            }
            case 0x0a: { // DATE
                if (pos + 4 > payload.size()) {
                    params.push_back("NULL");
                    pos = payload.size();
                    continue;
                }
                std::uint16_t y = static_cast<std::uint16_t>(payload[pos] | (payload[pos + 1] << 8));
                std::uint8_t mo = payload[pos + 2];
                std::uint8_t d = payload[pos + 3];
                pos += 4;
                char buf[16];
                std::snprintf(buf, sizeof(buf), "'%04u-%02u-%02u'", y, mo, d);
                val = buf;
                break;
            }
            case 0x0b:
            case 0x0c: { // DATETIME / TIMESTAMP
                std::size_t dlen = pos < payload.size() ? payload[pos] : 0;
                pos += 1;
                if (dlen >= 4 && pos + 4 <= payload.size()) {
                    std::uint16_t y = static_cast<std::uint16_t>(payload[pos] | (payload[pos + 1] << 8));
                    std::uint8_t mo = payload[pos + 2];
                    std::uint8_t d = payload[pos + 3];
                    std::uint8_t h = 0, mi = 0, s = 0;
                    if (dlen >= 7 && pos + 7 <= payload.size()) {
                        h = payload[pos + 4];
                        mi = payload[pos + 5];
                        s = payload[pos + 6];
                    }
                    pos += dlen;
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "'%04u-%02u-%02u %02u:%02u:%02u'", y, mo, d, h, mi, s);
                    val = buf;
                } else {
                    pos += dlen;
                    val = "NULL";
                }
                break;
            }
            default: { // VAR_STRING / BLOB / default: length-encoded string
                if (pos >= payload.size()) {
                    params.push_back("NULL");
                    continue;
                }
                auto [slen, nbytes] = read_lenenc(payload.data() + pos, payload.size() - pos);
                pos += nbytes;
                if (pos + slen > payload.size()) {
                    params.push_back("NULL");
                    continue;
                }
                std::string s(reinterpret_cast<const char*>(payload.data() + pos), slen);
                pos += slen;
                std::string escaped;
                for (char c : s) {
                    if (c == '\'') escaped += "''";
                    else escaped += c;
                }
                val = "'" + escaped + "'";
                break;
            }
        }
        params.push_back(val);
    }
    return params;
}

// ── Result set ─────────────────────────────────────────────────

std::string trim_copy(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Matches Rust's `.trim_matches('\0')` — strips NUL bytes from both ends.
std::string trim_nul(const std::string& s) {
    auto start = s.find_first_not_of('\0');
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of('\0');
    return s.substr(start, end - start + 1);
}

// Matches Rust's str::lines(): empty input yields zero lines, and a trailing '\n'
// ends the last line rather than introducing a spurious trailing empty one.
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    std::size_t start = 0;
    while (start < s.size()) {
        auto nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::optional<std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>> parse_table(const std::string& out) {
    std::string trimmed = trim_copy(out);
    if (trimmed.empty()) return std::nullopt;

    if (trimmed.front() == '+') {
        std::vector<std::string> cols;
        std::vector<std::vector<std::string>> rows;
        bool header = false;
        for (auto& line : split_lines(trimmed)) {
            if (!line.empty() && line.front() == '+') continue;
            if (!line.empty() && line.front() == '|') {
                std::vector<std::string> cells;
                std::size_t start = 0;
                for (;;) {
                    auto pipe = line.find('|', start);
                    std::string cell = line.substr(start, pipe == std::string::npos ? std::string::npos : pipe - start);
                    if (!cell.empty()) cells.push_back(trim_copy(cell));
                    if (pipe == std::string::npos) break;
                    start = pipe + 1;
                }
                if (!header) {
                    cols = cells;
                    header = true;
                } else {
                    rows.push_back(cells);
                }
            }
        }
        if (cols.empty()) return std::nullopt;
        return std::make_pair(cols, rows);
    }

    std::vector<std::string> lines;
    for (auto& l : split_lines(trimmed)) {
        if (!trim_copy(l).empty()) lines.push_back(l);
    }
    if (lines.empty()) return std::nullopt;

    std::vector<std::string> cols;
    {
        std::size_t start = 0;
        const std::string& first = lines[0];
        for (;;) {
            auto tab = first.find('\t', start);
            cols.push_back(trim_copy(first.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
    }

    if (cols.size() == 1 && lines.size() == 1) {
        const std::string& w = cols[0];
        bool looks_like_header = !w.empty() && (std::isalpha(static_cast<unsigned char>(w.front())) || w.front() == '_') &&
                                  w.find(' ') == std::string::npos &&
                                  w.find_first_of("()'") == std::string::npos;
        if (!looks_like_header) return std::nullopt;
        return std::make_pair(cols, std::vector<std::vector<std::string>>{});
    }

    std::size_t ncols = cols.size();
    std::vector<std::vector<std::string>> rows;
    for (std::size_t li = 1; li < lines.size(); li++) {
        std::vector<std::string> cells;
        std::size_t start = 0;
        const std::string& l = lines[li];
        for (;;) {
            auto tab = l.find('\t', start);
            cells.push_back(trim_copy(l.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        cells.resize(ncols);
        rows.push_back(std::move(cells));
    }
    return std::make_pair(cols, rows);
}

bool send_resultset(SOCKET sock, const std::vector<std::string>& cols, const std::vector<std::vector<std::string>>& rows,
                     std::uint8_t start_seq) {
    std::uint8_t seq = start_seq;

    std::vector<std::uint8_t> cnt;
    lenenc(cnt, cols.size());
    if (!write_packet(sock, seq++, cnt)) return false;

    for (auto& col : cols) {
        if (!write_packet(sock, seq++, col_def_pkt(col))) return false;
    }

    if (!write_packet(sock, seq++, eof_pkt())) return false;

    for (auto& row : rows) {
        std::vector<std::uint8_t> pkt;
        for (std::size_t i = 0; i < cols.size(); i++) {
            std::string v = i < row.size() ? row[i] : std::string();
            if (v == "NULL") pkt.push_back(0xfb);
            else lenstr(pkt, v);
        }
        if (!write_packet(sock, seq++, pkt)) return false;
    }

    return write_packet(sock, seq++, eof_pkt());
}

// ── MySQL-specific compatibility helpers ───────────────────────

std::string box_table(const std::vector<std::string>& cols, const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::size_t> widths;
    for (auto& c : cols) widths.push_back(c.size());
    for (auto& row : rows) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); i++) widths[i] = std::max(widths[i], row[i].size());
    }
    std::string sep;
    for (auto w : widths) sep += "+" + std::string(w + 2, '-');
    sep += "+";
    auto make_row = [&](const std::vector<std::string>& cells) {
        std::string s;
        for (std::size_t i = 0; i < widths.size(); i++) {
            std::string cell = i < cells.size() ? cells[i] : std::string();
            s += "| " + cell + std::string(widths[i] - cell.size(), ' ') + " ";
        }
        return s + "|";
    };
    std::string out = sep + "\n" + make_row(cols) + "\n" + sep + "\n";
    for (auto& row : rows) out += make_row(row) + "\n";
    out += sep + "\n";
    out += std::to_string(rows.size()) + " row(s) returned.";
    return out;
}

std::string to_upper_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}
std::string to_lower_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string handle_select_atvars(const std::string& q) {
    static const std::vector<std::pair<std::string, std::string>> known = {
        {"auto_increment_increment", "1"},
        {"autocommit", "ON"},
        {"character_set_client", "utf8mb4"},
        {"character_set_connection", "utf8mb4"},
        {"character_set_results", "utf8mb4"},
        {"character_set_server", "utf8mb4"},
        {"collation_connection", "utf8mb4_general_ci"},
        {"collation_server", "utf8mb4_general_ci"},
        {"init_connect", ""},
        {"interactive_timeout", "28800"},
        {"license", "GPL"},
        {"lower_case_table_names", "0"},
        {"max_allowed_packet", "67108864"},
        {"net_write_timeout", "60"},
        {"query_cache_size", "0"},
        {"query_cache_type", "OFF"},
        {"sql_mode", ""},
        {"system_time_zone", "UTC"},
        {"time_zone", "SYSTEM"},
        {"transaction_isolation", "READ-COMMITTED"},
        {"tx_isolation", "READ-COMMITTED"},
        {"version", "5.7.0-rusql"},
        {"version_comment", "RuSQL"},
        {"wait_timeout", "28800"},
    };
    std::vector<std::string> cols, vals;
    std::size_t i = 0;
    std::size_t len = q.size();
    while (i < len) {
        if (i + 1 < len && q[i] == '@' && q[i + 1] == '@') {
            i += 2;
            std::string rest_up = to_upper_str(q.substr(i));
            if (rest_up.rfind("SESSION.", 0) == 0) i += 8;
            else if (rest_up.rfind("GLOBAL.", 0) == 0) i += 7;
            std::size_t start = i;
            while (i < len && (std::isalnum(static_cast<unsigned char>(q[i])) || q[i] == '_')) i++;
            std::string var_name = to_lower_str(q.substr(start, i - start));
            while (i < len && q[i] == ' ') i++;
            std::string alias;
            if (to_upper_str(q.substr(i)).rfind("AS ", 0) == 0) {
                i += 3;
                while (i < len && q[i] == ' ') i++;
                std::size_t as0 = i;
                while (i < len && (std::isalnum(static_cast<unsigned char>(q[i])) || q[i] == '_')) i++;
                alias = q.substr(as0, i - as0);
            } else {
                alias = var_name;
            }
            std::string value;
            for (auto& [k, v] : known) {
                if (k == var_name) {
                    value = v;
                    break;
                }
            }
            cols.push_back(alias);
            vals.push_back(value);
        } else {
            i++;
        }
    }
    if (cols.empty()) return box_table({"result"}, {{""}});
    return box_table(cols, {vals});
}

bool like_match_inner(const std::string& s, std::size_t si, const std::string& p, std::size_t pi) {
    if (pi == p.size()) return si == s.size();
    if (p[pi] == '%') return like_match_inner(s, si, p, pi + 1) || (si < s.size() && like_match_inner(s, si + 1, p, pi));
    if (si == s.size()) return false;
    if (p[pi] == '_' || std::tolower(static_cast<unsigned char>(p[pi])) == std::tolower(static_cast<unsigned char>(s[si])))
        return like_match_inner(s, si + 1, p, pi + 1);
    return false;
}
bool like_match(const std::string& s, const std::string& pat) { return like_match_inner(s, 0, pat, 0); }

std::string show_variables_result(const std::string& up) {
    static const std::vector<std::pair<std::string, std::string>> vars = {
        {"autocommit", "ON"},
        {"character_set_client", "utf8mb4"},
        {"character_set_connection", "utf8mb4"},
        {"character_set_results", "utf8mb4"},
        {"character_set_server", "utf8mb4"},
        {"collation_connection", "utf8mb4_general_ci"},
        {"collation_server", "utf8mb4_general_ci"},
        {"interactive_timeout", "28800"},
        {"lower_case_table_names", "0"},
        {"max_allowed_packet", "67108864"},
        {"net_write_timeout", "60"},
        {"query_cache_size", "0"},
        {"query_cache_type", "OFF"},
        {"sql_mode", ""},
        {"system_time_zone", "UTC"},
        {"time_zone", "SYSTEM"},
        {"transaction_isolation", "READ-COMMITTED"},
        {"tx_isolation", "READ-COMMITTED"},
        {"version", "5.7.0-rusql"},
        {"version_comment", "RuSQL"},
        {"wait_timeout", "28800"},
    };
    std::optional<std::string> like_pat;
    if (auto idx = up.find(" LIKE "); idx != std::string::npos) {
        std::string rest = up.substr(idx + 6);
        // trim_matches('\'') on both ends
        std::size_t s0 = rest.find_first_not_of('\'');
        std::string rest2 = s0 == std::string::npos ? "" : rest.substr(s0);
        auto end = rest2.find('\'');
        like_pat = to_lower_str(rest2.substr(0, end == std::string::npos ? rest2.size() : end));
    }
    std::vector<std::vector<std::string>> rows;
    for (auto& [k, v] : vars) {
        if (!like_pat || like_match(k, *like_pat)) rows.push_back({k, v});
    }
    return box_table({"Variable_name", "Value"}, rows);
}

std::optional<std::string> extract_first_from(const std::string& q) {
    std::string up = to_upper_str(q);
    auto idx = up.find(" FROM ");
    if (idx == std::string::npos) return std::nullopt;
    std::string rest = trim_copy(q.substr(idx + 6));
    std::size_t start = rest.find_first_not_of('`');
    rest = start == std::string::npos ? "" : rest.substr(start);
    std::size_t end = 0;
    while (end < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[end])) || rest[end] == '_')) end++;
    std::string s = rest.substr(0, end);
    if (s.empty() || to_upper_str(s) == "WHERE") return std::nullopt;
    return s;
}

std::optional<std::string> extract_second_from(const std::string& q) {
    std::string up = to_upper_str(q);
    auto first = up.find(" FROM ");
    if (first == std::string::npos) return std::nullopt;
    std::string rest_up = up.substr(first + 6);
    auto second = rest_up.find(" FROM ");
    if (second == std::string::npos) return std::nullopt;
    std::size_t pos = first + 6 + second + 6;
    std::string rest = trim_copy(q.substr(pos));
    std::size_t start = rest.find_first_not_of('`');
    rest = start == std::string::npos ? "" : rest.substr(start);
    std::size_t end = 0;
    while (end < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[end])) || rest[end] == '_')) end++;
    std::string s = rest.substr(0, end);
    if (s.empty()) return std::nullopt;
    return s;
}

StringResult exec_inner(Executor& exec, const std::string& q) { return exec.execute_sql(trim_copy(q)); }

// ── MySQL-specific compatibility shims ─────────────────────────

std::optional<StringResult> mysql_compat(const std::string& q, Executor& exec) {
    std::string up = to_upper_str(trim_copy(q));

    if (up.rfind("SET ", 0) == 0) return StringResult::Ok("");

    if (up == "SELECT VERSION()" || up.rfind("SELECT VERSION() ", 0) == 0 || up == "SELECT @@VERSION" ||
        up.rfind("SELECT @@VERSION ", 0) == 0) {
        return StringResult::Ok(box_table({"version"}, {{"5.7.0-rusql"}}));
    }

    if (up == "SELECT USER()" || up == "SELECT CURRENT_USER()" || up == "SELECT USER() AS USER" || up == "SELECT CURRENT_USER() AS USER") {
        return StringResult::Ok(box_table({"user()"}, {{"root@localhost"}}));
    }

    if (up == "SELECT DATABASE()" || up == "SELECT SCHEMA()" || up == "SELECT DATABASE() AS DATABASE" || up == "SELECT SCHEMA() AS SCHEMA") {
        auto out = exec_inner(exec, "SELECT DATABASE()");
        if (out.is_ok() && !out.value().empty()) return out;
        return StringResult::Ok(box_table({"DATABASE()"}, {{""}}));
    }

    if (up.rfind("SELECT @@", 0) == 0 || (up.rfind("SELECT", 0) == 0 && up.find("@@") != std::string::npos && up.find(" FROM ") == std::string::npos)) {
        return StringResult::Ok(handle_select_atvars(q));
    }

    if (up.rfind("SHOW VARIABLES", 0) == 0 || up.rfind("SHOW SESSION VARIABLES", 0) == 0 || up.rfind("SHOW GLOBAL VARIABLES", 0) == 0) {
        return StringResult::Ok(show_variables_result(up));
    }

    if (up.rfind("SHOW STATUS", 0) == 0 || up.rfind("SHOW SESSION STATUS", 0) == 0 || up.rfind("SHOW GLOBAL STATUS", 0) == 0) {
        return StringResult::Ok(box_table({"Variable_name", "Value"}, {}));
    }

    if (up.rfind("SHOW COLLATION", 0) == 0) {
        std::vector<std::vector<std::string>> rows = {
            {"utf8_general_ci", "utf8", "33", "Yes", "Yes", "1"},
            {"utf8mb4_general_ci", "utf8mb4", "45", "Yes", "Yes", "1"},
            {"utf8mb4_unicode_ci", "utf8mb4", "224", "", "Yes", "8"},
            {"latin1_swedish_ci", "latin1", "8", "Yes", "Yes", "1"},
        };
        return StringResult::Ok(box_table({"Collation", "Charset", "Id", "Default", "Compiled", "Sortlen"}, rows));
    }

    if (up.rfind("SHOW CHARSET", 0) == 0 || up.rfind("SHOW CHARACTER SET", 0) == 0) {
        std::vector<std::vector<std::string>> rows = {
            {"utf8", "UTF-8 Unicode", "utf8_general_ci", "3"},
            {"utf8mb4", "UTF-8 Unicode", "utf8mb4_general_ci", "4"},
            {"latin1", "cp1252 West European", "latin1_swedish_ci", "1"},
        };
        return StringResult::Ok(box_table({"Charset", "Description", "Default collation", "Maxlen"}, rows));
    }

    if (up.rfind("SHOW ENGINES", 0) == 0) {
        std::vector<std::vector<std::string>> rows = {
            {"RuSQL", "DEFAULT", "RuSQL B+Tree Storage Engine", "YES", "YES", "YES"},
        };
        return StringResult::Ok(box_table({"Engine", "Support", "Comment", "Transactions", "XA", "Savepoints"}, rows));
    }

    if (up.rfind("SHOW PLUGINS", 0) == 0) return StringResult::Ok(box_table({"Name", "Status", "Type", "Library", "License"}, {}));
    if (up.rfind("SHOW WARNINGS", 0) == 0) return StringResult::Ok(box_table({"Level", "Code", "Message"}, {}));
    if (up.rfind("SHOW EVENTS", 0) == 0)
        return StringResult::Ok(box_table({"Db", "Name", "Definer", "Time zone", "Type", "Execute at", "Interval value", "Interval field",
                                            "Starts", "Ends", "Status", "Originator"},
                                           {}));

    if (up.rfind("SHOW FUNCTION STATUS", 0) == 0 || up.rfind("SHOW PROCEDURE STATUS", 0) == 0) {
        return StringResult::Ok(box_table({"Db", "Name", "Type", "Definer", "Modified", "Created", "Security_type", "Comment",
                                            "character_set_client", "collation_connection", "Database Collation"},
                                           {}));
    }

    if (up.rfind("SHOW TABLE STATUS", 0) == 0) {
        return StringResult::Ok(box_table({"Name", "Engine", "Version", "Row_format", "Rows", "Avg_row_length", "Data_length",
                                            "Max_data_length", "Index_length", "Data_free", "Auto_increment", "Create_time", "Update_time",
                                            "Check_time", "Collation", "Checksum", "Create_options", "Comment"},
                                           {}));
    }

    if (up.rfind("SHOW TRIGGERS", 0) == 0) {
        return StringResult::Ok(box_table({"Trigger", "Event", "Table", "Statement", "Timing", "Created", "sql_mode", "Definer",
                                            "character_set_client", "collation_connection", "Database Collation"},
                                           {}));
    }

    if (up.rfind("SHOW INDEX FROM", 0) == 0 || up.rfind("SHOW INDEXES FROM", 0) == 0 || up.rfind("SHOW KEYS FROM", 0) == 0) {
        auto tbl_opt = extract_first_from(q);
        std::vector<std::vector<std::string>> rows;
        if (tbl_opt) {
            auto out = exec_inner(exec, "SHOW INDEX FROM " + *tbl_opt);
            // exec_show_index's native TSV is Table/Key_name/Column_name/Index_type, with a
            // composite index's columns crammed into one comma-joined Column_name -- expand
            // that into one row per column (with an incrementing Seq_in_index) to match what
            // real MySQL's SHOW INDEX returns for a multi-column index.
            if (out.is_ok()) {
                if (auto parsed = parse_table(out.value())) {
                    for (auto& r : parsed->second) {
                        std::string table_name = r.size() > 0 ? r[0] : *tbl_opt;
                        std::string key_name = r.size() > 1 ? r[1] : "";
                        std::string col_list = r.size() > 2 ? r[2] : "";
                        std::string idx_type = r.size() > 3 ? r[3] : "BTREE";
                        std::string non_unique = key_name == "PRIMARY" ? "0" : "1";
                        int seq = 1;
                        std::size_t start = 0;
                        for (;;) {
                            auto comma = col_list.find(", ", start);
                            std::string col = trim_copy(col_list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                            if (!col.empty()) {
                                rows.push_back({table_name, non_unique, key_name, std::to_string(seq++), col, "A", "NULL", "NULL",
                                                 "NULL", "", idx_type, "", ""});
                            }
                            if (comma == std::string::npos) break;
                            start = comma + 2;
                        }
                    }
                }
            }
        }
        return StringResult::Ok(box_table({"Table", "Non_unique", "Key_name", "Seq_in_index", "Column_name", "Collation", "Cardinality",
                                            "Sub_part", "Packed", "Null", "Index_type", "Comment", "Index_comment"},
                                           rows));
    }

    if (up.rfind("SHOW FULL TABLES", 0) == 0) {
        auto db_opt = extract_first_from(q);
        std::optional<std::string> type_filter;
        if (up.find("'VIEW'") != std::string::npos) type_filter = "VIEW";
        else if (up.find("'BASE TABLE'") != std::string::npos) type_filter = "BASE TABLE";

        std::string tbl_q = db_opt ? "SHOW TABLES FROM " + *db_opt : "SHOW TABLES";
        std::vector<std::string> tbl_names;
        {
            auto out = exec_inner(exec, tbl_q);
            if (out.is_ok()) {
                if (auto parsed = parse_table(out.value())) {
                    for (auto& row : parsed->second) {
                        if (!row.empty()) tbl_names.push_back(row.front());
                    }
                }
            }
        }

        std::string full_col = "Tables_in_" + db_opt.value_or("");

        std::vector<std::vector<std::string>> rows;
        for (auto& name : tbl_names) {
            if (type_filter && *type_filter != "BASE TABLE") continue;
            rows.push_back({name, "BASE TABLE"});
        }
        return StringResult::Ok(box_table({full_col, "Table_type"}, rows));
    }

    if (up.rfind("SHOW FULL COLUMNS", 0) == 0 || up.rfind("SHOW COLUMNS", 0) == 0) {
        auto tbl_opt = extract_first_from(q);
        auto db_opt = extract_second_from(q);
        if (!tbl_opt) {
            return StringResult::Ok(
                box_table({"Field", "Type", "Collation", "Null", "Key", "Default", "Extra", "Privileges", "Comment"}, {}));
        }
        std::string desc_q = db_opt ? "DESCRIBE " + *db_opt + "." + *tbl_opt : "DESCRIBE " + *tbl_opt;
        std::vector<std::string> cols;
        std::vector<std::vector<std::string>> rows;
        {
            auto out = exec_inner(exec, desc_q);
            if (out.is_ok()) {
                if (auto parsed = parse_table(out.value())) {
                    cols = parsed->first;
                    rows = parsed->second;
                }
            }
        }
        auto get = [&](const std::vector<std::string>& row, const std::string& name) -> std::string {
            for (std::size_t i = 0; i < cols.size(); i++) {
                if (to_lower_str(cols[i]) == to_lower_str(name)) return i < row.size() ? row[i] : std::string();
            }
            return "";
        };
        std::vector<std::vector<std::string>> new_rows;
        for (auto& row : rows) {
            new_rows.push_back({get(row, "Field"), get(row, "Type"), "utf8mb4_general_ci", get(row, "Null"), get(row, "Key"),
                                 get(row, "Default"), get(row, "Extra"), "select,insert,update,references", ""});
        }
        return StringResult::Ok(
            box_table({"Field", "Type", "Collation", "Null", "Key", "Default", "Extra", "Privileges", "Comment"}, new_rows));
    }

    return std::nullopt;
}

// ── Query execution ────────────────────────────────────────────

StringResult exec_query(Executor& exec, const std::string& raw) {
    std::string q = trim_copy(raw);
    while (!q.empty() && q.back() == ';') q.pop_back();
    q = trim_copy(q);
    if (q.empty()) return StringResult::Ok("");

    if (auto r = mysql_compat(q, exec)) return *r;

    return exec.execute_sql(q);
}

std::uint64_t affected_rows(const std::string& msg) {
    std::size_t start = msg.find_first_not_of(" \t");
    if (start == std::string::npos) return 0;
    std::size_t end = msg.find_first_of(" \t", start);
    std::string first_tok = msg.substr(start, end == std::string::npos ? std::string::npos : end - start);
    try {
        return std::stoull(first_tok);
    } catch (...) {
        return 0;
    }
}

// ── Client handler ─────────────────────────────────────────────

void handle_mysql_client(SOCKET sock, std::shared_ptr<RwLock<SharedDatabase>> shared, std::uint32_t conn_id) {
    struct SocketGuard {
        SOCKET s;
        ~SocketGuard() { closesocket(s); }
    } guard{sock};

    std::string peer = "unknown";
    {
        sockaddr_in peer_addr{};
        int addr_len = sizeof(peer_addr);
        if (getpeername(sock, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len) == 0) {
            char ip_buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf))) {
                peer = std::string(ip_buf) + ":" + std::to_string(ntohs(peer_addr.sin_port));
            }
        }
    }

    // 1. Server sends Handshake
    auto nonce = make_nonce(conn_id);
    if (!write_packet(sock, 0, handshake_pkt(conn_id, nonce))) return;

    // 2. Client sends HandshakeResponse
    auto pkt = read_packet(sock);
    if (!pkt) return;
    auto& resp = pkt->second;

    if (resp.size() < 32) return;
    std::uint32_t client_caps = static_cast<std::uint32_t>(resp[0]) | (static_cast<std::uint32_t>(resp[1]) << 8) |
                                 (static_cast<std::uint32_t>(resp[2]) << 16) | (static_cast<std::uint32_t>(resp[3]) << 24);
    std::size_t pos = 32;

    auto find_nul = [&](std::size_t from) -> std::size_t {
        for (std::size_t i = from; i < resp.size(); i++) {
            if (resp[i] == 0) return i;
        }
        return resp.size();
    };

    std::size_t uend = find_nul(pos);
    std::string username(reinterpret_cast<const char*>(resp.data() + pos), uend - pos);
    pos = uend + 1;

    std::vector<std::uint8_t> auth_response;
    if (client_caps & 0x8000) {
        std::size_t auth_len = pos < resp.size() ? resp[pos] : 0;
        pos += 1;
        std::size_t avail = pos < resp.size() ? resp.size() - pos : 0;
        std::size_t take = std::min(auth_len, avail);
        auth_response.assign(resp.begin() + static_cast<std::ptrdiff_t>(pos), resp.begin() + static_cast<std::ptrdiff_t>(pos + take));
        pos += auth_len;
    } else {
        std::size_t end = find_nul(pos);
        auth_response.assign(resp.begin() + static_cast<std::ptrdiff_t>(pos), resp.begin() + static_cast<std::ptrdiff_t>(end));
        pos = end + 1;
    }

    std::string init_db;
    if ((client_caps & 0x0008) && pos < resp.size()) {
        std::size_t end = find_nul(pos);
        init_db.assign(reinterpret_cast<const char*>(resp.data() + pos), end - pos);
    }

    // 3. Verify auth
    bool auth_ok = shared->read()->verify_mysql_native_password(username, std::vector<std::uint8_t>(nonce.begin(), nonce.end()), auth_response);
    if (!auth_ok) {
        std::string msg = "Access denied for user '" + username + "' (using password: " + (auth_response.empty() ? "NO" : "YES") + ")";
        write_packet(sock, 2, err_pkt(msg));
        return;
    }

    // 4. Send OK
    if (!write_packet(sock, 2, ok_pkt(0))) return;

    // 5. Query loop
    Executor exec = Executor::new_session(shared);
    exec.register_process(username, peer);
    StmtMap stmts;
    std::uint32_t next_stmt_id = 1;

    if (!init_db.empty()) {
        auto ignore_ = exec_query(exec, "USE " + init_db);
        (void)ignore_;
    }

    for (;;) {
        auto p = read_packet(sock);
        if (!p) break;
        auto& payload = p->second;
        if (payload.empty()) break;

        std::uint8_t cmd = payload[0];
        switch (cmd) {
            case COM_QUIT: {
                exec.deregister_process();
                return;
            }
            case COM_PING: {
                write_packet(sock, 1, ok_pkt(0));
                break;
            }
            case COM_INIT_DB: {
                std::string db(reinterpret_cast<const char*>(payload.data() + 1), payload.size() - 1);
                db = trim_nul(db);
                auto res = exec_query(exec, "USE " + db);
                if (res.is_ok()) write_packet(sock, 1, ok_pkt(0));
                else write_packet(sock, 1, err_pkt(res.error()));
                break;
            }
            case COM_QUERY: {
                std::string query(reinterpret_cast<const char*>(payload.data() + 1), payload.size() - 1);
                query = trim_copy(query);
                while (!query.empty() && query.back() == ';') query.pop_back();
                query = trim_copy(query);
                auto res = exec_query(exec, query);
                if (res.is_err()) {
                    write_packet(sock, 1, err_pkt(res.error()));
                } else {
                    auto& out = res.value();
                    if (out.empty()) {
                        write_packet(sock, 1, ok_pkt(0));
                    } else if (auto parsed = parse_table(out)) {
                        send_resultset(sock, parsed->first, parsed->second, 1);
                    } else {
                        write_packet(sock, 1, ok_pkt(affected_rows(out)));
                    }
                }
                break;
            }
            case COM_STMT_PREPARE: {
                std::string query(reinterpret_cast<const char*>(payload.data() + 1), payload.size() - 1);
                query = trim_nul(query);
                std::size_t num_params = count_placeholders(query);
                std::uint32_t stmt_id = next_stmt_id++;
                stmts[stmt_id] = PreparedStmt{query, num_params};

                std::uint8_t seq = 1;
                write_packet(sock, seq++, stmt_prepare_ok(stmt_id, static_cast<std::uint16_t>(num_params), 0));
                if (num_params > 0) {
                    for (std::size_t i = 0; i < num_params; i++) write_packet(sock, seq++, col_def_pkt("?"));
                    write_packet(sock, seq++, eof_pkt());
                }
                break;
            }
            case COM_STMT_EXECUTE: {
                if (payload.size() < 5) {
                    write_packet(sock, 1, err_pkt("Invalid COM_STMT_EXECUTE"));
                    break;
                }
                std::uint32_t stmt_id = static_cast<std::uint32_t>(payload[1]) | (static_cast<std::uint32_t>(payload[2]) << 8) |
                                         (static_cast<std::uint32_t>(payload[3]) << 16) | (static_cast<std::uint32_t>(payload[4]) << 24);
                auto it = stmts.find(stmt_id);
                if (it == stmts.end()) {
                    write_packet(sock, 1, err_pkt("Unknown prepared statement"));
                    break;
                }
                std::string query = it->second.query;
                std::size_t num_params = it->second.num_params;
                std::vector<std::uint8_t> sub_payload(payload.begin() + 1, payload.end());
                auto params = parse_execute_params(sub_payload, num_params);
                std::string final_query = bind_params(query, params);
                auto res = exec_query(exec, final_query);
                if (res.is_err()) {
                    write_packet(sock, 1, err_pkt(res.error()));
                } else {
                    auto& out = res.value();
                    if (out.empty()) {
                        write_packet(sock, 1, ok_pkt(0));
                    } else if (auto parsed = parse_table(out)) {
                        send_resultset(sock, parsed->first, parsed->second, 1);
                    } else {
                        write_packet(sock, 1, ok_pkt(affected_rows(out)));
                    }
                }
                break;
            }
            case COM_STMT_CLOSE: {
                if (payload.size() >= 5) {
                    std::uint32_t stmt_id = static_cast<std::uint32_t>(payload[1]) | (static_cast<std::uint32_t>(payload[2]) << 8) |
                                             (static_cast<std::uint32_t>(payload[3]) << 16) | (static_cast<std::uint32_t>(payload[4]) << 24);
                    stmts.erase(stmt_id);
                }
                break;
            }
            case COM_STMT_RESET: {
                write_packet(sock, 1, ok_pkt(0));
                break;
            }
            default: {
                write_packet(sock, 1, err_pkt("Unsupported command"));
                break;
            }
        }
    }

    exec.deregister_process();
}

} // namespace

void start_mysql_listener(int port, const std::string& bind_addr, std::shared_ptr<RwLock<SharedDatabase>> shared) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "[mysql] Failed to create socket\n";
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    // Defaults to 127.0.0.1 (main.cpp's --mysql-bind default) rather than the previous
    // hardcoded INADDR_ANY (0.0.0.0) -- combined with the ensure_default_user() root/root
    // fallback account, binding all interfaces by default exposed the server to anyone on
    // the same network out of the box. Pass --mysql-bind 0.0.0.0 explicitly to restore that.
    inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr);

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[mysql] Failed to bind " << bind_addr << ":" << port << "\n";
        closesocket(listener);
        return;
    }
    if (listen(listener, SOMAXCONN) != 0) {
        std::cerr << "[mysql] Failed to listen on " << bind_addr << ":" << port << "\n";
        closesocket(listener);
        return;
    }

    {
        std::string s = std::to_string(port);
        std::string prefix = bind_addr + ":";
        std::size_t total_width = 16 + 8; // matches the original 0.0.0.0:{:<16} column width
        std::size_t used = prefix.size() + s.size();
        std::cout << "|   MySQL protocol on " << prefix << s << std::string(used >= total_width ? 0 : total_width - used, ' ') << "|\n";
    }

    auto counter = std::make_shared<std::atomic<std::uint32_t>>(1);
    std::thread([listener, shared, counter]() {
        for (;;) {
            SOCKET client_sock = accept(listener, nullptr, nullptr);
            if (client_sock == INVALID_SOCKET) break;
            std::uint32_t id = counter->fetch_add(1);
            std::thread(handle_mysql_client, client_sock, shared, id).detach();
        }
    }).detach();
}
