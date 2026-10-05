#pragma once

// What a string is worth as a number, and exact arithmetic on the numbers a table holds.
//
// Column values are text. In arithmetic (and in SUM / AVG) MySQL reads a string by the number it starts with: "12abc" is 12, " 5" is 5,
// "x" and "" are 0, and `+` adds -- it never joins strings. Integers are added, subtracted and multiplied as integers (a BIGINT near
// 2^63 is exact), and a sum of decimals is exact (0.1 ten times is 1, not 0.9999999999999999), so a HAVING or WHERE on the result
// finds what the decimal text says.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// The length of the number `s` starts with: an optional sign, digits with an optional fraction ("12", "12.5", "12.", ".5") and an
// optional exponent ("1e3", "1E-2"); 0 when it does not start with a number.
inline std::size_t number_length(std::string_view s) {
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    std::size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++, digits++;
    if (i < s.size() && s[i] == '.') {
        std::size_t j = i + 1;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++, digits++;
        i = j;
    }
    if (digits == 0) return 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) j++;
        const std::size_t exponent_start = j;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
        if (j > exponent_start) i = j;
    }
    return i;
}

// The value of text that is exactly one number (see number_length); 0 for one that is out of the range of a double.
inline double number_value(std::string_view number) {
    if (!number.empty() && number.front() == '+') number.remove_prefix(1); // from_chars does not read a plus sign
    double value = 0.0;
    auto result = std::from_chars(number.data(), number.data() + number.size(), value);
    return result.ec == std::errc() ? value : 0.0;
}

// A number the way arithmetic reads a string: white space before it is skipped, what follows it is ignored, no number at the start is 0.
inline double text_to_number(std::string_view s) {
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || (s[start] >= '\t' && s[start] <= '\r'))) start++;
    s.remove_prefix(start);
    return number_value(s.substr(0, number_length(s)));
}

// The value of a string that is nothing but an integer in the range of int64 ("12", "-5", "+7").
inline std::optional<std::int64_t> parse_int64_text(std::string_view s) {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    std::int64_t value = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec != std::errc() || result.ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

// Which of two numbers (texts that are exactly one number) is bigger: -1, 0 or 1. Integers are compared as integers, so 9007199254740993
// and 9007199254740992 differ; anything else as doubles.
inline int compare_numbers(std::string_view a, std::string_view b) {
    auto x = parse_int64_text(a), y = parse_int64_text(b);
    if (x && y) return *x < *y ? -1 : (*x > *y ? 1 : 0);
    const double u = number_value(a), v = number_value(b);
    return u < v ? -1 : (u > v ? 1 : 0);
}

// `a op b` for the integers a and b ('+', '-' or '*') as an integer; nullopt when it overflows int64.
inline std::optional<std::int64_t> int64_arith(char op, std::int64_t a, std::int64_t b) {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max(), min = std::numeric_limits<std::int64_t>::min();
    switch (op) {
    case '+':
        if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return std::nullopt;
        return a + b;
    case '-':
        if ((b < 0 && a > max + b) || (b > 0 && a < min + b)) return std::nullopt;
        return a - b;
    default:
        if (a == 0 || b == 0) return 0;
        if ((a == -1 && b == min) || (b == -1 && a == min)) return std::nullopt;
        if (a > 0 ? (b > 0 ? a > max / b : b < min / a) : (b > 0 ? a < min / b : a < max / b)) return std::nullopt;
        return a * b;
    }
}

// A sum of decimal numbers ("12", "-0.5", "100.10") kept as an integer count of 10^-scale, `scale` being the most decimal places any
// value has. `exact` turns false when a value is not a plain decimal (text, an exponent) or the sum no longer fits in an int64; the
// caller then adds up doubles instead.
struct DecimalSum {
    std::int64_t units = 0;
    int scale = 0;
    std::size_t count = 0; // the values added
    bool exact = true;

    static std::int64_t pow10(int n) {
        std::int64_t p = 1;
        while (n-- > 0) p *= 10;
        return p;
    }

    // `units` rescaled to `to` places (more than `scale`); false when that overflows.
    bool widen(int to) {
        if (to == scale) return true;
        auto widened = int64_arith('*', units, pow10(to - scale));
        if (!widened) return false;
        units = *widened;
        scale = to;
        return true;
    }

    void add(std::string_view text) {
        count++;
        if (!exact) return;
        std::size_t i = 0;
        bool negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
        std::int64_t mantissa = 0;
        int places = 0, digits = 0;
        bool after_point = false;
        for (; i < text.size(); i++) {
            if (text[i] == '.' && !after_point) {
                after_point = true;
                continue;
            }
            if (text[i] < '0' || text[i] > '9' || ++digits > 18) {
                exact = false;
                return;
            }
            mantissa = mantissa * 10 + (text[i] - '0');
            if (after_point) places++;
        }
        if (digits == 0) {
            exact = false;
            return;
        }
        while (places > 0 && mantissa % 10 == 0) mantissa /= 10, places--; // "100.10" is 100.1: the scale stays as small as it can
        if (negative) mantissa = -mantissa;
        if (places > scale && !widen(places)) {
            exact = false;
            return;
        }
        if (places < scale) {
            auto scaled = int64_arith('*', mantissa, pow10(scale - places));
            if (!scaled) {
                exact = false;
                return;
            }
            mantissa = *scaled;
        }
        auto total = int64_arith('+', units, mantissa);
        if (!total) {
            exact = false;
            return;
        }
        units = *total;
    }

    // 10 units of 10^-1 are 1 unit of 10^0: the scale of a sum is the fewest places it needs ("1.0" is "1")
    void normalize() {
        while (scale > 0 && units % 10 == 0) units /= 10, scale--;
    }

    static std::uint64_t magnitude(std::int64_t v) { return v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v); }

    // The sum as decimal text ("12", "100.1", "-0.5").
    std::string text() const {
        std::string digits = std::to_string(magnitude(units));
        if (scale > 0) {
            if (digits.size() <= static_cast<std::size_t>(scale)) digits.insert(0, scale - digits.size() + 1, '0');
            digits.insert(digits.size() - scale, ".");
        }
        return (units < 0 ? "-" : "") + digits;
    }

    // The sum divided by `divisor` (a count of values), rounded half away from zero to 4 decimal places, as decimal text with all 4
    // places ("1.6667"); nullopt when the arithmetic does not fit in an int64.
    std::optional<std::string> average_text(std::int64_t divisor) const {
        // result in units of 10^-4: units * 10^(4 - scale) / divisor
        std::int64_t numerator = units, denominator = divisor;
        if (scale <= 4) {
            auto scaled = int64_arith('*', numerator, pow10(4 - scale));
            if (!scaled) return std::nullopt;
            numerator = *scaled;
        } else {
            auto scaled = int64_arith('*', denominator, pow10(scale - 4));
            if (!scaled) return std::nullopt;
            denominator = *scaled;
        }
        std::int64_t quotient = numerator / denominator, remainder = numerator % denominator;
        const std::int64_t abs_remainder = remainder < 0 ? -remainder : remainder;
        if (abs_remainder >= denominator - abs_remainder) quotient += numerator < 0 ? -1 : 1;
        std::string digits = std::to_string(magnitude(quotient));
        if (digits.size() <= 4) digits.insert(0, 5 - digits.size(), '0');
        digits.insert(digits.size() - 4, ".");
        return (quotient < 0 ? "-" : "") + digits;
    }
};

// `v` with `places` decimal places.
inline std::string format_places(double v, int places) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", places, v);
    return buf;
}

// The shortest text that reads back as `v`; a whole number as an integer.
inline std::string format_shortest(double v) {
    if (v == std::trunc(v) && std::abs(v) < 1e15) return std::to_string(static_cast<long long>(v));
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, res.ptr);
}

// SUM of the values of an aggregate's argument (the texts of the rows where it is not NULL), each read by the number it starts with
// ('12abc' is 12, 'x' is 0). Integers and decimals are added exactly (0.1 ten times is 1); anything else (an exponent, a sum beyond
// int64) as doubles. No value at all -- an empty group, only NULLs -- has no sum: nullopt. `display` is the form a result column
// shows (an integer, else 4 places), otherwise the exact text a HAVING or an expression goes on with.
inline std::optional<std::string> sum_of_texts(const std::vector<const std::string*>& values, bool display) {
    if (values.empty()) return std::nullopt;
    DecimalSum sum;
    for (const std::string* v : values) sum.add(*v);
    sum.normalize();
    if (sum.exact) return display && sum.scale > 0 ? format_places(number_value(sum.text()), 4) : sum.text();
    double total = 0.0;
    for (const std::string* v : values) total += text_to_number(*v);
    if (display && total != std::trunc(total)) return format_places(total, 4);
    return format_shortest(total);
}

// AVG: the exact quotient rounded to 4 places -- what a result column shows -- so that a HAVING or an expression goes on with the same
// value (AVG(v) * 3 over 1, 2, 2 is 1.6667 * 3, as in MySQL). nullopt for no value.
inline std::optional<std::string> average_of_texts(const std::vector<const std::string*>& values) {
    if (values.empty()) return std::nullopt;
    DecimalSum sum;
    for (const std::string* v : values) sum.add(*v);
    if (sum.exact) {
        if (auto text = sum.average_text(static_cast<std::int64_t>(values.size()))) return text;
    }
    double total = 0.0;
    for (const std::string* v : values) total += text_to_number(*v);
    return format_places(total / static_cast<double>(values.size()), 4);
}

} // namespace engine
