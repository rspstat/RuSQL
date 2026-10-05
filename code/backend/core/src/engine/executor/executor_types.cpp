// Column type checking and conversion for INSERT and UPDATE (MySQL strict mode): a value must fit the column it is stored in, and
// is stored in the column's own text form. The engine keeps every value as text and used to store whatever was written: 'abc' in an
// INT column, 'toolong' in a VARCHAR(3), 'not a date' in a DATE, 123456.789 in a DECIMAL(5,2), and '07' next to 7 in an INT primary
// key (two keys for one number).
//
//   INT family   an integer; a fraction rounds half away from zero ('7.9' -> 8); a range error / non-number is an error
//   FLOAT/DOUBLE a number, stored in its shortest round-trip text
//   DECIMAL(p,s) a number rounded (exactly, on the digits) to s places, stored with exactly s places; more than p-s integer digits is an error
//   VARCHAR(n)   at most n characters (spaces beyond n are cut, anything else is "Data too long")
//   BOOLEAN      true / false / a number
//   DATE, DATETIME, TIMESTAMP, TIME, YEAR   a valid calendar value in the canonical text form
//   JSON         valid JSON text
// ENUM and SET are checked where they always were (exec_insert_inner / exec_update_inner).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine/executor/executor.hpp"

namespace engine {

namespace {

struct Decimal {
    bool negative = false;
    std::string integer;  // digits, no leading zeros ("" = 0)
    std::string fraction; // digits
};

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// [+-] digits [. digits] [e [+-] digits], or [+-] . digits ...: the whole text (spaces around it are ignored). nullopt: not a number.
std::optional<Decimal> parse_decimal(std::string_view text) {
    std::string_view s = trimmed(text);
    if (s.empty()) return std::nullopt;
    Decimal d;
    std::size_t i = 0;
    if (s[i] == '+' || s[i] == '-') {
        d.negative = s[i] == '-';
        i++;
    }
    std::string digits;
    std::size_t int_len = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits += s[i++]; any = true; }
    int_len = digits.size();
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits += s[i++]; any = true; }
    }
    if (!any) return std::nullopt;
    long exponent = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        if (i >= s.size()) return std::nullopt;
        long e = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            e = e * 10 + (s[i++] - '0');
            if (e > 10000) return std::nullopt;
        }
        exponent = eneg ? -e : e;
    }
    if (i != s.size()) return std::nullopt;
    // value = 0.digits * 10^(int_len + exponent)
    long point = static_cast<long>(int_len) + exponent;
    if (point > 400 || point < -400) return std::nullopt;
    std::string integer, fraction;
    if (point <= 0) {
        fraction = std::string(static_cast<std::size_t>(-point), '0') + digits;
    } else if (static_cast<std::size_t>(point) >= digits.size()) {
        integer = digits + std::string(static_cast<std::size_t>(point) - digits.size(), '0');
    } else {
        integer = digits.substr(0, static_cast<std::size_t>(point));
        fraction = digits.substr(static_cast<std::size_t>(point));
    }
    integer.erase(0, std::min(integer.find_first_not_of('0'), integer.size()));
    d.integer = std::move(integer);
    d.fraction = std::move(fraction);
    return d;
}

// Rounds half away from zero to `scale` places (in place on the digit strings); `scale` >= 0.
void round_to_scale(Decimal& d, std::size_t scale) {
    if (d.fraction.size() <= scale) return;
    const bool up = d.fraction[scale] >= '5';
    d.fraction.resize(scale);
    if (!up) return;
    std::string all = d.integer + d.fraction; // add one in the last place kept
    std::size_t i = all.size();
    while (i > 0 && all[i - 1] == '9') all[--i] = '0';
    if (i > 0) all[i - 1]++;
    else all.insert(all.begin(), '1'); // 99.9 -> 100.0
    const std::size_t int_len = all.size() - scale;
    d.integer = all.substr(0, int_len);
    d.fraction = all.substr(int_len);
    d.integer.erase(0, std::min(d.integer.find_first_not_of('0'), d.integer.size()));
}

bool is_zero(const Decimal& d) {
    return d.integer.empty() && d.fraction.find_first_not_of('0') == std::string::npos;
}

std::string decimal_text(const Decimal& d, std::size_t scale) {
    std::string out;
    if (d.negative && !is_zero(d)) out += '-';
    out += d.integer.empty() ? "0" : d.integer;
    if (scale > 0) {
        out += '.';
        out += d.fraction;
        out += std::string(scale - std::min(scale, d.fraction.size()), '0');
    }
    return out;
}

std::string shown(const std::string& v) { return v.size() > 64 ? v.substr(0, 64) + "..." : v; }

std::string at_row(std::size_t row) { return " at row " + std::to_string(row); }

std::optional<std::string> check_integer(const ColumnDef& col, std::string& value, std::size_t row, long long lo, long long hi) {
    // already canonical and small (-?[1-9][0-9]{0,8} or 0): the usual case
    if (!value.empty() && value.size() < 10) {
        const std::size_t i = value.front() == '-' ? 1 : 0;
        bool canonical = value == "0" || (i < value.size() && value[i] >= '1' && value[i] <= '9');
        for (std::size_t k = i; k < value.size() && canonical; k++) canonical = value[k] >= '0' && value[k] <= '9';
        if (canonical) {
            const long long n = std::stoll(value);
            if (n >= lo && n <= hi) return std::nullopt;
        }
    }
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true") { value = "1"; return std::nullopt; }
    if (lower == "false") { value = "0"; return std::nullopt; }
    auto d = parse_decimal(value);
    if (!d) return "Incorrect integer value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
    round_to_scale(*d, 0);
    if (d->integer.size() > 19) return "Out of range value for column '" + col.name + "'" + at_row(row);
    long long n = 0;
    if (!d->integer.empty()) {
        auto res = std::from_chars(d->integer.data(), d->integer.data() + d->integer.size(), n);
        if (res.ec != std::errc()) return "Out of range value for column '" + col.name + "'" + at_row(row);
    }
    if (d->negative) n = -n;
    if (n < lo || n > hi) return "Out of range value for column '" + col.name + "'" + at_row(row);
    value = std::to_string(n);
    return std::nullopt;
}

std::size_t utf8_length(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) n++;
    }
    return n;
}

bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int days_in(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : d[m - 1];
}

struct DateParts {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    bool has_time = false;
};

// 'YYYY-MM-DD', 'YYYY-M-D', 'YYYY/MM/DD', 'YYYYMMDD', each optionally followed by ' HH:MM:SS[.ffffff]' (or 'T')
std::optional<DateParts> parse_date_text(std::string_view text) {
    std::string_view s = trimmed(text);
    DateParts p;
    std::size_t i = 0;
    auto number = [&](int max_digits, int& out) {
        int n = 0, count = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && count < max_digits) { n = n * 10 + (s[i++] - '0'); count++; }
        out = n;
        return count;
    };
    // YYYYMMDD
    if (s.size() >= 8 && std::all_of(s.begin(), s.begin() + 8, [](char c) { return c >= '0' && c <= '9'; }) && (s.size() == 8 || s[8] == ' ' || s[8] == 'T')) {
        p.y = std::stoi(std::string(s.substr(0, 4)));
        p.mo = std::stoi(std::string(s.substr(4, 2)));
        p.d = std::stoi(std::string(s.substr(6, 2)));
        i = 8;
    } else {
        if (number(4, p.y) != 4) return std::nullopt;
        if (i >= s.size() || (s[i] != '-' && s[i] != '/' && s[i] != '.')) return std::nullopt;
        i++;
        int c = number(2, p.mo);
        if (c == 0) return std::nullopt;
        if (i >= s.size() || (s[i] != '-' && s[i] != '/' && s[i] != '.')) return std::nullopt;
        i++;
        c = number(2, p.d);
        if (c == 0) return std::nullopt;
    }
    if (i < s.size()) {
        if (s[i] != ' ' && s[i] != 'T') return std::nullopt;
        i++;
        p.has_time = true;
        if (number(2, p.h) == 0 || i >= s.size() || s[i] != ':') return std::nullopt;
        i++;
        if (number(2, p.mi) == 0) return std::nullopt;
        if (i < s.size() && s[i] == ':') {
            i++;
            if (number(2, p.s) == 0) return std::nullopt;
            if (i < s.size() && s[i] == '.') {
                i++;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            }
        }
        if (i != s.size()) return std::nullopt;
    }
    if (p.y < 1 || p.mo < 1 || p.mo > 12 || p.d < 1 || p.d > days_in(p.y, p.mo)) return std::nullopt;
    if (p.h > 23 || p.mi > 59 || p.s > 59) return std::nullopt;
    return p;
}

std::string two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

} // namespace

std::optional<std::string> Executor::coerce_column_value(const ColumnDef& col, std::string& value, std::size_t row) {
    if (value == EXECUTOR_NULL_VALUE) return std::nullopt;
    const DataType& dt = col.data_type;
    if (std::holds_alternative<DataType::Int>(dt.data)) return check_integer(col, value, row, -2147483648LL, 2147483647LL);
    if (std::holds_alternative<DataType::BigInt>(dt.data)) return check_integer(col, value, row, std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max());
    if (std::holds_alternative<DataType::SmallInt>(dt.data)) return check_integer(col, value, row, -32768, 32767);
    if (std::holds_alternative<DataType::TinyInt>(dt.data) || std::holds_alternative<DataType::Boolean>(dt.data)) {
        return check_integer(col, value, row, -128, 127);
    }
    if (std::holds_alternative<DataType::Float>(dt.data) || std::holds_alternative<DataType::Double>(dt.data)) {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "true") { value = "1"; return std::nullopt; }
        if (lower == "false") { value = "0"; return std::nullopt; }
        auto d = parse_decimal(value);
        if (!d) return "Incorrect double value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        std::string_view t = trimmed(value);
        double x = 0;
        auto res = std::from_chars(t.data() + (t.front() == '+' ? 1 : 0), t.data() + t.size(), x);
        if (res.ec != std::errc() || !std::isfinite(x)) return "Out of range value for column '" + col.name + "'" + at_row(row);
        char buf[64];
        std::to_chars_result out = std::holds_alternative<DataType::Float>(dt.data) ? std::to_chars(buf, buf + sizeof(buf), static_cast<float>(x))
                                                                                  : std::to_chars(buf, buf + sizeof(buf), x);
        std::string text(buf, out.ptr);
        if (text == "-0") text = "0";
        value = text;
        return std::nullopt;
    }
    if (auto* dec = std::get_if<DataType::Decimal>(&dt.data)) {
        auto d = parse_decimal(value);
        if (!d) return "Incorrect decimal value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        round_to_scale(*d, dec->scale);
        if (d->integer.size() + dec->scale > dec->precision) return "Out of range value for column '" + col.name + "'" + at_row(row);
        value = decimal_text(*d, dec->scale);
        return std::nullopt;
    }
    if (auto* vc = std::get_if<DataType::Varchar>(&dt.data)) {
        if (value.size() <= vc->length) return std::nullopt; // at most as many characters as bytes
        if (utf8_length(value) <= vc->length) return std::nullopt;
        // anything beyond the length but spaces is "too long"; spaces beyond it are cut
        std::size_t chars = 0, cut = value.size();
        for (std::size_t i = 0; i < value.size(); i++) {
            if ((static_cast<unsigned char>(value[i]) & 0xC0) == 0x80) continue;
            if (chars == vc->length) { cut = i; break; }
            chars++;
        }
        if (value.find_first_not_of(' ', cut) != std::string::npos) return "Data too long for column '" + col.name + "'" + at_row(row);
        value.resize(cut);
        return std::nullopt;
    }
    if (std::holds_alternative<DataType::Date>(dt.data)) {
        auto p = parse_date_text(value);
        if (!p) return "Incorrect date value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        value = std::to_string(p->y);
        value.insert(0, 4 - std::min<std::size_t>(4, value.size()), '0');
        value += "-" + two(p->mo) + "-" + two(p->d);
        return std::nullopt;
    }
    if (std::holds_alternative<DataType::DateTime>(dt.data) || std::holds_alternative<DataType::Timestamp>(dt.data)) {
        auto p = parse_date_text(value);
        if (!p) return "Incorrect datetime value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        std::string y = std::to_string(p->y);
        y.insert(0, 4 - std::min<std::size_t>(4, y.size()), '0');
        value = y + "-" + two(p->mo) + "-" + two(p->d) + " " + two(p->h) + ":" + two(p->mi) + ":" + two(p->s);
        return std::nullopt;
    }
    if (std::holds_alternative<DataType::Time>(dt.data)) {
        std::string_view t = trimmed(value);
        bool negative = !t.empty() && t.front() == '-';
        if (negative) t.remove_prefix(1);
        int parts[3] = {0, 0, 0};
        int n = 0;
        std::size_t i = 0;
        bool ok = !t.empty();
        while (ok && n < 3) {
            int v = 0, count = 0;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') { v = v * 10 + (t[i++] - '0'); count++; }
            if (count == 0) { ok = false; break; }
            parts[n++] = v;
            if (i < t.size() && t[i] == ':') i++; else break;
        }
        if (ok && i < t.size() && t[i] == '.') { // fractional seconds are dropped
            i++;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') i++;
        }
        if (!ok || i != t.size() || n < 2 || parts[1] > 59 || parts[2] > 59 || parts[0] > 838) {
            return "Incorrect time value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        }
        value = std::string(negative ? "-" : "") + two(parts[0]) + ":" + two(parts[1]) + ":" + two(parts[2]);
        return std::nullopt;
    }
    if (std::holds_alternative<DataType::Year>(dt.data)) {
        auto d = parse_decimal(value);
        if (!d || !d->fraction.empty() || d->negative) return "Incorrect integer value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        long y = d->integer.empty() ? 0 : std::stol(d->integer.size() > 6 ? "999999" : d->integer);
        if (y != 0 && (y < 1901 || y > 2155)) return "Out of range value for column '" + col.name + "'" + at_row(row);
        value = y == 0 ? "0000" : std::to_string(y);
        return std::nullopt;
    }
    if (std::holds_alternative<DataType::Json>(dt.data)) {
        if (!nlohmann::json::accept(value)) return "Invalid JSON text for column '" + col.name + "'" + at_row(row);
        return std::nullopt;
    }
    return std::nullopt; // TEXT, BLOB, ENUM, SET (checked elsewhere), Unknown
}

} // namespace engine
