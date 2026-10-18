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

std::optional<std::string> check_integer(const ColumnDef& col, std::string& value, std::size_t row, long long lo, unsigned long long hi) {
    // already canonical and small (-?[1-9][0-9]{0,8} or 0): the usual case
    if (!value.empty() && value.size() < 10) {
        const std::size_t i = value.front() == '-' ? 1 : 0;
        bool canonical = value == "0" || (i < value.size() && value[i] >= '1' && value[i] <= '9');
        for (std::size_t k = i; k < value.size() && canonical; k++) canonical = value[k] >= '0' && value[k] <= '9';
        if (canonical) {
            const long long n = std::stoll(value);
            if (n < 0 ? n >= lo : static_cast<unsigned long long>(n) <= hi) return std::nullopt;
        }
    }
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true") { value = "1"; return std::nullopt; }
    if (lower == "false") { value = "0"; return std::nullopt; }
    auto d = parse_decimal(value);
    if (!d) return "Incorrect integer value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
    const bool below_zero = d->negative && (d->integer.find_first_not_of('0') != std::string::npos || d->fraction.find_first_not_of('0') != std::string::npos);
    round_to_scale(*d, 0);
    if (d->integer.size() > 20) return "Out of range value for column '" + col.name + "'" + at_row(row);
    if (lo >= 0 && below_zero) return "Out of range value for column '" + col.name + "'" + at_row(row); // (MySQL: -0.4 is not 0 in an unsigned column)
    unsigned long long magnitude = 0; // the digits; the sign is kept apart so that -2^63 and 2^64 - 1 both fit
    if (!d->integer.empty()) {
        auto res = std::from_chars(d->integer.data(), d->integer.data() + d->integer.size(), magnitude);
        if (res.ec != std::errc()) return "Out of range value for column '" + col.name + "'" + at_row(row);
    }
    if (d->negative && magnitude != 0) {
        if (lo >= 0 || magnitude > 0ULL - static_cast<unsigned long long>(lo)) return "Out of range value for column '" + col.name + "'" + at_row(row);
        value = "-" + std::to_string(magnitude);
    } else {
        if (magnitude > hi) return "Out of range value for column '" + col.name + "'" + at_row(row);
        value = std::to_string(magnitude);
    }
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
    int micro = 0; // the fraction of the second in millionths (digits beyond the sixth are rounded away)
    bool has_time = false;
};

// The digits of a fraction of a second at s[i..] (after the '.'): millionths, the seventh digit rounds
int parse_micro(std::string_view s, std::size_t& i) {
    int micro = 0, digits = 0;
    bool round_up = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (digits < 6) micro = micro * 10 + (s[i] - '0');
        else if (digits == 6) round_up = s[i] >= '5';
        digits++;
        i++;
    }
    for (; digits < 6; digits++) micro *= 10;
    return micro + (round_up ? 1 : 0);
}

int year_of(int two_digits) { return two_digits < 70 ? 2000 + two_digits : 1900 + two_digits; }

// 'YYYY-MM-DD', 'YY-MM-DD', 'YYYY-M-D', 'YYYY/MM/DD', and the digits alone: YYYYMMDD, YYMMDD, YYYYMMDDhhmmss, YYMMDDhhmmss; each optionally followed by
// ' HH:MM[:SS[.ffffff]]' (or 'T') -- the digits of a whole datetime also by .ffffff
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
    std::size_t run = 0; // the digits the text starts with
    while (run < s.size() && s[run] >= '0' && s[run] <= '9') run++;
    const bool only_digits = run > 0 && (run == s.size() || s[run] == '.' || s[run] == ' ' || s[run] == 'T');
    if (only_digits && (run == 6 || run == 8 || run == 12 || run == 14)) {
        auto two_digits = [&](std::size_t at) { return (s[at] - '0') * 10 + (s[at + 1] - '0'); };
        std::size_t at = 0;
        if (run == 8 || run == 14) {
            p.y = two_digits(0) * 100 + two_digits(2);
            at = 4;
        } else {
            p.y = year_of(two_digits(0));
            at = 2;
        }
        p.mo = two_digits(at);
        p.d = two_digits(at + 2);
        at += 4;
        i = run;
        if (run >= 12) {
            p.has_time = true;
            p.h = two_digits(at);
            p.mi = two_digits(at + 2);
            p.s = two_digits(at + 4);
            if (i < s.size() && s[i] == '.') {
                i++;
                p.micro = parse_micro(s, i);
            }
            if (i != s.size()) return std::nullopt;
        }
    } else {
        const int year_digits = number(4, p.y);
        if (year_digits != 4 && year_digits != 2) return std::nullopt;
        if (year_digits == 2) p.y = year_of(p.y);
        if (i >= s.size() || (s[i] != '-' && s[i] != '/' && s[i] != '.')) return std::nullopt;
        i++;
        int c = number(2, p.mo);
        if (c == 0) return std::nullopt;
        if (i >= s.size() || (s[i] != '-' && s[i] != '/' && s[i] != '.')) return std::nullopt;
        i++;
        c = number(2, p.d);
        if (c == 0) return std::nullopt;
    }
    if (i < s.size() && !p.has_time) {
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
                p.micro = parse_micro(s, i);
            }
        }
        if (i != s.size()) return std::nullopt;
    }
    if (p.y < 1 || p.mo < 1 || p.mo > 12 || p.d < 1 || p.d > days_in(p.y, p.mo)) return std::nullopt;
    if (p.h > 23 || p.mi > 59 || p.s > 59) return std::nullopt;
    return p;
}

// Rounds the fraction of a second to `digits` places (half up); the number of seconds the rounding carried
int round_micro(int& micro, int digits) {
    int unit = 1;
    for (int k = digits; k < 6; k++) unit *= 10;
    micro = (micro + unit / 2) / unit * unit;
    if (micro < 1000000) return 0;
    micro -= 1000000;
    return 1;
}

// A datetime one second later; false: past the year 9999
bool next_second(DateParts& p) {
    if (++p.s < 60) return true;
    p.s = 0;
    if (++p.mi < 60) return true;
    p.mi = 0;
    if (++p.h < 24) return true;
    p.h = 0;
    if (++p.d <= days_in(p.y, p.mo)) return true;
    p.d = 1;
    if (++p.mo <= 12) return true;
    p.mo = 1;
    return ++p.y <= 9999;
}

// ".fff": the first `digits` places of the millionths, nothing when the column keeps no fraction
std::string micro_text(int micro, int digits) {
    if (digits <= 0) return "";
    std::string all = std::to_string(micro);
    all.insert(0, 6 - all.size(), '0');
    return "." + all.substr(0, static_cast<std::size_t>(digits));
}

// A time of day or duration: [-]HH:MM:SS[.ffffff] with the hours up to 838 -- read from the forms [D ]HH:MM[:SS[.f]], [D ]HH, and the digits alone
// (HHMMSS, MMSS, SS: the last two are the seconds, the two before them the minutes, the rest the hours)
struct TimeParts {
    bool negative = false;
    long long hours = 0;
    int minutes = 0, seconds = 0, micro = 0;
};

std::optional<TimeParts> parse_time_text(std::string_view text) {
    std::string_view t = trimmed(text);
    TimeParts r;
    if (!t.empty() && t.front() == '-') {
        r.negative = true;
        t.remove_prefix(1);
    }
    if (t.empty()) return std::nullopt;
    std::size_t i = 0;
    auto number = [&](long long& out) {
        long long v = 0;
        std::size_t count = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            if (v < 1000000000000LL) v = v * 10 + (t[i] - '0');
            i++;
            count++;
        }
        out = v;
        return count;
    };
    auto fraction = [&]() {
        if (i < t.size() && t[i] == '.') {
            i++;
            r.micro = parse_micro(t, i);
        }
    };
    long long first = 0;
    if (number(first) == 0) return std::nullopt;
    long long days = 0;
    bool with_days = false;
    if (i < t.size() && t[i] == ' ') { // 'D HH...'
        days = first;
        with_days = true;
        i++;
        if (number(first) == 0) return std::nullopt;
    }
    if (i < t.size() && t[i] == ':') {
        long long minutes = 0, seconds = 0;
        i++;
        if (number(minutes) == 0) return std::nullopt;
        if (i < t.size() && t[i] == ':') {
            i++;
            if (number(seconds) == 0) return std::nullopt;
            fraction();
        }
        r.hours = days * 24 + first;
        r.minutes = static_cast<int>(std::min<long long>(minutes, 99));
        r.seconds = static_cast<int>(std::min<long long>(seconds, 99));
    } else if (with_days) {
        if (r.negative) return std::nullopt; // ('-1 1' is no time; '-1 01:00:00' is)
        r.hours = days * 24 + first; // 'D HH'
    } else {
        r.seconds = static_cast<int>(first % 100);
        r.minutes = static_cast<int>(first / 100 % 100);
        r.hours = first / 10000;
        fraction();
    }
    if (i != t.size() || r.minutes > 59 || r.seconds > 59) return std::nullopt;
    return r;
}

std::string two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

} // namespace

std::optional<std::string> Executor::coerce_column_value(const ColumnDef& col, std::string& value, std::size_t row) {
    if (value == EXECUTOR_NULL_VALUE) return std::nullopt;
    const DataType& dt = col.data_type;
    const bool u = col.is_unsigned; // UNSIGNED: from 0 up to twice the signed maximum plus one
    if (std::holds_alternative<DataType::Int>(dt.data)) return u ? check_integer(col, value, row, 0, 4294967295ULL) : check_integer(col, value, row, -2147483648LL, 2147483647ULL);
    if (std::holds_alternative<DataType::BigInt>(dt.data)) {
        return u ? check_integer(col, value, row, 0, std::numeric_limits<unsigned long long>::max())
                 : check_integer(col, value, row, std::numeric_limits<long long>::min(), static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    }
    if (std::holds_alternative<DataType::SmallInt>(dt.data)) return u ? check_integer(col, value, row, 0, 65535ULL) : check_integer(col, value, row, -32768, 32767ULL);
    if (std::holds_alternative<DataType::TinyInt>(dt.data)) return u ? check_integer(col, value, row, 0, 255ULL) : check_integer(col, value, row, -128, 127ULL);
    if (std::holds_alternative<DataType::Boolean>(dt.data)) return check_integer(col, value, row, -128, 127ULL);
    if (std::holds_alternative<DataType::Float>(dt.data) || std::holds_alternative<DataType::Double>(dt.data)) {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "true") { value = "1"; return std::nullopt; }
        if (lower == "false") { value = "0"; return std::nullopt; }
        auto d = parse_decimal(value);
        if (!d) return "Incorrect double value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        if (u && d->negative && !is_zero(*d)) return "Out of range value for column '" + col.name + "'" + at_row(row); // (FLOAT / DOUBLE UNSIGNED)
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
        if (u && d->negative && !is_zero(*d)) return "Out of range value for column '" + col.name + "'" + at_row(row); // (DECIMAL UNSIGNED)
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
        const int digits = std::holds_alternative<DataType::DateTime>(dt.data) ? std::get<DataType::DateTime>(dt.data).fsp : std::get<DataType::Timestamp>(dt.data).fsp;
        auto p = parse_date_text(value);
        if (p && round_micro(p->micro, digits) == 1 && !next_second(*p)) p.reset();
        if (!p) return "Incorrect datetime value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        std::string y = std::to_string(p->y);
        y.insert(0, 4 - std::min<std::size_t>(4, y.size()), '0');
        value = y + "-" + two(p->mo) + "-" + two(p->d) + " " + two(p->h) + ":" + two(p->mi) + ":" + two(p->s) + micro_text(p->micro, digits);
        return std::nullopt;
    }
    if (std::holds_alternative<DataType::Time>(dt.data)) {
        const int digits = std::get<DataType::Time>(dt.data).fsp;
        auto p = parse_time_text(value);
        if (p) { // the fraction rounds to the digits the column keeps, and the carry runs into the seconds
            long long total = p->hours * 3600 + p->minutes * 60 + p->seconds + round_micro(p->micro, digits);
            p->hours = total / 3600;
            p->minutes = static_cast<int>(total / 60 % 60);
            p->seconds = static_cast<int>(total % 60);
        }
        if (!p || p->hours > 838) return "Incorrect time value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        std::string hours = std::to_string(p->hours);
        if (hours.size() < 2) hours.insert(0, 1, '0');
        const bool zero = p->hours == 0 && p->minutes == 0 && p->seconds == 0 && p->micro == 0;
        value = std::string(p->negative && !zero ? "-" : "") + hours + ":" + two(p->minutes) + ":" + two(p->seconds) + micro_text(p->micro, digits);
        return std::nullopt;
    }
    if (std::holds_alternative<DataType::Year>(dt.data)) {
        auto d = parse_decimal(value);
        if (!d || !d->fraction.empty() || d->negative) return "Incorrect integer value: '" + shown(value) + "' for column '" + col.name + "'" + at_row(row);
        long y = d->integer.empty() ? 0 : std::stol(d->integer.size() > 6 ? "999999" : d->integer);
        if (y >= 1 && y <= 99) y = year_of(static_cast<int>(y)); // two digits: 70-99 are 1970-1999, 1-69 are 2001-2069
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
