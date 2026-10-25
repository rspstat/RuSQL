#pragma once

// `x LIKE pattern ESCAPE 'c'`: the matcher (collation::like) knows one escape character, the backslash. The parser rewrites a pattern written as a string once, and the function
// __LIKE_ESCAPE(pattern, 'c') rewrites a pattern that comes from a column or an expression, row by row; both with the function below.
//   c + x  ->  \x   (x is any character: `\%`, `\_`, and the escape character itself stand for the characters themselves)
//   a backslash that is no escape character  ->  \\   (with ESCAPE '!' or ESCAPE '' it is an ordinary character)
//   an escape character that ends the pattern  ->  \c   (it stands for itself, as in MySQL)
// `escape` is one character (any UTF-8 character) or empty (no escape character at all).

#include <algorithm>
#include <string>

namespace engine {

inline std::size_t like_utf8_length(const std::string& s, std::size_t at) {
    const unsigned char lead = static_cast<unsigned char>(s[at]);
    const std::size_t n = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return std::min(n, s.size() - at);
}

inline std::string like_pattern_with_escape(const std::string& pattern, const std::string& escape) {
    std::string rewritten;
    for (std::size_t i = 0; i < pattern.size();) {
        const std::size_t n = like_utf8_length(pattern, i);
        const std::string ch = pattern.substr(i, n);
        if (!escape.empty() && ch == escape) {
            const std::size_t m = i + n < pattern.size() ? like_utf8_length(pattern, i + n) : 0;
            rewritten += '\\';
            rewritten += m ? pattern.substr(i + n, m) : ch;
            i += n + m;
        } else {
            if (ch == "\\") rewritten += '\\';
            rewritten += ch;
            i += n;
        }
    }
    return rewritten;
}

} // namespace engine
