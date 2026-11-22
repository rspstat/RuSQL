#pragma once

// std::from_chars for a double. MSVC (Windows) and libstdc++ (Linux) have it; libc++, the C++ library of Apple's compiler, only in its newest versions. Where it is missing this reads
// the text with strtod after checking it against from_chars' own grammar -- no leading white space or plus sign, no hexadecimal float, no inf / nan -- so that both give the same answers.

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <system_error>

namespace engine {

inline std::from_chars_result from_chars_double(const char* first, const char* last, double& value) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    return std::from_chars(first, last, value);
#else
    const char* p = first;
    if (p != last && *p == '-') ++p;
    const char* int_begin = p;
    while (p != last && *p >= '0' && *p <= '9') ++p;
    const std::size_t int_digits = static_cast<std::size_t>(p - int_begin);
    std::size_t fraction_digits = 0;
    if (p != last && *p == '.') {
        const char* f = p + 1;
        while (f != last && *f >= '0' && *f <= '9') ++f;
        fraction_digits = static_cast<std::size_t>(f - (p + 1));
        if (int_digits > 0 || fraction_digits > 0) p = f; // ("5." and ".5" are numbers, "." is not)
    }
    if (int_digits == 0 && fraction_digits == 0) return {first, std::errc::invalid_argument};
    if (p != last && (*p == 'e' || *p == 'E')) { // (an exponent counts only when it has digits: "1e" reads as 1)
        const char* e = p + 1;
        if (e != last && (*e == '+' || *e == '-')) ++e;
        const char* exponent_digits = e;
        while (e != last && *e >= '0' && *e <= '9') ++e;
        if (e != exponent_digits) p = e;
    }
    const std::string text(first, p);
    errno = 0;
    const double parsed = std::strtod(text.c_str(), nullptr);
    if (errno == ERANGE) return {p, std::errc::result_out_of_range}; // (`value` stays as it was, as with from_chars)
    value = parsed;
    return {p, std::errc()};
#endif
}

} // namespace engine
