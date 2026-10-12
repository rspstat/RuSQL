// utf8mb4_0900_ai_ci -- see engine/collation.hpp.

#include "engine/collation.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace engine::collation {

namespace {

// The engine's NULL is the text "NULL" (EXECUTOR_NULL_VALUE). It is not the word: 'null' and 'Null' are values and NULL is none, so NULL is never the same value as a
// text -- in a GROUP BY it is a group of its own, in a UNIQUE column it repeats and 'null' does not collide with it.
bool is_null_text(std::string_view s) { return s.size() == 4 && s == "NULL"; }

struct Run {
    std::uint32_t start;
    std::uint16_t count;
    std::uint8_t kind; // 1: the last weight goes up by one for each following code point, 0: they all have the same weights
    std::uint8_t n;    // number of weights (0: the code point is ignored)
    std::uint32_t pool;
};

#include "collation_data.inc"

constexpr std::size_t kRunCount = sizeof kRuns / sizeof kRuns[0];
constexpr int kRoom = 24; // weights of one character: at most 8 from the table, 6 for a Hangul syllable

// The weights of a code point that has a run: their number, or -1.
int from_runs(std::uint32_t cp, std::uint16_t* out) {
    std::size_t lo = 0, hi = kRunCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kRuns[mid].start <= cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return -1;
    const Run& run = kRuns[lo - 1];
    if (cp >= run.start + run.count) return -1;
    for (int i = 0; i < run.n; i++) out[i] = kPool[run.pool + static_cast<std::size_t>(i)];
    if (run.kind == 1 && run.n > 0) out[run.n - 1] = static_cast<std::uint16_t>(out[run.n - 1] + (cp - run.start));
    return run.n;
}

// An ideograph or a code point nobody has assigned: two weights, the first by its block (UCA "implicit weights").
int implicit_weights(std::uint32_t cp, std::uint16_t* out) {
    std::uint32_t base = 0xFBC0;
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || cp == 0xFA0E || cp == 0xFA0F || cp == 0xFA11 || cp == 0xFA13 || cp == 0xFA14 || cp == 0xFA1F || cp == 0xFA21 ||
        cp == 0xFA23 || cp == 0xFA24 || cp == 0xFA27 || cp == 0xFA28 || cp == 0xFA29) {
        base = 0xFB40;
    } else if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2EBEF)) {
        base = 0xFB80;
    }
    out[0] = static_cast<std::uint16_t>(base + (cp >> 15));
    out[1] = static_cast<std::uint16_t>((cp & 0x7FFF) | 0x8000);
    return 2;
}

// The weights of a code point (into `out`, room for kRoom), and how many.
int weights_of(std::uint32_t cp, std::uint16_t* out) {
    const int n = from_runs(cp, out);
    if (n >= 0) return n;
    if (cp >= 0xAC00 && cp <= 0xD7A3) { // a Hangul syllable weighs what its jamo weigh
        const std::uint32_t s = cp - 0xAC00;
        const std::uint32_t jamo[3] = {0x1100 + s / 588, 0x1161 + (s % 588) / 28, 0x11A7 + s % 28};
        const int count = s % 28 == 0 ? 2 : 3;
        int total = 0;
        for (int k = 0; k < count; k++) {
            const int m = from_runs(jamo[k], out + total);
            if (m > 0) total += m;
        }
        return total;
    }
    return implicit_weights(cp, out);
}

// The code points below 128 have at most one weight: a table, so that the usual text costs a lookup per byte.
struct AsciiTable {
    std::uint8_t n[128];
    std::uint16_t w[128];
    AsciiTable() {
        for (int c = 0; c < 128; c++) {
            std::uint16_t buf[kRoom];
            const int k = c == 0 ? 0 : weights_of(static_cast<std::uint32_t>(c), buf);
            n[c] = static_cast<std::uint8_t>(k > 1 ? 1 : k);
            w[c] = k > 0 ? buf[0] : 0;
        }
    }
};

const AsciiTable& ascii_table() {
    static const AsciiTable table;
    return table;
}

// The code point at `pos` (a byte that is not part of a well-formed UTF-8 sequence counts as U+FFFD), and the position after it.
std::uint32_t decode(std::string_view s, std::size_t& pos) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) {
        pos++;
        return c;
    }
    std::size_t len;
    std::uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07u;
    } else {
        pos++;
        return 0xFFFD;
    }
    if (pos + len > s.size()) {
        pos++;
        return 0xFFFD;
    }
    for (std::size_t k = 1; k < len; k++) {
        const unsigned char d = static_cast<unsigned char>(s[pos + k]);
        if ((d & 0xC0) != 0x80) {
            pos++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (d & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
        pos++;
        return 0xFFFD;
    }
    pos += len;
    return cp;
}

// The weights of a text, one after the other.
class Cursor {
public:
    explicit Cursor(std::string_view s) : s_(s), table_(ascii_table()) {}

    bool next(std::uint16_t& w) {
        for (;;) {
            if (i_ < n_) {
                w = buf_[i_++];
                return true;
            }
            if (pos_ >= s_.size()) return false;
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c < 0x80) {
                pos_++;
                if (table_.n[c]) {
                    w = table_.w[c];
                    return true;
                }
                continue; // (a control character: ignored)
            }
            n_ = weights_of(decode(s_, pos_), buf_);
            i_ = 0;
        }
    }

private:
    std::string_view s_;
    const AsciiTable& table_;
    std::size_t pos_ = 0;
    std::uint16_t buf_[kRoom];
    int n_ = 0, i_ = 0;
};

// weight -> the lower case ASCII letter / digit / punctuation mark that has it (for key())
const std::vector<std::pair<std::uint16_t, char>>& ascii_of_weight() {
    static const std::vector<std::pair<std::uint16_t, char>> table = [] {
        std::vector<std::pair<std::uint16_t, char>> t;
        const AsciiTable& a = ascii_table();
        for (int c = 0x20; c <= 0x7E; c++) {
            if (c >= 'A' && c <= 'Z') continue; // (the lower case letter has the same weight)
            t.emplace_back(a.w[c], static_cast<char>(c));
        }
        std::sort(t.begin(), t.end());
        return t;
    }();
    return table;
}

bool same_weights(std::uint32_t a, std::uint32_t b) {
    if (a == b) return true;
    std::uint16_t wa[kRoom], wb[kRoom];
    const int na = weights_of(a, wa), nb = weights_of(b, wb);
    return na == nb && std::equal(wa, wa + na, wb);
}

bool all_ascii(std::string_view s) {
    for (unsigned char c : s) {
        if (c >= 0x80) return false;
    }
    return true;
}

// like() for two texts without a byte above 127 (what a WHERE ... LIKE 'abc%' reads, row after row): the same walk over bytes, no allocation
bool like_ascii(std::string_view v, std::string_view p) {
    const AsciiTable& t = ascii_table();
    auto same = [&t](unsigned char a, unsigned char b) { return a == b || (t.n[a] == t.n[b] && (t.n[a] == 0 || t.w[a] == t.w[b])); };
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    std::size_t vi = 0, pi = 0, star = none, mark = 0;
    while (vi < v.size()) {
        if (pi < p.size() && p[pi] == '%') {
            star = pi++;
            mark = vi;
        } else if (pi < p.size() && (p[pi] == '_' || same(static_cast<unsigned char>(p[pi]), static_cast<unsigned char>(v[vi])))) {
            pi++;
            vi++;
        } else if (star != none) {
            pi = star + 1;
            vi = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '%') pi++;
    return pi == p.size();
}

std::vector<std::uint32_t> code_points(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) out.push_back(decode(s, pos));
    return out;
}

} // namespace

int compare(std::string_view a, std::string_view b) {
    if (is_null_text(a) || is_null_text(b)) return is_null_text(a) ? (is_null_text(b) ? 0 : -1) : 1; // (NULL first)
    Cursor ca(a), cb(b);
    for (;;) {
        std::uint16_t wa = 0, wb = 0;
        const bool ha = ca.next(wa), hb = cb.next(wb);
        if (!ha || !hb) return ha == hb ? 0 : (ha ? 1 : -1);
        if (wa != wb) return wa < wb ? -1 : 1;
    }
}

bool equal(std::string_view a, std::string_view b) { return a == b || compare(a, b) == 0; }

std::string sort_key(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    Cursor c(s);
    std::uint16_t w;
    while (c.next(w)) {
        out.push_back(static_cast<char>(w >> 8));
        out.push_back(static_cast<char>(w & 0xFF));
    }
    return out;
}

std::string key(std::string_view s) {
    if (is_null_text(s)) return "\x02N"; // (the key of a text is printable or starts with \x01)
    if (std::all_of(s.begin(), s.end(), [](char ch) { return ch >= 0x20 && ch <= 0x7E; })) {
        std::string out(s);
        for (char& ch : out) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        return out;
    }
    std::vector<std::uint16_t> weights;
    Cursor c(s);
    std::uint16_t w;
    while (c.next(w)) weights.push_back(w);
    const auto& table = ascii_of_weight();
    std::string plain;
    plain.reserve(weights.size());
    for (std::uint16_t weight : weights) {
        auto it = std::lower_bound(table.begin(), table.end(), std::make_pair(weight, '\0'));
        if (it == table.end() || it->first != weight) {
            plain.clear();
            break;
        }
        plain.push_back(it->second);
    }
    if (plain.size() == weights.size()) return plain; // (every weight is an ASCII character's: 'é' and 'e' have one key)
    std::string out = "\x01";
    out.reserve(1 + weights.size() * 2);
    for (std::uint16_t weight : weights) {
        out.push_back(static_cast<char>(weight >> 8));
        out.push_back(static_cast<char>(weight & 0xFF));
    }
    return out;
}

bool like(std::string_view value, std::string_view pattern) {
    if (is_null_text(value)) return false; // (NULL LIKE anything is not true)
    if (all_ascii(value) && all_ascii(pattern)) return like_ascii(value, pattern);
    const std::vector<std::uint32_t> v = code_points(value), p = code_points(pattern);
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    std::size_t vi = 0, pi = 0, star = none, mark = 0;
    while (vi < v.size()) {
        if (pi < p.size() && p[pi] == '%') {
            star = pi++;
            mark = vi;
        } else if (pi < p.size() && (p[pi] == '_' || same_weights(p[pi], v[vi]))) {
            pi++;
            vi++;
        } else if (star != none) {
            pi = star + 1;
            vi = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '%') pi++;
    return pi == p.size();
}

} // namespace engine::collation
