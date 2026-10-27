#pragma once

// Helpers for the built-in functions, written to MySQL's rules: UTF-8 strings counted by character, decimals rounded exactly (on their digits,
// not on a double), and dates calculated on the calendar.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace engine::sqlval {

// x / 0, x DIV 0 and x % 0 work out to NULL, which a SELECT shows; what writes (INSERT, UPDATE, a stored function) counts them, before and after it works its
// values out, to refuse them as MySQL does (error 1365 "Division by 0"). Per thread: a statement is evaluated on the thread that runs it.
inline thread_local unsigned long long division_by_zero_count = 0;
// The same for a text that is read as a number without being wholly one ('abc' + 1, '12abc' * 2): a SELECT reads the number it starts with, what writes
// refuses it (error 1292 "Truncated incorrect DOUBLE value: 'abc'"). A text of blanks is 0, as in MySQL.
inline thread_local unsigned long long truncated_number_count = 0;
inline thread_local std::string truncated_number_text;

// UPDATE and DELETE read their WHERE as strictly as the values they write: a text compared with a number is read as a number, and when it is not wholly one the
// statement fails (error 1292 "Truncated incorrect DOUBLE value"), where a SELECT reads the number it starts with. Raised while one of them evaluates its WHERE
// (StrictComparison); the comparison notes such a text in `truncated_number_count` then, and nothing otherwise.
inline thread_local int strict_comparison_depth = 0;
struct StrictComparison {
    StrictComparison() { ++strict_comparison_depth; }
    ~StrictComparison() { --strict_comparison_depth; }
    StrictComparison(const StrictComparison&) = delete;
    StrictComparison& operator=(const StrictComparison&) = delete;
};

// What a statement that writes asks before it keeps the values it worked out: the error MySQL gives for a division by zero or a text read as a number, if the
// evaluation since this was made had one.
struct StrictWrite {
    unsigned long long zeros = division_by_zero_count, cuts = truncated_number_count;
    std::optional<std::string> error() const {
        if (division_by_zero_count != zeros) return "Division by 0";
        if (truncated_number_count != cuts) return "Truncated incorrect DOUBLE value: '" + truncated_number_text + "'";
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------------------------------------------------------------------------

// The characters of a UTF-8 text, each as its bytes (a byte that is not part of a well-formed sequence is a character of its own).
inline std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        if (i + n > s.size()) n = 1;
        for (std::size_t k = 1; k < n; k++) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { n = 1; break; }
        }
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

inline std::size_t utf8_length(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); i++) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) n++;
    }
    return n;
}

inline std::uint32_t utf8_code_point(const std::string& ch) {
    if (ch.empty()) return 0;
    const unsigned char c = static_cast<unsigned char>(ch[0]);
    if (ch.size() == 1) return c;
    std::uint32_t cp = ch.size() == 2 ? (c & 0x1F) : ch.size() == 3 ? (c & 0x0F) : (c & 0x07);
    for (std::size_t i = 1; i < ch.size(); i++) cp = (cp << 6) | (static_cast<unsigned char>(ch[i]) & 0x3F);
    return cp;
}

inline std::string utf8_encode(std::uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

// Simple (one-to-one) case mapping for the scripts that have case: ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic.
inline std::uint32_t to_upper_cp(std::uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 0) ? c - 1 : c; // pairs start on an odd code point there
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if (c == 0x138 || c == 0x149) return c;
        return (c % 2 == 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3AC && c <= 0x3AF) return c == 0x3AC ? 0x386 : c - 37;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

inline std::uint32_t to_lower_cp(std::uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 1) ? c + 1 : c;
        if (c == 0x130) return 'i';
        if (c == 0x138 || c == 0x149 || c == 0x17F || c == 0x131) return c;
        return (c % 2 == 0) ? c + 1 : c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

inline std::string map_case(const std::string& s, bool upper) {
    std::string out;
    out.reserve(s.size());
    for (const std::string& ch : utf8_chars(s)) {
        if (ch.size() == 1 && static_cast<unsigned char>(ch[0]) < 0x80) {
            out.push_back(static_cast<char>(upper ? (ch[0] >= 'a' && ch[0] <= 'z' ? ch[0] - 32 : ch[0]) : (ch[0] >= 'A' && ch[0] <= 'Z' ? ch[0] + 32 : ch[0])));
        } else if (ch.size() == 1) {
            out += ch; // (a stray byte)
        } else {
            const std::uint32_t cp = utf8_code_point(ch);
            out += utf8_encode(upper ? to_upper_cp(cp) : to_lower_cp(cp));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------------------------------------------------------------------------

// The leading number of a text the way MySQL reads a string as a number: optional blanks, sign, digits, a fraction, an exponent; the rest is ignored.
// `ok` says whether the whole text was a number.
inline double leading_number(const std::string& s, bool* whole = nullptr) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    const std::size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    std::size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
    }
    if (digits == 0) {
        if (whole) *whole = false;
        return 0.0;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) j++;
        if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
            i = j;
        }
    }
    std::size_t end = i;
    while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) end++;
    if (whole) *whole = end == s.size();
    return std::strtod(s.substr(start, i - start).c_str(), nullptr);
}

// The shortest text that reads back as the same double, written the way MySQL writes a DOUBLE (1e20, 1.5e-7; whole numbers without a fraction).
// `single`: the shortest digits that read back as the FLOAT (single precision) nearest `v`, not as the double
inline std::string format_double(double v, bool single = false) {
    if (std::isnan(v)) return "NULL";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    if (v == 0.0) return "0";
    // the shortest digits that read back as `v`, and where the point goes: d.ddd e power
    char buf[64];
    auto r = single ? std::to_chars(buf, buf + sizeof buf, static_cast<float>(v), std::chars_format::scientific)
                    : std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string s(buf, r.ptr);
    const bool minus = s[0] == '-';
    if (minus) s.erase(0, 1);
    const std::size_t e = s.find('e');
    std::string digits = s.substr(0, e);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    const int power = std::atoi(s.c_str() + e + 1);
    std::string out;
    if (power >= -15 && power <= 14) { // (MySQL writes the numbers from 1e-15 up to 999999999999999 out, the others as 1.5e16 / 1e-16)
        if (power < 0) {
            out = "0." + std::string(static_cast<std::size_t>(-power - 1), '0') + digits;
        } else {
            const std::size_t whole = static_cast<std::size_t>(power) + 1;
            if (digits.size() < whole) digits.append(whole - digits.size(), '0');
            out = digits.substr(0, whole);
            if (digits.size() > whole) out += "." + digits.substr(whole);
        }
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += "e" + std::to_string(power);
    }
    return minus ? "-" + out : out;
}

// A FLOAT holds a single-precision number and MySQL shows it with six significant digits: 1.2345679 is 1.23457, 123456790 is 123457000, 16777217 is 16777200.
// `text` is the number as it is stored (or any number); anything that is no finite number comes back as it is.
inline std::string format_float_display(const std::string& text) {
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) return text;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.5e", v);
    return format_double(std::strtod(buf, nullptr));
}

// The number a FLOAT holds, written as the DOUBLE it becomes when it goes into a DOUBLE (or any other number) column: 0.1 in a FLOAT is 0.10000000149011612 there, 1234567
// stays 1234567. `text` is the FLOAT as it is stored (the shortest text that reads back as the float); anything that is no finite number comes back as it is.
inline std::string float_as_double_text(const std::string& text) {
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) return text;
    const double widened = static_cast<double>(static_cast<float>(v));
    return widened == v ? text : format_double(widened);
}

// An exact decimal: sign, the digits of the integer and fraction parts. Rounding and truncation work on the digits, so ROUND(1.255, 2) is 1.26
// (a double holds 1.25499999...).
struct Decimal {
    bool negative = false;
    std::string integer = "0";  // no leading zeros ("0" for none)
    std::string fraction;       // digits after the point

    static std::optional<Decimal> parse(const std::string& text) {
        std::size_t i = 0;
        while (i < text.size() && text[i] == ' ') i++;
        Decimal d;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) d.negative = text[i++] == '-';
        std::string ip, fp;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ip.push_back(text[i++]);
        if (i < text.size() && text[i] == '.') {
            i++;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') fp.push_back(text[i++]);
        }
        if (ip.empty() && fp.empty()) return std::nullopt;
        int exponent = 0;
        if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
            std::size_t j = i + 1;
            bool neg = false;
            if (j < text.size() && (text[j] == '+' || text[j] == '-')) neg = text[j++] == '-';
            if (j < text.size() && text[j] >= '0' && text[j] <= '9') {
                int e = 0;
                while (j < text.size() && text[j] >= '0' && text[j] <= '9' && e < 1000) e = e * 10 + (text[j++] - '0');
                exponent = neg ? -e : e;
                i = j;
            }
        }
        while (i < text.size() && text[i] == ' ') i++;
        if (i != text.size()) return std::nullopt;
        if (exponent > 0) {
            for (int k = 0; k < exponent; k++) {
                ip.push_back(fp.empty() ? '0' : fp.front());
                if (!fp.empty()) fp.erase(0, 1);
            }
        } else if (exponent < 0) {
            for (int k = 0; k < -exponent; k++) {
                fp.insert(fp.begin(), ip.empty() ? '0' : ip.back());
                if (!ip.empty()) ip.pop_back();
            }
        }
        const std::size_t nz = ip.find_first_not_of('0');
        d.integer = nz == std::string::npos ? "0" : ip.substr(nz);
        d.fraction = fp;
        return d;
    }

    bool is_zero() const {
        return integer == "0" && fraction.find_first_not_of('0') == std::string::npos;
    }

    // The digits as a text; `scale` digits after the point (fewer digits are padded with zeros, more are cut).
    std::string str(std::size_t scale) const {
        std::string f = fraction;
        f.resize(scale, '0');
        std::string out = (negative && !is_zero() ? "-" : "") + integer;
        if (scale > 0) out += "." + f;
        return out;
    }

    // The value with trailing fraction zeros dropped.
    std::string compact() const {
        std::string f = fraction;
        while (!f.empty() && f.back() == '0') f.pop_back();
        std::string out = (negative && !is_zero() ? "-" : "") + integer;
        if (!f.empty()) out += "." + f;
        return out;
    }

    // Round to `places` digits after the point (negative: before it), half away from zero; or cut them off.
    Decimal rounded(long long places, bool cut) const {
        Decimal r = *this;
        if (places >= 0) {
            if (r.fraction.size() <= static_cast<std::size_t>(places)) return r;
            const bool up = !cut && r.fraction[static_cast<std::size_t>(places)] >= '5';
            r.fraction.resize(static_cast<std::size_t>(places));
            if (up) r.add_unit(static_cast<std::size_t>(places));
            return r;
        }
        const std::size_t drop = static_cast<std::size_t>(-places);
        if (drop > r.integer.size()) { // everything is below the rounding digit
            Decimal z;
            z.negative = r.negative;
            return z;
        }
        const std::string kept = r.integer.substr(0, r.integer.size() - drop);
        const bool up = !cut && r.integer[r.integer.size() - drop] >= '5';
        Decimal out;
        out.negative = r.negative;
        out.integer = kept.empty() ? "0" : kept;
        if (up) out.add_unit(0);
        out.integer += std::string(drop, '0');
        if (out.integer.find_first_not_of('0') == std::string::npos) out.integer = "0";
        return out;
    }

    // Add one unit in the last place of `places` fraction digits.
    void add_unit(std::size_t places) {
        std::string digits = integer + fraction.substr(0, places);
        std::size_t i = digits.size();
        while (i > 0) {
            if (digits[i - 1] == '9') { digits[i - 1] = '0'; i--; }
            else { digits[i - 1]++; break; }
        }
        if (i == 0) digits.insert(digits.begin(), '1');
        integer = digits.substr(0, digits.size() - places);
        fraction = digits.substr(digits.size() - places);
        if (integer.empty()) integer = "0";
    }

    // Integer part of the value cut toward zero, if it fits a signed 64-bit integer.
    std::optional<long long> to_int64() const {
        if (integer.size() > 19) return std::nullopt;
        unsigned long long v = 0;
        for (char c : integer) {
            if (v > (std::numeric_limits<unsigned long long>::max() - 9) / 10) return std::nullopt;
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (negative) {
            if (v > 9223372036854775808ULL) return std::nullopt;
            return static_cast<long long>(0 - v);
        }
        if (v > 9223372036854775807ULL) return std::nullopt;
        return static_cast<long long>(v);
    }
};


// ---------------------------------------------------------------------------------------------------------------------------------------------
// Exact decimal arithmetic (MySQL's DECIMAL): value = (negative ? -1 : 1) * digits / 10^scale
// ---------------------------------------------------------------------------------------------------------------------------------------------

struct Num {
    bool negative = false;
    std::string digits = "0"; // the unscaled integer, no leading zeros
    int scale = 0;

    static Num from_decimal(const Decimal& d) {
        Num n;
        std::string all = d.integer + d.fraction;
        const std::size_t nz = all.find_first_not_of('0');
        n.digits = nz == std::string::npos ? "0" : all.substr(nz);
        n.scale = static_cast<int>(d.fraction.size());
        n.negative = d.negative && n.digits != "0";
        return n;
    }

    static Num from_int(long long v) {
        Num n;
        n.negative = v < 0;
        unsigned long long a = v < 0 ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
        n.digits = std::to_string(a);
        return n;
    }

    bool is_zero() const { return digits == "0"; }

    Decimal to_decimal() const {
        Decimal d;
        d.negative = negative && !is_zero();
        std::string all = digits;
        if (static_cast<int>(all.size()) <= scale) all.insert(0, static_cast<std::size_t>(scale) - all.size() + 1, '0');
        d.integer = all.substr(0, all.size() - static_cast<std::size_t>(scale));
        d.fraction = all.substr(all.size() - static_cast<std::size_t>(scale));
        return d;
    }
};

inline int cmp_abs_digits(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

inline std::string strip_zeros(std::string s) {
    const std::size_t nz = s.find_first_not_of('0');
    return nz == std::string::npos ? "0" : s.substr(nz);
}

inline std::string add_abs_digits(const std::string& a, const std::string& b) {
    std::string out;
    int carry = 0;
    for (std::size_t i = 0; i < std::max(a.size(), b.size()) || carry; i++) {
        const int x = i < a.size() ? a[a.size() - 1 - i] - '0' : 0;
        const int y = i < b.size() ? b[b.size() - 1 - i] - '0' : 0;
        const int sum = x + y + carry;
        out.push_back(static_cast<char>('0' + sum % 10));
        carry = sum / 10;
    }
    std::reverse(out.begin(), out.end());
    return out.empty() ? "0" : out;
}

// a - b for |a| >= |b|
inline std::string sub_abs_digits(const std::string& a, const std::string& b) {
    std::string out;
    int borrow = 0;
    for (std::size_t i = 0; i < a.size(); i++) {
        int x = a[a.size() - 1 - i] - '0' - borrow;
        const int y = i < b.size() ? b[b.size() - 1 - i] - '0' : 0;
        if (x < y) { x += 10; borrow = 1; } else { borrow = 0; }
        out.push_back(static_cast<char>('0' + x - y));
    }
    std::reverse(out.begin(), out.end());
    return strip_zeros(out);
}

inline std::string mul_abs_digits(const std::string& a, const std::string& b) {
    if (a == "0" || b == "0") return "0";
    std::vector<int> cells(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); i++) {
        for (std::size_t j = 0; j < b.size(); j++) cells[i + j + 1] += (a[i] - '0') * (b[j] - '0');
    }
    for (std::size_t k = cells.size(); k-- > 1;) {
        cells[k - 1] += cells[k] / 10;
        cells[k] %= 10;
    }
    std::string out;
    for (int c : cells) out.push_back(static_cast<char>('0' + c));
    return strip_zeros(out);
}

// the integer quotient of two digit strings
inline std::string div_abs_digits(const std::string& num, const std::string& den) {
    std::string q, rem = "0";
    for (char ch : num) {
        rem = strip_zeros(rem + ch);
        int count = 0;
        while (cmp_abs_digits(rem, den) >= 0) {
            rem = sub_abs_digits(rem, den);
            count++;
        }
        q.push_back(static_cast<char>('0' + count));
    }
    return strip_zeros(q);
}

inline Num align_scale(Num n, int scale) {
    if (n.scale < scale && n.digits != "0") n.digits += std::string(static_cast<std::size_t>(scale - n.scale), '0');
    n.scale = scale;
    return n;
}

inline Num num_add(Num a, Num b, bool subtract) {
    if (subtract) b.negative = !b.negative;
    const int scale = std::max(a.scale, b.scale);
    a = align_scale(a, scale);
    b = align_scale(b, scale);
    Num r;
    r.scale = scale;
    if (a.negative == b.negative) {
        r.digits = add_abs_digits(a.digits, b.digits);
        r.negative = a.negative;
    } else {
        const int c = cmp_abs_digits(a.digits, b.digits);
        if (c == 0) { r.digits = "0"; r.negative = false; }
        else if (c > 0) { r.digits = sub_abs_digits(a.digits, b.digits); r.negative = a.negative; }
        else { r.digits = sub_abs_digits(b.digits, a.digits); r.negative = b.negative; }
    }
    if (r.digits == "0") r.negative = false;
    return r;
}

inline Num num_mul(const Num& a, const Num& b) {
    Num r;
    r.digits = mul_abs_digits(a.digits, b.digits);
    r.scale = a.scale + b.scale;
    r.negative = r.digits != "0" && a.negative != b.negative;
    return r;
}

// Round (half away from zero) or cut a number to `scale` digits after the point; more digits are padded with zeros.
inline Num num_round(Num n, int scale, bool cut = false) {
    if (n.scale <= scale) return align_scale(n, scale);
    const std::size_t drop = static_cast<std::size_t>(n.scale - scale);
    std::string digits = n.digits;
    if (digits.size() <= drop) digits.insert(0, drop - digits.size() + 1, '0');
    const bool up = !cut && digits[digits.size() - drop] >= '5';
    digits.resize(digits.size() - drop);
    if (up) digits = add_abs_digits(digits, "1");
    n.digits = strip_zeros(digits);
    n.scale = scale;
    if (n.digits == "0") n.negative = false;
    return n;
}

// a / b rounded to `scale` digits after the point (b must not be zero)
inline Num num_div(const Num& a, const Num& b, int scale, bool cut = false) {
    // numerator * 10^(scale + b.scale - a.scale + 1) / denominator, one digit more for the rounding
    const int shift = scale + 1 + b.scale - a.scale;
    std::string numerator = a.digits, denominator = b.digits;
    if (shift >= 0) numerator += std::string(static_cast<std::size_t>(shift), '0');
    else denominator += std::string(static_cast<std::size_t>(-shift), '0');
    Num q;
    q.digits = div_abs_digits(numerator, denominator);
    q.scale = scale + 1;
    q.negative = q.digits != "0" && a.negative != b.negative;
    return num_round(q, scale, cut);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------------------------------------------------------------------------

inline bool is_leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline int days_in_month(long long y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && is_leap(y) ? 29 : d[m - 1];
}

// Days since 0000-03-01 style epoch used by the civil-calendar algorithms: days_from_civil(1970, 1, 1) == 0.
inline long long days_from_civil(long long y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

inline void civil_from_days(long long z, long long& y, int& m, int& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
}

struct DateTime {
    long long year = 0;
    int month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;
    long long micro = 0;
    bool has_date = false, has_time = false;

    long long epoch_days() const { return days_from_civil(year, month, day); }

    // seconds since 1970-01-01 00:00:00 (as if UTC)
    long long epoch_seconds() const { return epoch_days() * 86400 + hour * 3600LL + minute * 60LL + second; }

    static DateTime from_epoch_seconds(long long s, long long micro = 0) {
        DateTime t;
        long long days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
        long long rest = s - days * 86400;
        civil_from_days(days, t.year, t.month, t.day);
        t.hour = static_cast<int>(rest / 3600);
        t.minute = static_cast<int>(rest % 3600 / 60);
        t.second = static_cast<int>(rest % 60);
        t.micro = micro;
        t.has_date = t.has_time = true;
        return t;
    }

    bool valid_date() const {
        return year >= 0 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
    }

    std::string date_str() const {
        char b[32];
        std::snprintf(b, sizeof b, "%04lld-%02d-%02d", year, month, day);
        return b;
    }

    std::string time_str() const {
        char b[48];
        if (micro) std::snprintf(b, sizeof b, "%02d:%02d:%02d.%06lld", hour, minute, second, micro);
        else std::snprintf(b, sizeof b, "%02d:%02d:%02d", hour, minute, second);
        return b;
    }

    std::string datetime_str() const { return date_str() + " " + time_str(); }
};

// '2024-03-05', '2024-03-05 14:07:09[.123]', '20240305', '20240305140709', '2024/3/5', '2024-3-5': a date (and time); nullopt when it is none.
// The rest after a valid prefix is ignored the way MySQL does for a datetime string.
inline std::optional<DateTime> parse_datetime(const std::string& text) {
    std::string s = text;
    const std::size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::nullopt;
    s = s.substr(a);
    DateTime t;
    auto digits_at = [&](std::size_t& i, int min_n, int max_n, long long& out) {
        std::size_t start = i;
        long long v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && static_cast<int>(i - start) < max_n) v = v * 10 + (s[i++] - '0');
        if (static_cast<int>(i - start) < min_n) return false;
        out = v;
        return true;
    };
    std::size_t i = 0;
    const bool all_digits = std::all_of(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(s.size(), 14)), [](char c) { return c >= '0' && c <= '9'; });
    if (all_digits && (s.size() == 8 || s.size() == 14)) {
        long long y, mo, d, h = 0, mi = 0, se = 0;
        if (!digits_at(i, 4, 4, y) || !digits_at(i, 2, 2, mo) || !digits_at(i, 2, 2, d)) return std::nullopt;
        if (s.size() == 14 && (!digits_at(i, 2, 2, h) || !digits_at(i, 2, 2, mi) || !digits_at(i, 2, 2, se))) return std::nullopt;
        t.year = y; t.month = static_cast<int>(mo); t.day = static_cast<int>(d);
        t.hour = static_cast<int>(h); t.minute = static_cast<int>(mi); t.second = static_cast<int>(se);
        t.has_date = true; t.has_time = s.size() == 14;
        return t;
    }
    long long y, mo, d;
    if (!digits_at(i, 1, 4, y)) return std::nullopt;
    if (i >= s.size() || (s[i] != '-' && s[i] != '/')) return std::nullopt;
    const char sep = s[i++];
    if (!digits_at(i, 1, 2, mo)) return std::nullopt;
    if (i >= s.size() || s[i] != sep) return std::nullopt;
    i++;
    if (!digits_at(i, 1, 2, d)) return std::nullopt;
    t.year = y < 70 && s.find('-') < 3 ? 2000 + y : (y < 100 && s.find('-') < 3 ? 1900 + y : y);
    t.month = static_cast<int>(mo);
    t.day = static_cast<int>(d);
    t.has_date = true;
    if (i < s.size() && (s[i] == ' ' || s[i] == 'T')) {
        std::size_t j = i + 1;
        long long h, mi, se = 0;
        if (digits_at(j, 1, 2, h) && j < s.size() && s[j] == ':') {
            j++;
            if (digits_at(j, 1, 2, mi)) {
                if (j < s.size() && s[j] == ':') {
                    j++;
                    if (!digits_at(j, 1, 2, se)) se = 0;
                }
                t.hour = static_cast<int>(h); t.minute = static_cast<int>(mi); t.second = static_cast<int>(se);
                t.has_time = true;
                if (j < s.size() && s[j] == '.') {
                    j++;
                    std::string frac;
                    while (j < s.size() && s[j] >= '0' && s[j] <= '9' && frac.size() < 6) frac.push_back(s[j++]);
                    frac.resize(6, '0');
                    t.micro = frac.empty() ? 0 : std::atoll(frac.c_str());
                    if (j < s.size() && s[j] >= '5' && s[j] <= '9') { // the seventh place rounds the sixth; a carry out of the last millionth is a second
                        if (++t.micro == 1000000) {
                            t.micro = 0;
                            const DateTime next = DateTime::from_epoch_seconds(t.epoch_seconds() + 1);
                            t.year = next.year; t.month = next.month; t.day = next.day;
                            t.hour = next.hour; t.minute = next.minute; t.second = next.second;
                        }
                    }
                }
            }
        }
    }
    return t;
}

// '14:07:09', '-01:30:00', '838:59:59', '140709', '1 12:00:00' as seconds (with the sign), nullopt when it is no time.
inline std::optional<long long> parse_time_seconds(const std::string& text, long long* micro = nullptr) {
    std::string s = text;
    const std::size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::nullopt;
    s = s.substr(a);
    if (auto dt = parse_datetime(s); dt && dt->has_time) {
        if (micro) *micro = dt->micro;
        return dt->hour * 3600LL + dt->minute * 60 + dt->second;
    }
    bool negative = false;
    std::size_t i = 0;
    if (s[i] == '-') { negative = true; i++; }
    long long parts[3] = {0, 0, 0};
    std::vector<long long> v;
    long long cur = 0;
    bool any = false;
    for (; i < s.size(); i++) {
        if (s[i] >= '0' && s[i] <= '9') { cur = cur * 10 + (s[i] - '0'); any = true; }
        else if (s[i] == ':') { v.push_back(cur); cur = 0; any = false; }
        else if (s[i] == '.') break;
        else return std::nullopt;
    }
    if (!any && v.empty()) return std::nullopt;
    v.push_back(cur);
    long long carry = 0; // (a seventh place of 5 or more rounds the sixth; all nines carry into the seconds)
    long long ignored_micro = 0;
    if (!micro) micro = &ignored_micro;
    {
        *micro = 0;
        if (i < s.size() && s[i] == '.') {
            std::string frac = s.substr(i + 1, 6);
            frac.resize(6, '0');
            *micro = std::atoll(frac.c_str());
            if (i + 7 < s.size() && s[i + 7] >= '5' && s[i + 7] <= '9' && ++*micro == 1000000) {
                *micro = 0;
                carry = 1;
            }
        }
    }
    if (v.size() == 1) { // HHMMSS
        long long x = v[0];
        parts[0] = x / 10000; parts[1] = x / 100 % 100; parts[2] = x % 100;
    } else if (v.size() == 2) { parts[0] = v[0]; parts[1] = v[1]; }
    else if (v.size() == 3) { parts[0] = v[0]; parts[1] = v[1]; parts[2] = v[2]; }
    else return std::nullopt;
    if (parts[1] > 59 || parts[2] > 59) return std::nullopt;
    const long long total = parts[0] * 3600 + parts[1] * 60 + parts[2] + carry;
    return negative ? -total : total;
}

// A text that is read as a number without being wholly one ('abc', '12abc', '0x10'; blanks are 0) is counted, so what writes can refuse it as MySQL does. A date or
// a time (the value of a DATE / TIME / DATETIME column is text here, and MySQL turns it into a number) is not one.
inline void note_truncated_number(const std::string& text) {
    bool all = false;
    leading_number(text, &all);
    if (!all && text.find_first_not_of(" \t\r\n") != std::string::npos && !parse_datetime(text) && !parse_time_seconds(text)) {
        ++truncated_number_count;
        truncated_number_text = text;
    }
}

// 0 = Monday ... 6 = Sunday (MySQL's WEEKDAY)
inline int weekday_monday0(long long epoch_days) {
    long long w = (epoch_days + 3) % 7; // 1970-01-01 was a Thursday (3)
    if (w < 0) w += 7;
    return static_cast<int>(w);
}

inline const char* kMonthNames[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
inline const char* kDayNames[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"}; // index = WEEKDAY()

// Add `amount` of `unit` to a date(time) the MySQL way: months and years keep the day if it fits and otherwise move to the last day of the month.
inline std::optional<DateTime> add_interval(DateTime t, const std::string& unit, long long amount) {
    if (unit == "MONTH" || unit == "YEAR" || unit == "QUARTER") {
        const long long months = unit == "YEAR" ? amount * 12 : unit == "QUARTER" ? amount * 3 : amount;
        long long total = t.year * 12 + (t.month - 1) + months;
        if (total < 0) return std::nullopt;
        t.year = total / 12;
        t.month = static_cast<int>(total % 12) + 1;
        t.day = std::min(t.day, days_in_month(t.year, t.month));
        if (t.year > 9999) return std::nullopt;
        return t;
    }
    long long seconds = 0, micro = 0;
    if (unit == "DAY") seconds = amount * 86400;
    else if (unit == "WEEK") seconds = amount * 7 * 86400;
    else if (unit == "HOUR") seconds = amount * 3600;
    else if (unit == "MINUTE") seconds = amount * 60;
    else if (unit == "SECOND") seconds = amount;
    else if (unit == "MICROSECOND") { seconds = amount / 1000000; micro = amount % 1000000; }
    else return std::nullopt;
    long long total_micro = t.micro + micro;
    seconds += total_micro >= 1000000 ? 1 : (total_micro < 0 ? -1 : 0);
    total_micro = ((total_micro % 1000000) + 1000000) % 1000000;
    DateTime out = DateTime::from_epoch_seconds(t.epoch_seconds() + seconds, total_micro);
    out.has_date = true;
    out.has_time = t.has_time || unit == "HOUR" || unit == "MINUTE" || unit == "SECOND" || unit == "MICROSECOND";
    if (out.year < 0 || out.year > 9999) return std::nullopt;
    return out;
}

} // namespace engine::sqlval
