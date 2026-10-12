#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "catch.hpp"
#include "engine/collation.hpp"

using namespace engine;

// The default collation of MySQL 8.4 (utf8mb4_0900_ai_ci): case and accents do not count, a space does, texts are ordered by the weights of the Unicode
// Collation Algorithm. The pairs below come from a real MySQL (code/test/collation/gen_collation_cases.py): texts of many scripts, with and without case, accents,
// spaces and punctuation, and the answer of its STRCMP for each.

namespace {
struct Pair {
    const char* a;
    const char* b;
    int expected;
};

const Pair kPairs[] = {
#include "collation_cases.inc"
};

int sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }
} // namespace

TEST_CASE("texts compare as MySQL's default collation compares them", "[collation]") {
    std::size_t equal_pairs = 0;
    for (const Pair& p : kPairs) {
        INFO("a = [" << p.a << "] b = [" << p.b << "] expected " << p.expected);
        REQUIRE(sign(collation::compare(p.a, p.b)) == p.expected);
        REQUIRE(sign(collation::compare(p.b, p.a)) == -p.expected);
        REQUIRE(collation::equal(p.a, p.b) == (p.expected == 0));
        equal_pairs += p.expected == 0;
    }
    REQUIRE(equal_pairs > 100); // (the pairs include plenty of texts that differ only in what the collation ignores)
}

TEST_CASE("the key of a text is the same for texts that are equal and for no others", "[collation]") {
    for (const Pair& p : kPairs) {
        INFO("a = [" << p.a << "] b = [" << p.b << "] expected " << p.expected);
        REQUIRE((collation::key(p.a) == collation::key(p.b)) == (p.expected == 0));
    }
}

TEST_CASE("the sort key keeps the order", "[collation]") {
    for (const Pair& p : kPairs) {
        INFO("a = [" << p.a << "] b = [" << p.b << "] expected " << p.expected);
        const std::string ka = collation::sort_key(p.a), kb = collation::sort_key(p.b);
        REQUIRE(sign(ka.compare(kb)) == p.expected);
    }
}

TEST_CASE("LIKE follows the collation and keeps % and _", "[collation]") {
    // case and accents
    REQUIRE(collation::like("Apple", "a%"));
    REQUIRE(collation::like("Apple", "APPLE"));
    REQUIRE(collation::like("r\303\251sum\303\251", "RESUME"));
    REQUIRE(collation::like("r\303\251sum\303\251", "r_sum_"));
    REQUIRE_FALSE(collation::like("r\303\251sum\303\251", "r_sum"));
    // wildcards
    REQUIRE(collation::like("abc", "%"));
    REQUIRE(collation::like("", "%"));
    REQUIRE_FALSE(collation::like("", "_"));
    REQUIRE(collation::like("abcabc", "%b%c"));
    REQUIRE(collation::like("abcabxd", "%b_d"));
    REQUIRE_FALSE(collation::like("abcabd", "%b_d")); // (the last b is followed by d only)
    REQUIRE_FALSE(collation::like("abcabxd", "%b_e"));
    REQUIRE(collation::like("a", "a%%"));
    REQUIRE_FALSE(collation::like("ab", "a"));
    REQUIRE_FALSE(collation::like("a ", "a")); // (a trailing space counts)
    // one character is one code point, whatever its bytes
    REQUIRE(collation::like("\355\225\234\352\270\200", "__"));
    REQUIRE(collation::like("\355\225\234\352\270\200", "\355\225\234_"));
    REQUIRE_FALSE(collation::like("\355\225\234\352\270\200", "___"));
}

TEST_CASE("a plain ASCII text has its lower case as the key", "[collation]") {
    REQUIRE(collation::key("Hello, World 42") == "hello, world 42");
    REQUIRE(collation::key("") == "");
    REQUIRE(collation::key("\x01") == "");     // (a control character is ignored)
    REQUIRE(collation::key("\303\211") == "e");       // (the same key as 'e')
    REQUIRE(collation::key("\303\237") == "ss");
    REQUIRE(collation::key("a\xFF") == collation::key("a\xFE")); // (bytes that are not UTF-8 are all U+FFFD)
}

TEST_CASE("LIKE compares one code point to one code point, by their weights (answers of a real MySQL 8.4)", "[collation]") {
    // a character with several weights ('ß' is s + s) is not the letters it expands to, but it is one character for '_'
    REQUIRE_FALSE(collation::like("\303\237", "s"));
    REQUIRE_FALSE(collation::like("\303\237", "ss"));
    REQUIRE_FALSE(collation::like("ss", "\303\237"));
    REQUIRE_FALSE(collation::like("\303\237", "s%"));
    REQUIRE(collation::like("\303\237", "_"));
    REQUIRE_FALSE(collation::like("\303\237", "__"));
    REQUIRE(collation::like("ss", "s_"));
    REQUIRE_FALSE(collation::like("\303\246", "a"));     // 'æ'
    REQUIRE_FALSE(collation::like("\303\246", "ae"));
    REQUIRE_FALSE(collation::like("ae", "\303\246"));
    REQUIRE(collation::like("\303\246", "\303\246"));
    REQUIRE(collation::like("\303\206", "\303\246"));    // 'Æ' and 'æ'
    REQUIRE_FALSE(collation::like("\307\206", "dz"));    // 'ǆ'
    REQUIRE_FALSE(collation::like("\307\206", "d%"));
    REQUIRE_FALSE(collation::like("\307\206", "d\305\276")); // 'dž'
    REQUIRE_FALSE(collation::like("\304\263", "ij"));    // 'ĳ'
    REQUIRE_FALSE(collation::like("\304\263", "i%"));
    REQUIRE(collation::like("e", "\303\251"));
    REQUIRE_FALSE(collation::like("\352\260\200", "\341\204\200")); // a Hangul syllable is not its first jamo
    REQUIRE(collation::like("\352\260\200", "_"));
}

namespace {
void append_utf8(std::string& out, std::uint32_t cp) {
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
} // namespace

TEST_CASE("bytes that are not UTF-8 count as U+FFFD, one for each byte", "[collation]") {
    const std::string bad = "\357\277\275"; // U+FFFD
    REQUIRE(collation::equal("\xC0\x80", bad + bad));                          // 0xC0 and 0xC1 never start a sequence
    REQUIRE(collation::equal("\xE0\x80\x80", bad + bad + bad));                // an overlong form of U+0000
    REQUIRE(collation::equal("\xF4\x90\x80\x80", bad + bad + bad + bad));      // beyond U+10FFFF
    REQUIRE(collation::equal("\xF0\x80\x80\x80", bad + bad + bad + bad));      // an overlong 4-byte form
    REQUIRE(collation::equal("\xC3" "a", bad + "a"));                          // a lead byte whose next byte is not a continuation byte
    REQUIRE(collation::equal("a\xE2\x82", "a" + bad + bad));                   // a sequence cut short by the end of the text
    const std::string euro = "\342\202\254";                                   // a text is not NUL terminated and may be a part of a longer buffer
    REQUIRE(collation::equal(std::string_view(euro).substr(0, 2), bad + bad));
    REQUIRE_FALSE(collation::equal("\xC3\xA9", bad));                          // (a well-formed sequence is itself)
}

TEST_CASE("every code point has one key, shared exactly by the texts that compare equal", "[collation]") {
    // every code point of the table's range, and every text of two printable ASCII characters (a key is the text itself when all its weights are an ASCII
    // character's, and a prefix and the weights otherwise: the two kinds never meet)
    std::vector<std::string> texts;
    for (std::uint32_t cp = 1; cp <= 0x1FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        texts.emplace_back();
        append_utf8(texts.back(), cp);
    }
    for (std::uint32_t cp : {0x2F800u, 0x2F900u, 0x2FA00u, 0x2FA1Du, 0x2FA1Eu, 0xE0000u, 0xE0001u, 0xE0020u, 0xE007Fu, 0xE0100u, 0xE01EFu, 0xE01F0u}) { // (beyond the dump's first plane)
        texts.emplace_back();
        append_utf8(texts.back(), cp);
    }
    for (char a = 0x20; a <= 0x7E; a++) {
        for (char b = 0x20; b <= 0x7E; b++) texts.push_back(std::string{a, b});
    }
    std::stable_sort(texts.begin(), texts.end(), [](const std::string& x, const std::string& y) { return collation::compare(x, y) < 0; });
    std::map<std::string, std::string> owner; // key -> the first text that has it
    std::string previous_key, previous_sort_key;
    for (std::size_t i = 0; i < texts.size(); i++) {
        const std::string key = collation::key(texts[i]), sort_key = collation::sort_key(texts[i]);
        auto [it, inserted] = owner.emplace(key, texts[i]);
        if (!inserted && collation::compare(it->second, texts[i]) != 0) FAIL("two texts that differ share the key " << key.size() << " bytes: " << i);
        if (i > 0) {
            const bool same = collation::compare(texts[i - 1], texts[i]) == 0;
            if (same != (key == previous_key)) FAIL("key and compare disagree at " << i);
            if (same != (sort_key == previous_sort_key) || (!same && sort_key < previous_sort_key)) FAIL("sort key and compare disagree at " << i);
        }
        previous_key = key;
        previous_sort_key = sort_key;
    }
}

TEST_CASE("the weights of every code point are the ones MySQL gives (RUSQL_COLLATION_WEIGHTS=<weights.json from dump_weights.py>)", "[collation][weights]") {
    // Not part of a normal run (the dump is 24 MB and needs a MySQL to make): point the variable at the file and every code point of Unicode is compared with
    // HEX(WEIGHT_STRING(char)) -- the sort key of a single character is exactly its weights, two bytes each.
    const char* path = std::getenv("RUSQL_COLLATION_WEIGHTS");
    if (!path) {
        SUCCEED("RUSQL_COLLATION_WEIGHTS is not set");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t checked = 0, pos = 0;
    while ((pos = json.find('"', pos)) != std::string::npos) {
        const std::size_t end_cp = json.find('"', pos + 1);
        const std::size_t start_hex = json.find('"', end_cp + 1);
        const std::size_t end_hex = json.find('"', start_hex + 1);
        REQUIRE(end_hex != std::string::npos);
        const std::uint32_t cp = static_cast<std::uint32_t>(std::stoul(json.substr(pos + 1, end_cp - pos - 1)));
        const std::string hex = json.substr(start_hex + 1, end_hex - start_hex - 1);
        std::string text;
        append_utf8(text, cp);
        std::string got;
        for (unsigned char byte : collation::sort_key(text)) {
            static const char digits[] = "0123456789ABCDEF";
            got.push_back(digits[byte >> 4]);
            got.push_back(digits[byte & 15]);
        }
        if (got != hex) FAIL("U+" << std::hex << cp << ": MySQL " << hex << ", here " << got);
        checked++;
        pos = end_hex + 1;
    }
    REQUIRE(checked > 1000000);
}

namespace {
// LIKE as the definition says it, one character against one character
bool reference_like(const std::string& v, std::size_t vi, const std::string& p, std::size_t pi) {
    if (pi == p.size()) return vi == v.size();
    if (p[pi] == '%') {
        for (std::size_t k = vi; k <= v.size(); k++) {
            if (reference_like(v, k, p, pi + 1)) return true;
        }
        return false;
    }
    if (vi == v.size()) return false;
    if (p[pi] == '_' || collation::equal(std::string(1, p[pi]), std::string(1, v[vi]))) return reference_like(v, vi + 1, p, pi + 1);
    return false;
}
} // namespace

TEST_CASE("LIKE on ASCII texts (the fast path) is the definition of LIKE", "[collation][random]") {
    // letters of both cases, a space, the wildcards, a tab and two control characters (the collation ignores those)
    const std::string alphabet = "aAbB _%\t\x01\x02";
    std::uint32_t state = 12345;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int round = 0; round < 20000; round++) {
        std::string value, pattern;
        for (std::uint32_t n = next() % 7; n > 0; n--) value.push_back(alphabet[next() % alphabet.size()]);
        for (std::uint32_t n = next() % 7; n > 0; n--) pattern.push_back(alphabet[next() % alphabet.size()]);
        INFO("value [" << value << "] pattern [" << pattern << "]");
        REQUIRE(collation::like(value, pattern) == reference_like(value, 0, pattern, 0));
    }
}
