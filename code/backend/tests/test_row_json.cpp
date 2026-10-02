#include <optional>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "catch.hpp"
#include "engine/row_json.hpp"

using namespace engine;

// row_to_json must be nlohmann::json(row).dump() byte for byte -- the stored row images are compared, hashed and parsed
// back by code written against that exact text -- including throwing when nlohmann throws (invalid UTF-8).
namespace {
std::string random_text(std::mt19937& rng, bool allow_invalid) {
    static const char* pieces[] = {
        "a", "Z", "0", " ", "~", "!", "\"", "\\", "/", "\x7f",                                        // ASCII, quote, backslash, DEL
        "\b", "\f", "\n", "\r", "\t", "\x01", "\x1f", "\x0b", "",                                     // control characters (index 18 = NUL, built below)
        "\xc3\xa9", "\xea\xb0\x80", "\xed\x95\x9c\xea\xb8\x80", "\xf0\x9f\x98\x80", "\xef\xbf\xbf",   // valid: 2, 3, 3x2, 4 bytes, U+FFFF
        "\xed\x9f\xbf", "\xee\x80\x80", "\xf4\x8f\xbf\xbf", "\xc2\x80", "\xe0\xa0\x80",               // valid boundary code points
        "\xe2\x80\xa8", "\xe2\x80\xa9"                                                               // U+2028 / U+2029 (not escaped)
    };
    static const char* bad[] = {"\x80", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff", "\xc3", "\xe2\x82", "\xf0\x9f\x98",
                                "\xf5\x80\x80\x80", "\xc1\xbf"};
    std::string s;
    std::size_t n = rng() % 12;
    for (std::size_t i = 0; i < n; i++) {
        if (allow_invalid && rng() % 25 == 0) {
            s += bad[rng() % (sizeof bad / sizeof *bad)];
        } else if (rng() % 3 == 0) {
            s += std::string(1, static_cast<char>('a' + rng() % 26));
        } else {
            std::size_t k = rng() % (sizeof pieces / sizeof *pieces);
            // the NUL piece is built from an empty C string above; handle it explicitly
            s += (k == 18) ? std::string(1, '\0') : std::string(pieces[k]);
        }
    }
    return s;
}

bool dumps(const Row& row, std::string& out) {
    try {
        out = nlohmann::json(row).dump();
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}
bool mine(const Row& row, std::string& out) {
    try {
        out = row_to_json(row);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}
} // namespace

TEST_CASE("row_to_json is byte-identical to nlohmann::json(row).dump()", "[row_json]") {
    std::mt19937 rng(2026);
    int thrown = 0;
    for (int iter = 0; iter < 60000; iter++) {
        Row row;
        std::size_t n = rng() % (iter % 50 == 0 ? 60 : 8); // some rows wider than the sort's stack buffer
        for (std::size_t i = 0; i < n; i++) row[random_text(rng, false) + std::to_string(rng() % 4)] = random_text(rng, true);
        std::string a, b;
        bool a_ok = dumps(row, a), b_ok = mine(row, b);
        INFO("iteration " << iter << ", " << row.size() << " entries");
        REQUIRE(a_ok == b_ok);
        if (a_ok) {
            REQUIRE(a == b);
        } else {
            thrown++;
        }
    }
    REQUIRE(thrown > 100); // the invalid-UTF-8 branch really was exercised
}

TEST_CASE("row_to_json / rows_to_json: empty and ordinary shapes", "[row_json]") {
    REQUIRE(row_to_json(Row{}) == "{}");
    REQUIRE(rows_to_json({}) == "[]");
    Row r{{"id", "7"}, {"name", "Alice \"A\" \\ \xea\xb0\x80"}, {"_xmax", "0"}, {"_xmin", "12"}, {"", ""}};
    REQUIRE(row_to_json(r) == nlohmann::json(r).dump());
    std::vector<Row> rows{r, Row{}, Row{{"k", "v"}}};
    REQUIRE(rows_to_json(rows) == nlohmann::json(rows).dump());
}

// ---------------------------------------------------------------------------------------------------------------------
// row_from_json / rows_from_json must give what nlohmann::json::parse(text).get<Row>() gives -- or throw when it throws.

namespace {
// outcome of the reference: nullopt = it threw
std::optional<Row> reference_row(const std::string& text) {
    try {
        return nlohmann::json::parse(text).get<Row>();
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}
std::optional<Row> mine_row(const std::string& text) {
    try {
        return row_from_json(text);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}
std::optional<std::vector<Row>> reference_rows(const std::string& text) {
    try {
        return nlohmann::json::parse(text).get<std::vector<Row>>();
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}
std::optional<std::vector<Row>> mine_rows(const std::string& text) {
    try {
        return rows_from_json(text);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}
// the order a Row iterates in depends on the order its entries went in: the same insertion order must give the same order
std::vector<std::pair<std::string, std::string>> iteration_order(const Row& r) {
    return {r.begin(), r.end()};
}
} // namespace

TEST_CASE("row_from_json round-trips what row_to_json writes, entry by entry in the same order", "[row_json]") {
    std::mt19937 rng(515);
    for (int iter = 0; iter < 40000; iter++) {
        Row row;
        std::size_t n = rng() % (iter % 40 == 0 ? 50 : 8);
        for (std::size_t i = 0; i < n; i++) row[random_text(rng, false) + std::to_string(rng() % 4)] = random_text(rng, false);
        std::string text = row_to_json(row);
        INFO("iteration " << iter << ": " << text);
        Row back = row_from_json(text);
        REQUIRE(back == row);
        auto ref = reference_row(text);
        REQUIRE(ref.has_value());
        REQUIRE(iteration_order(back) == iteration_order(*ref));
    }
}

TEST_CASE("row_from_json agrees with nlohmann on odd and damaged text", "[row_json]") {
    const std::vector<std::string> texts = {
        "{}", " { } ", "{\"a\":\"b\"}", "{ \"a\" : \"b\" , \"c\" : \"d\" }", "{\"a\":\"\\u00e9\\u0041\\/\"}", "{\"a\":\"\\uD83D\\uDE00\"}",
        "{\"a\":\"\\uD83D\"}", "{\"a\":\"\\uDE00\"}", "{\"a\":\"\\uD83Dx\"}", "{\"a\":\"\\u12\"}", "{\"a\":\"\\x\"}", "{\"a\":\"\\u0000z\"}",
        "{\"b\":\"1\",\"a\":\"2\"}", "{\"a\":\"1\",\"a\":\"2\"}", "{\"a\":1}", "{\"a\":null}", "{\"a\":[\"b\"]}", "{\"a\":{\"b\":\"c\"}}", "{\"a\":true}",
        "{\"a\":\"b\"} x", "{\"a\":\"b\"", "{\"a\":\"b", "{\"a\"\"b\"}", "{\"a\":\"b\",}", "{,\"a\":\"b\"}", "", "   ", "null", "[]", "[{\"a\":\"b\"}]", "\"s\"", "{'a':'b'}",
        std::string("{\"a\":\"line\nbreak\"}"), std::string("{\"a\":\"tab\there\"}"), "{\"a\":\"\xc3\xa9\"}", "{\"a\":\"\xc3\"}", "{\"a\":\"\xff\"}",
        "{\"a\":\"\xed\xa0\x80\"}", "{\"\xc3\xa9\":\"\xea\xb0\x80\"}", "{\"a\":\"b\"}\n", "\xEF\xBB\xBF{\"a\":\"b\"}"};
    for (auto& t : texts) {
        INFO("text: " << t);
        auto a = reference_row(t), b = mine_row(t);
        REQUIRE(a.has_value() == b.has_value());
        if (a) {
            REQUIRE(*a == *b);
            REQUIRE(iteration_order(*a) == iteration_order(*b));
        }
    }
    const std::vector<std::string> arrays = {"[]", " [ ] ", "[{}]", "[{\"a\":\"b\"},{\"c\":\"d\"}]", "[{\"a\":\"b\"} , {\"c\":\"d\"} ]", "[{\"a\":\"b\"},]", "[{\"a\":\"b\"}",
                                              "[1]", "[{\"a\":1}]", "[{\"b\":\"1\",\"a\":\"2\"}]", "{}", "", "[{\"a\":\"b\"}] x", "[null]", "[[]]"};
    for (auto& t : arrays) {
        INFO("array: " << t);
        auto a = reference_rows(t), b = mine_rows(t);
        REQUIRE(a.has_value() == b.has_value());
        if (a) REQUIRE(*a == *b);
    }

    // random damage to valid text: flip, drop or insert a byte, or cut the text short
    std::mt19937 rng(8801);
    int exceptions = 0, fast_ok = 0;
    for (int iter = 0; iter < 60000; iter++) {
        Row row;
        for (std::size_t i = 0, n = 1 + rng() % 5; i < n; i++) row[random_text(rng, false) + std::to_string(i)] = random_text(rng, false);
        std::string text = row_to_json(row);
        switch (rng() % 5) {
            case 0: if (!text.empty()) text[rng() % text.size()] = static_cast<char>(rng() % 256); break;
            case 1: if (!text.empty()) text.erase(rng() % text.size(), 1); break;
            case 2: text.insert(rng() % (text.size() + 1), 1, static_cast<char>(rng() % 256)); break;
            case 3: text.resize(rng() % (text.size() + 1)); break;
            default: break; // untouched
        }
        INFO("iteration " << iter << ": " << text);
        auto a = reference_row(text), b = mine_row(text);
        REQUIRE(a.has_value() == b.has_value());
        if (a) {
            REQUIRE(*a == *b);
            REQUIRE(iteration_order(*a) == iteration_order(*b));
            fast_ok++;
        } else {
            exceptions++;
        }
    }
    REQUIRE(exceptions > 5000); // the damage really breaks text...
    REQUIRE(fast_ok > 5000);    // ...and plenty of text still parses
}
