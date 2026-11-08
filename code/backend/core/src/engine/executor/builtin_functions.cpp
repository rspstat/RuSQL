#include "engine/collation.hpp"
#include "engine/executor/builtin_functions.hpp"
#include "engine/like_escape.hpp"
#include "engine/value_class.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "engine/sql_value_util.hpp"
#include "engine/statement_error.hpp"

namespace engine {

namespace {

using namespace sqlval;

const std::string kNull = "NULL";

bool is_null(const std::string& v) { return v == kNull; }

std::string upper_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

[[noreturn]] void wrong_count(const BuiltinCall& c) {
    throw StatementError("Incorrect parameter count in the call to native function '" + lower_ascii(c.name) + "'");
}

// Is argument i a string written in quotes (a literal)? Not a number, a column or the value of an expression.
bool string_literal(const BuiltinCall& c, std::size_t i) {
    if (i < c.kinds.size()) return c.kinds[i] == 'S';
    return c.raw[i].size() >= 2 && c.raw[i][0] == '\'';
}

// Is argument i a number MySQL holds as a DOUBLE (a FLOAT / DOUBLE column, a string, an expression done in floating point)? ROUND and FORMAT round those
// half to even (ROUND(2.5) is 2), a DECIMAL or an integer half away from zero (3)
bool double_arg(const BuiltinCall& c, std::size_t i) { return i < c.kinds.size() && (c.kinds[i] == 'D' || c.kinds[i] == 'S'); }

double round_double(double v, long long places) {
    if (places > 30) return v;
    if (places < -30) return 0.0;
    const double scale = std::pow(10.0, static_cast<double>(places < 0 ? -places : places));
    const double shifted = places >= 0 ? v * scale : v / scale;
    if (!std::isfinite(shifted)) return v;
    const double rounded = std::nearbyint(shifted);
    return places >= 0 ? rounded / scale : rounded * scale;
}

bool number_literal(const BuiltinCall& c, std::size_t i) {
    if (i < c.kinds.size()) return c.kinds[i] == 'N';
    return !string_literal(c, i) && !c.raw[i].empty() && (std::isdigit(static_cast<unsigned char>(c.raw[i][0])) || c.raw[i][0] == '-');
}

// ---- argument access --------------------------------------------------------------------------------------------------------------------------

std::size_t argc(const BuiltinCall& c) { return c.raw.size(); }

bool any_null(const BuiltinCall& c, std::size_t first = 0) {
    for (std::size_t i = first; i < argc(c); i++) {
        if (is_null(c.value(i))) return true;
    }
    return false;
}

// The text of a number the way MySQL reads a string as one: its leading number (0 when there is none), as an exact decimal text.
Decimal decimal_of(const std::string& text) {
    if (auto d = Decimal::parse(text)) return *d;
    bool whole = false;
    const double v = leading_number(text, &whole);
    if (auto d = Decimal::parse(format_double(v))) return *d;
    return Decimal{};
}

// An integer argument: a decimal is rounded half away from zero.
long long int_of(const std::string& text) {
    const Decimal d = decimal_of(text).rounded(0, false);
    if (auto v = d.to_int64()) return *v;
    return d.negative ? std::numeric_limits<long long>::min() : std::numeric_limits<long long>::max();
}

double double_of(const std::string& text) { return leading_number(text); }

std::string int_text(long long v) { return std::to_string(v); }

// ---- strings ----------------------------------------------------------------------------------------------------------------------------------

std::string fn_upper(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    return is_null(v) ? kNull : map_case(v, true);
}

std::string fn_lower(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    return is_null(v) ? kNull : map_case(v, false);
}

std::string fn_length(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    return is_null(v) ? kNull : int_text(static_cast<long long>(v.size()));
}

std::string fn_char_length(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    return is_null(v) ? kNull : int_text(static_cast<long long>(utf8_length(v)));
}

std::string fn_bit_length(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    return is_null(v) ? kNull : int_text(static_cast<long long>(v.size()) * 8);
}

std::string fn_concat(const BuiltinCall& c) {
    if (argc(c) < 1) wrong_count(c);
    std::string out;
    for (std::size_t i = 0; i < argc(c); i++) {
        const std::string v = c.value(i);
        if (is_null(v)) return kNull;
        out += v;
    }
    return out;
}

std::string fn_concat_ws(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    const std::string sep = c.value(0);
    if (is_null(sep)) return kNull;
    std::string out;
    bool first = true;
    for (std::size_t i = 1; i < argc(c); i++) {
        const std::string v = c.value(i);
        if (is_null(v)) continue;
        if (!first) out += sep;
        out += v;
        first = false;
    }
    return out;
}

// SUBSTRING(str, pos[, len]): pos counts from 1, a negative pos from the end, 0 gives nothing
std::string substring_chars(const std::vector<std::string>& chars, long long pos, std::optional<long long> len) {
    const long long n = static_cast<long long>(chars.size());
    if (pos == 0) return "";
    long long start = pos > 0 ? pos - 1 : n + pos;
    if (start < 0 || start >= n) return "";
    long long count = n - start;
    if (len) {
        if (*len <= 0) return "";
        count = std::min(count, *len);
    }
    std::string out;
    for (long long i = start; i < start + count; i++) out += chars[static_cast<std::size_t>(i)];
    return out;
}

std::string fn_substring(const BuiltinCall& c) {
    if (argc(c) < 2 || argc(c) > 3) wrong_count(c);
    if (any_null(c)) return kNull;
    std::optional<long long> len;
    if (argc(c) == 3) len = int_of(c.value(2));
    return substring_chars(utf8_chars(c.value(0)), int_of(c.value(1)), len);
}

std::string fn_left(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long n = int_of(c.value(1));
    if (n <= 0) return "";
    return substring_chars(utf8_chars(c.value(0)), 1, n);
}

std::string fn_right(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long n = int_of(c.value(1));
    if (n <= 0) return "";
    const auto chars = utf8_chars(c.value(0));
    const long long total = static_cast<long long>(chars.size());
    return substring_chars(chars, n >= total ? 1 : total - n + 1, std::nullopt);
}

std::string fn_reverse(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    auto chars = utf8_chars(v);
    std::string out;
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) out += *it;
    return out;
}

// the pattern of `LIKE p ESCAPE 'c'` where p is no string written in the statement (a column, an expression): the form the matcher reads, see engine/like_escape.hpp
std::string fn_like_escape(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    return like_pattern_with_escape(v, c.value(1));
}

std::string fn_repeat(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long n = int_of(c.value(1));
    const std::string v = c.value(0);
    std::string out;
    if (n <= 0 || v.empty()) return out;
    if (static_cast<unsigned long long>(n) * v.size() > (1ULL << 26)) return kNull;
    out.reserve(static_cast<std::size_t>(n) * v.size());
    for (long long i = 0; i < n; i++) out += v;
    return out;
}

std::string fn_space(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long n = int_of(c.value(0));
    if (n <= 0) return "";
    if (n > (1LL << 26)) return kNull;
    return std::string(static_cast<std::size_t>(n), ' ');
}

std::string fn_replace(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (any_null(c)) return kNull;
    const std::string s = c.value(0), from = c.value(1), to = c.value(2);
    if (from.empty()) return s;
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) { out += s.substr(pos); break; }
        out += s.substr(pos, hit - pos) + to;
        pos = hit + from.size();
    }
    return out;
}

std::string trim_with(const std::string& s, const std::string& rem, bool leading, bool trailing) {
    if (rem.empty()) return s;
    std::size_t b = 0, e = s.size();
    if (leading) {
        while (e - b >= rem.size() && s.compare(b, rem.size(), rem) == 0) b += rem.size();
    }
    if (trailing) {
        while (e - b >= rem.size() && s.compare(e - rem.size(), rem.size(), rem) == 0) e -= rem.size();
    }
    return s.substr(b, e - b);
}

std::string fn_trim(const BuiltinCall& c) {
    if (argc(c) == 1) {
        const std::string v = c.value(0);
        return is_null(v) ? kNull : trim_with(v, " ", true, true);
    }
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return trim_with(c.value(1), c.value(0), true, true); // TRIM(remstr FROM str)
}

std::string fn_trim_both(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return trim_with(c.value(1), c.value(0), true, true);
}

std::string fn_trim_leading(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return trim_with(c.value(1), c.value(0), true, false);
}

std::string fn_trim_trailing(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return trim_with(c.value(1), c.value(0), false, true);
}

std::string fn_ltrim(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    return is_null(v) ? kNull : trim_with(v, " ", true, false);
}

std::string fn_rtrim(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    return is_null(v) ? kNull : trim_with(v, " ", false, true);
}

std::string pad(const BuiltinCall& c, bool left) {
    if (argc(c) != 3) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long len = int_of(c.value(1));
    if (len < 0) return kNull;
    const auto chars = utf8_chars(c.value(0));
    const auto padding = utf8_chars(c.value(2));
    if (static_cast<long long>(chars.size()) >= len) return substring_chars(chars, 1, len);
    if (padding.empty()) return "";
    std::string fill;
    const long long need = len - static_cast<long long>(chars.size());
    for (long long i = 0; i < need; i++) fill += padding[static_cast<std::size_t>(i) % padding.size()];
    std::string s;
    for (auto& ch : chars) s += ch;
    return left ? fill + s : s + fill;
}

std::string fn_lpad(const BuiltinCall& c) { return pad(c, true); }
std::string fn_rpad(const BuiltinCall& c) { return pad(c, false); }

// 1-based character position of `sub` in `s` at or after character `from` (1-based); 0 when it is not there
// INSTR / LOCATE / POSITION do not tell upper from lower case but DO tell accents (INSTR('Hello', 'h') is 1, INSTR('h\303\251llo', 'e') is 0).
long long locate_chars(const std::string& sub, const std::string& s, long long from) {
    const auto hay = utf8_chars(map_case(s, false));
    const auto needle = utf8_chars(map_case(sub, false));
    const long long n = static_cast<long long>(hay.size()), m = static_cast<long long>(needle.size());
    if (from < 1 || from > n + 1) return 0;
    for (long long i = from - 1; i + m <= n; i++) {
        bool same = true;
        for (long long k = 0; k < m && same; k++) same = hay[static_cast<std::size_t>(i + k)] == needle[static_cast<std::size_t>(k)];
        if (same) return i + 1;
    }
    return 0;
}

std::string fn_instr(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return int_text(locate_chars(c.value(1), c.value(0), 1));
}

std::string fn_locate(const BuiltinCall& c) {
    if (argc(c) < 2 || argc(c) > 3) wrong_count(c);
    if (any_null(c)) return kNull;
    return int_text(locate_chars(c.value(0), c.value(1), argc(c) == 3 ? int_of(c.value(2)) : 1));
}

std::string fn_ascii(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    return int_text(v.empty() ? 0 : static_cast<unsigned char>(v[0]));
}

std::string fn_ord(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    if (v.empty()) return "0";
    const auto chars = utf8_chars(v);
    unsigned long long n = 0;
    for (unsigned char b : chars.front()) n = n * 256 + b;
    return std::to_string(n);
}

std::string fn_char(const BuiltinCall& c) {
    std::string out;
    for (std::size_t i = 0; i < argc(c); i++) {
        const std::string v = c.value(i);
        if (is_null(v)) continue;
        unsigned long long n = static_cast<unsigned long long>(int_of(v));
        std::string bytes;
        do {
            bytes.insert(bytes.begin(), static_cast<char>(n & 0xFF));
            n >>= 8;
        } while (n > 0);
        out += bytes;
    }
    return out;
}

std::string fn_strcmp(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const int r = collation::compare(c.value(0), c.value(1));
    return r < 0 ? "-1" : (r > 0 ? "1" : "0");
}

std::string fn_field(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return "0";
    for (std::size_t i = 1; i < argc(c); i++) {
        if (collation::equal(c.value(i), v)) return int_text(static_cast<long long>(i));
    }
    return "0";
}

std::string fn_elt(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    const std::string n = c.value(0);
    if (is_null(n)) return kNull;
    const long long k = int_of(n);
    if (k < 1 || k >= static_cast<long long>(argc(c))) return kNull;
    return c.value(static_cast<std::size_t>(k));
}

std::string fn_find_in_set(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const std::string needle = c.value(0), list = c.value(1);
    if (needle.find(',') != std::string::npos) return "0";
    long long index = 1;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t comma = list.find(',', pos);
        const std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (collation::equal(item, needle)) return int_text(index);
        if (comma == std::string::npos) break;
        pos = comma + 1;
        index++;
    }
    return "0";
}

std::string fn_insert(const BuiltinCall& c) {
    if (argc(c) != 4) wrong_count(c);
    if (any_null(c)) return kNull;
    const auto chars = utf8_chars(c.value(0));
    const long long n = static_cast<long long>(chars.size());
    const long long pos = int_of(c.value(1)), len = int_of(c.value(2));
    if (pos < 1 || pos > n) return c.value(0);
    std::string out;
    for (long long i = 0; i < pos - 1; i++) out += chars[static_cast<std::size_t>(i)];
    out += c.value(3);
    const long long rest = len < 0 || pos - 1 + len >= n ? n : pos - 1 + len;
    for (long long i = rest; i < n; i++) out += chars[static_cast<std::size_t>(i)];
    return out;
}

std::string fn_substring_index(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (any_null(c)) return kNull;
    const std::string s = c.value(0), delim = c.value(1);
    const long long count = int_of(c.value(2));
    if (delim.empty() || count == 0) return "";
    std::vector<std::size_t> hits;
    for (std::size_t pos = s.find(delim); pos != std::string::npos; pos = s.find(delim, pos + delim.size())) hits.push_back(pos);
    const long long total = static_cast<long long>(hits.size());
    if (count > 0) {
        if (count > total) return s;
        return s.substr(0, hits[static_cast<std::size_t>(count - 1)]);
    }
    if (-count > total) return s;
    return s.substr(hits[static_cast<std::size_t>(total + count)] + delim.size());
}

std::string with_thousands(const std::string& integer) {
    std::string out;
    for (std::size_t i = 0; i < integer.size(); i++) {
        if (i > 0 && (integer.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(integer[i]);
    }
    return out;
}

std::string fn_format(const BuiltinCall& c) {
    if (argc(c) < 2 || argc(c) > 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1))) return kNull;
    long long places = int_of(c.value(1));
    if (places < 0) places = 0;
    if (places > 30) places = 30;
    const Decimal d = double_arg(c, 0) ? decimal_of(format_double(round_double(double_of(c.value(0)), places))).rounded(places, false)
                                       : decimal_of(c.value(0)).rounded(places, false);
    std::string out = (d.negative && !d.is_zero() ? "-" : "") + with_thousands(d.integer);
    if (places > 0) {
        std::string fraction = d.fraction;
        fraction.resize(static_cast<std::size_t>(places), '0');
        out += "." + fraction;
    }
    return out;
}

std::string hex_of_bytes(const std::string& s) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char b : s) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

std::string base_text(unsigned long long n, int base) {
    if (n == 0) return "0";
    std::string out;
    while (n > 0) {
        const int d = static_cast<int>(n % static_cast<unsigned>(base));
        out.insert(out.begin(), static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
        n /= static_cast<unsigned>(base);
    }
    return out;
}

std::string fn_hex(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    if (number_literal(c, 0)) return base_text(static_cast<unsigned long long>(int_of(v)), 16);
    return hex_of_bytes(v);
}

std::string fn_unhex(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    std::string h = v;
    if (h.size() % 2 == 1) h.insert(h.begin(), '0');
    std::string out;
    for (std::size_t i = 0; i < h.size(); i += 2) {
        auto nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        const int a = nibble(h[i]), b = nibble(h[i + 1]);
        if (a < 0 || b < 0) return kNull;
        out.push_back(static_cast<char>(a * 16 + b));
    }
    return out;
}

std::string fn_bin(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    return base_text(static_cast<unsigned long long>(int_of(c.value(0))), 2);
}

std::string fn_oct(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    return base_text(static_cast<unsigned long long>(int_of(c.value(0))), 8);
}

std::string fn_conv(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long from = int_of(c.value(1)), to = int_of(c.value(2));
    const long long afrom = from < 0 ? -from : from, ato = to < 0 ? -to : to;
    if (afrom < 2 || afrom > 36 || ato < 2 || ato > 36) return kNull;
    std::string s = c.value(0);
    bool negative = false;
    std::size_t i = 0;
    if (!s.empty() && s[0] == '-') { negative = true; i = 1; }
    unsigned long long n = 0;
    for (; i < s.size(); i++) {
        const char ch = s[i];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : (ch >= 'a' && ch <= 'z' ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'Z' ? ch - 'A' + 10 : 99));
        if (d >= afrom) break;
        n = n * static_cast<unsigned long long>(afrom) + static_cast<unsigned>(d);
    }
    if (negative) n = 0 - n;
    if (to < 0 && static_cast<long long>(n) < 0) return "-" + base_text(0 - n, static_cast<int>(ato));
    return base_text(n, static_cast<int>(ato));
}

std::string fn_to_base64(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    while (i < v.size()) {
        const unsigned a = static_cast<unsigned char>(v[i]);
        const unsigned b = i + 1 < v.size() ? static_cast<unsigned char>(v[i + 1]) : 0;
        const unsigned d = i + 2 < v.size() ? static_cast<unsigned char>(v[i + 2]) : 0;
        out.push_back(tbl[a >> 2]);
        out.push_back(tbl[((a & 3) << 4) | (b >> 4)]);
        out.push_back(i + 1 < v.size() ? tbl[((b & 15) << 2) | (d >> 6)] : '=');
        out.push_back(i + 2 < v.size() ? tbl[d & 63] : '=');
        i += 3;
    }
    return out;
}

std::string fn_from_base64(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (char ch : v) {
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+') d = 62;
        else if (ch == '/') d = 63;
        else if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ') continue;
        else return kNull;
        buffer = (buffer << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

// ---- numbers ----------------------------------------------------------------------------------------------------------------------------------

std::string math1(const BuiltinCall& c, double (*f)(double), bool (*domain)(double) = nullptr) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    const double x = double_of(v);
    if (domain && !domain(x)) return kNull;
    const double r = f(x);
    if (std::isnan(r)) return kNull;
    if (std::isinf(r)) throw StatementError("DOUBLE value is out of range in '" + lower_ascii(c.name) + "(" + v + ")'");
    return format_double(r);
}

std::string fn_sqrt(const BuiltinCall& c) { return math1(c, [](double x) { return std::sqrt(x); }, [](double x) { return x >= 0; }); }
std::string fn_exp(const BuiltinCall& c) { return math1(c, [](double x) { return std::exp(x); }); }
std::string fn_ln(const BuiltinCall& c) { return math1(c, [](double x) { return std::log(x); }, [](double x) { return x > 0; }); }
std::string fn_log2(const BuiltinCall& c) { return math1(c, [](double x) { return std::log2(x); }, [](double x) { return x > 0; }); }
std::string fn_log10(const BuiltinCall& c) { return math1(c, [](double x) { return std::log10(x); }, [](double x) { return x > 0; }); }
std::string fn_sin(const BuiltinCall& c) { return math1(c, [](double x) { return std::sin(x); }); }
std::string fn_cos(const BuiltinCall& c) { return math1(c, [](double x) { return std::cos(x); }); }
std::string fn_tan(const BuiltinCall& c) { return math1(c, [](double x) { return std::tan(x); }); }
std::string fn_asin(const BuiltinCall& c) { return math1(c, [](double x) { return std::asin(x); }, [](double x) { return x >= -1 && x <= 1; }); }
std::string fn_acos(const BuiltinCall& c) { return math1(c, [](double x) { return std::acos(x); }, [](double x) { return x >= -1 && x <= 1; }); }
std::string fn_degrees(const BuiltinCall& c) { return math1(c, [](double x) { return x * 180.0 / 3.14159265358979323846; }); }
std::string fn_radians(const BuiltinCall& c) { return math1(c, [](double x) { return x * 3.14159265358979323846 / 180.0; }); }
std::string fn_cot(const BuiltinCall& c) {
    if (argc(c) == 1 && !is_null(c.value(0)) && double_of(c.value(0)) == 0) throw StatementError("DOUBLE value is out of range in 'cot(0)'");
    return math1(c, [](double x) { return 1.0 / std::tan(x); });
}

std::string fn_atan(const BuiltinCall& c) {
    if (argc(c) == 1) return math1(c, [](double x) { return std::atan(x); });
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return format_double(std::atan2(double_of(c.value(0)), double_of(c.value(1))));
}

std::string fn_pi(const BuiltinCall& c) {
    if (argc(c) != 0) wrong_count(c);
    return "3.141592653589793";
}

std::string fn_log(const BuiltinCall& c) {
    if (argc(c) == 1) return fn_ln(c);
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const double b = double_of(c.value(0)), x = double_of(c.value(1));
    if (b <= 1 || x <= 0) return kNull;
    return format_double(std::log(x) / std::log(b));
}

std::string fn_pow(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const double r = std::pow(double_of(c.value(0)), double_of(c.value(1)));
    if (std::isnan(r)) return kNull;
    if (std::isinf(r)) throw StatementError("DOUBLE value is out of range in 'pow(" + c.value(0) + "," + c.value(1) + ")'");
    return format_double(r);
}

std::string fn_sign(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    const Decimal d = decimal_of(v);
    return d.is_zero() ? "0" : (d.negative ? "-1" : "1");
}

std::string fn_abs(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    Decimal d = decimal_of(v);
    d.negative = false;
    return d.str(d.fraction.size()); // (the places the value was written with: ABS(-10.50) is 10.50)
}

// ROUND / TRUNCATE of a DECIMAL: it keeps the places it has up to the places asked for, never more (ROUND(10.50, 4) is 10.50, ROUND(12.00, 1) is 12.0,
// ROUND(3.5000, 2) is 3.50), and none for a negative number of places
std::string rounded_decimal(const Decimal& in, long long places, bool cut) {
    const Decimal r = in.rounded(places, cut);
    return r.str(std::min<std::size_t>(in.fraction.size(), static_cast<std::size_t>(std::max<long long>(places, 0))));
}

std::string fn_round(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long places = argc(c) == 2 ? int_of(c.value(1)) : 0;
    if (double_arg(c, 0)) return format_double(round_double(double_of(c.value(0)), places));
    return rounded_decimal(decimal_of(c.value(0)), places, false);
}

std::string fn_truncate(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return rounded_decimal(decimal_of(c.value(0)), int_of(c.value(1)), true);
}

std::string fn_floor(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    Decimal d = decimal_of(v);
    Decimal cut = d.rounded(0, true);
    if (d.negative && d.fraction.find_first_not_of('0') != std::string::npos) cut.add_unit(0); // (toward minus infinity: one more in magnitude)
    return cut.compact();
}

std::string fn_ceil(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    Decimal d = decimal_of(v);
    Decimal cut = d.rounded(0, true);
    if (!d.negative && d.fraction.find_first_not_of('0') != std::string::npos) cut.add_unit(0);
    return cut.compact();
}

std::string fn_mod(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const Decimal a = decimal_of(c.value(0)), b = decimal_of(c.value(1));
    if (b.is_zero()) {
        ++sqlval::division_by_zero_count;
        return kNull;
    }
    if (a.fraction.empty() && b.fraction.empty()) {
        if (auto x = a.to_int64()) {
            if (auto y = b.to_int64()) {
                if (*y == -1) return "0";
                return int_text(*x % *y); // (the sign of the dividend, as in MySQL)
            }
        }
    }
    if (!double_arg(c, 0) && !double_arg(c, 1)) {
        // DECIMALs: the remainder of the digits at the larger of the two scales, which it keeps (MOD(10.50, 3) is 1.50), with the sign of the dividend
        const sqlval::Num x = sqlval::align_scale(sqlval::Num::from_decimal(a), std::max<int>(static_cast<int>(a.fraction.size()), static_cast<int>(b.fraction.size())));
        const sqlval::Num y = sqlval::align_scale(sqlval::Num::from_decimal(b), x.scale);
        sqlval::Num r;
        r.scale = x.scale;
        r.digits = sqlval::sub_abs_digits(x.digits, sqlval::mul_abs_digits(sqlval::div_abs_digits(x.digits, y.digits), y.digits));
        r.negative = a.negative && r.digits != "0";
        return r.to_decimal().str(static_cast<std::size_t>(r.scale));
    }
    const double r = std::fmod(double_of(c.value(0)), double_of(c.value(1)));
    return format_double(r);
}

// GREATEST / LEAST: numbers when every argument is one, else texts
std::string extreme(const BuiltinCall& c, bool greatest) {
    if (argc(c) < 2) wrong_count(c);
    if (any_null(c)) return kNull;
    bool all_numbers = true;
    for (std::size_t i = 0; i < argc(c) && all_numbers; i++) {
        bool whole = false;
        leading_number(c.value(i), &whole);
        all_numbers = whole && !string_literal(c, i); // (a quoted string is a string, even a digit string)
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < argc(c); i++) {
        const std::string a = c.value(i), b = c.value(best);
        const bool better = all_numbers ? (greatest ? double_of(a) > double_of(b) : double_of(a) < double_of(b)) : (greatest ? collation::compare(a, b) > 0 : collation::compare(a, b) < 0);
        if (better) best = i;
    }
    return c.value(best);
}

std::string fn_greatest(const BuiltinCall& c) { return extreme(c, true); }
std::string fn_least(const BuiltinCall& c) { return extreme(c, false); }

// ---- dates ------------------------------------------------------------------------------------------------------------------------------------

long long mysql_daynr(long long y, int m, int d) {
    if (y == 0 && m == 0) return 0;
    long long delsum = 365 * y + 31 * (m - 1) + d;
    long long yy = y;
    if (m <= 2) yy--;
    else delsum -= (m * 4 + 23) / 10;
    const long long temp = ((yy / 100 + 1) * 3) / 4;
    return delsum + yy / 4 - temp;
}

int mysql_weekday(long long daynr, bool sunday_first) { return static_cast<int>((daynr + 5 + (sunday_first ? 1 : 0)) % 7); }

long long days_in_year(long long y) { return is_leap(y) ? 366 : 365; }

// MySQL's calc_week
unsigned calc_week(const DateTime& t, unsigned behaviour, long long* year_out) {
    const long long daynr = mysql_daynr(t.year, t.month, t.day);
    long long first_daynr = mysql_daynr(t.year, 1, 1);
    const bool monday_first = behaviour & 1, week_year = (behaviour & 2) != 0, first_weekday = (behaviour & 4) != 0;
    bool wy = week_year;
    int weekday = mysql_weekday(first_daynr, !monday_first);
    long long year = t.year;
    long long days;
    if (t.month == 1 && t.day <= static_cast<int>(7 - weekday)) {
        if (!wy && ((first_weekday && weekday != 0) || (!first_weekday && weekday >= 4))) {
            if (year_out) *year_out = year;
            return 0;
        }
        wy = true;
        year--;
        days = days_in_year(year);
        first_daynr -= days;
        weekday = static_cast<int>((weekday + 53 * 7 - days) % 7);
    }
    if ((first_weekday && weekday != 0) || (!first_weekday && weekday >= 4)) days = daynr - (first_daynr + (7 - weekday));
    else days = daynr - (first_daynr - weekday);
    if (wy && days >= 52 * 7) {
        weekday = static_cast<int>((weekday + days_in_year(year)) % 7);
        if ((!first_weekday && weekday < 4) || (first_weekday && weekday == 0)) {
            year++;
            if (year_out) *year_out = year;
            return 1;
        }
    }
    if (year_out) *year_out = year;
    return static_cast<unsigned>(days / 7 + 1);
}

unsigned week_behaviour(unsigned mode) {
    unsigned f = mode & 7;
    if (!(f & 1)) f ^= 4;
    return f;
}

std::optional<DateTime> date_arg(const std::string& v) {
    auto t = parse_datetime(v);
    if (!t || !t->valid_date()) return std::nullopt;
    return t;
}

std::string fn_year(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text(t->year) : kNull;
}

std::string fn_month(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text(t->month) : kNull;
}

std::string fn_day(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text(t->day) : kNull;
}

std::string fn_quarter(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text((t->month + 2) / 3) : kNull;
}

std::string time_part(const BuiltinCall& c, int which) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    auto secs = parse_time_seconds(c.value(0));
    if (!secs) return kNull;
    const long long s = *secs < 0 ? -*secs : *secs;
    return int_text(which == 0 ? s / 3600 : (which == 1 ? s / 60 % 60 : s % 60));
}

std::string fn_hour(const BuiltinCall& c) { return time_part(c, 0); }
std::string fn_minute(const BuiltinCall& c) { return time_part(c, 1); }
std::string fn_second(const BuiltinCall& c) { return time_part(c, 2); }

std::string fn_dayofweek(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text((weekday_monday0(t->epoch_days()) + 1) % 7 + 1) : kNull;
}

std::string fn_weekday(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text(weekday_monday0(t->epoch_days())) : kNull;
}

std::string fn_dayofyear(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text(t->epoch_days() - days_from_civil(t->year, 1, 1) + 1) : kNull;
}

std::string fn_week(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    auto t = date_arg(c.value(0));
    if (!t) return kNull;
    const unsigned mode = argc(c) == 2 ? static_cast<unsigned>(int_of(c.value(1))) : 0;
    long long y = 0;
    return int_text(calc_week(*t, week_behaviour(mode), &y));
}

std::string fn_weekofyear(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    if (!t) return kNull;
    long long y = 0;
    return int_text(calc_week(*t, week_behaviour(3), &y));
}

std::string fn_monthname(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? kMonthNames[t->month - 1] : kNull;
}

std::string fn_dayname(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? kDayNames[weekday_monday0(t->epoch_days())] : kNull;
}

std::string fn_last_day(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    if (!t) return kNull;
    t->day = days_in_month(t->year, t->month);
    return t->date_str();
}

std::string fn_date(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? t->date_str() : kNull;
}

int fraction_places(const std::string& text);
std::optional<long long> time_in_micro(const std::string& text);
std::string micro_to_time(long long micro, int places);

std::string fn_time(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    auto micro = time_in_micro(c.value(0));
    if (!micro) return kNull;
    return micro_to_time(*micro, fraction_places(c.value(0))); // (as many places of the fraction as the argument is written with)
}

std::string fn_datediff(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    auto a = date_arg(c.value(0)), b = date_arg(c.value(1));
    if (!a || !b) return kNull;
    return int_text(a->epoch_days() - b->epoch_days());
}

std::string fn_to_days(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    return t ? int_text(t->epoch_days() + 719528) : kNull;
}

std::string fn_from_days(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    const long long n = int_of(c.value(0));
    if (n < 366) return "0000-00-00";
    DateTime t = DateTime::from_epoch_seconds((n - 719528) * 86400);
    return t.date_str();
}

std::string fn_makedate(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long y = int_of(c.value(0)), doy = int_of(c.value(1));
    if (y < 0 || y > 9999 || doy < 1) return kNull;
    DateTime t = DateTime::from_epoch_seconds((days_from_civil(y, 1, 1) + doy - 1) * 86400);
    return t.year > 9999 ? kNull : t.date_str();
}

std::string seconds_to_time(long long s) {
    char b[48];
    s = std::clamp<long long>(s, -3020399, 3020399); // (a TIME value goes no further than 838:59:59)
    const long long a = s < 0 ? -s : s;
    std::snprintf(b, sizeof b, "%s%02lld:%02lld:%02lld", s < 0 ? "-" : "", a / 3600, a / 60 % 60, a % 60);
    return b;
}

// How many places the fraction of a second is written with in a time, a datetime or a number ("01:00:00.50" has 2); at most 6
int fraction_places(const std::string& text) {
    const std::size_t dot = text.rfind('.');
    if (dot == std::string::npos) return 0;
    int places = 0;
    for (std::size_t i = dot + 1; i < text.size() && text[i] >= '0' && text[i] <= '9'; i++) places++;
    return std::min(places, 6);
}

// A time as signed millionths of a second; nullopt when the text is no time
std::optional<long long> time_in_micro(const std::string& text) {
    long long micro = 0;
    auto seconds = parse_time_seconds(text, &micro);
    if (!seconds) return std::nullopt;
    const std::size_t first = text.find_first_not_of(' ');
    const bool negative = first != std::string::npos && text[first] == '-';
    const long long total = (*seconds < 0 ? -*seconds : *seconds) * 1000000 + micro;
    return negative ? -total : total;
}

// Millionths of a second rounded (half away from zero) to `places` places
long long round_to_places(long long micro, int places) {
    long long unit = 1;
    for (int k = places; k < 6; k++) unit *= 10;
    const long long magnitude = micro < 0 ? -micro : micro;
    const long long rounded = (magnitude + unit / 2) / unit * unit;
    return micro < 0 ? -rounded : rounded;
}

// "[-]HH:MM:SS[.ffffff]" for signed millionths of a second, with exactly `places` places of the fraction (rounded)
std::string micro_to_time(long long micro, int places) {
    micro = round_to_places(micro, places);
    {   // (a TIME value goes no further than 838:59:59.999999, with the places it is shown with)
        long long unit = 1;
        for (int k = places; k < 6; k++) unit *= 10;
        const long long top = 3020399LL * 1000000 + (1000000 - unit);
        micro = std::clamp(micro, -top, top);
    }
    const bool negative = micro < 0;
    const long long a = negative ? -micro : micro;
    const long long seconds = a / 1000000, fraction = a % 1000000;
    char b[64];
    std::snprintf(b, sizeof b, "%s%02lld:%02lld:%02lld", negative ? "-" : "", seconds / 3600, seconds / 60 % 60, seconds % 60);
    std::string out = b;
    if (places > 0) {
        char f[16];
        std::snprintf(f, sizeof f, "%06lld", fraction);
        out += "." + std::string(f).substr(0, static_cast<std::size_t>(places));
    }
    return out;
}

std::string fn_time_to_sec(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    auto s = parse_time_seconds(c.value(0));
    return s ? int_text(*s) : kNull;
}

std::string fn_sec_to_time(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    const int places = fraction_places(c.value(0));
    if (places == 0) return seconds_to_time(int_of(c.value(0)));
    // the seconds with their fraction, exactly: 3725.5 is 3725500000 millionths
    const Decimal d = decimal_of(c.value(0));
    std::string fraction = d.fraction.substr(0, 6);
    fraction.resize(6, '0');
    long long micro = std::atoll(d.integer.c_str()) * 1000000 + std::atoll(fraction.c_str());
    return micro_to_time(d.negative ? -micro : micro, places);
}

// TIME_FORMAT is DATE_FORMAT of a time
std::string fn_time_format(const BuiltinCall& c);

// ADDTIME / SUBTIME(a time or a datetime, a time)
std::string shift_time(const BuiltinCall& c, long long sign) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto amount = time_in_micro(c.value(1));
    if (!amount) return kNull;
    const int places = std::max(fraction_places(c.value(0)), fraction_places(c.value(1)));
    if (auto t = parse_datetime(c.value(0)); t && t->has_date && t->valid_date()) {
        const long long total = round_to_places(t->micro + sign * *amount, 6);
        long long seconds = total / 1000000, micro = total % 1000000;
        if (micro < 0) { micro += 1000000; seconds--; }
        const DateTime moved = DateTime::from_epoch_seconds(t->epoch_seconds() + seconds, micro);
        if (moved.year < 0 || moved.year > 9999) return kNull;
        return moved.date_str() + " " + micro_to_time(((moved.hour * 60LL + moved.minute) * 60 + moved.second) * 1000000 + moved.micro, places);
    }
    auto base = time_in_micro(c.value(0));
    if (!base) return kNull;
    return micro_to_time(*base + sign * *amount, places);
}

std::string fn_addtime(const BuiltinCall& c) { return shift_time(c, 1); }
std::string fn_subtime(const BuiltinCall& c) { return shift_time(c, -1); }

// TIMEDIFF(a, b): two datetimes or two times (a mix is NULL)
std::string fn_timediff(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto a = parse_datetime(c.value(0)), b = parse_datetime(c.value(1));
    const bool dated_a = a && a->has_date && a->valid_date(), dated_b = b && b->has_date && b->valid_date();
    const int places = std::max(fraction_places(c.value(0)), fraction_places(c.value(1)));
    if (dated_a && dated_b) return micro_to_time((a->epoch_seconds() - b->epoch_seconds()) * 1000000 + (a->micro - b->micro), places);
    if (dated_a != dated_b) return kNull;
    auto x = time_in_micro(c.value(0)), y = time_in_micro(c.value(1));
    if (!x || !y) return kNull;
    return micro_to_time(*x - *y, places);
}

// PERIOD_ADD(P, N) / PERIOD_DIFF(P1, P2): P is YYMM or YYYYMM
long long period_to_months(long long p) {
    long long year = p / 100;
    const long long month = p % 100;
    if (year < 70) year += 2000;
    else if (year < 100) year += 1900;
    return year * 12 + month - 1;
}

std::string fn_period_add(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long p = int_of(c.value(0));
    if (p == 0) return "0";
    const long long total = period_to_months(p) + int_of(c.value(1));
    return int_text(total / 12 * 100 + total % 12 + 1);
}

std::string fn_period_diff(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return int_text(period_to_months(int_of(c.value(0))) - period_to_months(int_of(c.value(1))));
}

std::string fn_yearweek(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    auto t = date_arg(c.value(0));
    if (!t) return kNull;
    const unsigned mode = argc(c) == 2 ? static_cast<unsigned>(int_of(c.value(1))) : 0;
    long long year = 0;
    const unsigned week = calc_week(*t, week_behaviour(mode) | 2, &year);
    return int_text(year * 100 + week);
}

std::string fn_to_seconds(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    auto t = date_arg(c.value(0));
    if (!t) return kNull;
    return int_text((t->epoch_days() + 719528) * 86400 + t->hour * 3600LL + t->minute * 60LL + t->second);
}

std::string fn_bit_count(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    unsigned long long bits = static_cast<unsigned long long>(int_of(c.value(0)));
    int n = 0;
    for (; bits; bits &= bits - 1) n++;
    return int_text(n);
}

// QUOTE(str): the string as a literal (backslash, quote, NUL and Ctrl-Z escaped); NULL is the word NULL
std::string fn_quote(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return "NULL";
    std::string out = "'";
    for (char ch : v) {
        if (ch == '\\' || ch == '\'') { out.push_back('\\'); out.push_back(ch); }
        else if (ch == '\0') out += "\\0";
        else if (ch == '\x1a') out += "\\Z";
        else out.push_back(ch);
    }
    return out + "'";
}

// EXPORT_SET(bits, on, off [, separator [, number_of_bits]])
std::string fn_export_set(const BuiltinCall& c) {
    if (argc(c) < 3 || argc(c) > 5) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1)) || is_null(c.value(2))) return kNull;
    const unsigned long long bits = static_cast<unsigned long long>(int_of(c.value(0)));
    const std::string on = c.value(1), off = c.value(2);
    const std::string separator = argc(c) >= 4 && !is_null(c.value(3)) ? c.value(3) : ",";
    long long count = argc(c) == 5 && !is_null(c.value(4)) ? int_of(c.value(4)) : 64;
    if (count < 0 || count > 64) count = 64;
    std::string out;
    for (long long i = 0; i < count; i++) {
        if (i) out += separator;
        out += (bits >> i) & 1 ? on : off;
    }
    return out;
}

// MAKE_SET(bits, s1, s2, ...): the strings whose bit is set, joined by commas (a NULL string is left out)
std::string fn_make_set(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    const unsigned long long bits = static_cast<unsigned long long>(int_of(c.value(0)));
    std::string out;
    bool first = true;
    for (std::size_t i = 1; i < argc(c) && i - 1 < 64; i++) {
        if (!((bits >> (i - 1)) & 1) || is_null(c.value(i))) continue;
        if (!first) out.push_back(',');
        out += c.value(i);
        first = false;
    }
    return out;
}

std::string unit_word(const BuiltinCall& c, std::size_t i) {
    std::string u = c.raw[i];
    if (u.size() >= 2 && u.front() == '\'' && u.back() == '\'') u = u.substr(1, u.size() - 2);
    return upper_ascii(u);
}

std::string add_to_date(const BuiltinCall& c, const std::string& date_text, long long amount, const std::string& unit) {
    auto t = date_arg(date_text);
    if (!t) {
        // a time alone: HOUR / MINUTE / SECOND move it round the clock
        return kNull;
    }
    auto r = add_interval(*t, unit, amount);
    if (!r) return kNull;
    const bool time_unit = unit == "HOUR" || unit == "MINUTE" || unit == "SECOND" || unit == "MICROSECOND";
    return (t->has_time || time_unit) ? r->datetime_str() : r->date_str();
}

// The units made of two or more of the plain ones: the plain unit each counts in, and how many of it each part of the text stands for, left to right
// (in months for YEAR_MONTH, in the smallest unit of the compound otherwise; the last part of a *_MICROSECOND unit is a fraction of a second)
struct CompoundUnit {
    const char* name;
    const char* plain;
    std::vector<long long> weights;
};

const CompoundUnit* compound_unit(const std::string& unit) {
    static const std::vector<CompoundUnit> units = {
        {"YEAR_MONTH", "MONTH", {12, 1}},
        {"DAY_HOUR", "HOUR", {24, 1}},
        {"DAY_MINUTE", "MINUTE", {1440, 60, 1}},
        {"DAY_SECOND", "SECOND", {86400, 3600, 60, 1}},
        {"DAY_MICROSECOND", "MICROSECOND", {86400000000LL, 3600000000LL, 60000000LL, 1000000LL, 1}},
        {"HOUR_MINUTE", "MINUTE", {60, 1}},
        {"HOUR_SECOND", "SECOND", {3600, 60, 1}},
        {"HOUR_MICROSECOND", "MICROSECOND", {3600000000LL, 60000000LL, 1000000LL, 1}},
        {"MINUTE_SECOND", "SECOND", {60, 1}},
        {"MINUTE_MICROSECOND", "MICROSECOND", {60000000LL, 1000000LL, 1}},
        {"SECOND_MICROSECOND", "MICROSECOND", {1000000LL, 1}},
    };
    for (const CompoundUnit& u : units) {
        if (unit == u.name) return &u;
    }
    return nullptr;
}

// '1:30' HOUR_MINUTE, '1 12:30' DAY_MINUTE, '2-3' YEAR_MONTH, '1.5' SECOND_MICROSECOND: the amount in the plain unit; fewer numbers than the unit has parts are the
// last parts ('30' HOUR_MINUTE is thirty minutes); a minus in front makes all of it negative. nullopt: no number in the text.
std::optional<long long> compound_amount(const CompoundUnit& unit, const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    bool negative = false;
    for (std::size_t i = 0; i < text.size(); i++) {
        const char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            current.push_back(ch);
        } else {
            if (!current.empty()) parts.push_back(current);
            current.clear();
            if (ch == '-' && parts.empty() && current.empty()) negative = true;
        }
    }
    if (!current.empty()) parts.push_back(current);
    if (parts.empty()) return 0; // (a text with no number in it is 0)
    if (parts.size() > unit.weights.size()) return std::nullopt;
    const bool ends_in_fraction = std::string(unit.name).find("MICROSECOND") != std::string::npos;
    long long total = 0;
    const std::size_t offset = unit.weights.size() - parts.size(); // (the parts that were left out are the leftmost)
    for (std::size_t i = 0; i < parts.size(); i++) {
        std::string digits = parts[i];
        if (ends_in_fraction && offset + i + 1 == unit.weights.size()) { // the fraction of a second: .5 is 500000 millionths
            digits.resize(6, '0');
        }
        if (digits.size() > 12) return std::nullopt;
        total += std::atoll(digits.c_str()) * unit.weights[offset + i];
    }
    return negative ? -total : total;
}

// The unit and the amount an INTERVAL gives to DATE_ADD / DATE_SUB: the plain unit and the number of it
void interval_of(const BuiltinCall& c, std::size_t amount_at, std::size_t unit_at, std::string& unit, std::optional<long long>& amount) {
    unit = unit_word(c, unit_at);
    if (const CompoundUnit* compound = compound_unit(unit)) {
        amount = compound_amount(*compound, c.value(amount_at));
        unit = compound->plain;
        return;
    }
    static const std::unordered_set<std::string> units = {"MICROSECOND", "SECOND", "MINUTE", "HOUR", "DAY", "WEEK", "MONTH", "QUARTER", "YEAR"};
    if (!units.count(unit)) throw StatementError("You have an error in your SQL syntax; unknown INTERVAL unit '" + unit + "'");
    amount = int_of(c.value(amount_at));
}

std::string fn_date_add(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1))) return kNull;
    std::string unit;
    std::optional<long long> amount;
    interval_of(c, 1, 2, unit, amount);
    if (!amount) return kNull;
    return add_to_date(c, c.value(0), *amount, unit);
}

std::string fn_date_sub(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1))) return kNull;
    std::string unit;
    std::optional<long long> amount;
    interval_of(c, 1, 2, unit, amount);
    if (!amount) return kNull;
    return add_to_date(c, c.value(0), -*amount, unit);
}

std::string fn_adddate(const BuiltinCall& c) {
    if (argc(c) == 3) return fn_date_add(c);
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return add_to_date(c, c.value(0), int_of(c.value(1)), "DAY");
}

std::string fn_subdate(const BuiltinCall& c) {
    if (argc(c) == 3) return fn_date_sub(c);
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return add_to_date(c, c.value(0), -int_of(c.value(1)), "DAY");
}

std::string fn_timestampadd(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (is_null(c.value(1)) || is_null(c.value(2))) return kNull;
    return add_to_date(c, c.value(2), int_of(c.value(1)), unit_word(c, 0));
}

std::string fn_timestampdiff(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    auto a = date_arg(c.value(1)), b = date_arg(c.value(2));
    if (!a || !b) return kNull;
    const std::string unit = unit_word(c, 0);
    const long long secs_a = a->epoch_seconds(), secs_b = b->epoch_seconds();
    if (unit == "MONTH" || unit == "QUARTER" || unit == "YEAR") {
        const bool forward = std::make_tuple(secs_b, b->micro) >= std::make_tuple(secs_a, a->micro);
        const DateTime& lo = forward ? *a : *b;
        const DateTime& hi = forward ? *b : *a;
        long long months = (hi.year - lo.year) * 12 + (hi.month - lo.month);
        if (std::make_tuple(hi.day, hi.hour, hi.minute, hi.second, hi.micro) < std::make_tuple(lo.day, lo.hour, lo.minute, lo.second, lo.micro)) months--;
        if (!forward) months = -months;
        return int_text(unit == "MONTH" ? months : (unit == "QUARTER" ? months / 3 : months / 12));
    }
    const long long micro_diff = (secs_b - secs_a) * 1000000 + (b->micro - a->micro);
    long long div;
    if (unit == "MICROSECOND") div = 1;
    else if (unit == "SECOND") div = 1000000;
    else if (unit == "MINUTE") div = 60LL * 1000000;
    else if (unit == "HOUR") div = 3600LL * 1000000;
    else if (unit == "DAY") div = 86400LL * 1000000;
    else if (unit == "WEEK") div = 7 * 86400LL * 1000000;
    else throw StatementError("You have an error in your SQL syntax; unknown TIMESTAMPDIFF unit '" + unit + "'");
    return int_text(micro_diff / div);
}

std::string fn_extract(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    const std::string unit = unit_word(c, 0);
    auto t = parse_datetime(c.value(1));
    if (const CompoundUnit* compound = compound_unit(unit); compound && std::string(compound->name) != "YEAR_MONTH") {
        // DAY_HOUR ... SECOND_MICROSECOND: the parts from the first to the last written one after the other, two digits each (the fraction of a second in six)
        if (is_null(c.value(1))) return kNull;
        long long micro = 0;
        bool negative_time = false;
        long long parts[5] = {0, 0, 0, 0, 0}; // day, hour, minute, second, microsecond
        if (t && t->has_date && t->valid_date()) {
            parts[0] = t->day;
            parts[1] = t->hour;
            parts[2] = t->minute;
            parts[3] = t->second;
            micro = t->micro;
        } else if (auto s = parse_time_seconds(c.value(1), &micro)) {
            const long long a = *s < 0 ? -*s : *s;
            parts[1] = a / 3600;
            parts[2] = a / 60 % 60;
            parts[3] = a % 60;
            const std::size_t first_character = c.value(1).find_first_not_of(' ');
            negative_time = first_character != std::string::npos && c.value(1)[first_character] == '-';
        } else {
            return kNull;
        }
        parts[4] = micro;
        static const long long scales[5] = {1, 100, 100, 100, 1000000};
        auto index_of = [](const std::string& word) { return word == "DAY" ? 0 : word == "HOUR" ? 1 : word == "MINUTE" ? 2 : word == "SECOND" ? 3 : 4; };
        const std::string name = compound->name;
        const std::size_t underscore = name.find('_');
        const int first = index_of(name.substr(0, underscore)), last = index_of(name.substr(underscore + 1));
        long long value = parts[first];
        for (int i = first + 1; i <= last; i++) value = value * scales[i] + parts[i];
        return int_text(negative_time ? -value : value);
    }
    if (unit == "HOUR" || unit == "MINUTE" || unit == "SECOND" || unit == "MICROSECOND") {
        if (is_null(c.value(1))) return kNull;
        long long micro = 0;
        auto s = parse_time_seconds(c.value(1), &micro);
        if (!s) return kNull;
        const long long a = *s < 0 ? -*s : *s;
        if (unit == "HOUR") return int_text(a / 3600);
        if (unit == "MINUTE") return int_text(a / 60 % 60);
        if (unit == "SECOND") return int_text(a % 60);
        return int_text(micro);
    }
    if (!t || !t->valid_date()) return kNull;
    if (unit == "YEAR") return int_text(t->year);
    if (unit == "MONTH") return int_text(t->month);
    if (unit == "DAY") return int_text(t->day);
    if (unit == "QUARTER") return int_text((t->month + 2) / 3);
    if (unit == "WEEK") { long long y = 0; return int_text(calc_week(*t, week_behaviour(0), &y)); }
    if (unit == "YEAR_MONTH") return int_text(t->year * 100 + t->month);
    throw StatementError("You have an error in your SQL syntax; unknown EXTRACT unit '" + unit + "'");
}

std::string two(long long n) {
    char b[16];
    std::snprintf(b, sizeof b, "%02lld", n);
    return b;
}

// GET_FORMAT(DATE | TIME | DATETIME, 'USA' | 'JIS' | 'ISO' | 'EUR' | 'INTERNAL'): the DATE_FORMAT / TIME_FORMAT string MySQL uses for that locale
std::string fn_get_format(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (is_null(c.value(1))) return kNull;
    std::string kind = unit_word(c, 0);
    if (kind == "TIMESTAMP") kind = "DATETIME";
    const std::string locale = upper_ascii(c.value(1));
    static const std::unordered_map<std::string, const char*> formats = {
        {"DATE/USA", "%m.%d.%Y"}, {"DATE/JIS", "%Y-%m-%d"}, {"DATE/ISO", "%Y-%m-%d"}, {"DATE/EUR", "%d.%m.%Y"}, {"DATE/INTERNAL", "%Y%m%d"},
        {"DATETIME/USA", "%Y-%m-%d %H.%i.%s"}, {"DATETIME/JIS", "%Y-%m-%d %H:%i:%s"}, {"DATETIME/ISO", "%Y-%m-%d %H:%i:%s"},
        {"DATETIME/EUR", "%Y-%m-%d %H.%i.%s"}, {"DATETIME/INTERNAL", "%Y%m%d%H%i%s"},
        {"TIME/USA", "%h:%i:%s %p"}, {"TIME/JIS", "%H:%i:%s"}, {"TIME/ISO", "%H:%i:%s"}, {"TIME/EUR", "%H.%i.%s"}, {"TIME/INTERNAL", "%H%i%s"},
    };
    auto it = formats.find(kind + "/" + locale);
    return it == formats.end() ? kNull : std::string(it->second);
}

// MAKETIME(hour, minute, second): [-]HH:MM:SS[.f]; NULL for a minute past 59 or a second of 60 or more; the hours stop at 838
std::string fn_maketime(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long hour = int_of(c.value(0)), minute = int_of(c.value(1));
    Decimal seconds = decimal_of(c.value(2));
    const bool negative_seconds = seconds.negative;
    if (minute < 0 || minute > 59 || negative_seconds) return kNull;
    const std::string whole = seconds.integer.empty() ? "0" : seconds.integer;
    if (whole.size() > 2 || std::stoi(whole) > 59) return kNull;
    std::string fraction = seconds.fraction.substr(0, 6);
    long long h = hour < 0 ? -hour : hour;
    std::string second_text = (whole.size() < 2 ? "0" : "") + whole;
    if (h > 838) {
        h = 838;
        return std::string(hour < 0 ? "-" : "") + "838:59:59";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%02lld:%02lld:", hour < 0 ? "-" : "", h, minute);
    return std::string(buf) + second_text + (fraction.empty() ? "" : "." + fraction);
}

// MICROSECOND(time or datetime): the fraction of the second in millionths
std::string fn_microsecond(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    long long micro = 0;
    if (!parse_time_seconds(c.value(0), &micro)) return kNull;
    return int_text(micro);
}

// TIMESTAMP(date or datetime [, time]): the datetime, with the time added when it is given
std::string fn_timestamp(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto t = date_arg(c.value(0));
    if (!t) return kNull;
    DateTime moved = *t;
    if (argc(c) == 2) {
        long long micro = 0;
        auto amount = parse_time_seconds(c.value(1), &micro);
        if (!amount) return kNull;
        const bool negative = c.value(1).find('-') != std::string::npos;
        moved = DateTime::from_epoch_seconds(t->epoch_seconds() + *amount, t->micro);
        if (micro) {
            const auto stepped = add_interval(moved, "MICROSECOND", negative ? -micro : micro);
            if (!stepped) return kNull;
            moved = *stepped;
        }
    }
    const int places = std::max(fraction_places(c.value(0)), argc(c) == 2 ? fraction_places(c.value(1)) : 0);
    return moved.date_str() + " " + micro_to_time(((moved.hour * 60LL + moved.minute) * 60 + moved.second) * 1000000 + moved.micro, places);
}

std::string ordinal_day(int d) {
    const char* suffix = (d % 100 >= 11 && d % 100 <= 13) ? "th" : (d % 10 == 1 ? "st" : d % 10 == 2 ? "nd" : d % 10 == 3 ? "rd" : "th");
    return std::to_string(d) + suffix;
}

std::string fn_date_format(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto t = parse_datetime(c.value(0));
    if (!t || !t->valid_date()) {
        // a bare time ('14:07:09') formats with the time specifiers only
        long long micro = 0;
        auto s = parse_time_seconds(c.value(0), &micro);
        if (!s) return kNull;
        DateTime tm;
        tm.hour = static_cast<int>(*s / 3600);
        tm.minute = static_cast<int>(*s / 60 % 60);
        tm.second = static_cast<int>(*s % 60);
        tm.micro = micro;
        t = tm;
        t->year = 0; t->month = 0; t->day = 0;
    }
    const std::string fmt = c.value(1);
    const bool dated = t->year != 0 || t->month != 0;
    std::string out;
    const long long days = dated ? t->epoch_days() : 0;
    for (std::size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) { out.push_back(fmt[i]); continue; }
        const char f = fmt[++i];
        const int hour12 = t->hour % 12 == 0 ? 12 : t->hour % 12;
        switch (f) {
            case 'a': out += dated ? std::string(kDayNames[weekday_monday0(days)]).substr(0, 3) : ""; break;
            case 'b': out += dated ? std::string(kMonthNames[t->month - 1]).substr(0, 3) : ""; break;
            case 'c': out += std::to_string(t->month); break;
            case 'D': out += ordinal_day(t->day); break;
            case 'd': out += two(t->day); break;
            case 'e': out += std::to_string(t->day); break;
            case 'f': { char b[16]; std::snprintf(b, sizeof b, "%06lld", t->micro); out += b; break; }
            case 'H': out += two(t->hour); break;
            case 'h': case 'I': out += two(hour12); break;
            case 'i': out += two(t->minute); break;
            case 'j': { char b[16]; std::snprintf(b, sizeof b, "%03lld", dated ? days - days_from_civil(t->year, 1, 1) + 1 : 0); out += b; break; }
            case 'k': out += std::to_string(t->hour); break;
            case 'l': out += std::to_string(hour12); break;
            case 'M': out += dated && t->month >= 1 ? kMonthNames[t->month - 1] : ""; break;
            case 'm': out += two(t->month); break;
            case 'p': out += t->hour < 12 ? "AM" : "PM"; break;
            case 'r': out += two(hour12) + ":" + two(t->minute) + ":" + two(t->second) + (t->hour < 12 ? " AM" : " PM"); break;
            case 'S': case 's': out += two(t->second); break;
            case 'T': out += two(t->hour) + ":" + two(t->minute) + ":" + two(t->second); break;
            case 'U': { long long y; out += two(calc_week(*t, week_behaviour(0), &y)); break; }
            case 'u': { long long y; out += two(calc_week(*t, week_behaviour(1), &y)); break; }
            case 'V': { long long y; out += two(calc_week(*t, week_behaviour(2), &y)); break; }
            case 'v': { long long y; out += two(calc_week(*t, week_behaviour(3), &y)); break; }
            case 'W': out += dated ? kDayNames[weekday_monday0(days)] : ""; break;
            case 'w': out += std::to_string((weekday_monday0(days) + 1) % 7); break;
            case 'X': { long long y = 0; calc_week(*t, week_behaviour(2), &y); char b[16]; std::snprintf(b, sizeof b, "%04lld", y); out += b; break; }
            case 'x': { long long y = 0; calc_week(*t, week_behaviour(3), &y); char b[16]; std::snprintf(b, sizeof b, "%04lld", y); out += b; break; }
            case 'Y': { char b[16]; std::snprintf(b, sizeof b, "%04lld", t->year); out += b; break; }
            case 'y': out += two(t->year % 100); break;
            case '%': out.push_back('%'); break;
            default: out.push_back(f); break; // (an unknown specifier is the character itself)
        }
    }
    return out;
}

std::string fn_time_format(const BuiltinCall& c) { return fn_date_format(c); }

std::string fn_str_to_date(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const std::string s = c.value(0), fmt = c.value(1);
    long long year = -1, month = -1, day = -1, hour = 0, minute = 0, second = 0, micro = 0, day_of_year = -1;
    int pm = -1;
    bool has_time = false;
    std::size_t p = 0;
    auto number = [&](std::size_t max_digits, long long& out) {
        std::size_t start = p;
        long long v = 0;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9' && p - start < max_digits) v = v * 10 + (s[p++] - '0');
        if (p == start) return false;
        out = v;
        return true;
    };
    for (std::size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            if (fmt[i] == ' ') { while (p < s.size() && s[p] == ' ') p++; continue; }
            if (p >= s.size() || s[p] != fmt[i]) return kNull;
            p++;
            continue;
        }
        const char f = fmt[++i];
        switch (f) {
            case 'Y': if (!number(4, year)) return kNull; break;
            case 'y': if (!number(2, year)) return kNull; year += year < 70 ? 2000 : 1900; break;
            case 'm': case 'c': if (!number(2, month)) return kNull; break;
            case 'd': case 'e': if (!number(2, day)) return kNull; break;
            case 'j': if (!number(3, day_of_year)) return kNull; break;
            case 'H': case 'k': if (!number(2, hour)) return kNull; has_time = true; break;
            case 'h': case 'I': case 'l': if (!number(2, hour)) return kNull; has_time = true; break;
            case 'i': if (!number(2, minute)) return kNull; has_time = true; break;
            case 'S': case 's': if (!number(2, second)) return kNull; has_time = true; break;
            case 'f': { std::size_t start = p; long long v = 0; if (!number(6, v)) return kNull; for (std::size_t k = p - start; k < 6; k++) v *= 10; micro = v; has_time = true; break; }
            case 'p': {
                if (p + 1 < s.size()) {
                    const std::string w = upper_ascii(s.substr(p, 2));
                    if (w == "AM") pm = 0; else if (w == "PM") pm = 1; else return kNull;
                    p += 2;
                } else return kNull;
                break;
            }
            case 'M': case 'b': {
                int found = 0;
                for (int k = 0; k < 12 && !found; k++) {
                    const std::string name = kMonthNames[k];
                    const std::string cand = f == 'M' ? name : name.substr(0, 3);
                    if (upper_ascii(s.substr(p, cand.size())) == upper_ascii(cand)) { found = k + 1; p += cand.size(); }
                }
                if (!found) return kNull;
                month = found;
                break;
            }
            case 'T': {
                long long a, b, d;
                if (!number(2, a) || p >= s.size() || s[p++] != ':' || !number(2, b) || p >= s.size() || s[p++] != ':' || !number(2, d)) return kNull;
                hour = a; minute = b; second = d; has_time = true;
                break;
            }
            case '%': if (p >= s.size() || s[p] != '%') return kNull; p++; break;
            default: return kNull;
        }
    }
    if (pm >= 0) hour = hour % 12 + (pm ? 12 : 0);
    if (day_of_year >= 0 && year >= 0 && month < 0 && day < 0) { // %j: the day of the year says the month and the day
        if (day_of_year < 1 || day_of_year > (is_leap(year) ? 366 : 365)) return kNull;
        long long y2;
        int m2, d2;
        civil_from_days(days_from_civil(year, 1, 1) + day_of_year - 1, y2, m2, d2);
        month = m2;
        day = d2;
    }
    if (year < 0 && month < 0 && day < 0) { // a time only
        if (!has_time) return kNull;
        return seconds_to_time(hour * 3600 + minute * 60 + second);
    }
    DateTime t;
    t.year = year < 0 ? 0 : year; t.month = month < 0 ? 0 : static_cast<int>(month); t.day = day < 0 ? 0 : static_cast<int>(day);
    t.hour = static_cast<int>(hour); t.minute = static_cast<int>(minute); t.second = static_cast<int>(second); t.micro = micro;
    if (!t.valid_date()) return kNull;
    if (t.hour > 23 || t.minute > 59 || t.second > 59) return kNull;
    return has_time ? t.datetime_str() : t.date_str();
}


// CONVERT_TZ(datetime, from, to): the zones known without MySQL's time zone tables, '+hh:mm' / '-hh:mm' and 'UTC'; a named zone ('Asia/Seoul') is NULL, as on a MySQL
// that has not loaded the tables
std::optional<long long> zone_offset_seconds(std::string zone) {
    for (char& ch : zone) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (zone == "UTC" || zone == "+00:00" || zone == "-00:00") return 0;
    if (zone.size() < 5 || (zone[0] != '+' && zone[0] != '-')) return std::nullopt;
    const std::size_t colon = zone.find(':');
    if (colon == std::string::npos || colon < 2 || colon + 3 != zone.size()) return std::nullopt;
    for (std::size_t k = 1; k < zone.size(); k++) {
        if (k != colon && !std::isdigit(static_cast<unsigned char>(zone[k]))) return std::nullopt;
    }
    const long long hours = std::atoll(zone.substr(1, colon - 1).c_str()), minutes = std::atoll(zone.substr(colon + 1).c_str());
    if (minutes > 59 || hours > 14 || (hours == 14 && minutes > 0) || (zone[0] == '-' && hours > 13)) return std::nullopt;
    const long long seconds = hours * 3600 + minutes * 60;
    return zone[0] == '-' ? -seconds : seconds;
}

std::string fn_convert_tz(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (any_null(c)) return kNull;
    auto t = parse_datetime(c.value(0));
    if (!t || !t->has_date || !t->valid_date()) return kNull;
    auto from = zone_offset_seconds(c.value(1)), to = zone_offset_seconds(c.value(2));
    if (!from || !to) return kNull;
    const DateTime moved = DateTime::from_epoch_seconds(t->epoch_seconds() - *from + *to, t->micro);
    if (moved.year < 0 || moved.year > 9999) return kNull;
    return moved.datetime_str();
}

// ---- CAST / CONVERT ---------------------------------------------------------------------------------------------------------------------------

// The number a string starts with, as MySQL converts a string to a number for CAST: sign, digits and a fraction (no exponent).
Decimal string_prefix_decimal(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '	')) i++;
    std::size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    std::size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
    }
    if (digits == 0) return Decimal{};
    if (auto d = Decimal::parse(s.substr(start, i - start))) return *d;
    return Decimal{};
}

Decimal cast_number(const BuiltinCall& c, const std::string& value) {
    if (value == "true") return *Decimal::parse("1");
    if (value == "false") return Decimal{};
    if (!string_literal(c, 0)) {
        if (auto d = Decimal::parse(value)) return *d;
    }
    return string_prefix_decimal(value);
}

// A negative number as an unsigned one: MySQL makes it a BIGINT first, so what is below the smallest BIGINT is the smallest (2^63), the rest wraps round
unsigned long long negative_as_unsigned(unsigned long long magnitude) {
    constexpr unsigned long long smallest = 1ULL << 63;
    return magnitude > smallest ? smallest : 0ULL - magnitude;
}

std::string json_cast(const std::string& text); // (the JSON functions are further down)

std::string fn_cast(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    const std::string val = c.value(0);
    if (is_null(val)) return kNull;
    std::string type = c.raw[1];
    if (type.size() >= 2 && type.front() == '\'' && type.back() == '\'') type = type.substr(1, type.size() - 2);
    type = upper_ascii(type);
    std::string base = type;
    std::vector<long long> params;
    if (const std::size_t lp = type.find('('); lp != std::string::npos) {
        base = type.substr(0, lp);
        std::string inner = type.substr(lp + 1, type.size() - lp - 2);
        std::size_t pos = 0;
        while (pos <= inner.size()) {
            const std::size_t comma = inner.find(',', pos);
            params.push_back(std::atoll(inner.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str()));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    if (base == "SIGNED" || base == "INT" || base == "INTEGER" || base == "BIGINT" || base == "SMALLINT" || base == "TINYINT" || base == "MEDIUMINT") {
        const Decimal d = cast_number(c, val).rounded(0, string_literal(c, 0)); // (a quoted string is cut at its integer part, a number is rounded)
        if (auto v = d.to_int64()) return int_text(*v);
        return d.negative ? "-9223372036854775808" : "9223372036854775807";
    }
    if (base == "UNSIGNED") {
        const Decimal d = cast_number(c, val).rounded(0, string_literal(c, 0));
        if (d.integer.size() > 20) return d.negative ? "9223372036854775808" : "18446744073709551615";
        unsigned long long v = 0;
        for (char ch : d.integer) {
            constexpr unsigned long long largest = std::numeric_limits<unsigned long long>::max();
            if (v > largest / 10 || (v == largest / 10 && static_cast<unsigned>(ch - '0') > largest % 10)) return d.negative ? "9223372036854775808" : "18446744073709551615";
            v = v * 10 + static_cast<unsigned>(ch - '0');
        }
        if (d.negative && v != 0) v = negative_as_unsigned(v); // (a negative number wraps round, as in MySQL)
        return std::to_string(v);
    }
    if (base == "DECIMAL" || base == "NUMERIC" || base == "DEC" || base == "FIXED") {
        long long precision = params.size() > 0 ? params[0] : 10, scale = params.size() > 1 ? params[1] : 0;
        if (precision < 1) precision = 10;
        if (scale < 0) scale = 0;
        if (scale > precision) scale = precision;
        Decimal d = cast_number(c, val).rounded(scale, false);
        if (static_cast<long long>(d.integer == "0" ? 0 : d.integer.size()) > precision - scale) { // too many digits: the largest value that fits
            d.integer = std::string(static_cast<std::size_t>(precision - scale), '9');
            if (precision == scale) d.integer = "0";
            d.fraction = std::string(static_cast<std::size_t>(scale), '9');
        }
        return d.str(static_cast<std::size_t>(scale));
    }
    if (base == "FLOAT") return format_float_display(format_double(static_cast<double>(static_cast<float>(double_of(val))))); // (single precision, shown with six digits)
    if (base == "DOUBLE" || base == "REAL") return format_double(double_of(val));
    if (base == "JSON") return json_cast(val);
    if (base == "CHAR" || base == "VARCHAR" || base == "NCHAR" || base == "TEXT" || base == "BINARY") {
        if (!params.empty() && params[0] >= 0) return substring_chars(utf8_chars(val), 1, params[0]);
        return val;
    }
    if (base == "DATE") {
        auto t = date_arg(val);
        return t ? t->date_str() : kNull;
    }
    if (base == "DATETIME" || base == "TIMESTAMP") {
        auto t = date_arg(val);
        if (!t) return kNull;
        const int places = params.empty() ? 0 : static_cast<int>(std::clamp<long long>(params[0], 0, 6));
        // the fraction of the second rounds to the places asked for, and a carry moves the date on
        const long long total = round_to_places(t->micro, places);
        DateTime moved = DateTime::from_epoch_seconds(t->epoch_seconds() + total / 1000000, total % 1000000);
        if (moved.year < 0 || moved.year > 9999) return kNull;
        return moved.date_str() + " " + micro_to_time(((moved.hour * 60LL + moved.minute) * 60 + moved.second) * 1000000 + moved.micro, places);
    }
    if (base == "TIME") {
        auto micro = time_in_micro(val);
        if (!micro) return kNull;
        return micro_to_time(*micro, params.empty() ? 0 : static_cast<int>(std::clamp<long long>(params[0], 0, 6)));
    }
    if (base == "YEAR") { // 1-69 are 2001-2069, 70-99 are 1970-1999, the number 0 is 0000 (the string '0' is 2000); outside 1901-2155 is NULL
        const bool quoted = string_literal(c, 0);
        if (quoted && !std::any_of(val.begin(), val.end(), [](unsigned char ch) { return std::isdigit(ch); })) return kNull; // ('abc')
        auto v = decimal_of(val).rounded(0, false).to_int64();
        if (!v) return kNull;
        long long year = *v;
        if (year >= 1 && year <= 99) year += year < 70 ? 2000 : 1900;
        if (year == 0) return quoted ? "2000" : "0000";
        return year < 1901 || year > 2155 ? kNull : int_text(year);
    }
    if (base == "BOOLEAN" || base == "BOOL") {
        const bool b = !val.empty() && val != "0" && lower_ascii(val) != "false";
        return b ? "1" : "0";
    }
    throw StatementError("You have an error in your SQL syntax; unsupported type '" + type + "' in CAST");
}


// ---- operators written as functions (the parser makes them) ---------------------------------------------------------------------------------

unsigned long long unsigned_of(const std::string& text) {
    const Decimal d = decimal_of(text).rounded(0, false);
    if (d.integer.size() > 20) return d.negative ? 1ULL << 63 : std::numeric_limits<unsigned long long>::max();
    unsigned long long v = 0;
    for (char ch : d.integer) {
        constexpr unsigned long long largest = std::numeric_limits<unsigned long long>::max();
        if (v > largest / 10 || (v == largest / 10 && static_cast<unsigned>(ch - '0') > largest % 10)) return d.negative ? 1ULL << 63 : largest; // (the digits past the largest 64-bit number)
        v = v * 10 + static_cast<unsigned>(ch - '0');
    }
    return d.negative ? negative_as_unsigned(v) : v;
}

std::string fn_bit_and(const BuiltinCall& c) { if (argc(c) != 2) wrong_count(c); if (any_null(c)) return kNull; return std::to_string(unsigned_of(c.value(0)) & unsigned_of(c.value(1))); }
std::string fn_bit_or(const BuiltinCall& c) { if (argc(c) != 2) wrong_count(c); if (any_null(c)) return kNull; return std::to_string(unsigned_of(c.value(0)) | unsigned_of(c.value(1))); }
std::string fn_bit_xor(const BuiltinCall& c) { if (argc(c) != 2) wrong_count(c); if (any_null(c)) return kNull; return std::to_string(unsigned_of(c.value(0)) ^ unsigned_of(c.value(1))); }
std::string fn_bit_not(const BuiltinCall& c) { if (argc(c) != 1) wrong_count(c); if (any_null(c)) return kNull; return std::to_string(~unsigned_of(c.value(0))); }
std::string fn_shl(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const unsigned long long n = unsigned_of(c.value(1));
    return n >= 64 ? "0" : std::to_string(unsigned_of(c.value(0)) << n);
}
std::string fn_shr(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const unsigned long long n = unsigned_of(c.value(1));
    return n >= 64 ? "0" : std::to_string(unsigned_of(c.value(0)) >> n);
}

std::string fn_null_safe_eq(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    const std::string a = c.value(0), b = c.value(1);
    if (is_null(a) || is_null(b)) return is_null(a) && is_null(b) ? "1" : "0";
    auto da = Decimal::parse(a), db = Decimal::parse(b);
    if (da && db) return da->compact() == db->compact() ? "1" : "0";
    return a == b ? "1" : "0";
}

std::string fn_int_div(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const Decimal a = decimal_of(c.value(0)), b = decimal_of(c.value(1));
    if (b.is_zero()) {
        ++sqlval::division_by_zero_count;
        return kNull;
    }
    const Num q = num_div(Num::from_decimal(a), Num::from_decimal(b), 0, true); // (cut toward zero)
    const Decimal d = q.to_decimal();
    if (!d.to_int64()) throw StatementError("BIGINT value is out of range in '(" + c.value(0) + " DIV " + c.value(1) + ")'");
    return d.compact();
}

// ANY_VALUE(x) is x: a function, not an aggregate (a query without GROUP BY gives one row per row). What it does is tell ONLY_FULL_GROUP_BY that x may be any value of the
// group, and the row a group is shown with is its first.
std::string fn_any_value(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    return c.value(0);
}

std::string fn_xor(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const bool a = !decimal_of(c.value(0)).is_zero(), b = !decimal_of(c.value(1)).is_zero();
    return a != b ? "1" : "0";
}


// ---- hashes -----------------------------------------------------------------------------------------------------------------------------------

std::string hex_digest(const unsigned char* bytes, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < n; i++) {
        out.push_back(d[bytes[i] >> 4]);
        out.push_back(d[bytes[i] & 15]);
    }
    return out;
}

std::string sha1_hex(const std::string& msg) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string data = msg;
    const std::uint64_t bits = static_cast<std::uint64_t>(msg.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; i--) data.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4 + 2])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    unsigned char out[20];
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<unsigned char>(h[i] >> (24 - j * 8));
    return hex_digest(out, 20);
}

std::string sha256_hex(const std::string& msg) {
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string data = msg;
    const std::uint64_t bits = static_cast<std::uint64_t>(msg.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; i--) data.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    auto ror = [](std::uint32_t v, int n) { return (v >> n) | (v << (32 - n)); };
    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4 + 2])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(data[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 64; i++) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const std::uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    unsigned char out[32];
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<unsigned char>(h[i] >> (24 - j * 8));
    return hex_digest(out, 32);
}

std::string fn_sha1(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    return is_null(c.value(0)) ? kNull : sha1_hex(c.value(0));
}

std::string fn_sha2(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const long long bits = int_of(c.value(1));
    if (bits == 256 || bits == 0) return sha256_hex(c.value(0));
    return kNull; // (the other lengths are not offered)
}

std::string fn_crc32(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    std::uint32_t crc = 0xFFFFFFFF;
    for (unsigned char b : c.value(0)) {
        crc ^= b;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return std::to_string(~crc);
}

std::string fn_soundex(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    const std::string v = c.value(0);
    if (is_null(v)) return kNull;
    auto code = [](char ch) -> char {
        switch (std::toupper(static_cast<unsigned char>(ch))) {
            case 'B': case 'F': case 'P': case 'V': return '1';
            case 'C': case 'G': case 'J': case 'K': case 'Q': case 'S': case 'X': case 'Z': return '2';
            case 'D': case 'T': return '3';
            case 'L': return '4';
            case 'M': case 'N': return '5';
            case 'R': return '6';
            default: return '0';
        }
    };
    std::string out;
    char last = '0';
    for (char ch : v) {
        if (!std::isalpha(static_cast<unsigned char>(ch))) continue;
        const char cd = code(ch);
        if (out.empty()) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            last = cd;
        } else {
            const char upper_ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (cd != '0' && cd != last) out.push_back(cd);
            if (upper_ch != 'H' && upper_ch != 'W') last = cd; // (H and W do not separate two letters of the same code)
        }
    }
    if (out.empty()) return "";
    while (out.size() < 4) out.push_back('0');
    return out;
}

// ---- JSON -------------------------------------------------------------------------------------------------------------------------------------
//
// A JSON value is read into a tree and written back the way MySQL writes it: one space after every `,` and `:`, the members of an object ordered (the
// shorter keys first, then by bytes, a key that comes twice keeps its last value), a number as MySQL keeps it (an integer as written, a fraction as the
// shortest DOUBLE that reads back, `1e3` as `1000.0`), a string with its escapes decoded. Values are text everywhere in this engine, so a function that
// is handed a value decides from the way it was written (a quoted string, a number, TRUE) and from the text itself what JSON value it is.

struct Json {
    enum Kind { Null, Bool, Int, UInt, Dbl, Dec, String, Array, Object } kind = Null;
    bool b = false;
    double d = 0;     // Dbl
    std::string text; // Int, UInt, Dec: the number; String: the string
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members; // MySQL's order: shorter keys first, then by bytes
};

void sort_members(Json& j) {
    std::stable_sort(j.members.begin(), j.members.end(), [](const auto& a, const auto& b) {
        return a.first.size() != b.first.size() ? a.first.size() < b.first.size() : a.first < b.first;
    });
}

struct JsonParser {
    const std::string& s;
    std::size_t i = 0;
    std::string reason; // why the text is no JSON (MySQL's words), and where
    std::size_t at = 0;
    explicit JsonParser(const std::string& text) : s(text) {}
    bool fail(const char* why, std::size_t pos) {
        if (reason.empty()) { reason = why; at = pos; }
        return false;
    }
    void blank() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool hex4(std::size_t p, std::uint32_t& cp) const {
        if (p + 4 > s.size()) return false;
        cp = 0;
        for (std::size_t k = 0; k < 4; k++) {
            const char h = s[p + k];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string_token(std::string& out) { // (s[i] is the opening quote)
        i++;
        while (i < s.size()) {
            const unsigned char ch = static_cast<unsigned char>(s[i]);
            if (ch == '"') { i++; return true; }
            if (ch < 0x20) return fail("Invalid encoding in string.", i);
            if (ch != '\\') { out.push_back(static_cast<char>(ch)); i++; continue; }
            if (i + 1 >= s.size()) break;
            switch (s[i + 1]) {
                case '"': out.push_back('"'); i += 2; break;
                case '\\': out.push_back('\\'); i += 2; break;
                case '/': out.push_back('/'); i += 2; break;
                case 'b': out.push_back('\b'); i += 2; break;
                case 'f': out.push_back('\f'); i += 2; break;
                case 'n': out.push_back('\n'); i += 2; break;
                case 'r': out.push_back('\r'); i += 2; break;
                case 't': out.push_back('\t'); i += 2; break;
                case 'u': {
                    std::uint32_t cp = 0, low = 0;
                    if (!hex4(i + 2, cp)) return fail("Incorrect hex digit in \\u escape.", i);
                    i += 6;
                    if (cp >= 0xD800 && cp <= 0xDBFF) { // a high surrogate wants the low one that follows it
                        if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u' && hex4(i + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i += 6;
                        } else {
                            return fail("Invalid encoding in string.", i);
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("Invalid encoding in string.", i);
                    }
                    out += utf8_encode(cp);
                    break;
                }
                default: return fail("Invalid escape character in string.", i + 1);
            }
        }
        return fail("Missing a closing quotation mark in string.", s.size());
    }
    bool number(Json& j) {
        const std::size_t start = i;
        if (i < s.size() && s[i] == '-') i++;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("Invalid value.", i);
        if (s[i] == '0') i++;
        else while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        bool fraction = false;
        if (i < s.size() && s[i] == '.') {
            i++;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("Miss fraction part in number.", i);
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            fraction = true;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("Miss exponent in number.", i);
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            fraction = true;
        }
        const std::string token = s.substr(start, i - start);
        if (!fraction) {
            errno = 0;
            if (token[0] == '-') {
                const long long v = std::strtoll(token.c_str(), nullptr, 10);
                if (errno == 0) { j.kind = Json::Int; j.text = std::to_string(v); return true; }
            } else {
                const unsigned long long v = std::strtoull(token.c_str(), nullptr, 10);
                if (errno == 0) { j.kind = v <= static_cast<unsigned long long>(std::numeric_limits<long long>::max()) ? Json::Int : Json::UInt; j.text = std::to_string(v); return true; }
            }
        }
        j.kind = Json::Dbl;
        j.d = std::strtod(token.c_str(), nullptr);
        return true;
    }
    bool value(Json& j, int depth) {
        if (depth > 100) return fail("The JSON document exceeds the maximum depth.", i);
        blank();
        if (i >= s.size()) return fail(depth == 0 ? "The document is empty." : "Invalid value.", i);
        const char ch = s[i];
        if (ch == '{') {
            i++;
            j.kind = Json::Object;
            blank();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                blank();
                std::string key;
                if (i >= s.size() || s[i] != '"') return fail("Missing a name for object member.", i);
                if (!string_token(key)) return false;
                blank();
                if (i >= s.size() || s[i] != ':') return fail("Missing a colon after a name of object member.", i);
                i++;
                Json v;
                if (!value(v, depth + 1)) return false;
                bool replaced = false;
                for (auto& m : j.members) if (m.first == key) { m.second = v; replaced = true; }
                if (!replaced) j.members.emplace_back(std::move(key), std::move(v));
                blank();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                return fail("Missing a comma or '}' after an object member.", i);
            }
            sort_members(j);
            return true;
        }
        if (ch == '[') {
            i++;
            j.kind = Json::Array;
            blank();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            for (;;) {
                Json v;
                if (!value(v, depth + 1)) return false;
                j.items.push_back(std::move(v));
                blank();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                return fail("Missing a comma or ']' after an array element.", i);
            }
            return true;
        }
        if (ch == '"') {
            j.kind = Json::String;
            return string_token(j.text);
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; j.kind = Json::Bool; j.b = true; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; j.kind = Json::Bool; j.b = false; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; j.kind = Json::Null; return true; }
        return number(j);
    }
    std::optional<Json> parse() {
        Json j;
        if (!value(j, 0)) return std::nullopt;
        blank();
        if (i != s.size()) { fail("The document root must not be followed by other values.", i); return std::nullopt; }
        return j;
    }
};

std::optional<Json> parse_json(const std::string& text) { return JsonParser(text).parse(); }

// A document argument: the JSON text of argument `n` (1-based) of the function `fname`, or MySQL's error 3141 saying why it is none.
Json parse_json_arg(const std::string& text, std::size_t n, const std::string& fname) {
    JsonParser parser(text);
    auto j = parser.parse();
    if (!j) {
        throw StatementError("Invalid JSON text in argument " + std::to_string(n) + " to function " + fname + ": \"" + parser.reason + "\" at position " +
                             std::to_string(parser.at) + ".");
    }
    return *j;
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); out += b; }
                else out.push_back(static_cast<char>(ch));
        }
    }
    return out + "\"";
}

// A DOUBLE the way MySQL writes it in a JSON document: the shortest digits that read back, `1000.0` for a whole number, `1e15` from 10^15 on
// (the digits then as d.ddd), a small number out in full (`0.00001`).
std::string json_double_text(double v) {
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    if (!std::isfinite(v)) return "0.0";
    char buf[48];
    for (int digits = 1; digits <= 17; digits++) {
        std::snprintf(buf, sizeof buf, "%.*e", digits - 1, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    const std::string shown = buf; // d.ddde+XX
    const bool negative = shown[0] == '-';
    const std::size_t e = shown.find('e');
    std::string digits;
    for (std::size_t k = negative ? 1 : 0; k < e; k++) if (shown[k] != '.') digits.push_back(shown[k]);
    const int exponent = std::atoi(shown.c_str() + e + 1);
    std::string out = negative ? "-" : "";
    if (exponent >= 15) {
        out += digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        return out + "e" + std::to_string(exponent);
    }
    if (exponent < 0) return out + "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + digits;
    const std::size_t whole = static_cast<std::size_t>(exponent) + 1;
    if (digits.size() <= whole) return out + digits + std::string(whole - digits.size(), '0') + ".0";
    return out + digits.substr(0, whole) + "." + digits.substr(whole);
}

std::string json_text(const Json& j) {
    switch (j.kind) {
        case Json::Null: return "null";
        case Json::Bool: return j.b ? "true" : "false";
        case Json::Int: case Json::UInt: case Json::Dec: return j.text;
        case Json::Dbl: return json_double_text(j.d);
        case Json::String: return json_quote(j.text);
        case Json::Array: {
            std::string out = "[";
            for (std::size_t k = 0; k < j.items.size(); k++) out += (k ? ", " : "") + json_text(j.items[k]);
            return out + "]";
        }
        case Json::Object: {
            std::string out = "{";
            for (std::size_t k = 0; k < j.members.size(); k++) out += (k ? ", " : "") + json_quote(j.members[k].first) + ": " + json_text(j.members[k].second);
            return out + "}";
        }
    }
    return "null";
}

// JSON_PRETTY: two spaces for every level, `"key": value`, an empty array or object as [] and {}
void json_pretty_into(const Json& j, int level, std::string& out) {
    const std::string pad(static_cast<std::size_t>(level) * 2 + 2, ' ');
    const std::string close_pad(static_cast<std::size_t>(level) * 2, ' ');
    if (j.kind == Json::Array && !j.items.empty()) {
        out += "[\n";
        for (std::size_t k = 0; k < j.items.size(); k++) {
            out += pad;
            json_pretty_into(j.items[k], level + 1, out);
            out += k + 1 < j.items.size() ? ",\n" : "\n";
        }
        out += close_pad + "]";
    } else if (j.kind == Json::Object && !j.members.empty()) {
        out += "{\n";
        for (std::size_t k = 0; k < j.members.size(); k++) {
            out += pad + json_quote(j.members[k].first) + ": ";
            json_pretty_into(j.members[k].second, level + 1, out);
            out += k + 1 < j.members.size() ? ",\n" : "\n";
        }
        out += close_pad + "}";
    } else {
        out += json_text(j);
    }
}

bool is_json_number_text(const std::string& v) {
    JsonParser p(v);
    Json j;
    if (v.empty() || !(v[0] == '-' || (v[0] >= '0' && v[0] <= '9')) || !p.number(j)) return false;
    return p.i == v.size();
}

// An SQL value used as a JSON value. A string written in quotes is a string; TRUE and FALSE are booleans; a number is a number (one written with an
// exponent, or made in floating point, a DOUBLE; one with a fraction a DECIMAL kept as written); text that is a JSON object, array or string (a
// function's answer such as JSON_ARRAY(...) or JSON_EXTRACT(...) arrives as such text) is that document; anything else is a string.
Json json_from_sql(const BuiltinCall& c, std::size_t i) {
    const std::string v = c.value(i);
    Json j;
    if (is_null(v)) return j;
    const std::string word = upper_ascii(c.raw[i]);
    if (word == "TRUE" || word == "FALSE") { j.kind = Json::Bool; j.b = word == "TRUE"; return j; }
    if (!string_literal(c, i)) {
        if (v == "true" || v == "false") { j.kind = Json::Bool; j.b = v == "true"; return j; }
        if (is_json_number_text(v)) {
            const bool exponent = is_json_number_text(c.raw[i]) && c.raw[i].find_first_of("eE") != std::string::npos; // (a literal such as 1e3 or 2.5E0)
            const bool floating = i < c.kinds.size() && c.kinds[i] == 'D' && v.find_first_of(".eE") != std::string::npos; // (a DOUBLE column or an expression done in floating point)
            if (exponent || floating) { j.kind = Json::Dbl; j.d = std::strtod(v.c_str(), nullptr); return j; }
            if (v.find('.') != std::string::npos) { j.kind = Json::Dec; j.text = v; return j; }
            JsonParser p(v);
            p.number(j);
            if (j.kind == Json::Dbl) { j.kind = Json::Dec; j.text = v; } // (an integer too large for 64 bits is a DECIMAL)
            return j;
        }
        if (!v.empty() && (v[0] == '{' || v[0] == '[' || v[0] == '"')) {
            if (auto parsed = parse_json(v)) return *parsed;
        }
    }
    j.kind = Json::String;
    j.text = v;
    return j;
}

bool json_is_number(const Json& j) { return j.kind == Json::Int || j.kind == Json::UInt || j.kind == Json::Dec || j.kind == Json::Dbl; }

long double json_number(const Json& j) { return j.kind == Json::Dbl ? static_cast<long double>(j.d) : std::strtold(j.text.c_str(), nullptr); }

bool json_equal(const Json& a, const Json& b) {
    if (json_is_number(a) && json_is_number(b)) return json_number(a) == json_number(b);
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Json::Null: return true;
        case Json::Bool: return a.b == b.b;
        case Json::String: return a.text == b.text;
        case Json::Array:
            if (a.items.size() != b.items.size()) return false;
            for (std::size_t k = 0; k < a.items.size(); k++) if (!json_equal(a.items[k], b.items[k])) return false;
            return true;
        case Json::Object:
            if (a.members.size() != b.members.size()) return false;
            for (std::size_t k = 0; k < a.members.size(); k++) {
                if (a.members[k].first != b.members[k].first || !json_equal(a.members[k].second, b.members[k].second)) return false;
            }
            return true;
        default: return false;
    }
}

// How JSON values order: by type first (null, numbers, strings, objects, arrays, booleans), within a type by value. A boolean against 0 or 1 compares as that
// number, because the literal TRUE is the number 1 in this engine (`doc->'$.on' = TRUE`).
int json_type_rank(const Json& j) {
    switch (j.kind) {
        case Json::Null: return 0;
        case Json::String: return 2;
        case Json::Object: return 3;
        case Json::Array: return 4;
        case Json::Bool: return 5;
        default: return 1;
    }
}

int json_order(const Json& a, const Json& b) {
    auto sign = [](long double x, long double y) { return x < y ? -1 : (x > y ? 1 : 0); };
    if (json_is_number(a) && json_is_number(b)) return sign(json_number(a), json_number(b));
    if (a.kind == Json::Bool && json_is_number(b) && (json_number(b) == 0 || json_number(b) == 1)) return sign(a.b ? 1 : 0, json_number(b));
    if (b.kind == Json::Bool && json_is_number(a) && (json_number(a) == 0 || json_number(a) == 1)) return sign(json_number(a), b.b ? 1 : 0);
    const int ra = json_type_rank(a), rb = json_type_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (a.kind) {
        case Json::String: { const int c = a.text.compare(b.text); return c < 0 ? -1 : (c > 0 ? 1 : 0); }
        case Json::Bool: return sign(a.b ? 1 : 0, b.b ? 1 : 0);
        case Json::Array:
            for (std::size_t k = 0; k < a.items.size() && k < b.items.size(); k++) {
                if (const int c = json_order(a.items[k], b.items[k])) return c;
            }
            return sign(static_cast<long double>(a.items.size()), static_cast<long double>(b.items.size()));
        case Json::Object:
            if (a.members.size() != b.members.size()) return sign(static_cast<long double>(a.members.size()), static_cast<long double>(b.members.size()));
            for (std::size_t k = 0; k < a.members.size(); k++) {
                if (const int c = a.members[k].first.compare(b.members[k].first)) return c < 0 ? -1 : 1;
                if (const int c = json_order(a.members[k].second, b.members[k].second)) return c;
            }
            return 0;
        default: return 0;
    }
}

// ---- paths ----

struct PathLeg {
    enum Kind { Key, KeyWild, Index, Range, IndexWild, Descent } kind = Key;
    std::string key;
    long long from = 0, to = 0;       // Index: from; Range: from .. to
    bool from_last = false, to_last = false; // counted from the end of the array (`last`, `last-2`)
};

struct JsonPath {
    std::vector<PathLeg> legs;
    bool wild = false; // a `*`, `**` or a range: more than one value may match
};

[[noreturn]] void bad_path(std::size_t position) {
    throw StatementError("Invalid JSON path expression. The error is around character position " + std::to_string(position) + ".");
}

JsonPath parse_path(const std::string& text) {
    JsonPath path;
    const std::string& s = text;
    std::size_t i = 0;
    auto blank = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; };
    auto digits = [&](long long& out) {
        const std::size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i == start || i - start > 17) return false;
        out = std::atoll(s.substr(start, i - start).c_str());
        return true;
    };
    // an array position: a number, `last` or `last - n`
    auto position = [&](long long& n, bool& from_last) {
        blank();
        from_last = false;
        if (s.compare(i, 4, "last") == 0) {
            i += 4;
            from_last = true;
            n = 0;
            blank();
            if (i < s.size() && s[i] == '-') {
                i++;
                blank();
                if (!digits(n)) bad_path(i);
            }
        } else if (!digits(n)) {
            bad_path(i);
        }
        blank();
    };
    blank();
    if (i >= s.size() || s[i] != '$') bad_path(1);
    i++;
    for (;;) {
        blank();
        if (i >= s.size()) break;
        PathLeg leg;
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            leg.kind = PathLeg::Descent;
            path.wild = true;
            path.legs.push_back(leg);
            blank();
            if (i >= s.size() || (s[i] != '.' && s[i] != '[')) bad_path(i); // (** has to be followed by a member or an element)
            continue;
        }
        if (s[i] == '.') {
            i++;
            blank();
            if (i < s.size() && s[i] == '*') {
                i++;
                leg.kind = PathLeg::KeyWild;
                path.wild = true;
            } else if (i < s.size() && s[i] == '"') {
                i++;
                std::string key;
                while (i < s.size() && s[i] != '"') {
                    if (s[i] == '\\' && i + 1 < s.size()) i++;
                    key.push_back(s[i++]);
                }
                if (i >= s.size()) bad_path(i);
                i++;
                leg.key = key;
            } else {
                const std::size_t start = i;
                auto first = [](unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; };
                auto rest = [&](unsigned char c) { return first(c) || std::isdigit(c); };
                if (i >= s.size() || !first(static_cast<unsigned char>(s[i]))) {
                    if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { while (i < s.size() && rest(static_cast<unsigned char>(s[i]))) i++; bad_path(i); }
                    bad_path(i);
                }
                while (i < s.size() && rest(static_cast<unsigned char>(s[i]))) i++;
                leg.key = s.substr(start, i - start);
            }
        } else if (s[i] == '[') {
            i++;
            blank();
            if (i < s.size() && s[i] == '*') {
                i++;
                blank();
                leg.kind = PathLeg::IndexWild;
                path.wild = true;
            } else {
                position(leg.from, leg.from_last);
                leg.kind = PathLeg::Index;
                if (s.compare(i, 2, "to") == 0 && i + 2 < s.size() && (s[i + 2] == ' ' || s[i + 2] == 'l' || (s[i + 2] >= '0' && s[i + 2] <= '9'))) {
                    i += 2;
                    position(leg.to, leg.to_last);
                    leg.kind = PathLeg::Range;
                    path.wild = true;
                    if (!leg.from_last && !leg.to_last && leg.from > leg.to) bad_path(i);
                }
            }
            if (i >= s.size() || s[i] != ']') bad_path(i);
            i++;
        } else {
            bad_path(i);
        }
        path.legs.push_back(leg);
    }
    if (!path.legs.empty() && path.legs.back().kind == PathLeg::Descent) bad_path(i);
    return path;
}

[[noreturn]] void wildcards_not_allowed() {
    throw StatementError("In this situation, path expressions may not contain the * and ** tokens or an array range.");
}

// The path in argument i; `plain`: a path of one value only (no `*`, `**`, range) is allowed.
JsonPath path_arg(const BuiltinCall& c, std::size_t i, bool plain) {
    JsonPath path = parse_path(c.value(i));
    if (plain && path.wild) wildcards_not_allowed();
    return path;
}

// The element of an array a position stands for (`last` counts from the end), or -1 when it is outside.
long long element_at(long long n, bool from_last, std::size_t size) {
    const long long at = from_last ? static_cast<long long>(size) - 1 - n : n;
    return at >= 0 && at < static_cast<long long>(size) ? at : -1;
}

void add_once(std::vector<Json*>& out, Json* node) {
    if (std::find(out.begin(), out.end(), node) == out.end()) out.push_back(node);
}

// every node the path leads to from `node`, in document order, each once
void walk_path(Json* node, const JsonPath& path, std::size_t leg, std::vector<Json*>& out) {
    if (leg == path.legs.size()) { add_once(out, node); return; }
    const PathLeg& l = path.legs[leg];
    switch (l.kind) {
        case PathLeg::Key:
            if (node->kind == Json::Object) for (auto& m : node->members) if (m.first == l.key) walk_path(&m.second, path, leg + 1, out);
            break;
        case PathLeg::KeyWild:
            if (node->kind == Json::Object) for (auto& m : node->members) walk_path(&m.second, path, leg + 1, out);
            break;
        case PathLeg::Index:
            if (node->kind == Json::Array) {
                const long long at = element_at(l.from, l.from_last, node->items.size());
                if (at >= 0) walk_path(&node->items[static_cast<std::size_t>(at)], path, leg + 1, out);
            } else if (l.from == 0) { // (a value that is no array is an array of that one value)
                walk_path(node, path, leg + 1, out);
            }
            break;
        case PathLeg::Range:
            if (node->kind == Json::Array) {
                long long a = l.from_last ? static_cast<long long>(node->items.size()) - 1 - l.from : l.from;
                long long b = l.to_last ? static_cast<long long>(node->items.size()) - 1 - l.to : l.to;
                if (a < 0) a = 0;
                if (b >= static_cast<long long>(node->items.size())) b = static_cast<long long>(node->items.size()) - 1;
                for (long long at = a; at <= b; at++) walk_path(&node->items[static_cast<std::size_t>(at)], path, leg + 1, out);
            } else if ((l.from_last ? 0 : l.from) == 0) {
                walk_path(node, path, leg + 1, out);
            }
            break;
        case PathLeg::IndexWild:
            if (node->kind == Json::Array) for (auto& item : node->items) walk_path(&item, path, leg + 1, out);
            break;
        case PathLeg::Descent: {
            walk_path(node, path, leg + 1, out);
            if (node->kind == Json::Array) for (auto& item : node->items) walk_path(&item, path, leg, out);
            else if (node->kind == Json::Object) for (auto& m : node->members) walk_path(&m.second, path, leg, out);
            break;
        }
    }
}

std::vector<Json*> nodes_at(Json& doc, const JsonPath& path) {
    std::vector<Json*> out;
    walk_path(&doc, path, 0, out);
    return out;
}

// the node a path of plain legs leads to, or nullptr
Json* node_at(Json& doc, const JsonPath& path, std::size_t legs) {
    JsonPath cut;
    cut.legs.assign(path.legs.begin(), path.legs.begin() + static_cast<std::ptrdiff_t>(legs));
    auto found = nodes_at(doc, cut);
    return found.empty() ? nullptr : found.front();
}

// ---- the functions ----

Json json_array_of(std::vector<const Json*> nodes) {
    Json arr;
    arr.kind = Json::Array;
    for (const Json* n : nodes) arr.items.push_back(*n);
    return arr;
}

std::string fn_json_valid(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    return parse_json(c.value(0)) ? "1" : "0";
}

std::string fn_json_type(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    const Json j = parse_json_arg(c.value(0), 1, "json_type");
    switch (j.kind) {
        case Json::Object: return "OBJECT";
        case Json::Array: return "ARRAY";
        case Json::String: return "STRING";
        case Json::Int: return "INTEGER";
        case Json::UInt: return "UNSIGNED INTEGER";
        case Json::Dec: return "DECIMAL";
        case Json::Dbl: return "DOUBLE";
        case Json::Bool: return "BOOLEAN";
        default: return "NULL";
    }
}

std::string fn_json_length(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    if (any_null(c)) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_length");
    const Json* target = &doc;
    if (argc(c) == 2) {
        auto found = nodes_at(doc, path_arg(c, 1, false));
        if (found.empty()) return kNull;
        target = found.front();
    }
    return int_text(target->kind == Json::Array ? static_cast<long long>(target->items.size()) : target->kind == Json::Object ? static_cast<long long>(target->members.size()) : 1);
}

long long json_depth(const Json& j) {
    long long deepest = 0;
    for (const Json& item : j.items) deepest = std::max(deepest, json_depth(item));
    for (const auto& m : j.members) deepest = std::max(deepest, json_depth(m.second));
    return deepest + 1;
}

std::string fn_json_depth(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    return int_text(json_depth(parse_json_arg(c.value(0), 1, "json_depth")));
}

std::string fn_json_keys(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    if (any_null(c)) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_keys");
    Json* target = &doc;
    if (argc(c) == 2) {
        target = nullptr;
        auto found = nodes_at(doc, path_arg(c, 1, true));
        if (!found.empty()) target = found.front();
        if (!target) return kNull;
    }
    if (target->kind != Json::Object) return kNull;
    Json keys;
    keys.kind = Json::Array;
    for (auto& m : target->members) {
        Json k;
        k.kind = Json::String;
        k.text = m.first;
        keys.items.push_back(std::move(k));
    }
    return json_text(keys);
}

std::string fn_json_array(const BuiltinCall& c) {
    Json arr;
    arr.kind = Json::Array;
    for (std::size_t i = 0; i < argc(c); i++) arr.items.push_back(json_from_sql(c, i));
    return json_text(arr);
}

// the member name an SQL value makes (a number is written out)
std::string json_member_name(const BuiltinCall& c, std::size_t i) {
    const std::string v = c.value(i);
    if (is_null(v)) throw StatementError("JSON documents may not contain NULL member names.");
    return v;
}

std::string fn_json_object(const BuiltinCall& c) {
    if (argc(c) % 2 != 0) wrong_count(c);
    Json obj;
    obj.kind = Json::Object;
    for (std::size_t i = 0; i < argc(c); i += 2) {
        const std::string key = json_member_name(c, i);
        Json v = json_from_sql(c, i + 1);
        bool replaced = false;
        for (auto& m : obj.members) if (m.first == key) { m.second = v; replaced = true; }
        if (!replaced) obj.members.emplace_back(key, std::move(v));
    }
    sort_members(obj);
    return json_text(obj);
}

std::string fn_json_quote(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    if (number_literal(c, 0)) throw StatementError("Incorrect type for argument 1 in function json_quote.");
    return json_quote(c.value(0));
}

// the string a JSON string text stands for: "é" is é; text that is not in quotes is taken as it is
std::string json_unquote_text(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        const Json j = parse_json_arg(v, 1, "json_unquote");
        if (j.kind == Json::String) return j.text;
    }
    return v;
}

std::string fn_json_unquote(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    if (number_literal(c, 0)) throw StatementError("Incorrect type for argument 1 in function json_unquote.");
    return json_unquote_text(c.value(0));
}

// the answer of JSON_EXTRACT for the matches of its paths: one match of a plain path as it is, else an array of them; NULL when nothing matched
std::string json_extract_text(Json& doc, const std::vector<JsonPath>& paths) {
    std::vector<const Json*> matches;
    bool array = paths.size() > 1;
    for (const JsonPath& path : paths) {
        array = array || path.wild;
        for (Json* node : nodes_at(doc, path)) matches.push_back(node);
    }
    if (matches.empty()) return kNull;
    if (!array) return json_text(*matches.front());
    return json_text(json_array_of(matches));
}

std::string fn_json_extract(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    if (any_null(c)) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_extract");
    std::vector<JsonPath> paths;
    for (std::size_t i = 1; i < argc(c); i++) paths.push_back(path_arg(c, i, false));
    return json_extract_text(doc, paths);
}

// JSON_VALUE(doc, path [, 'RETURNING type']): the value as SQL sees it -- a string without its quotes, a number as text, JSON null as SQL NULL, an object
// or array as JSON text. The type, when asked for, has to take the value as it is (a string that is no number is NULL for SIGNED, not 0).
std::string fn_json_value(const BuiltinCall& c) {
    if (argc(c) < 2 || argc(c) > 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1))) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_value");
    auto found = nodes_at(doc, path_arg(c, 1, false));
    if (found.empty() || found.front()->kind == Json::Null) return kNull;
    const Json& v = *found.front();
    std::string out = v.kind == Json::String ? v.text : json_text(v);
    if (argc(c) == 3) {
        const std::string type = upper_ascii(c.value(2));
        const bool signed_int = type == "SIGNED" || type == "SIGNED INTEGER" || type == "INTEGER" || type == "UNSIGNED" || type == "UNSIGNED INTEGER";
        if (signed_int) {
            if (v.kind == Json::String) {
                if (!is_json_number_text(out) || out.find_first_of(".eE") != std::string::npos) return kNull;
            } else if (v.kind == Json::Bool) {
                return v.b ? "1" : "0";
            } else if (!json_is_number(v)) {
                return kNull;
            }
            return int_text(int_of(out));
        }
        if (type.rfind("DECIMAL", 0) == 0 || type == "DOUBLE" || type == "FLOAT") {
            if (v.kind == Json::String && !is_json_number_text(out)) return kNull;
            if (v.kind != Json::String && !json_is_number(v)) return kNull;
            long long precision = 10, scale = 0;
            if (type.rfind("DECIMAL", 0) == 0) {
                if (std::sscanf(type.c_str(), "DECIMAL(%lld,%lld)", &precision, &scale) < 1) precision = 10;
                const Decimal d = decimal_of(out).rounded(scale, false);
                if (static_cast<long long>(d.integer == "0" ? 0 : d.integer.size()) > precision - scale) return kNull; // (more digits than the type holds)
                return d.str(static_cast<std::size_t>(scale));
            }
        }
        if (type.rfind("CHAR", 0) == 0 || type.rfind("VARCHAR", 0) == 0) {
            long long length = 0;
            if (std::sscanf(type.c_str(), type[0] == 'V' ? "VARCHAR(%lld)" : "CHAR(%lld)", &length) == 1 && static_cast<long long>(utf8_chars(out).size()) > length) return kNull;
        }
    }
    return out;
}

bool json_contains(const Json& target, const Json& cand) {
    if (target.kind == Json::Object) {
        if (cand.kind != Json::Object) return false;
        for (auto& m : cand.members) {
            const Json* found = nullptr;
            for (auto& t : target.members) if (t.first == m.first) found = &t.second;
            if (!found || !json_contains(*found, m.second)) return false;
        }
        return true;
    }
    if (target.kind == Json::Array) {
        if (cand.kind == Json::Array) {
            for (auto& ci : cand.items) {
                bool any = false;
                for (auto& ti : target.items) any = any || json_contains(ti, ci);
                if (!any) return false;
            }
            return true;
        }
        for (auto& ti : target.items) if (json_contains(ti, cand)) return true;
        return false;
    }
    return json_equal(target, cand);
}

std::string fn_json_contains(const BuiltinCall& c) {
    if (argc(c) < 2 || argc(c) > 3) wrong_count(c);
    if (any_null(c)) return kNull;
    Json target = parse_json_arg(c.value(0), 1, "json_contains");
    Json cand = parse_json_arg(c.value(1), 2, "json_contains");
    Json* t = &target;
    if (argc(c) == 3) {
        auto found = nodes_at(target, path_arg(c, 2, true));
        if (found.empty()) return kNull;
        t = found.front();
    }
    return json_contains(*t, cand) ? "1" : "0";
}

std::string fn_json_contains_path(const BuiltinCall& c) {
    if (argc(c) < 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1))) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_contains_path");
    const std::string mode = lower_ascii(c.value(1));
    if (mode != "one" && mode != "all") throw StatementError("The oneOrAll argument to json_contains_path may take these values: 'one' or 'all'.");
    bool saw_null = false;
    for (std::size_t i = 2; i < argc(c); i++) {
        if (is_null(c.value(i))) { saw_null = true; continue; }
        const bool found = !nodes_at(doc, path_arg(c, i, false)).empty();
        if (found && mode == "one") return "1";
        if (!found && mode == "all") return "0";
    }
    if (saw_null) return kNull;
    return mode == "one" ? "0" : "1";
}

bool json_overlaps(const Json& a, const Json& b) {
    if (a.kind == Json::Array && b.kind == Json::Array) {
        for (auto& x : a.items) for (auto& y : b.items) if (json_equal(x, y)) return true;
        return false;
    }
    if (a.kind == Json::Array) { for (auto& x : a.items) if (json_equal(x, b)) return true; return false; }
    if (b.kind == Json::Array) { for (auto& y : b.items) if (json_equal(a, y)) return true; return false; }
    if (a.kind == Json::Object && b.kind == Json::Object) {
        for (auto& x : a.members) for (auto& y : b.members) if (x.first == y.first && json_equal(x.second, y.second)) return true;
        return false;
    }
    return json_equal(a, b);
}

std::string fn_json_overlaps(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    return json_overlaps(parse_json_arg(c.value(0), 1, "json_overlaps"), parse_json_arg(c.value(1), 2, "json_overlaps")) ? "1" : "0";
}

// `value MEMBER OF(array)`: is the value one of the elements (a document that is no array is a one-element array)
std::string fn_member_of(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    const Json target = parse_json_arg(c.value(1), 2, "member of");
    const Json v = json_from_sql(c, 0);
    if (target.kind == Json::Array) {
        for (auto& item : target.items) if (json_equal(item, v)) return "1";
        return "0";
    }
    return json_equal(target, v) ? "1" : "0";
}

// LIKE over a string value: % any run, _ one character, `escape` takes the next one literally; letters are compared exactly
bool json_like(const std::string& s, std::size_t si, const std::string& p, std::size_t pi, char escape) {
    while (pi < p.size()) {
        const char pc = p[pi];
        if (pc == escape && pi + 1 < p.size()) {
            if (si >= s.size() || s[si] != p[pi + 1]) return false;
            si++;
            pi += 2;
        } else if (pc == '%') {
            while (pi < p.size() && p[pi] == '%') pi++;
            if (pi == p.size()) return true;
            for (std::size_t k = si; k <= s.size(); k++) if (json_like(s, k, p, pi, escape)) return true;
            return false;
        } else if (pc == '_') {
            if (si >= s.size()) return false;
            si++;
            while (si < s.size() && (static_cast<unsigned char>(s[si]) & 0xC0) == 0x80) si++; // (one character, not one byte)
            pi++;
        } else {
            if (si >= s.size() || s[si] != pc) return false;
            si++;
            pi++;
        }
    }
    return si == s.size();
}

bool json_plain_name(const std::string& k) {
    if (k.empty()) return false;
    for (std::size_t n = 0; n < k.size(); n++) {
        const unsigned char ch = static_cast<unsigned char>(k[n]);
        if (!(std::isalpha(ch) || ch == '_' || ch == '$' || ch >= 0x80 || (n > 0 && std::isdigit(ch)))) return false;
    }
    return true;
}

void json_search_into(const Json& node, const std::string& at, const std::string& pattern, char escape, bool all, std::vector<std::string>& found) {
    if (!all && !found.empty()) return;
    if (node.kind == Json::String) {
        if (json_like(node.text, 0, pattern, 0, escape)) found.push_back(at);
    } else if (node.kind == Json::Array) {
        for (std::size_t k = 0; k < node.items.size(); k++) json_search_into(node.items[k], at + "[" + std::to_string(k) + "]", pattern, escape, all, found);
    } else if (node.kind == Json::Object) {
        for (auto& m : node.members) json_search_into(m.second, at + (json_plain_name(m.first) ? "." + m.first : ".\"" + m.first + "\""), pattern, escape, all, found);
    }
}

std::string fn_json_search(const BuiltinCall& c) {
    if (argc(c) < 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1)) || is_null(c.value(2))) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_search");
    const std::string mode = lower_ascii(c.value(1));
    if (mode != "one" && mode != "all") throw StatementError("The oneOrAll argument to json_search may take these values: 'one' or 'all'.");
    char escape = '\\';
    if (argc(c) >= 4 && !is_null(c.value(3)) && !c.value(3).empty()) escape = c.value(3)[0];
    std::vector<std::string> found;
    if (argc(c) <= 4) {
        json_search_into(doc, "$", c.value(2), escape, mode == "all", found);
    } else {
        for (std::size_t i = 4; i < argc(c); i++) {
            if (is_null(c.value(i))) return kNull;
            JsonPath path = path_arg(c, i, false);
            for (Json* node : nodes_at(doc, path)) {
                // (the text of the path that was given, then the steps below it)
                std::string at = c.value(i);
                while (!at.empty() && at.back() == ' ') at.pop_back();
                json_search_into(*node, at, c.value(2), escape, mode == "all", found);
            }
        }
    }
    if (found.empty()) return kNull;
    if (mode == "one" || found.size() == 1) return json_quote(found.front());
    Json arr;
    arr.kind = Json::Array;
    for (auto& f : found) { Json s; s.kind = Json::String; s.text = f; arr.items.push_back(std::move(s)); }
    return json_text(arr);
}

// JSON_SET (set or add), JSON_INSERT (add only), JSON_REPLACE (replace only)
std::string json_modify(const BuiltinCall& c, bool may_add, bool may_replace) {
    if (argc(c) < 3 || argc(c) % 2 == 0) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, lower_ascii(c.name));
    std::vector<JsonPath> paths;
    for (std::size_t i = 1; i + 1 < argc(c); i += 2) {
        if (is_null(c.value(i))) return kNull;
        paths.push_back(path_arg(c, i, true));
    }
    for (std::size_t k = 0; k < paths.size(); k++) {
        const JsonPath& path = paths[k];
        Json value = json_from_sql(c, 2 + 2 * k);
        if (path.legs.empty()) { if (may_replace) doc = value; continue; }
        Json* parent = node_at(doc, path, path.legs.size() - 1);
        if (!parent) continue;
        const PathLeg& last = path.legs.back();
        if (last.kind == PathLeg::Key) {
            if (parent->kind != Json::Object) continue;
            Json* existing = nullptr;
            for (auto& m : parent->members) if (m.first == last.key) existing = &m.second;
            if (existing) { if (may_replace) *existing = value; }
            else if (may_add) { parent->members.emplace_back(last.key, value); sort_members(*parent); }
        } else if (parent->kind == Json::Array) {
            const long long at = element_at(last.from, last.from_last, parent->items.size());
            if (at >= 0) { if (may_replace) parent->items[static_cast<std::size_t>(at)] = value; }
            else if (may_add && !last.from_last) parent->items.push_back(value); // (a position past the end appends)
        } else if (last.from == 0) { // (a value that is no array is an array of that one value: [0] is the value itself)
            if (may_replace) *parent = value;
        } else if (may_add && !last.from_last) {
            Json arr;
            arr.kind = Json::Array;
            arr.items.push_back(*parent);
            arr.items.push_back(value);
            *parent = arr;
        }
    }
    return json_text(doc);
}

std::string fn_json_set(const BuiltinCall& c) { return json_modify(c, true, true); }
std::string fn_json_insert(const BuiltinCall& c) { return json_modify(c, true, false); }
std::string fn_json_replace(const BuiltinCall& c) { return json_modify(c, false, true); }

std::string fn_json_remove(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    if (any_null(c)) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_remove");
    for (std::size_t i = 1; i < argc(c); i++) {
        JsonPath path = path_arg(c, i, true);
        if (path.legs.empty()) throw StatementError("The path expression '$' is not allowed in this context.");
        Json* parent = node_at(doc, path, path.legs.size() - 1);
        if (!parent) continue;
        const PathLeg& last = path.legs.back();
        if (last.kind == PathLeg::Key) {
            if (parent->kind == Json::Object) {
                parent->members.erase(std::remove_if(parent->members.begin(), parent->members.end(), [&](const auto& m) { return m.first == last.key; }), parent->members.end());
            }
        } else if (parent->kind == Json::Array) {
            const long long at = element_at(last.from, last.from_last, parent->items.size());
            if (at >= 0) parent->items.erase(parent->items.begin() + at);
        }
    }
    return json_text(doc);
}

// JSON_ARRAY_APPEND(doc, path, value ...): the value goes to the end of the array the path leads to (a value that is no array becomes one)
std::string fn_json_array_append(const BuiltinCall& c) {
    if (argc(c) < 3 || argc(c) % 2 == 0) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_array_append");
    std::vector<JsonPath> paths;
    for (std::size_t i = 1; i + 1 < argc(c); i += 2) {
        if (is_null(c.value(i))) return kNull;
        paths.push_back(path_arg(c, i, true));
    }
    for (std::size_t k = 0; k < paths.size(); k++) {
        Json* target = node_at(doc, paths[k], paths[k].legs.size());
        if (!target) continue;
        Json value = json_from_sql(c, 2 + 2 * k);
        if (target->kind != Json::Array) {
            Json arr;
            arr.kind = Json::Array;
            arr.items.push_back(*target);
            *target = arr;
        }
        target->items.push_back(value);
    }
    return json_text(doc);
}

// JSON_ARRAY_INSERT(doc, path, value ...): the path ends in an array position; the value goes in there and the elements after it move up
std::string fn_json_array_insert(const BuiltinCall& c) {
    if (argc(c) < 3 || argc(c) % 2 == 0) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    Json doc = parse_json_arg(c.value(0), 1, "json_array_insert");
    std::vector<JsonPath> paths;
    for (std::size_t i = 1; i + 1 < argc(c); i += 2) {
        if (is_null(c.value(i))) return kNull;
        JsonPath path = path_arg(c, i, true);
        if (path.legs.empty() || path.legs.back().kind != PathLeg::Index) throw StatementError("A path expression is not a path to a cell in an array.");
        paths.push_back(path);
    }
    for (std::size_t k = 0; k < paths.size(); k++) {
        Json* parent = node_at(doc, paths[k], paths[k].legs.size() - 1);
        if (!parent || parent->kind != Json::Array) continue;
        const PathLeg& last = paths[k].legs.back();
        long long at = last.from_last ? static_cast<long long>(parent->items.size()) - 1 - last.from : last.from;
        if (at < 0) at = 0;
        if (at > static_cast<long long>(parent->items.size())) at = static_cast<long long>(parent->items.size());
        parent->items.insert(parent->items.begin() + at, json_from_sql(c, 2 + 2 * k));
    }
    return json_text(doc);
}

// RFC 7396: a patch that is no object replaces the target; an object patch sets its members in the target, and a member that is null removes it
Json json_patch(Json target, const Json& patch) {
    if (patch.kind != Json::Object) return patch;
    if (target.kind != Json::Object) { target = Json(); target.kind = Json::Object; }
    for (auto& m : patch.members) {
        if (m.second.kind == Json::Null) {
            target.members.erase(std::remove_if(target.members.begin(), target.members.end(), [&](const auto& t) { return t.first == m.first; }), target.members.end());
            continue;
        }
        Json* existing = nullptr;
        for (auto& t : target.members) if (t.first == m.first) existing = &t.second;
        if (existing) *existing = json_patch(*existing, m.second);
        else { target.members.emplace_back(m.first, json_patch(Json(), m.second)); }
    }
    sort_members(target);
    return target;
}

std::string fn_json_merge_patch(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    std::optional<Json> acc;
    for (std::size_t i = 0; i < argc(c); i++) {
        if (is_null(c.value(i))) { acc.reset(); continue; }
        const Json next = parse_json_arg(c.value(i), i + 1, "json_merge_patch");
        if (i == 0) acc = next;
        else if (next.kind != Json::Object) acc = next;
        else if (acc) acc = json_patch(*acc, next);
    }
    return acc ? json_text(*acc) : kNull;
}

// JSON_MERGE_PRESERVE: arrays join, objects merge member by member (a member in both has its two values merged), anything else becomes an array
Json json_merge_preserve(Json a, const Json& b) {
    if (a.kind == Json::Object && b.kind == Json::Object) {
        for (auto& m : b.members) {
            Json* existing = nullptr;
            for (auto& t : a.members) if (t.first == m.first) existing = &t.second;
            if (existing) *existing = json_merge_preserve(*existing, m.second);
            else a.members.emplace_back(m.first, m.second);
        }
        sort_members(a);
        return a;
    }
    Json out;
    out.kind = Json::Array;
    if (a.kind == Json::Array) out.items = a.items; else out.items.push_back(a);
    if (b.kind == Json::Array) for (auto& item : b.items) out.items.push_back(item); else out.items.push_back(b);
    return out;
}

std::string fn_json_merge_preserve(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    if (any_null(c)) return kNull;
    Json acc = parse_json_arg(c.value(0), 1, lower_ascii(c.name));
    for (std::size_t i = 1; i < argc(c); i++) acc = json_merge_preserve(acc, parse_json_arg(c.value(i), i + 1, lower_ascii(c.name)));
    return json_text(acc);
}

std::string fn_json_pretty(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    std::string out;
    json_pretty_into(parse_json_arg(c.value(0), 1, "json_pretty"), 0, out);
    return out;
}

// CAST(x AS JSON) and the value written to a JSON column: the document in MySQL's form
std::string json_cast(const std::string& text) { return json_text(parse_json_arg(text, 1, "cast_as_json")); }

// ---- the table --------------------------------------------------------------------------------------------------------------------------------

using Handler = std::string (*)(const BuiltinCall&);

const std::unordered_map<std::string, Handler>& table() {
    static const std::unordered_map<std::string, Handler> t = {
        {"UPPER", fn_upper}, {"UCASE", fn_upper}, {"LOWER", fn_lower}, {"LCASE", fn_lower},
        {"LENGTH", fn_length}, {"OCTET_LENGTH", fn_length}, {"CHAR_LENGTH", fn_char_length}, {"CHARACTER_LENGTH", fn_char_length}, {"BIT_LENGTH", fn_bit_length},
        {"CONCAT", fn_concat}, {"CONCAT_WS", fn_concat_ws},
        {"SUBSTRING", fn_substring}, {"SUBSTR", fn_substring}, {"MID", fn_substring},
        {"__LIKE_ESCAPE", fn_like_escape}, {"LEFT", fn_left}, {"RIGHT", fn_right}, {"REVERSE", fn_reverse}, {"REPEAT", fn_repeat}, {"SPACE", fn_space}, {"REPLACE", fn_replace},
        {"TRIM", fn_trim}, {"TRIM_BOTH", fn_trim_both}, {"TRIM_LEADING", fn_trim_leading}, {"TRIM_TRAILING", fn_trim_trailing},
        {"LTRIM", fn_ltrim}, {"RTRIM", fn_rtrim}, {"LPAD", fn_lpad}, {"RPAD", fn_rpad},
        {"INSTR", fn_instr}, {"LOCATE", fn_locate},
        {"ASCII", fn_ascii}, {"ORD", fn_ord}, {"CHAR", fn_char}, {"STRCMP", fn_strcmp},
        {"FIELD", fn_field}, {"ELT", fn_elt}, {"FIND_IN_SET", fn_find_in_set}, {"INSERT", fn_insert}, {"SUBSTRING_INDEX", fn_substring_index},
        {"FORMAT", fn_format}, {"HEX", fn_hex}, {"UNHEX", fn_unhex}, {"BIN", fn_bin}, {"OCT", fn_oct}, {"CONV", fn_conv},
        {"TO_BASE64", fn_to_base64}, {"FROM_BASE64", fn_from_base64},
        {"SQRT", fn_sqrt}, {"EXP", fn_exp}, {"LN", fn_ln}, {"LOG", fn_log}, {"LOG2", fn_log2}, {"LOG10", fn_log10}, {"POW", fn_pow}, {"POWER", fn_pow},
        {"SIN", fn_sin}, {"COS", fn_cos}, {"TAN", fn_tan}, {"COT", fn_cot}, {"ASIN", fn_asin}, {"ACOS", fn_acos}, {"ATAN", fn_atan}, {"ATAN2", fn_atan},
        {"DEGREES", fn_degrees}, {"RADIANS", fn_radians}, {"PI", fn_pi},
        {"SIGN", fn_sign}, {"ABS", fn_abs}, {"ROUND", fn_round}, {"TRUNCATE", fn_truncate}, {"FLOOR", fn_floor}, {"CEIL", fn_ceil}, {"CEILING", fn_ceil},
        {"MOD", fn_mod}, {"GREATEST", fn_greatest}, {"LEAST", fn_least},
        {"YEAR", fn_year}, {"MONTH", fn_month}, {"DAY", fn_day}, {"DAYOFMONTH", fn_day}, {"QUARTER", fn_quarter},
        {"HOUR", fn_hour}, {"MINUTE", fn_minute}, {"SECOND", fn_second},
        {"DAYOFWEEK", fn_dayofweek}, {"WEEKDAY", fn_weekday}, {"DAYOFYEAR", fn_dayofyear}, {"WEEK", fn_week}, {"WEEKOFYEAR", fn_weekofyear},
        {"MONTHNAME", fn_monthname}, {"DAYNAME", fn_dayname}, {"LAST_DAY", fn_last_day}, {"DATE", fn_date}, {"TIME", fn_time},
        {"DATEDIFF", fn_datediff}, {"TO_DAYS", fn_to_days}, {"FROM_DAYS", fn_from_days}, {"MAKEDATE", fn_makedate},
        {"TIME_TO_SEC", fn_time_to_sec}, {"SEC_TO_TIME", fn_sec_to_time}, {"TIME_FORMAT", fn_time_format}, {"ADDTIME", fn_addtime}, {"SUBTIME", fn_subtime},
        {"TIMEDIFF", fn_timediff}, {"PERIOD_ADD", fn_period_add}, {"PERIOD_DIFF", fn_period_diff}, {"YEARWEEK", fn_yearweek}, {"TO_SECONDS", fn_to_seconds},
        {"BIT_COUNT", fn_bit_count}, {"QUOTE", fn_quote}, {"EXPORT_SET", fn_export_set}, {"MAKE_SET", fn_make_set},
        {"DATE_ADD", fn_date_add}, {"DATE_SUB", fn_date_sub}, {"ADDDATE", fn_adddate}, {"SUBDATE", fn_subdate},
        {"TIMESTAMPADD", fn_timestampadd}, {"TIMESTAMPDIFF", fn_timestampdiff}, {"EXTRACT", fn_extract},
        {"GET_FORMAT", fn_get_format}, {"MAKETIME", fn_maketime}, {"MICROSECOND", fn_microsecond}, {"TIMESTAMP", fn_timestamp},
        {"DATE_FORMAT", fn_date_format}, {"STR_TO_DATE", fn_str_to_date}, {"CONVERT_TZ", fn_convert_tz}, {"CAST", fn_cast}, {"CONVERT", fn_cast},
        {"OP_BIT_AND", fn_bit_and}, {"OP_BIT_OR", fn_bit_or}, {"OP_BIT_XOR", fn_bit_xor}, {"OP_BIT_NOT", fn_bit_not}, {"OP_SHL", fn_shl}, {"OP_SHR", fn_shr},
        {"OP_NULL_SAFE_EQ", fn_null_safe_eq}, {"OP_XOR", fn_xor}, {"OP_DIV", fn_int_div}, {"ANY_VALUE", fn_any_value},
        {"SHA1", fn_sha1}, {"SHA", fn_sha1}, {"SHA2", fn_sha2}, {"CRC32", fn_crc32}, {"SOUNDEX", fn_soundex},
        {"JSON_VALID", fn_json_valid}, {"JSON_TYPE", fn_json_type}, {"JSON_LENGTH", fn_json_length}, {"JSON_KEYS", fn_json_keys},
        {"JSON_ARRAY", fn_json_array}, {"JSON_OBJECT", fn_json_object}, {"JSON_CONTAINS", fn_json_contains}, {"JSON_SET", fn_json_set},
        {"JSON_INSERT", fn_json_insert}, {"JSON_REPLACE", fn_json_replace}, {"JSON_REMOVE", fn_json_remove},
        {"JSON_EXTRACT", fn_json_extract}, {"JSON_UNQUOTE", fn_json_unquote}, {"JSON_VALUE", fn_json_value}, {"JSON_DEPTH", fn_json_depth},
        {"JSON_ARRAY_APPEND", fn_json_array_append}, {"JSON_ARRAY_INSERT", fn_json_array_insert}, {"JSON_MERGE_PATCH", fn_json_merge_patch},
        {"JSON_MERGE_PRESERVE", fn_json_merge_preserve}, {"JSON_MERGE", fn_json_merge_preserve}, {"JSON_PRETTY", fn_json_pretty},
        {"JSON_CONTAINS_PATH", fn_json_contains_path}, {"JSON_OVERLAPS", fn_json_overlaps}, {"JSON_SEARCH", fn_json_search}, {"JSON_QUOTE", fn_json_quote},
        {"MEMBER_OF", fn_member_of},
    };
    return t;
}

// the functions the older dispatcher (Executor::apply_scalar_func) still serves
const std::unordered_set<std::string>& legacy_names() {
    static const std::unordered_set<std::string> s = {
        "NOW", "CURDATE", "COALESCE", "IFNULL", "IF", "NULLIF", "CAST", "CONVERT", "REGEXP_LIKE", "REGEXP", "REGEXP_REPLACE", "REGEXP_MATCH",
        "REGEXP_SUBSTR", "RAND", "CURTIME", "CURRENT_TIME", "CURRENT_TIMESTAMP", "CURRENT_DATE", "LOCALTIME", "LOCALTIMESTAMP", "UNIX_TIMESTAMP",
        "FROM_UNIXTIME", "ISNULL", "MD5", "UUID", "DATABASE", "SCHEMA", "VERSION", "CURRENT_USER",
        "USER", "SESSION_USER", "SYSTEM_USER", "VALUES", "CASE", "REGEXP_INSTR", "LAST_INSERT_ID", "ROW_COUNT", "FOUND_ROWS", "SLEEP", "UTC_DATE", "UTC_TIME",
        "UTC_TIMESTAMP",
    };
    return s;
}

} // namespace

// One side of a comparison with a JSON document as the JSON value its class says: a JSON side is read as a document (text that is none is a string), a number
// is a JSON number, a string a JSON string; a side whose type is not known is a number when it reads as one.
static Json json_of_side(ValueClass cls, const std::string& text) {
    if (cls == ValueClass::Json) {
        if (auto parsed = parse_json(text)) return *parsed;
    } else if (cls == ValueClass::Number || (cls == ValueClass::Unknown && is_json_number_text(text))) {
        if (is_json_number_text(text)) {
            JsonParser p(text);
            Json j;
            p.number(j);
            if (text.find('.') != std::string::npos && j.kind == Json::Dbl) { j.kind = Json::Dec; j.text = text; }
            return j;
        }
    }
    Json j;
    j.kind = Json::String;
    j.text = text;
    return j;
}

std::optional<std::string> json_canonical_text(const std::string& text, std::string* why) {
    JsonParser parser(text);
    auto j = parser.parse();
    if (!j) {
        if (why) *why = "\"" + parser.reason + "\" at position " + std::to_string(parser.at);
        return std::nullopt;
    }
    return json_text(*j);
}

std::string json_aggregate_text(bool object, const std::vector<std::string>& rows) {
    Json out;
    out.kind = object ? Json::Object : Json::Array;
    for (const std::string& text : rows) {
        auto row = parse_json(text);
        if (!row || row->kind != Json::Array || row->items.empty()) continue;
        if (!object) { out.items.push_back(row->items[0]); continue; }
        const Json& key = row->items[0];
        if (key.kind == Json::Null) throw StatementError("JSON documents may not contain NULL member names.");
        const std::string name = key.kind == Json::String ? key.text : json_text(key);
        Json value = row->items.size() > 1 ? row->items[1] : Json();
        bool replaced = false;
        for (auto& m : out.members) if (m.first == name) { m.second = value; replaced = true; }
        if (!replaced) out.members.emplace_back(name, std::move(value));
    }
    if (object) sort_members(out);
    return json_text(out);
}

int json_compare_classed(ValueClass a, ValueClass b, std::string_view x, std::string_view y) {
    return json_order(json_of_side(a, std::string(x)), json_of_side(b, std::string(y)));
}

std::optional<std::string> call_builtin_function(const BuiltinCall& call) {
    auto it = table().find(call.name);
    if (it == table().end()) return std::nullopt;
    return it->second(call);
}

bool is_known_function_name(const std::string& name) { return table().count(name) > 0 || legacy_names().count(name) > 0; }

bool is_builtin_function(const std::string& name) { return table().count(name) > 0; }

} // namespace engine
