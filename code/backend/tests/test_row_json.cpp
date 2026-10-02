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
