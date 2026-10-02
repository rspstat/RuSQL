#include "engine/row_json.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include <nlohmann/json.hpp>

namespace engine {

namespace {

// Length of the valid UTF-8 sequence starting at s[i] (a lead byte >= 0x80), or 0 if there is none -- exactly the
// sequences nlohmann's serializer accepts (no overlong forms, no surrogates, nothing above U+10FFFF).
std::size_t utf8_sequence_length(std::string_view s, std::size_t i) {
    auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    auto cont = [&](std::size_t k, unsigned lo = 0x80, unsigned hi = 0xBF) { return k < s.size() && at(k) >= lo && at(k) <= hi; };
    unsigned char b = at(i);
    if (b >= 0xC2 && b <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (b == 0xE0) return cont(i + 1, 0xA0, 0xBF) && cont(i + 2) ? 3 : 0;
    if (b == 0xED) return cont(i + 1, 0x80, 0x9F) && cont(i + 2) ? 3 : 0;
    if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) return cont(i + 1) && cont(i + 2) ? 3 : 0;
    if (b == 0xF0) return cont(i + 1, 0x90, 0xBF) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    if (b >= 0xF1 && b <= 0xF3) return cont(i + 1) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    if (b == 0xF4) return cont(i + 1, 0x80, 0x8F) && cont(i + 2) && cont(i + 3) ? 4 : 0;
    return 0;
}

void append_json_string(std::string& out, const std::string& s) {
    const std::size_t mark = out.size();
    out.push_back('"');
    std::size_t run = 0; // start of the pending verbatim run
    auto flush = [&](std::size_t upto) { out.append(s, run, upto - run); };
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\' && c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0x80) {
            std::size_t len = utf8_sequence_length(s, i);
            if (len == 0) { // invalid UTF-8: let nlohmann produce its exception (or, should it ever accept it, its text)
                out.resize(mark);
                out += nlohmann::json(s).dump();
                return;
            }
            i += len;
            continue;
        }
        flush(i);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            }
        }
        i++;
        run = i;
    }
    flush(s.size());
    out.push_back('"');
}

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

std::size_t skip_ws(std::string_view s, std::size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool read_hex4(std::string_view s, std::size_t i, unsigned& out) {
    if (i + 4 > s.size()) return false;
    unsigned v = 0;
    for (std::size_t k = 0; k < 4; k++) {
        int d = hex_value(s[i + k]);
        if (d < 0) return false;
        v = v * 16 + static_cast<unsigned>(d);
    }
    out = v;
    return true;
}

void append_utf8(std::string& out, unsigned cp) {
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
}

// The string literal that starts at s[i] == '"': its value is appended to `out`, and the index after the closing quote is
// returned. kNpos = something nlohmann has to judge (a raw control character, invalid UTF-8, an unknown or malformed escape,
// a lone surrogate, no closing quote).
std::size_t read_json_string(std::string_view s, std::size_t i, std::string& out) {
    i++; // opening quote
    while (i < s.size()) {
        std::size_t run = i; // verbatim stretch
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"' || c == '\\' || c < 0x20) break;
            if (c >= 0x80) {
                std::size_t len = utf8_sequence_length(s, i);
                if (len == 0) return kNpos;
                i += len;
            } else {
                i++;
            }
        }
        out.append(s.data() + run, i - run);
        if (i >= s.size()) return kNpos;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') return i + 1;
        if (c < 0x20) return kNpos;
        // backslash escape
        if (i + 1 >= s.size()) return kNpos;
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
                unsigned cp;
                if (!read_hex4(s, i + 2, cp)) return kNpos;
                i += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF) { // needs its low half
                    unsigned lo;
                    if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u' && read_hex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else {
                        return kNpos;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return kNpos;
                }
                append_utf8(out, cp);
                break;
            }
            default: return kNpos;
        }
    }
    return kNpos;
}

// The object that starts at s[i] == '{' -- only strings as values, keys strictly increasing (the order nlohmann's std::map
// iterates and the order this engine writes, so the Row ends up built in the same insertion order as before) -- read into
// `row`; the index after the closing brace, or kNpos.
std::size_t read_flat_object(std::string_view s, std::size_t i, Row& row) {
    i = skip_ws(s, i + 1);
    if (i < s.size() && s[i] == '}') return i + 1;
    std::string prev_key, key, value;
    bool first = true;
    while (true) {
        if (i >= s.size() || s[i] != '"') return kNpos;
        key.clear();
        i = read_json_string(s, i, key);
        if (i == kNpos) return kNpos;
        if (!first && !(prev_key < key)) return kNpos;
        i = skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return kNpos;
        i = skip_ws(s, i + 1);
        if (i >= s.size() || s[i] != '"') return kNpos;
        value.clear();
        i = read_json_string(s, i, value);
        if (i == kNpos) return kNpos;
        prev_key = key;
        row.emplace(std::move(key), std::move(value));
        key = std::string();
        value = std::string();
        first = false;
        i = skip_ws(s, i);
        if (i >= s.size()) return kNpos;
        if (s[i] == '}') return i + 1;
        if (s[i] != ',') return kNpos;
        i = skip_ws(s, i + 1);
    }
}

} // namespace

Row row_from_json(std::string_view text) {
    Row row;
    std::size_t i = skip_ws(text, 0);
    if (i < text.size() && text[i] == '{') {
        i = read_flat_object(text, i, row);
        if (i != kNpos && skip_ws(text, i) == text.size()) return row;
    }
    return nlohmann::json::parse(std::string(text)).get<Row>();
}

std::vector<Row> rows_from_json(std::string_view text) {
    std::vector<Row> rows;
    std::size_t i = skip_ws(text, 0);
    if (i < text.size() && text[i] == '[') {
        i = skip_ws(text, i + 1);
        bool ok = true;
        if (i < text.size() && text[i] == ']') {
            i++;
        } else {
            while (ok) {
                if (i >= text.size() || text[i] != '{') { ok = false; break; }
                Row row;
                i = read_flat_object(text, i, row);
                if (i == kNpos) { ok = false; break; }
                rows.push_back(std::move(row));
                i = skip_ws(text, i);
                if (i >= text.size()) { ok = false; break; }
                if (text[i] == ']') { i++; break; }
                if (text[i] != ',') { ok = false; break; }
                i = skip_ws(text, i + 1);
            }
        }
        if (ok && skip_ws(text, i) == text.size()) return rows;
    }
    return nlohmann::json::parse(std::string(text)).get<std::vector<Row>>();
}

void append_row_json(std::string& out, const Row& row) {
    // nlohmann builds a std::map: keys come out in std::less<std::string> order
    std::array<const Row::value_type*, 24> small;
    std::vector<const Row::value_type*> big;
    const Row::value_type** begin = small.data();
    if (row.size() > small.size()) {
        big.resize(row.size());
        begin = big.data();
    }
    std::size_t n = 0;
    for (auto& kv : row) begin[n++] = &kv;
    std::sort(begin, begin + n, [](const Row::value_type* a, const Row::value_type* b) { return a->first < b->first; });

    out.push_back('{');
    for (std::size_t i = 0; i < n; i++) {
        if (i) out.push_back(',');
        append_json_string(out, begin[i]->first);
        out.push_back(':');
        append_json_string(out, begin[i]->second);
    }
    out.push_back('}');
}

std::string row_to_json(const Row& row) {
    std::string out;
    out.reserve(row.size() * 24 + 8);
    append_row_json(out, row);
    return out;
}

std::string rows_to_json(const std::vector<Row>& rows) {
    std::string out;
    out.push_back('[');
    for (std::size_t i = 0; i < rows.size(); i++) {
        if (i) out.push_back(',');
        append_row_json(out, rows[i]);
    }
    out.push_back(']');
    return out;
}

} // namespace engine
