#include "engine/collation.hpp"
#include "engine/executor/builtin_functions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    if (b.is_zero()) return kNull;
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

std::string fn_time(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    long long micro = 0;
    auto secs = parse_time_seconds(c.value(0), &micro);
    if (!secs) return kNull;
    DateTime t;
    const long long s = *secs < 0 ? -*secs : *secs;
    char b[48];
    if (micro) std::snprintf(b, sizeof b, "%s%02lld:%02lld:%02lld.%06lld", *secs < 0 ? "-" : "", s / 3600, s / 60 % 60, s % 60, micro);
    else std::snprintf(b, sizeof b, "%s%02lld:%02lld:%02lld", *secs < 0 ? "-" : "", s / 3600, s / 60 % 60, s % 60);
    return b;
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
    const long long a = s < 0 ? -s : s;
    std::snprintf(b, sizeof b, "%s%02lld:%02lld:%02lld", s < 0 ? "-" : "", a / 3600, a / 60 % 60, a % 60);
    return b;
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
    return seconds_to_time(int_of(c.value(0)));
}

// TIME_FORMAT is DATE_FORMAT of a time
std::string fn_time_format(const BuiltinCall& c);

// ADDTIME / SUBTIME(a time or a datetime, a time)
std::string shift_time(const BuiltinCall& c, long long sign) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto amount = parse_time_seconds(c.value(1));
    if (!amount) return kNull;
    if (auto t = parse_datetime(c.value(0)); t && t->has_date && t->valid_date()) {
        const DateTime moved = DateTime::from_epoch_seconds(t->epoch_seconds() + sign * *amount, t->micro);
        return moved.date_str() + " " + moved.time_str();
    }
    auto base = parse_time_seconds(c.value(0));
    if (!base) return kNull;
    return seconds_to_time(*base + sign * *amount);
}

std::string fn_addtime(const BuiltinCall& c) { return shift_time(c, 1); }
std::string fn_subtime(const BuiltinCall& c) { return shift_time(c, -1); }

// TIMEDIFF(a, b): two datetimes or two times (a mix is NULL)
std::string fn_timediff(const BuiltinCall& c) {
    if (argc(c) != 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto a = parse_datetime(c.value(0)), b = parse_datetime(c.value(1));
    const bool dated_a = a && a->has_date && a->valid_date(), dated_b = b && b->has_date && b->valid_date();
    if (dated_a && dated_b) return seconds_to_time(a->epoch_seconds() - b->epoch_seconds());
    if (dated_a != dated_b) return kNull;
    auto x = parse_time_seconds(c.value(0)), y = parse_time_seconds(c.value(1));
    if (!x || !y) return kNull;
    return seconds_to_time(*x - *y);
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

std::string fn_date_add(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1))) return kNull;
    const std::string unit = unit_word(c, 2);
    static const std::unordered_set<std::string> units = {"MICROSECOND", "SECOND", "MINUTE", "HOUR", "DAY", "WEEK", "MONTH", "QUARTER", "YEAR"};
    if (!units.count(unit)) throw StatementError("You have an error in your SQL syntax; unknown INTERVAL unit '" + unit + "'");
    return add_to_date(c, c.value(0), int_of(c.value(1)), unit);
}

std::string fn_date_sub(const BuiltinCall& c) {
    if (argc(c) != 3) wrong_count(c);
    if (is_null(c.value(0)) || is_null(c.value(1))) return kNull;
    const std::string unit = unit_word(c, 2);
    static const std::unordered_set<std::string> units = {"MICROSECOND", "SECOND", "MINUTE", "HOUR", "DAY", "WEEK", "MONTH", "QUARTER", "YEAR"};
    if (!units.count(unit)) throw StatementError("You have an error in your SQL syntax; unknown INTERVAL unit '" + unit + "'");
    return add_to_date(c, c.value(0), -int_of(c.value(1)), unit);
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
    long long year = -1, month = -1, day = -1, hour = 0, minute = 0, second = 0, micro = 0;
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
        if (d.integer.size() > 20) return d.negative ? "0" : "18446744073709551615";
        unsigned long long v = 0;
        for (char ch : d.integer) {
            if (v > (std::numeric_limits<unsigned long long>::max() - 9) / 10) return d.negative ? "0" : "18446744073709551615";
            v = v * 10 + static_cast<unsigned>(ch - '0');
        }
        if (d.negative && v != 0) v = 0ULL - v; // (a negative number wraps round, as in MySQL)
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
    if (base == "DOUBLE" || base == "FLOAT" || base == "REAL") return format_double(double_of(val));
    if (base == "CHAR" || base == "VARCHAR" || base == "NCHAR" || base == "TEXT" || base == "BINARY" || base == "JSON") {
        if (!params.empty() && base != "JSON" && params[0] >= 0) return substring_chars(utf8_chars(val), 1, params[0]);
        return val;
    }
    if (base == "DATE") {
        auto t = date_arg(val);
        return t ? t->date_str() : kNull;
    }
    if (base == "DATETIME" || base == "TIMESTAMP") {
        auto t = date_arg(val);
        if (!t) return kNull;
        t->has_time = true;
        return t->datetime_str();
    }
    if (base == "TIME") {
        auto s = parse_time_seconds(val);
        return s ? seconds_to_time(*s) : kNull;
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
    if (d.integer.size() > 20) return d.negative ? 0ULL : std::numeric_limits<unsigned long long>::max();
    unsigned long long v = 0;
    for (char ch : d.integer) {
        if (v > (std::numeric_limits<unsigned long long>::max() - 9) / 10) return std::numeric_limits<unsigned long long>::max();
        v = v * 10 + static_cast<unsigned>(ch - '0');
    }
    return d.negative ? 0ULL - v : v;
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
    if (b.is_zero()) return kNull;
    const Num q = num_div(Num::from_decimal(a), Num::from_decimal(b), 0, true); // (cut toward zero)
    const Decimal d = q.to_decimal();
    if (!d.to_int64()) throw StatementError("BIGINT value is out of range in '(" + c.value(0) + " DIV " + c.value(1) + ")'");
    return d.compact();
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

struct Json {
    enum Kind { Null, Bool, Int, Double, String, Array, Object } kind = Null;
    bool b = false;
    std::string text; // the number as written, or the string
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members; // kept in MySQL's order: shorter keys first, then by bytes
};

void sort_members(Json& j) {
    std::stable_sort(j.members.begin(), j.members.end(), [](const auto& a, const auto& b) {
        return a.first.size() != b.first.size() ? a.first.size() < b.first.size() : a.first < b.first;
    });
}

struct JsonParser {
    const std::string& s;
    std::size_t i = 0;
    explicit JsonParser(const std::string& text) : s(text) {}
    void blank() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool string_token(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                if (++i >= s.size()) return false;
                switch (s[i]) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (i + 4 >= s.size()) return false;
                        const std::uint32_t cp = static_cast<std::uint32_t>(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
                        out += utf8_encode(cp);
                        i += 4;
                        break;
                    }
                    default: out.push_back(s[i]); break;
                }
                i++;
            } else {
                out.push_back(s[i++]);
            }
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    bool value(Json& j, int depth = 0) {
        if (depth > 100) return false;
        blank();
        if (i >= s.size()) return false;
        const char ch = s[i];
        if (ch == '{') {
            i++;
            j.kind = Json::Object;
            blank();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                blank();
                std::string key;
                if (!string_token(key)) return false;
                blank();
                if (i >= s.size() || s[i] != ':') return false;
                i++;
                Json v;
                if (!value(v, depth + 1)) return false;
                bool replaced = false;
                for (auto& m : j.members) if (m.first == key) { m.second = v; replaced = true; }
                if (!replaced) j.members.emplace_back(std::move(key), std::move(v));
                blank();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                return false;
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
                return false;
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
        std::size_t start = i;
        if (i < s.size() && s[i] == '-') i++;
        std::size_t digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
        bool fractional = false;
        if (i < s.size() && s[i] == '.') { fractional = true; i++; while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            fractional = true;
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        if (digits == 0) return false;
        j.kind = fractional ? Json::Double : Json::Int;
        j.text = s.substr(start, i - start);
        return true;
    }
    std::optional<Json> parse() {
        Json j;
        if (!value(j)) return std::nullopt;
        blank();
        if (i != s.size()) return std::nullopt;
        return j;
    }
};

std::optional<Json> parse_json(const std::string& text) { return JsonParser(text).parse(); }

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

std::string json_text(const Json& j) {
    switch (j.kind) {
        case Json::Null: return "null";
        case Json::Bool: return j.b ? "true" : "false";
        case Json::Int: case Json::Double: return j.text;
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

// an SQL value used as a JSON value: a number is a number, NULL is null, a text that is a JSON object or array is that (a function's result such as
// JSON_ARRAY(...) arrives as such text), anything else is a string
Json json_from_sql(const BuiltinCall& c, std::size_t i) {
    const std::string v = c.value(i);
    Json j;
    if (is_null(v)) return j;
    const bool quoted = string_literal(c, i);
    if (!quoted) {
        if (v == "true" || v == "false") { j.kind = Json::Bool; j.b = v == "true"; return j; }
        if (auto d = Decimal::parse(v)) { j.kind = d->fraction.empty() ? Json::Int : Json::Double; j.text = v; return j; }
    }
    if (!quoted && !v.empty() && (v[0] == '{' || v[0] == '[')) { // (an object or array that is not a string literal: JSON_OBJECT(...) and the like)
        if (auto parsed = parse_json(v)) return *parsed;
    }
    if (!quoted) {
        if (auto parsed = parse_json(v); parsed && parsed->kind != Json::String) return *parsed;
    }
    j.kind = Json::String;
    j.text = v;
    return j;
}

std::optional<Json> json_document(const std::string& v) { return parse_json(v); }

struct PathStep { bool index = false; long long n = 0; std::string key; };

std::optional<std::vector<PathStep>> parse_path(const std::string& path) {
    if (path.empty() || path[0] != '$') return std::nullopt;
    std::vector<PathStep> steps;
    std::size_t i = 1;
    while (i < path.size()) {
        if (path[i] == '.') {
            i++;
            std::string key;
            if (i < path.size() && path[i] == '"') {
                i++;
                while (i < path.size() && path[i] != '"') key.push_back(path[i++]);
                if (i >= path.size()) return std::nullopt;
                i++;
            } else {
                while (i < path.size() && path[i] != '.' && path[i] != '[') key.push_back(path[i++]);
            }
            if (key.empty()) return std::nullopt;
            steps.push_back(PathStep{false, 0, key});
        } else if (path[i] == '[') {
            const std::size_t close = path.find(']', i);
            if (close == std::string::npos) return std::nullopt;
            const std::string inner = path.substr(i + 1, close - i - 1);
            if (inner.empty() || inner.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
            steps.push_back(PathStep{true, std::atoll(inner.c_str()), ""});
            i = close + 1;
        } else {
            return std::nullopt;
        }
    }
    return steps;
}

Json* json_step(Json& j, const PathStep& s) {
    if (s.index) {
        if (j.kind == Json::Array) return s.n < static_cast<long long>(j.items.size()) ? &j.items[static_cast<std::size_t>(s.n)] : nullptr;
        return s.n == 0 ? &j : nullptr; // (a scalar is a one-element array)
    }
    if (j.kind != Json::Object) return nullptr;
    for (auto& m : j.members) if (m.first == s.key) return &m.second;
    return nullptr;
}

std::string fn_json_valid(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    return parse_json(c.value(0)) ? "1" : "0";
}

std::string fn_json_type(const BuiltinCall& c) {
    if (argc(c) != 1) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    auto j = json_document(c.value(0));
    if (!j) throw StatementError("Invalid JSON text in argument 1 to function json_type");
    switch (j->kind) {
        case Json::Object: return "OBJECT";
        case Json::Array: return "ARRAY";
        case Json::String: return "STRING";
        case Json::Int: return "INTEGER";
        case Json::Double: return "DOUBLE";
        case Json::Bool: return "BOOLEAN";
        default: return "NULL";
    }
}

std::string fn_json_length(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto j = json_document(c.value(0));
    if (!j) throw StatementError("Invalid JSON text in argument 1 to function json_length");
    Json* target = &*j;
    if (argc(c) == 2) {
        auto path = parse_path(c.value(1));
        if (!path) throw StatementError("Invalid JSON path expression");
        for (auto& step : *path) {
            target = target ? json_step(*target, step) : nullptr;
        }
        if (!target) return kNull;
    }
    return int_text(target->kind == Json::Array ? static_cast<long long>(target->items.size()) : target->kind == Json::Object ? static_cast<long long>(target->members.size()) : 1);
}

std::string fn_json_keys(const BuiltinCall& c) {
    if (argc(c) < 1 || argc(c) > 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto j = json_document(c.value(0));
    if (!j) throw StatementError("Invalid JSON text in argument 1 to function json_keys");
    Json* target = &*j;
    if (argc(c) == 2) {
        auto path = parse_path(c.value(1));
        if (!path) throw StatementError("Invalid JSON path expression");
        for (auto& step : *path) target = target ? json_step(*target, step) : nullptr;
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

std::string fn_json_object(const BuiltinCall& c) {
    if (argc(c) % 2 != 0) wrong_count(c);
    Json obj;
    obj.kind = Json::Object;
    for (std::size_t i = 0; i < argc(c); i += 2) {
        if (is_null(c.value(i))) throw StatementError("JSON documents may not contain NULL member names.");
        Json v = json_from_sql(c, i + 1);
        bool replaced = false;
        for (auto& m : obj.members) if (m.first == c.value(i)) { m.second = v; replaced = true; }
        if (!replaced) obj.members.emplace_back(c.value(i), std::move(v));
    }
    sort_members(obj);
    return json_text(obj);
}

bool json_equal(const Json& a, const Json& b) { return json_text(a) == json_text(b); }

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
    auto target = json_document(c.value(0));
    auto cand = json_document(c.value(1));
    if (!target) throw StatementError("Invalid JSON text in argument 1 to function json_contains");
    if (!cand) throw StatementError("Invalid JSON text in argument 2 to function json_contains");
    Json* t = &*target;
    if (argc(c) == 3) {
        auto path = parse_path(c.value(2));
        if (!path) throw StatementError("Invalid JSON path expression");
        for (auto& step : *path) t = t ? json_step(*t, step) : nullptr;
        if (!t) return kNull;
    }
    return json_contains(*t, *cand) ? "1" : "0";
}

// JSON_SET (set or add), JSON_INSERT (add only), JSON_REPLACE (replace only)
std::string json_modify(const BuiltinCall& c, bool may_add, bool may_replace) {
    if (argc(c) < 3 || argc(c) % 2 == 0) wrong_count(c);
    if (is_null(c.value(0))) return kNull;
    auto doc = json_document(c.value(0));
    if (!doc) throw StatementError("Invalid JSON text in argument 1 to function " + lower_ascii(c.name));
    for (std::size_t i = 1; i + 1 < argc(c); i += 2) {
        if (is_null(c.value(i))) return kNull;
        auto path = parse_path(c.value(i));
        if (!path) throw StatementError("Invalid JSON path expression");
        Json value = json_from_sql(c, i + 1);
        if (path->empty()) { if (may_replace) *doc = value; continue; }
        Json* parent = &*doc;
        for (std::size_t k = 0; k + 1 < path->size() && parent; k++) parent = json_step(*parent, (*path)[k]);
        if (!parent) continue;
        const PathStep& last = path->back();
        Json* existing = json_step(*parent, last);
        if (existing) {
            if (may_replace) *existing = value;
        } else if (may_add) {
            if (last.index) {
                if (parent->kind != Json::Array) { Json arr; arr.kind = Json::Array; arr.items.push_back(*parent); *parent = arr; }
                parent->items.push_back(value);
            } else if (parent->kind == Json::Object) {
                parent->members.emplace_back(last.key, value);
                sort_members(*parent);
            }
        }
    }
    return json_text(*doc);
}

std::string fn_json_set(const BuiltinCall& c) { return json_modify(c, true, true); }
std::string fn_json_insert(const BuiltinCall& c) { return json_modify(c, true, false); }
std::string fn_json_replace(const BuiltinCall& c) { return json_modify(c, false, true); }

std::string fn_json_remove(const BuiltinCall& c) {
    if (argc(c) < 2) wrong_count(c);
    if (any_null(c)) return kNull;
    auto doc = json_document(c.value(0));
    if (!doc) throw StatementError("Invalid JSON text in argument 1 to function json_remove");
    for (std::size_t i = 1; i < argc(c); i++) {
        auto path = parse_path(c.value(i));
        if (!path || path->empty()) throw StatementError("Invalid JSON path expression");
        Json* parent = &*doc;
        for (std::size_t k = 0; k + 1 < path->size() && parent; k++) parent = json_step(*parent, (*path)[k]);
        if (!parent) continue;
        const PathStep& last = path->back();
        if (last.index && parent->kind == Json::Array && last.n < static_cast<long long>(parent->items.size())) {
            parent->items.erase(parent->items.begin() + last.n);
        } else if (!last.index && parent->kind == Json::Object) {
            parent->members.erase(std::remove_if(parent->members.begin(), parent->members.end(), [&](const auto& m) { return m.first == last.key; }), parent->members.end());
        }
    }
    return json_text(*doc);
}

// ---- the table --------------------------------------------------------------------------------------------------------------------------------

using Handler = std::string (*)(const BuiltinCall&);

const std::unordered_map<std::string, Handler>& table() {
    static const std::unordered_map<std::string, Handler> t = {
        {"UPPER", fn_upper}, {"UCASE", fn_upper}, {"LOWER", fn_lower}, {"LCASE", fn_lower},
        {"LENGTH", fn_length}, {"OCTET_LENGTH", fn_length}, {"CHAR_LENGTH", fn_char_length}, {"CHARACTER_LENGTH", fn_char_length}, {"BIT_LENGTH", fn_bit_length},
        {"CONCAT", fn_concat}, {"CONCAT_WS", fn_concat_ws},
        {"SUBSTRING", fn_substring}, {"SUBSTR", fn_substring}, {"MID", fn_substring},
        {"LEFT", fn_left}, {"RIGHT", fn_right}, {"REVERSE", fn_reverse}, {"REPEAT", fn_repeat}, {"SPACE", fn_space}, {"REPLACE", fn_replace},
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
        {"DATE_FORMAT", fn_date_format}, {"STR_TO_DATE", fn_str_to_date}, {"CAST", fn_cast}, {"CONVERT", fn_cast},
        {"OP_BIT_AND", fn_bit_and}, {"OP_BIT_OR", fn_bit_or}, {"OP_BIT_XOR", fn_bit_xor}, {"OP_BIT_NOT", fn_bit_not}, {"OP_SHL", fn_shl}, {"OP_SHR", fn_shr},
        {"OP_NULL_SAFE_EQ", fn_null_safe_eq}, {"OP_XOR", fn_xor}, {"OP_DIV", fn_int_div},
        {"SHA1", fn_sha1}, {"SHA", fn_sha1}, {"SHA2", fn_sha2}, {"CRC32", fn_crc32}, {"SOUNDEX", fn_soundex},
        {"JSON_VALID", fn_json_valid}, {"JSON_TYPE", fn_json_type}, {"JSON_LENGTH", fn_json_length}, {"JSON_KEYS", fn_json_keys},
        {"JSON_ARRAY", fn_json_array}, {"JSON_OBJECT", fn_json_object}, {"JSON_CONTAINS", fn_json_contains}, {"JSON_SET", fn_json_set},
        {"JSON_INSERT", fn_json_insert}, {"JSON_REPLACE", fn_json_replace}, {"JSON_REMOVE", fn_json_remove},
    };
    return t;
}

// the functions the older dispatcher (Executor::apply_scalar_func) still serves
const std::unordered_set<std::string>& legacy_names() {
    static const std::unordered_set<std::string> s = {
        "NOW", "CURDATE", "COALESCE", "IFNULL", "IF", "NULLIF", "CAST", "CONVERT", "REGEXP_LIKE", "REGEXP", "REGEXP_REPLACE", "REGEXP_MATCH",
        "REGEXP_SUBSTR", "RAND", "CURTIME", "CURRENT_TIME", "CURRENT_TIMESTAMP", "CURRENT_DATE", "LOCALTIME", "LOCALTIMESTAMP", "UNIX_TIMESTAMP",
        "FROM_UNIXTIME", "ISNULL", "MD5", "UUID", "JSON_EXTRACT", "JSON_UNQUOTE", "JSON_VALUE", "DATABASE", "SCHEMA", "VERSION", "CURRENT_USER",
        "USER", "SESSION_USER", "SYSTEM_USER", "VALUES", "CASE", "REGEXP_INSTR", "LAST_INSERT_ID", "ROW_COUNT", "FOUND_ROWS", "SLEEP", "UTC_DATE", "UTC_TIME",
        "UTC_TIMESTAMP",
    };
    return s;
}

} // namespace

std::optional<std::string> call_builtin_function(const BuiltinCall& call) {
    auto it = table().find(call.name);
    if (it == table().end()) return std::nullopt;
    return it->second(call);
}

bool is_known_function_name(const std::string& name) { return table().count(name) > 0 || legacy_names().count(name) > 0; }

bool is_builtin_function(const std::string& name) { return table().count(name) > 0; }

} // namespace engine
