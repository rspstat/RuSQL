#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// An aggregate of no value (an empty group, only NULLs) is NULL -- SUM, AVG, STDDEV, VARIANCE, MEDIAN, GROUP_CONCAT, JSON_AGG, the window
// forms, and so what is computed from them -- COUNT is 0. A text operand of arithmetic, SUM or AVG is the number it starts with ('12abc' is 12,
// 'x' is 0) and `+` adds. Integers are exact (a BIGINT beyond 2^53), decimals add up exactly (0.1 ten times is 1), and AVG is the exact quotient
// rounded to the 4 places the select list shows, which a HAVING or an expression then computes with. A select without FROM evaluates CASE,
// subqueries and aggregates, and SUM / COUNT of a CASE are not column names to the unknown-column check.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
}

std::string text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return r.value();
}

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

Rows cells(const std::string& text) {
    Rows rows;
    std::istringstream in(text);
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

Rows q(Executor& ex, const std::string& sql) { return cells(text(ex, sql)); }

// the one cell of a one-row, one-column answer
std::string one(Executor& ex, const std::string& sql) {
    Rows rows = q(ex, sql);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 1);
    return rows[0][0];
}

// MySQL's reading of a string as a number, written independently of the engine's (a regular expression)
double mysql_number(const std::string& s) {
    static const std::regex re(R"(^[ \t\n\r\v\f]*([+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?))");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return 0.0;
    return std::stod(m[1].str());
}

bool same_number(const std::string& got, double want, double tolerance = 2e-6) {
    if (got == N) return false;
    try {
        return std::abs(std::stod(got) - want) <= tolerance * (1 + std::abs(want));
    } catch (...) {
        return false;
    }
}

void seed(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, g INT, v INT, w INT, s VARCHAR(20))");
    ok(ex, "INSERT INTO a VALUES (1, 1, 10, 2, 'x'), (2, 1, NULL, 3, NULL), (3, 2, 30, NULL, 'z'), (4, 2, 0, 0, '12'), (5, 3, NULL, NULL, NULL)");
}
} // namespace

TEST_CASE("aggregates of no value are NULL and COUNT is 0", "[aggregate_semantics][empty]") {
    TempDataDir dir("as_empty");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    const std::string all = "SUM(v), AVG(v), MIN(v), MAX(v), COUNT(v), COUNT(*), STDDEV(v), VARIANCE(v), MEDIAN(v), GROUP_CONCAT(s), JSON_AGG(v), "
                            "SUM(DISTINCT v), AVG(DISTINCT v), COUNT(DISTINCT v)";
    // no row at all, and only NULLs
    REQUIRE(q(ex, "SELECT " + all + " FROM a WHERE id > 100") == Rows{{N, N, N, N, "0", "0", N, N, N, N, N, N, N, "0"}});
    REQUIRE(q(ex, "SELECT " + all + " FROM a WHERE id = 5") == Rows{{N, N, N, N, "0", "1", N, N, N, N, "[null]", N, N, "0"}});
    // a value is a value
    REQUIRE(q(ex, "SELECT SUM(v), AVG(v), STDDEV(v), VARIANCE(v), MEDIAN(v), GROUP_CONCAT(s) FROM a WHERE id = 1") ==
            Rows{{"10", "10.0000", "0.0000", "0.0000", "10.0000", "x"}});
    REQUIRE(q(ex, "SELECT SUM(v), AVG(v), MIN(v), MAX(v) FROM a WHERE id = 4") == Rows{{"0", "0.0000", "0", "0"}});

    // per group: the group of only NULLs
    REQUIRE(q(ex, "SELECT g, SUM(v), AVG(v), MIN(v), COUNT(v), COUNT(*), STDDEV(v), MEDIAN(v), GROUP_CONCAT(s) FROM a GROUP BY g ORDER BY g") ==
            Rows{{"1", "10", "10.0000", "10", "1", "2", "0.0000", "10.0000", "x"},
                 {"2", "30", "15.0000", "0", "2", "2", "15.0000", "15.0000", "z,12"},
                 {"3", N, N, N, "0", "1", N, N, N}});
    REQUIRE(q(ex, "SELECT g, SUM(w), AVG(w) FROM a WHERE id IN (3, 5) GROUP BY g ORDER BY g") == Rows{{"2", N, N}, {"3", N, N}});

    // window functions: a frame of only NULLs
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (PARTITION BY g), AVG(v) OVER (PARTITION BY g) FROM a WHERE id IN (2, 3, 5) ORDER BY id") ==
            Rows{{"2", N, N}, {"3", "30", "30.0000"}, {"5", N, N}});
    REQUIRE(q(ex, "SELECT id, SUM(w) OVER (ORDER BY id), AVG(w) OVER (ORDER BY id) FROM a WHERE id IN (3, 5) ORDER BY id") ==
            Rows{{"3", N, N}, {"5", N, N}});
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM a WHERE g IN (1, 3) ORDER BY id") ==
            Rows{{"1", "10"}, {"2", "10"}, {"5", N}});

    // what is computed from an aggregate of nothing
    REQUIRE(q(ex, "SELECT SUM(v) + 1, AVG(v) * 2, COALESCE(SUM(v), -1), COALESCE(AVG(v), -1) FROM a WHERE id = 5") == Rows{{N, N, "-1", "-1"}});
    REQUIRE(q(ex, "SELECT SUM(CASE WHEN v > 5 THEN 1 END), SUM(v > 5), SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) FROM a WHERE id > 100") ==
            Rows{{N, N, N}});
    // ... and HAVING, subqueries
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g HAVING SUM(w) IS NULL ORDER BY g") == Rows{{"3"}});
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g HAVING SUM(w) >= 0 ORDER BY g") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g HAVING AVG(v) >= 0 ORDER BY g") == Rows{{"1"}, {"2"}});
    // x IN (NULL) and x = NULL are never true: the row with v = 0 must not match the "0" an empty SUM used to be
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a WHERE v IN (SELECT SUM(v) FROM a WHERE id > 100)") == Rows{{"0"}});
    REQUIRE(text(ex, "SELECT id FROM a WHERE v = (SELECT SUM(v) FROM a WHERE id > 100)").find("0 rows returned") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, (SELECT SUM(v) FROM a b WHERE b.g = a.g AND b.id > 100) FROM a WHERE id = 1") == Rows{{"1", N}});

    // BIT_AND / BIT_OR: the identity of an empty set, as an unsigned 64-bit number
    REQUIRE(q(ex, "SELECT BIT_AND(v), BIT_OR(v) FROM a WHERE id > 100") == Rows{{"18446744073709551615", "0"}});
    REQUIRE(q(ex, "SELECT BIT_AND(v), BIT_OR(v) FROM a WHERE id = 5") == Rows{{"18446744073709551615", "0"}});
    REQUIRE(q(ex, "SELECT BIT_AND(w), BIT_OR(w) FROM a WHERE id IN (1, 2)") == Rows{{"2", "3"}});
    // JSON_AGG / ARRAY_AGG of no row
    REQUIRE(q(ex, "SELECT JSON_AGG(v), ARRAY_AGG(v) FROM a WHERE id > 100") == Rows{{N, N}});
}

TEST_CASE("a text operand is the number it starts with and + never joins strings", "[aggregate_semantics][text_number]") {
    TempDataDir dir("as_text");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(20))");
    // strings that are numbers, start with one, or do not
    const std::vector<std::string> pool = {"x", "z", "", "12", "12abc", " 5", "  -7.5kg", "-3.5x", "1e2z", "1e", "e5", ".5", "5.", "+7", "0x10", "1,5",
                                           "--1", "++2", "1 2", "007", "-0", "3.25", "abc12", "9.99e1", "-.5e1x", "\t8"};
    for (std::size_t i = 0; i < pool.size(); i++) {
        ok(ex, "INSERT INTO t VALUES (" + std::to_string(i + 1) + ", '" + pool[i] + "')");
    }
    // the examples
    REQUIRE(q(ex, "SELECT 'x' + 1, '12abc' * 2, '3.5x' + 1, 'x' + 'y', '1e2' + 1, ' 5' + 1, '-3' + 1, '.5' + 1, '' + 1") ==
            Rows{{"1", "24", "4.5", "0", "101", "6", "-2", "1.5", "1"}});
    REQUIRE(q(ex, "SELECT '0x10' + 1, '1,5' + 1, '++1' + 1, '1 2' + 1, '5' + '6', '5' - '6', 9 - '1a', 2 * '2.5'") ==
            Rows{{"1", "2", "1", "2", "11", "-1", "8", "5"}});
    // division by something that is 0 is NULL
    REQUIRE(q(ex, "SELECT 10 / 'x', 10 / '', 7 % 'x', MOD(7, 'q'), 5 / 0, 0 / 0, 1 + NULL, NULL * 3") == Rows{{N, N, N, N, N, N, N, N}});
    REQUIRE(q(ex, "SELECT 7 % '4q', MOD('10x', '3y'), '9' / 3, '7' / '2'") == Rows{{"3", "1", "3", "3.5"}});

    // a reference for every string of the pool, with a literal and with a column, each operator and both orders
    for (std::size_t i = 0; i < pool.size(); i++) {
        const double x = mysql_number(pool[i]);
        const std::string id = std::to_string(i + 1);
        INFO("'" << pool[i] << "'");
        Rows got = q(ex, "SELECT s + 1, 1 + s, s - 1, 1 - s, s * 3, 3 * s, s / 4, s % 5 FROM t WHERE id = " + id);
        REQUIRE(got.size() == 1);
        REQUIRE(got[0].size() == 8);
        REQUIRE(same_number(got[0][0], x + 1));
        REQUIRE(same_number(got[0][1], 1 + x));
        REQUIRE(same_number(got[0][2], x - 1));
        REQUIRE(same_number(got[0][3], 1 - x));
        REQUIRE(same_number(got[0][4], x * 3));
        REQUIRE(same_number(got[0][5], 3 * x));
        REQUIRE(same_number(got[0][6], x / 4));
        REQUIRE(same_number(got[0][7], std::fmod(x, 5.0)));
        // two text operands
        for (std::size_t j = 0; j < pool.size(); j += 5) {
            const double y = mysql_number(pool[j]);
            Rows both = q(ex, "SELECT a.s + b.s, a.s * b.s, a.s - b.s FROM t a JOIN t b ON a.id = " + id + " AND b.id = " + std::to_string(j + 1));
            REQUIRE(both.size() == 1);
            REQUIRE(same_number(both[0][0], x + y));
            REQUIRE(same_number(both[0][1], x * y));
            REQUIRE(same_number(both[0][2], x - y));
        }
    }

    // SUM and AVG of a text column: each value counts, by its number
    double sum = 0;
    for (auto& s : pool) sum += mysql_number(s);
    REQUIRE(same_number(one(ex, "SELECT SUM(s) FROM t"), sum));
    REQUIRE(same_number(one(ex, "SELECT AVG(s) FROM t"), sum / static_cast<double>(pool.size()), 5e-5)); // (4 places)
    REQUIRE(one(ex, "SELECT COUNT(s) FROM t") == std::to_string(pool.size()));

    // functions with a numeric argument read text the same way
    REQUIRE(q(ex, "SELECT ABS('x'), ABS('-12abc'), ROUND('3.7x'), ROUND('x', 2), CEIL('2.1z'), FLOOR(' 2.9'), SIGN('-5q'), TRUNCATE('1.99z', 1)") ==
            Rows{{"0", "12", "4", "0", "3", "2", "-1", "1.9"}});
    REQUIRE(q(ex, "SELECT SQRT('16x'), POWER('2x', '3'), CAST('12abc' AS SIGNED), CAST('x' AS SIGNED), CAST(' 7' AS SIGNED), CAST('3.9z' AS DECIMAL)") ==
            Rows{{"4.000000", "8", "12", "0", "7", "3.9"}});
    // LOG(base, x)
    REQUIRE(q(ex, "SELECT LOG(2, 8), LOG(10, 100), LOG(8), LOG(1, 5), LOG(2, 0)") == Rows{{"3.000000", "2.000000", "2.079442", N, N}});
}

TEST_CASE("integers and decimals are exact", "[aggregate_semantics][exact]") {
    TempDataDir dir("as_exact");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE m (id INT PRIMARY KEY, g INT, x DECIMAL(10, 2), n BIGINT)");
    ok(ex, "INSERT INTO m VALUES (1, 1, 0.10, 9007199254740993), (2, 1, 0.10, 9007199254740993), (3, 1, 0.10, 1), (4, 2, 0.10, 1), (5, 2, 0.10, 1), "
           "(6, 2, 0.10, 1), (7, 3, 0.10, 1), (8, 3, 0.10, 1), (9, 3, 0.10, 1), (10, 3, 0.10, 1)");
    // ten times 0.10 is 1, not 0.9999999999999999: what a HAVING compares with is what the decimal text says
    REQUIRE(one(ex, "SELECT SUM(x) FROM m") == "1");
    REQUIRE(q(ex, "SELECT g FROM m GROUP BY g HAVING SUM(x) = 0.3 ORDER BY g") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT g FROM m GROUP BY g HAVING SUM(x) = 0.4 ORDER BY g") == Rows{{"3"}});
    REQUIRE(q(ex, "SELECT g, SUM(x) FROM m GROUP BY g HAVING SUM(x) = 0.4") == Rows{{"3", "0.4000"}});
    REQUIRE(one(ex, "SELECT SUM(x) * 10 FROM m") == "10");
    // integers beyond 2^53
    REQUIRE(q(ex, "SELECT SUM(n), SUM(n) + 1, MAX(n), MIN(n) FROM m WHERE id <= 2") == Rows{{"18014398509481986", "18014398509481987", "9007199254740993", "9007199254740993"}});
    REQUIRE(q(ex, "SELECT MAX(n), MIN(n) FROM m WHERE id <= 3") == Rows{{"9007199254740993", "1"}});
    ok(ex, "CREATE TABLE bigs (id INT PRIMARY KEY, n BIGINT)");
    ok(ex, "INSERT INTO bigs VALUES (1, 9007199254740992), (2, 9007199254740993), (3, 9007199254740991)");
    REQUIRE(q(ex, "SELECT MAX(n), MIN(n), SUM(n) FROM bigs") == Rows{{"9007199254740993", "9007199254740991", "27021597764222976"}});
    REQUIRE(q(ex, "SELECT n + 1, n * 2, n - 1 FROM m WHERE id = 1") == Rows{{"9007199254740994", "18014398509481986", "9007199254740992"}});
    REQUIRE(q(ex, "SELECT 9007199254740993 + 1, 9223372036854775806 + 1, -9223372036854775807 - 1, 3037000499 * 3037000499") ==
            Rows{{"9007199254740994", "9223372036854775807", "-9223372036854775808", "9223372030926249001"}});
    // beyond int64 it is a double, not a wrong integer
    REQUIRE(same_number(one(ex, "SELECT 9223372036854775807 + 1"), 9223372036854775808.0));
    REQUIRE(same_number(one(ex, "SELECT 4611686018427387904 * 4"), 18446744073709551616.0));
    REQUIRE(same_number(one(ex, "SELECT -9223372036854775807 - 2"), -9223372036854775809.0));

    // SUM of decimals of different scales, negatives, a sum that does not fit an int64 (the double path)
    ok(ex, "CREATE TABLE d (id INT PRIMARY KEY, v VARCHAR(30))");
    ok(ex, "INSERT INTO d VALUES (1, '100.10'), (2, '-0.05'), (3, '7'), (4, '0.005'), (5, NULL)");
    REQUIRE(one(ex, "SELECT SUM(v) FROM d") == "107.0550");
    REQUIRE(q(ex, "SELECT id FROM d GROUP BY id HAVING SUM(v) = 100.1") == Rows{{"1"}});
    ok(ex, "INSERT INTO d VALUES (6, '0.12345')");
    REQUIRE(q(ex, "SELECT id FROM d GROUP BY id HAVING SUM(v) = 0.12345") == Rows{{"6"}});
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, v VARCHAR(30))");
    ok(ex, "INSERT INTO big VALUES (1, '9223372036854775807'), (2, '9223372036854775807'), (3, '1e3')");
    REQUIRE(same_number(one(ex, "SELECT SUM(v) FROM big"), 18446744073709551616.0 + 1000.0));
}

TEST_CASE("AVG is rounded to 4 places and a HAVING or an expression computes with that value", "[aggregate_semantics][avg]") {
    TempDataDir dir("as_avg");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE f (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO f VALUES (1, 1, 1), (2, 1, 2), (3, 1, 2), (4, 2, -1), (5, 2, -2), (6, 2, -2), (7, 3, 1), (8, 3, 2)");
    REQUIRE(q(ex, "SELECT g, AVG(v), AVG(v) * 3 FROM f GROUP BY g ORDER BY g") == Rows{{"1", "1.6667", "5.0001"}, {"2", "-1.6667", "-5.0001"}, {"3", "1.5000", "4.5"}});
    REQUIRE(q(ex, "SELECT g FROM f GROUP BY g HAVING AVG(v) = 1.6667") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT g, AVG(v) FROM f GROUP BY g HAVING AVG(v) = -1.6667") == Rows{{"2", "-1.6667"}});
    REQUIRE(q(ex, "SELECT g FROM f GROUP BY g HAVING AVG(v) * 3 > 5 ORDER BY g") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT g FROM f GROUP BY g HAVING AVG(v) * 3 = 5.0001") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT AVG(v) + 0, ROUND(AVG(v), 2), AVG(v) - 1 FROM f WHERE g = 1") == Rows{{"1.6667", "1.67", "0.6667"}});
    REQUIRE(q(ex, "SELECT AVG(DISTINCT v), SUM(DISTINCT v), SUM(v), COUNT(DISTINCT v) FROM f WHERE g = 1") == Rows{{"1.5000", "3", "5", "2"}});
    // half away from zero at the 5th place: 1/32 = 0.03125
    ok(ex, "CREATE TABLE h (id INT PRIMARY KEY, v INT)");
    std::string values;
    for (int i = 1; i <= 32; i++) values += std::string(i > 1 ? ", " : "") + "(" + std::to_string(i) + ", " + (i == 1 ? "1" : "0") + ")";
    ok(ex, "INSERT INTO h VALUES " + values);
    REQUIRE(one(ex, "SELECT AVG(v) FROM h") == "0.0313");
    ok(ex, "UPDATE h SET v = -1 WHERE id = 1");
    REQUIRE(one(ex, "SELECT AVG(v) FROM h") == "-0.0313");
    // a window AVG shows the same 4 places
    REQUIRE(q(ex, "SELECT id, AVG(v) OVER (ORDER BY id) FROM f WHERE g = 1 ORDER BY id") == Rows{{"1", "1.0000"}, {"2", "1.5000"}, {"3", "1.6667"}});
}

TEST_CASE("SUM, AVG, MIN, MAX, COUNT match a reference over random values", "[aggregate_semantics][random]") {
    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=30 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    static const char* strings[] = {"x", "12abc", " 5", "-3.5x", "1e2z", ".5", "7", "", "0.25", "abc9"};
    for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
        INFO("seed " << k);
        std::mt19937 rng(4242 + k);
        TempDataDir dir("as_random");
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE r (id INT PRIMARY KEY, g INT, n DECIMAL(12, 3), s VARCHAR(12))");
        struct Row {
            int id, g;
            bool n_null;
            long long thousandths; // n * 1000
            bool s_null;
            std::string s;
        };
        std::vector<Row> rows;
        const int count = 20 + static_cast<int>(rng() % 120);
        std::string values;
        for (int i = 1; i <= count; i++) {
            Row r{i, static_cast<int>(rng() % 5), rng() % 4 == 0, static_cast<long long>(rng() % 2000001) - 1000000, rng() % 5 == 0, strings[rng() % 10]};
            rows.push_back(r);
            std::string n_text = "NULL";
            if (!r.n_null) {
                long long a = r.thousandths < 0 ? -r.thousandths : r.thousandths;
                std::string frac = std::to_string(a % 1000);
                n_text = (r.thousandths < 0 ? "-" : "") + std::to_string(a / 1000) + "." + std::string(3 - frac.size(), '0') + frac;
            }
            values += std::string(i > 1 ? ", " : "") + "(" + std::to_string(i) + ", " + std::to_string(r.g) + ", " + n_text + ", " + (r.s_null ? "NULL" : "'" + r.s + "'") + ")";
        }
        ok(ex, "INSERT INTO r VALUES " + values);

        auto thousandths_text = [](long long t, int places) { // t thousandths as text with `places` (3 or 4) decimals, no rounding needed
            const long long a = t < 0 ? -t : t;
            std::string frac = std::to_string(a % 1000);
            frac = std::string(3 - frac.size(), '0') + frac;
            if (places == 4) frac += "0";
            return std::string(t < 0 ? "-" : "") + std::to_string(a / 1000) + "." + frac;
        };
        for (int iter = 0; iter < 12; iter++) {
            const int cut = static_cast<int>(rng() % static_cast<unsigned>(count + 1)); // WHERE id > cut
            std::vector<const Row*> in_scope;
            for (auto& r : rows) {
                if (r.id > cut) in_scope.push_back(&r);
            }
            INFO("id > " << cut);
            // ungrouped
            Rows got = q(ex, "SELECT SUM(n), AVG(n), MIN(n), MAX(n), COUNT(n), COUNT(*), SUM(s), AVG(s), COUNT(s) FROM r WHERE id > " + std::to_string(cut));
            REQUIRE(got.size() == 1);
            long long total = 0, n_count = 0;
            bool have_min = false;
            long long lo = 0, hi = 0;
            double s_total = 0;
            long long s_count = 0;
            for (const Row* r : in_scope) {
                if (!r->n_null) {
                    total += r->thousandths;
                    n_count++;
                    lo = have_min ? std::min(lo, r->thousandths) : r->thousandths;
                    hi = have_min ? std::max(hi, r->thousandths) : r->thousandths;
                    have_min = true;
                }
                if (!r->s_null) {
                    s_total += mysql_number(r->s);
                    s_count++;
                }
            }
            if (n_count == 0) {
                REQUIRE(got[0][0] == N);
                REQUIRE(got[0][1] == N);
                REQUIRE(got[0][2] == N);
                REQUIRE(got[0][3] == N);
            } else {
                // SUM: an integer when the sum has no fraction, else 4 places; AVG: the exact quotient rounded half away from zero to 4 places
                REQUIRE(got[0][0] == (total % 1000 == 0 ? std::to_string(total / 1000) : thousandths_text(total, 4)));
                long long numerator = total * 10, quotient = numerator / n_count;
                const long long remainder = numerator % n_count;
                if (2 * (remainder < 0 ? -remainder : remainder) >= n_count) quotient += numerator < 0 ? -1 : 1;
                const long long a = quotient < 0 ? -quotient : quotient;
                std::string frac = std::to_string(a % 10000);
                REQUIRE(got[0][1] == std::string(quotient < 0 ? "-" : "") + std::to_string(a / 10000) + "." + std::string(4 - frac.size(), '0') + frac);
                REQUIRE(same_number(got[0][2], static_cast<double>(lo) / 1000.0));
                REQUIRE(same_number(got[0][3], static_cast<double>(hi) / 1000.0));
            }
            REQUIRE(got[0][4] == std::to_string(n_count));
            REQUIRE(got[0][5] == std::to_string(in_scope.size()));
            if (s_count == 0) {
                REQUIRE(got[0][6] == N);
                REQUIRE(got[0][7] == N);
            } else {
                REQUIRE(same_number(got[0][6], s_total));
                REQUIRE(same_number(got[0][7], s_total / static_cast<double>(s_count), 5e-5)); // (4 places)
            }
            REQUIRE(got[0][8] == std::to_string(s_count));
            // grouped: a group with no value has NULL sums
            Rows grouped = q(ex, "SELECT g, SUM(n), COUNT(n), AVG(n) FROM r WHERE id > " + std::to_string(cut) + " GROUP BY g ORDER BY g");
            std::size_t at = 0;
            for (int g = 0; g < 5; g++) {
                long long gt = 0, gc = 0;
                bool any = false;
                for (const Row* r : in_scope) {
                    if (r->g != g) continue;
                    any = true;
                    if (!r->n_null) {
                        gt += r->thousandths;
                        gc++;
                    }
                }
                if (!any) continue;
                REQUIRE(at < grouped.size());
                REQUIRE(grouped[at][0] == std::to_string(g));
                REQUIRE(grouped[at][2] == std::to_string(gc));
                if (gc == 0) {
                    REQUIRE(grouped[at][1] == N);
                    REQUIRE(grouped[at][3] == N);
                } else {
                    REQUIRE(grouped[at][1] == (gt % 1000 == 0 ? std::to_string(gt / 1000) : thousandths_text(gt, 4)));
                }
                at++;
            }
            REQUIRE(at == grouped.size());
        }
    }
}

TEST_CASE("a select without FROM evaluates CASE, subqueries and aggregates", "[aggregate_semantics][dual]") {
    TempDataDir dir("as_dual");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    REQUIRE(q(ex, "SELECT CASE WHEN 1 = 1 THEN 'a' ELSE 'b' END") == Rows{{"a"}});
    REQUIRE(q(ex, "SELECT CASE WHEN 1 = 2 THEN 'a' ELSE 'b' END AS c, CASE WHEN 1 = 2 THEN 'a' END AS d") == Rows{{"b", N}});
    REQUIRE(q(ex, "SELECT IF(1 > 0, 'y', 'n'), IF(1 > 5, 'y', 'n')") == Rows{{"y", "n"}});
    REQUIRE(q(ex, "SELECT (SELECT COUNT(*) FROM a)") == Rows{{"5"}});
    REQUIRE(q(ex, "SELECT (SELECT MAX(v) FROM a), (SELECT MIN(v) FROM a) AS lo, 1 + 1, 'x'") == Rows{{"30", "0", "2", "x"}});
    // a subquery that gives NULL (no row, or a NULL) shows NULL
    REQUIRE(q(ex, "SELECT (SELECT MAX(v) FROM a WHERE id > 100), (SELECT v FROM a WHERE id = 2), (SELECT v FROM a WHERE id > 100)") == Rows{{N, N, N}});
    REQUIRE(q(ex, "SELECT * FROM (SELECT (SELECT v FROM a WHERE id = 2) AS x) d") == Rows{{N}});
    REQUIRE(q(ex, "SELECT COUNT(*)") == Rows{{"1"}});
    // a failing subquery fails the statement
    auto bad = ex.execute_sql("SELECT (SELECT v FROM nosuchtable)");
    REQUIRE(bad.is_err());
    // the other kinds that read from a table fail instead of showing ''
    REQUIRE(ex.execute_sql("SELECT ROW_NUMBER() OVER ()").is_err());
}

TEST_CASE("SUM and COUNT of a CASE or a condition are not column names", "[aggregate_semantics][case_aggregate]") {
    TempDataDir dir("as_case");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    // (the unknown-column check read the parser's placeholder "__case__" as a column: every conditional aggregate failed)
    REQUIRE(q(ex, "SELECT SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) FROM a") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT SUM(CASE WHEN v > 100 THEN 1 END) FROM a") == Rows{{N}});
    REQUIRE(q(ex, "SELECT g, SUM(CASE WHEN v > 5 THEN v ELSE 0 END) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "10"}, {"2", "30"}, {"3", "0"}});
    REQUIRE(q(ex, "SELECT SUM(v > 5), COUNT(CASE WHEN v > 5 THEN 1 END), COUNT(*) FROM a") == Rows{{"2", "2", "5"}});
    REQUIRE(q(ex, "SELECT g, SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) AS big FROM a GROUP BY g ORDER BY g") == Rows{{"1", "1"}, {"2", "1"}, {"3", "0"}});
    // a column the CASE names that does not exist is still an error
    auto bad = ex.execute_sql("SELECT SUM(CASE WHEN nosuch > 1 THEN 1 ELSE 0 END) FROM a");
    REQUIRE(bad.is_err());
    REQUIRE(bad.error() == "Unknown column 'nosuch' in 'field list'");
    auto bad_count = ex.execute_sql("SELECT COUNT(CASE WHEN nosuch > 1 THEN 1 END) FROM a");
    REQUIRE(bad_count.is_err());
    auto bad_plain = ex.execute_sql("SELECT SUM(nosuch) FROM a");
    REQUIRE(bad_plain.is_err());
}
