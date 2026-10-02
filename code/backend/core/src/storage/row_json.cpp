#include "engine/row_json.hpp"

#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

namespace engine {

namespace {

// Length of the valid UTF-8 sequence starting at s[i] (a lead byte >= 0x80), or 0 if there is none -- exactly the
// sequences nlohmann's serializer accepts (no overlong forms, no surrogates, nothing above U+10FFFF).
std::size_t utf8_sequence_length(const std::string& s, std::size_t i) {
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

} // namespace

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
