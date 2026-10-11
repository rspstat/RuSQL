#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Answers taken from a real MySQL 8.4 for the arithmetic and number formatting that the statement-by-statement conformance cases
// (mysql_conformance_*.inc, which compare numbers by value) do not pin down to the digit: DOUBLE columns in floating point, DECIMAL arithmetic with
// every digit kept until the end, what a numeric function / comparison / stored value reads of a quotient, how a DOUBLE is written.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

Rows table_cells(const std::string& output) {
    Rows rows;
    std::istringstream in(output);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        std::vector<std::string> row;
        std::size_t pos = 1;
        while (pos < line.size()) {
            std::size_t bar = line.find('|', pos);
            if (bar == std::string::npos) break;
            std::string v = line.substr(pos, bar - pos);
            v.erase(0, v.find_first_not_of(' '));
            v.erase(v.find_last_not_of(' ') + 1);
            row.push_back(v);
            pos = bar + 1;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return table_cells(r.value());
}

const std::string N = "NULL";
} // namespace

TEST_CASE("arithmetic that reads a DOUBLE column is floating point", "[mysql_semantics]") {
    TempDataDir dir("mysql_semantics_double");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE tt (id INT PRIMARY KEY, d DOUBLE, g INT, i INT)");
    ok(ex, "INSERT INTO tt VALUES (1, 3, 1, 10), (2, 2, 1, NULL), (3, 1.5, 2, 4), (4, 7, 2, 6)");
    // d / 3 * 3 is 3 and not 3.0001 (a DECIMAL quotient with 4 places); (1 / 3) is the DECIMAL 0.333333333 that the DOUBLE d multiplies
    REQUIRE(rows_of(ex, "SELECT id, d / 3 * 3, d * (1 / 3), d / 3, d * 1.1, d + 0.1 FROM tt ORDER BY id") ==
            Rows{{"1", "3", "0.999999999", "1", "3.3000000000000003", "3.1"},
                 {"2", "2", "0.666666666", "0.6666666666666666", "2.2", "2.1"},
                 {"3", "1.5", "0.4999999995", "0.5", "1.6500000000000001", "1.6"},
                 {"4", "7", "2.333333331", "2.3333333333333335", "7.700000000000001", "7.1"}});
    // STDDEV / VARIANCE are DOUBLEs (the sample forms of one value are NULL)
    REQUIRE(rows_of(ex, "SELECT g, STDDEV_SAMP(i), VAR_SAMP(i), VAR_SAMP(d), STDDEV_POP(i) FROM tt GROUP BY g ORDER BY g") ==
            Rows{{"1", N, N, "0.5", "0"}, {"2", "1.4142135623730951", "2", "15.125", "1"}});
}

TEST_CASE("a numeric function, a comparison and a stored value read all the digits of a quotient", "[mysql_semantics]") {
    TempDataDir dir("mysql_semantics_quotient");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // 8 / 59.27 shows 0.1350 but holds 0.134975535...: ROUND(, 2) is 0.13, >= 0.135 is false, FLOOR(10 / 3 * 3) is 9 (the product holds 9.999999999)
    REQUIRE(rows_of(ex, "SELECT ROUND(8/59.27, 2), 8/59.27 >= 0.135, 8/59.27 > 0.13497, FLOOR(10/3*3), CEIL(2/3*3), TRUNCATE(8/59.27, 3), SQRT(8/59.27)") ==
            Rows{{"0.13", "0", "1", "9", "2", "0.134", "0.3673901672609108"}});
    // ... while the number itself shows the dividend's scale plus 4 places (RuSQL leaves the trailing zeros out)
    REQUIRE(rows_of(ex, "SELECT 10/3*3, 2/3*3, 10/4, 1/3, 8/59.27") == Rows{{"10", "2", "2.5", "0.3333", "0.135"}});
    ok(ex, "CREATE TABLE dd (id INT PRIMARY KEY, p DECIMAL(10,2), n INT, v VARCHAR(20))");
    ok(ex, "INSERT INTO dd VALUES (1, 8/59.27, 7/2, 8/59.27), (2, 0, 0, 'x')");
    REQUIRE(rows_of(ex, "SELECT id, p, n, v FROM dd ORDER BY id") == Rows{{"1", "0.13", "4", "0.134975535"}, {"2", "0.00", "0", "x"}});
    ok(ex, "UPDATE dd SET p = 51/35.79, n = 10/4, v = 51/35.79 WHERE id = 2");
    REQUIRE(rows_of(ex, "SELECT id, p, n, v FROM dd ORDER BY id") == Rows{{"1", "0.13", "4", "0.134975535"}, {"2", "1.42", "3", "1.424979044"}});
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE 8/59.27 >= 0.135").empty());
    // ... on either side of a comparison, in BETWEEN, and in FORMAT
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE 0.135 > 8/59.27 AND id = 1") == Rows{{"1"}});
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE 0.13497 >= 8/59.27").empty());
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE 8/59.27 BETWEEN 0.13497 AND 0.135 AND id = 1") == Rows{{"1"}});
    REQUIRE(rows_of(ex, "SELECT FORMAT(8/59.27, 2), FORMAT(51/35.79, 2), ROUND(8/59.27, 2)") == Rows{{"0.13", "1.42", "0.13"}});
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE 8/59.27 > 0.13497 AND id = 1") == Rows{{"1"}});
    // ... against a subquery too
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE 8/59.27 >= (SELECT 0.135)").empty());
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE 8/59.27 > (SELECT 0.13497) AND id = 1") == Rows{{"1"}});
}

TEST_CASE("ROUND and FORMAT round a DOUBLE half to even and a DECIMAL half away from zero", "[mysql_semantics]") {
    TempDataDir dir("mysql_semantics_round");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE rr (id INT PRIMARY KEY, d DOUBLE, n DECIMAL(10,3), s VARCHAR(20))");
    ok(ex, "INSERT INTO rr VALUES (1, 2.5, 2.5, '2.5'), (2, 3.25, 3.25, '3.25'), (3, 0.5, 0.5, '0.5'), (4, 1.5, 1.5, '1.5'), (5, -2.5, -2.5, '-2.5'), "
           "(6, 2.675, 2.675, '2.675'), (7, 1234567.125, 1234567.125, '1234567.125')");
    REQUIRE(rows_of(ex, "SELECT id, ROUND(d), ROUND(n), ROUND(s), ROUND(d, 1), ROUND(n, 1), ROUND(d, 2), ROUND(n, 2) FROM rr ORDER BY id") ==
            Rows{{"1", "2", "3", "2", "2.5", "2.5", "2.5", "2.5"},
                 {"2", "3", "3", "3", "3.2", "3.3", "3.25", "3.25"},
                 {"3", "0", "1", "0", "0.5", "0.5", "0.5", "0.5"},
                 {"4", "2", "2", "2", "1.5", "1.5", "1.5", "1.5"},
                 {"5", "-2", "-3", "-2", "-2.5", "-2.5", "-2.5", "-2.5"},
                 {"6", "3", "3", "3", "2.7", "2.7", "2.68", "2.68"},
                 {"7", "1234567", "1234567", "1234567", "1234567.1", "1234567.1", "1234567.12", "1234567.13"}});
    // a string is a DOUBLE too (ROUND('2.5') is 2), and the places beyond 2 are rounded the same way
    REQUIRE(rows_of(ex, "SELECT ROUND('2.5'), ROUND('3.5'), ROUND('0.5'), ROUND('1.5'), FORMAT('2.5', 0), FORMAT('3.5', 0)") == Rows{{"2", "4", "0", "2", "2", "4"}});
    ok(ex, "CREATE TABLE pi (id INT PRIMARY KEY, d DOUBLE)");
    ok(ex, "INSERT INTO pi VALUES (7, 3.14159265)");
    REQUIRE(rows_of(ex, "SELECT ROUND(d, 4), ROUND(d, 6), ROUND(d, 8) FROM pi") == Rows{{"3.1416", "3.141593", "3.14159265"}});
    // (an expression done in floating point is a DOUBLE too)
    REQUIRE(rows_of(ex, "SELECT id, ROUND(d * 1), ROUND(d + 0), ROUND(n * 1) FROM rr WHERE id <= 3 ORDER BY id") ==
            Rows{{"1", "2", "2", "3"}, {"2", "3", "3", "3"}, {"3", "0", "0", "1"}});
    REQUIRE(rows_of(ex, "SELECT id, FORMAT(d, 1), FORMAT(n, 1), FORMAT(d, 2), FORMAT(d, 0) FROM rr ORDER BY id") ==
            Rows{{"1", "2.5", "2.5", "2.50", "2"},
                 {"2", "3.2", "3.3", "3.25", "3"},
                 {"3", "0.5", "0.5", "0.50", "0"},
                 {"4", "1.5", "1.5", "1.50", "2"},
                 {"5", "-2.5", "-2.5", "-2.50", "-2"},
                 {"6", "2.7", "2.7", "2.68", "3"},
                 {"7", "1,234,567.1", "1,234,567.1", "1,234,567.12", "1,234,567"}});
}

TEST_CASE("integer arithmetic gives MySQL's digits, signs and limits", "[mysql_semantics]") {
    TempDataDir dir("mysql_semantics_integers");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // quotients of integers: 9 digits are kept (cut off, not rounded), 4 are shown; the divisor may be as big as an integer gets
    REQUIRE(rows_of(ex, "SELECT 1000000000000000000 / 999999999999999999, 7 / 900000000000000001, 123456789 / 987654321012345678, -7 / 2, 7 / -2, -7 / -2, 0 / 5, 0 / -5") ==
            Rows{{"1", "0", "0", "-3.5", "-3.5", "3.5", "0", "0"}});
    REQUIRE(rows_of(ex, "SELECT 1 / 3 * 3, 2 / 3 * 3, -1 / 3, 100 / 7, 9223372036854775807 / 2, 123456789012345678 / 7") ==
            Rows{{"1", "2", "-0.3333", "14.2857", "4611686018427387903.5", "17636684144620811.1429"}});
    // ... and the numbers that are too big for the quick way (a 19 digit integer, a remainder that cannot be multiplied by 10 in 64 bits)
    REQUIRE(rows_of(ex, "SELECT 5000000000000000000 / 9000000000000000000, 9999999999999999999 + 1, 9000000000000000000 / 4000000000000000000, "
                        "9223372036854775807 / 9223372036854775806") == Rows{{"0.5556", "10000000000000000000", "2.25", "1"}});
    // texts read as numbers: leading zeros, a sign, spaces, a number followed by letters
    REQUIRE(rows_of(ex, "SELECT '007' + 1, '-12' * 2, ' 5' + 1, '+5' + 1, '12abc' + 1, -0 + 0") == Rows{{"8", "-24", "6", "6", "13", "0"}});
}

TEST_CASE("a DOUBLE is written the way MySQL writes it", "[mysql_semantics]") {
    TempDataDir dir("mysql_semantics_format");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // a DOUBLE column times one: up to 999999999999999 and down to 1e-15 in full, beyond that 1e15 / 1e-16
    ok(ex, "CREATE TABLE fx (id INT PRIMARY KEY, d DOUBLE)");
    ok(ex, "INSERT INTO fx VALUES (1, 123456789012345), (2, 999999999999999), (3, 1000000000000000), (4, 0.000001), (5, 1.5e-15), (6, 1e-16), (7, 3.14159265)");
    REQUIRE(rows_of(ex, "SELECT id, d * 1 FROM fx ORDER BY id") == Rows{{"1", "123456789012345"}, {"2", "999999999999999"}, {"3", "1e15"}, {"4", "0.000001"},
                                                                    {"5", "0.0000000000000015"}, {"6", "1e-16"}, {"7", "3.14159265"}});
    // up to 999999999999999 and down to 1e-15 in full, beyond that 1.5e16 / 1e-16
    REQUIRE(rows_of(ex, "SELECT 1e14*1, 1e15*1, 123456789012345e0*1, 1234567890123456e0*1, 1e-5*1, 1e-6*1, 1.5e-15*1, 1e-16*1, 1.5e300*1, -2.5e-7*1") ==
            Rows{{"100000000000000", "1e15", "123456789012345", "1.234567890123456e15", "0.00001", "0.000001", "0.0000000000000015", "1e-16", "1.5e300",
                  "-0.00000025"}});
    REQUIRE(rows_of(ex, "SELECT 1.234e-4*1, 1.234e-9*1, 1.234e-15*1, 1.5e14*1, 1.5e15*1, 1.5e20*1, -1.5e15*1, 999999999999999e0*1, 9999999999999999e0*1") ==
            Rows{{"0.0001234", "0.000000001234", "0.000000000000001234", "150000000000000", "1.5e15", "1.5e20", "-1.5e15", "999999999999999", "1e16"}});
}

TEST_CASE("a leap year is one by the Gregorian rule, and a built-in function is not replaced by a stored one", "[mysql_semantics]") {
    TempDataDir dir("mysql_semantics_dates");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    REQUIRE(rows_of(ex, "SELECT LAST_DAY('1900-02-10'), LAST_DAY('2000-02-10'), LAST_DAY('2100-02-10'), DATE_ADD('1900-01-31', INTERVAL 1 MONTH), "
                        "DATEDIFF('1900-03-01', '1900-02-01')") == Rows{{"1900-02-28", "2000-02-29", "2100-02-28", "1900-02-28", "28"}});
    // (MySQL calls the built-in REVERSE, whatever a stored function of that name returns)
    ok(ex, "CREATE FUNCTION reverse(s VARCHAR(10)) RETURNS VARCHAR(10) RETURN 'u'");
    REQUIRE(rows_of(ex, "SELECT reverse('abc')") == Rows{{"cba"}});
    ok(ex, "CREATE FUNCTION ifnull(a INT, b INT) RETURNS INT RETURN 99");
    ok(ex, "CREATE FUNCTION coalesce(a INT, b INT) RETURNS INT RETURN 99");
    REQUIRE(rows_of(ex, "SELECT IFNULL(NULL, 5), COALESCE(NULL, 5)") == Rows{{"5", "5"}});
}
