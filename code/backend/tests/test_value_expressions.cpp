#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Expressions are values wherever a value goes: a function, an aggregate, a CASE, an IF or a CAST can start an expression and go on
// (`ROUND(x) * 100`, `COALESCE(a, 0) + COALESCE(b, 0)`, `SUM(CASE ...) * 100 / COUNT(*)`), the results of a CASE are expressions (and a quoted
// string is a string, never the column of that name), a condition is a value (1, 0 or NULL), a value is a condition (true when it is not NULL and
// not 0), BETWEEN takes expressions, and an INSERT value, an UPDATE assignment and a HAVING take any of these.

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
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

std::string text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return r.value();
}

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

Rows cells(const std::string& output) {
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

Rows q(Executor& ex, const std::string& sql) { return cells(text(ex, sql)); }

std::string one(Executor& ex, const std::string& sql) {
    Rows rows = q(ex, sql);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 1);
    return rows[0][0];
}

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

// a number as the engine prints it (60, 60.0000) against the number it should be
bool is_number(const std::string& got, double want) {
    if (got == N) return false;
    char* end = nullptr;
    const double v = std::strtod(got.c_str(), &end);
    return end != got.c_str() && *end == '\0' && std::abs(v - want) < 1e-9;
}

// ids 1..5; v > 5 for 1, 3, 5
void table_a(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, v INT, w INT, d DOUBLE, s VARCHAR(20), g INT)");
    ok(ex, "INSERT INTO a VALUES (1, 10, 2, 1.5, 'x', 1), (2, NULL, 3, NULL, NULL, 1), (3, 30, NULL, 2.5, 'z', 2), (4, 0, 0, 0, '12', 2), (5, 7, 7, 3.25, 'abc', 3)");
}
} // namespace

TEST_CASE("a function, an aggregate or a CAST can start an expression", "[value_expressions]") {
    TempDataDir dir("vx_functions");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    REQUIRE(q(ex, "SELECT id, COALESCE(v, 0) + COALESCE(w, 0) FROM a ORDER BY id") == Rows{{"1", "12"}, {"2", "3"}, {"3", "30"}, {"4", "0"}, {"5", "14"}});
    REQUIRE(q(ex, "SELECT id, LENGTH(s) + 1 FROM a ORDER BY id") == Rows{{"1", "2"}, {"2", N}, {"3", "2"}, {"4", "3"}, {"5", "4"}});
    REQUIRE(q(ex, "SELECT id, UPPER(s) || '!' FROM a ORDER BY id") == Rows{{"1", "X!"}, {"2", N}, {"3", "Z!"}, {"4", "12!"}, {"5", "ABC!"}});
    REQUIRE(q(ex, "SELECT id, CAST(v AS SIGNED) + 1 FROM a ORDER BY id") == Rows{{"1", "11"}, {"2", N}, {"3", "31"}, {"4", "1"}, {"5", "8"}});
    REQUIRE(q(ex, "SELECT id, ABS(v - 20) * 2 FROM a ORDER BY id") == Rows{{"1", "20"}, {"2", N}, {"3", "20"}, {"4", "40"}, {"5", "26"}});
    REQUIRE(q(ex, "SELECT id, 2 * ABS(v - 20) FROM a ORDER BY id") == Rows{{"1", "20"}, {"2", N}, {"3", "20"}, {"4", "40"}, {"5", "26"}});
    REQUIRE(q(ex, "SELECT id, (v + w) * 2 FROM a ORDER BY id") == Rows{{"1", "24"}, {"2", N}, {"3", N}, {"4", "0"}, {"5", "28"}});
    REQUIRE(q(ex, "SELECT id, IFNULL(v, -1) + 1 FROM a ORDER BY id") == Rows{{"1", "11"}, {"2", "0"}, {"3", "31"}, {"4", "1"}, {"5", "8"}});
    // the type of a CAST: SIGNED / UNSIGNED with the optional INT / INTEGER
    REQUIRE(q(ex, "SELECT id, CAST(v AS SIGNED INT) + 1, CAST(w AS UNSIGNED INTEGER) * 2, CAST(v AS SIGNED INTEGER) FROM a WHERE id = 1") == Rows{{"1", "11", "4", "10"}});
    // a function or an aggregate followed by a comparison is an expression too
    REQUIRE(q(ex, "SELECT id, LENGTH(s) >= 2, UPPER(s) = 'X', ABS(v) <= 7, MOD(v, 4) < 3 FROM a WHERE id <= 3 ORDER BY id") ==
            Rows{{"1", "0", "1", "0", "1"}, {"2", N, N, N, N}, {"3", "0", "0", "0", "1"}});
    REQUIRE(q(ex, "SELECT COUNT(*) > 4, SUM(v) <> 47, MAX(v) = 30, COUNT(*) >= 5, MIN(v) <= 0, SUM(v) < 40 FROM a") == Rows{{"1", "0", "1", "1", "1", "0"}});
    REQUIRE(q(ex, "SELECT id, ROUND(d * 10) * 100 FROM a WHERE id = 5") == Rows{{"5", "3300"}});
    REQUIRE(q(ex, "SELECT ROUND(2.7) * 2, ABS(-3) + 1, LENGTH('abc') * LENGTH('de')") == Rows{{"6", "4", "6"}});
    // a function on the right of a comparison, a parenthesised value on the left
    REQUIRE(q(ex, "SELECT id FROM a WHERE (v + w) * 2 > 20 ORDER BY id") == Rows{{"1"}, {"5"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE CAST(v AS SIGNED) > 5 ORDER BY id") == Rows{{"1"}, {"3"}, {"5"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE 20 < (v + w) * 2 ORDER BY id") == Rows{{"1"}, {"5"}});
    // an aggregate with an operator after it, and aggregates in a function
    REQUIRE(one(ex, "SELECT COUNT(*) - COUNT(v) FROM a") == "1");
    REQUIRE(one(ex, "SELECT MAX(v) - MIN(v) FROM a") == "30");
    REQUIRE(one(ex, "SELECT SUM(v) * 2 FROM a") == "94");
    REQUIRE(is_number(one(ex, "SELECT SUM(v) / COUNT(v) FROM a"), 11.75));
    REQUIRE(q(ex, "SELECT g, COALESCE(SUM(v), 0) + 1 FROM a GROUP BY g ORDER BY g") == Rows{{"1", "11"}, {"2", "31"}, {"3", "8"}});
    REQUIRE(q(ex, "SELECT g, IFNULL(MAX(v), 0) + 1 FROM a GROUP BY g ORDER BY g") == Rows{{"1", "11"}, {"2", "31"}, {"3", "8"}});
    REQUIRE(q(ex, "SELECT g, ABS(SUM(v) - 20) * 2 FROM a GROUP BY g ORDER BY g") == Rows{{"1", "20"}, {"2", "20"}, {"3", "26"}});
    // CURRENT_DATE and NOW need no parentheses
    REQUIRE(one(ex, "SELECT LENGTH(CURRENT_DATE)") == "10");
    REQUIRE(one(ex, "SELECT LENGTH(CURRENT_TIMESTAMP)") == "19");
    REQUIRE(q(ex, "SELECT id, CURRENT_DATE = CURDATE() FROM a WHERE id = 1") == Rows{{"1", "1"}});
}

TEST_CASE("date functions, DATABASE() and USER() are expressions", "[value_expressions]") {
    TempDataDir dir("vx_dates");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE ev (id INT PRIMARY KEY, day DATE, n INT)");
    ok(ex, "INSERT INTO ev VALUES (1, '2024-01-31', 1), (2, '2024-02-28', 30), (3, '2023-12-25', NULL)");
    REQUIRE(one(ex, "SELECT DATE_ADD('2024-01-31', INTERVAL 1 DAY)") == "2024-02-01");
    REQUIRE(one(ex, "SELECT DATE_SUB('2024-03-01', INTERVAL 1 DAY)") == "2024-02-29");
    REQUIRE(one(ex, "SELECT DATE_ADD('2024-01-31', INTERVAL 1 MONTH)") == "2024-01-31"); // (a month with no 31st leaves the date as it is)
    REQUIRE(one(ex, "SELECT DATE_ADD('2023-02-28', INTERVAL 2 YEAR)") == "2025-02-28");
    REQUIRE(one(ex, "SELECT DATE_ADD('2024-01-01', INTERVAL -1 DAY)") == "2023-12-31");
    REQUIRE(one(ex, "SELECT DATEDIFF(DATE_ADD('2024-01-01', INTERVAL 10 DAY), '2024-01-01') * 2") == "20");
    REQUIRE(q(ex, "SELECT id, DATE_ADD(day, INTERVAL n DAY) FROM ev ORDER BY id") == Rows{{"1", "2024-02-01"}, {"2", "2024-03-29"}, {"3", N}});
    REQUIRE(q(ex, "SELECT id, DATE_ADD(day, INTERVAL n + 1 DAY) AS later FROM ev WHERE id = 1") == Rows{{"1", "2024-02-02"}});
    REQUIRE(q(ex, "SELECT id FROM ev WHERE day > DATE_SUB('2024-03-01', INTERVAL 31 DAY) ORDER BY id") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT id FROM ev WHERE day > DATE_SUB('2024-03-01', INTERVAL 30 DAY) ORDER BY id") == Rows{{"2"}}); // (30 days before March 1st is January 31st)
    REQUIRE(q(ex, "SELECT id FROM ev WHERE DATE_ADD(day, INTERVAL 1 DAY) = '2024-02-01'") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT id, DATE_FORMAT(day, '%Y') FROM ev WHERE id = 1") == Rows{{"1", "2024"}});
    REQUIRE(q(ex, "SELECT id FROM ev WHERE DATE_FORMAT(day, '%m') = '02'") == Rows{{"2"}});
    REQUIRE(one(ex, "SELECT DATABASE()") == "d");
    REQUIRE(one(ex, "SELECT LENGTH(DATABASE()) + 1") == "2");
    REQUIRE(!one(ex, "SELECT USER()").empty());
    REQUIRE(q(ex, "SELECT id FROM ev WHERE day < DATE_SUB(CURDATE(), INTERVAL 1 DAY) ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}});
    REQUIRE(q(ex, "SELECT id FROM ev WHERE day > DATE_ADD(CURRENT_DATE, INTERVAL 1 DAY)").empty());
    ok(ex, "INSERT INTO ev VALUES (4, DATE_ADD('2024-05-31', INTERVAL 1 DAY), 7 * 6)");
    REQUIRE(one(ex, "SELECT date_add('2024-01-31', interval 1 day)") == "2024-02-01"); // (in lower case too)
    REQUIRE(q(ex, "SELECT day, n FROM ev WHERE id = 4") == Rows{{"2024-06-01", "42"}});
    REQUIRE(error_of(ex, "SELECT DATE_ADD('2024-01-01', 1 DAY)").find("INTERVAL") != std::string::npos);
    // an aggregate over a function of dates
    REQUIRE(one(ex, "SELECT MAX(DATE_ADD(day, INTERVAL 1 DAY)) FROM ev WHERE id <= 3") == "2024-02-29");
}

TEST_CASE("small things an expression needs", "[value_expressions]") {
    TempDataDir dir("vx_small");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    // NULLIF is NULL when the two are equal as `=` says: numbers by value
    REQUIRE(q(ex, "SELECT id, NULLIF(w, 2.00) FROM a WHERE id <= 2 ORDER BY id") == Rows{{"1", N}, {"2", "3"}});
    REQUIRE(q(ex, "SELECT NULLIF('a', 'a'), NULLIF('a', 'b'), NULLIF(5, 5.0), NULLIF(5, 6)") == Rows{{N, "a", N, "5"}});
    // -0 is 0
    REQUIRE(q(ex, "SELECT -0, 0 - 0, -(0), 5 * -0, 1 + -0") == Rows{{"0", "0", "0", "0", "1"}});
    REQUIRE(q(ex, "SELECT - 0, - 0.0, -0.00, 1 - 0") == Rows{{"0", "0.0", "0.00", "1"}});
    REQUIRE(q(ex, "SELECT CASE WHEN 1 = 1 THEN 1 ELSE -0 END, CASE WHEN 1 = 2 THEN 1 ELSE -0 END") == Rows{{"1", "0"}});
    // an operator after the NULL on the right of a comparison
    REQUIRE(q(ex, "SELECT id FROM a WHERE v < NULL + 3").empty());
    REQUIRE(q(ex, "SELECT id FROM a WHERE v IS NULL OR v < NULL + 3") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE v > 5 - NULL OR id = 5") == Rows{{"5"}});
    // a sum shows every place it has (at least 4 for a fraction)
    ok(ex, "CREATE TABLE m (id INT PRIMARY KEY, p DECIMAL(10,2), r DECIMAL(10,4))");
    ok(ex, "INSERT INTO m VALUES (1, 10.25, 1.0825), (2, 3.10, 1.0825)");
    REQUIRE(one(ex, "SELECT SUM(p) FROM m") == "13.3500");
    REQUIRE(one(ex, "SELECT SUM(p * r) FROM m") == "14.451375");
    REQUIRE(one(ex, "SELECT SUM(id) FROM m") == "3");
    REQUIRE(one(ex, "SELECT SUM(p) + 0 FROM m") == "13.35");
    // a table with a partitioned key takes expressions as values
    ok(ex, "CREATE TABLE pt (id INT PRIMARY KEY, val VARCHAR(20)) PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)");
    ok(ex, "INSERT INTO pt VALUES (5 * 2, 'a'), (100 + 50, UPPER('b'))");
    REQUIRE(q(ex, "SELECT id, val FROM pt ORDER BY id") == Rows{{"10", "a"}, {"150", "B"}});
    REQUIRE(q(ex, "SELECT id FROM pt WHERE id >= 100") == Rows{{"150"}});
    // a procedure's parameter and a variable in an expression value
    ok(ex, "CREATE TABLE lg (id INT, note VARCHAR(20), n INT)");
    ok(ex, "CREATE PROCEDURE p_log(IN pid INT, IN pn INT) BEGIN INSERT INTO lg VALUES (pid, CASE WHEN pn > 5 THEN 'big' ELSE 'small' END, pn * 2 + pid); END");
    ok(ex, "CALL p_log(1, 10)");
    ok(ex, "CALL p_log(2, 3)");
    REQUIRE(q(ex, "SELECT id, note, n FROM lg ORDER BY id") == Rows{{"1", "big", "21"}, {"2", "small", "8"}});
    ok(ex, "SET @factor = 3");
    ok(ex, "INSERT INTO lg VALUES (3, IF(@factor > 2, 'x', 'y'), @factor * 7)");
    REQUIRE(q(ex, "SELECT note, n FROM lg WHERE id = 3") == Rows{{"x", "21"}});
    // SET takes a value expression: a CASE, a condition (user variables and a procedure's variables)
    ok(ex, "SET @flag = @factor > 2 AND @factor < 10");
    ok(ex, "SET @label = CASE WHEN @factor > 2 THEN 'big' ELSE 'small' END");
    ok(ex, "SET @outside = NOT @factor BETWEEN 1 AND 5");
    REQUIRE(q(ex, "SELECT @flag, @label, @outside") == Rows{{"1", "big", "0"}});
    ok(ex, "CREATE PROCEDURE p_set(IN pn INT) BEGIN DECLARE kind VARCHAR(10); DECLARE ok INT; SET kind = CASE WHEN pn > 5 THEN 'big' ELSE 'small' END; SET ok = pn BETWEEN 1 AND 9; INSERT INTO lg VALUES (pn, kind, ok); END");
    ok(ex, "CALL p_set(7)");
    ok(ex, "CALL p_set(20)");
    REQUIRE(q(ex, "SELECT id, note, n FROM lg WHERE id IN (7, 20) ORDER BY id") == Rows{{"7", "big", "1"}, {"20", "big", "0"}});
    // a function defined with a CASE or a condition
    ok(ex, "CREATE FUNCTION bigger(a INT, b INT) RETURNS INT RETURN CASE WHEN a > b THEN a ELSE b END");
    ok(ex, "CREATE FUNCTION in_range(a INT) RETURNS INT RETURN a BETWEEN 1 AND 9");
    REQUIRE(q(ex, "SELECT bigger(3, 8), bigger(9, 2), in_range(5), in_range(50)") == Rows{{"8", "9", "1", "0"}});
}

TEST_CASE("CASE and IF: results are expressions and a quoted string is a string", "[value_expressions]") {
    TempDataDir dir("vx_case");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    REQUIRE(q(ex, "SELECT id, CASE WHEN v > 5 THEN v * 2 ELSE w + 1 END FROM a ORDER BY id") == Rows{{"1", "20"}, {"2", "4"}, {"3", "60"}, {"4", "1"}, {"5", "14"}});
    REQUIRE(q(ex, "SELECT id, CASE WHEN v > 5 THEN v ELSE w END + 1 FROM a ORDER BY id") == Rows{{"1", "11"}, {"2", "4"}, {"3", "31"}, {"4", "1"}, {"5", "8"}});
    REQUIRE(q(ex, "SELECT id, CASE v WHEN 10 THEN 'ten' WHEN 7 THEN 'seven' ELSE 'other' END FROM a ORDER BY id") ==
            Rows{{"1", "ten"}, {"2", "other"}, {"3", "other"}, {"4", "other"}, {"5", "seven"}});
    REQUIRE(q(ex, "SELECT id, CASE s WHEN 'x' THEN 1 WHEN 'z' THEN 2 END FROM a ORDER BY id") == Rows{{"1", "1"}, {"2", N}, {"3", "2"}, {"4", N}, {"5", N}});
    REQUIRE(q(ex, "SELECT id, CASE WHEN v IS NULL THEN 'none' WHEN v = 0 THEN 'zero' END FROM a ORDER BY id") ==
            Rows{{"1", N}, {"2", "none"}, {"3", N}, {"4", "zero"}, {"5", N}});
    // (a CASE x WHEN NULL never matches: not a NULL x, and not a 0 either)
    REQUIRE(q(ex, "SELECT id, CASE v WHEN NULL THEN 'null' ELSE 'other' END FROM a WHERE id IN (2, 4) ORDER BY id") == Rows{{"2", "other"}, {"4", "other"}});
    REQUIRE(q(ex, "SELECT id, IF(v > 5, v, w) * 3 FROM a ORDER BY id") == Rows{{"1", "30"}, {"2", "9"}, {"3", "90"}, {"4", "0"}, {"5", "21"}});
    REQUIRE(q(ex, "SELECT id, IF(v IS NULL, 'n', 'y') FROM a ORDER BY id") == Rows{{"1", "y"}, {"2", "n"}, {"3", "y"}, {"4", "y"}, {"5", "y"}});
    // an unknown condition is not true: the ELSE answers
    REQUIRE(q(ex, "SELECT id, IF(v > 5, 'big', 'small') FROM a WHERE id = 2") == Rows{{"2", "small"}});
    // nested, and the whole CASE as an operand
    REQUIRE(q(ex, "SELECT id, CASE WHEN v > 5 THEN CASE WHEN v > 20 THEN 'huge' ELSE 'big' END ELSE 'small' END FROM a ORDER BY id") ==
            Rows{{"1", "big"}, {"2", "small"}, {"3", "huge"}, {"4", "small"}, {"5", "big"}});
    REQUIRE(q(ex, "SELECT id, 1 + CASE WHEN v > 5 THEN 1 ELSE 0 END * 10 FROM a ORDER BY id") == Rows{{"1", "11"}, {"2", "1"}, {"3", "11"}, {"4", "1"}, {"5", "11"}});
    // in WHERE, ORDER BY's alias and UPDATE
    REQUIRE(q(ex, "SELECT id FROM a WHERE CASE WHEN v > 5 THEN 1 ELSE 0 END = 1 ORDER BY id") == Rows{{"1"}, {"3"}, {"5"}});
    REQUIRE(q(ex, "SELECT id, CASE WHEN v > 5 THEN 'a' ELSE 'b' END AS k FROM a ORDER BY id") == Rows{{"1", "a"}, {"2", "b"}, {"3", "a"}, {"4", "b"}, {"5", "a"}});
    // a string that is the name of a column is a string
    ok(ex, "CREATE TABLE k (id INT PRIMARY KEY, n INT, big INT, name VARCHAR(10))");
    ok(ex, "INSERT INTO k VALUES (1, 10, 1, 'a'), (2, 3, 2, 'b')");
    REQUIRE(q(ex, "SELECT id, CASE WHEN n > 5 THEN 'n' ELSE 'big' END FROM k ORDER BY id") == Rows{{"1", "n"}, {"2", "big"}});
    REQUIRE(q(ex, "SELECT id, IF(n > 5, 'name', 'big') FROM k ORDER BY id") == Rows{{"1", "name"}, {"2", "big"}});
    REQUIRE(q(ex, "SELECT id, CASE WHEN n > 5 THEN n ELSE big END FROM k ORDER BY id") == Rows{{"1", "10"}, {"2", "2"}});
    REQUIRE(q(ex, "SELECT id, CASE name WHEN 'a' THEN 'name' ELSE 'n' END FROM k ORDER BY id") == Rows{{"1", "name"}, {"2", "n"}});
    // a quote inside a string
    REQUIRE(one(ex, "SELECT CASE WHEN 1 = 1 THEN 'it''s' END") == "it's");
    // a CASE of strings compares as strings, of numbers as numbers
    REQUIRE(q(ex, "SELECT id FROM k WHERE CASE WHEN n > 5 THEN '10' ELSE '9' END < '10a' ORDER BY id") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT id FROM k WHERE CASE WHEN n > 5 THEN 10 ELSE 9 END < 9.5 ORDER BY id") == Rows{{"2"}});
    // malformed
    REQUIRE(error_of(ex, "SELECT CASE WHEN n > 5 THEN 1 FROM k").find("END") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT CASE n WHEN 1 FROM k").find("THEN") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT CASE END FROM k").find("WHEN") != std::string::npos);
    // an unknown column in a CASE is an error, in a condition and in a result
    REQUIRE(error_of(ex, "SELECT CASE WHEN nosuch > 1 THEN 1 END FROM k").find("Unknown column 'nosuch'") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT CASE WHEN n > 1 THEN nosuch END FROM k").find("Unknown column 'nosuch'") != std::string::npos);
}

TEST_CASE("a condition is a value and a value is a condition", "[value_expressions]") {
    TempDataDir dir("vx_predicates");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    auto column = [&](const std::string& expr) {
        Rows rows = q(ex, "SELECT " + expr + " FROM a ORDER BY id");
        std::vector<std::string> out;
        for (auto& r : rows) out.push_back(r[0]);
        return out;
    };
    using V = std::vector<std::string>;
    REQUIRE(column("v BETWEEN 5 AND 20") == V{"1", N, "0", "0", "1"});
    REQUIRE(column("v NOT BETWEEN 5 AND 20") == V{"0", N, "1", "1", "0"});
    REQUIRE(column("v IN (7, 10)") == V{"1", N, "0", "0", "1"});
    REQUIRE(column("v NOT IN (7, 10)") == V{"0", N, "1", "1", "0"});
    REQUIRE(column("v IN (7, NULL)") == V{N, N, N, N, "1"});
    REQUIRE(column("v IS NULL") == V{"0", "1", "0", "0", "0"});
    REQUIRE(column("v IS NOT NULL") == V{"1", "0", "1", "1", "1"});
    REQUIRE(column("v > 5") == V{"1", N, "1", "0", "1"});
    REQUIRE(column("(v > 5) AND (w > 1)") == V{"1", N, N, "0", "1"});
    REQUIRE(column("v > 5 AND w > 1") == V{"1", N, N, "0", "1"});
    REQUIRE(column("(v > 5) OR (w > 1)") == V{"1", "1", "1", "0", "1"});
    REQUIRE(column("NOT (v > 5)") == V{"0", N, "0", "1", "0"});
    REQUIRE(column("NOT v > 5 OR w = 0") == V{"0", N, N, "1", "0"});
    REQUIRE(column("v IS TRUE") == V{"1", "0", "1", "0", "1"});
    REQUIRE(column("v IS NOT TRUE") == V{"0", "1", "0", "1", "0"});
    REQUIRE(column("v IS FALSE") == V{"0", "0", "0", "1", "0"});
    REQUIRE(column("v IS NOT FALSE") == V{"1", "1", "1", "0", "1"});
    REQUIRE(column("v IS UNKNOWN") == V{"0", "1", "0", "0", "0"});
    REQUIRE(column("s LIKE 'a%'") == V{"0", N, "0", "0", "1"});
    REQUIRE(column("s NOT LIKE 'a%'") == V{"1", N, "1", "1", "0"});
    // the value of a condition goes into arithmetic and functions
    REQUIRE(column("(v > 5) + 1") == V{"2", N, "2", "1", "2"});
    REQUIRE(column("COALESCE(v > 5, 9) + 0") == V{"1", "9", "1", "0", "1"});
    // a value as a condition: not NULL and not 0 (a text by the number it starts with)
    auto ids = [&](const std::string& where) {
        Rows rows = q(ex, "SELECT id FROM a WHERE " + where + " ORDER BY id");
        std::vector<std::string> out;
        for (auto& r : rows) out.push_back(r[0]);
        return out;
    };
    REQUIRE(ids("TRUE") == V{"1", "2", "3", "4", "5"});
    REQUIRE(ids("FALSE") == V{});
    REQUIRE(ids("v") == V{"1", "3", "5"});
    REQUIRE(ids("NOT v") == V{"4"});
    REQUIRE(ids("v AND w") == V{"1", "5"});
    REQUIRE(ids("v OR w") == V{"1", "2", "3", "5"});
    REQUIRE(ids("s") == V{"4"});
    REQUIRE(ids("(v)") == V{"1", "3", "5"});
    REQUIRE(ids("(v) AND NOT (w = 7)") == V{"1"});
    REQUIRE(ids("(v > 5) = 1") == V{"1", "3", "5"});
    REQUIRE(ids("v > 5 AND (w < 3 OR w IS NULL)") == V{"1", "3"});
    // BETWEEN takes expressions and columns
    REQUIRE(ids("v BETWEEN w AND w + 20") == V{"1", "4", "5"});
    REQUIRE(ids("v NOT BETWEEN w AND w + 20") == V{});
    REQUIRE(ids("v BETWEEN w AND 20") == V{"1", "4", "5"});
    REQUIRE(ids("v BETWEEN 5 AND 20") == V{"1", "5"});
    REQUIRE(ids("v BETWEEN 2 * 2 AND 10 + 10") == V{"1", "5"});
    REQUIRE(ids("v NOT BETWEEN 2 * 2 AND 10 + 10") == V{"3", "4"});
    // a negative number and a string as bounds
    REQUIRE(ids("v BETWEEN -5 AND 8") == V{"4", "5"});
    REQUIRE(ids("s BETWEEN 'x' AND 'zz'") == V{"1", "3"});
}

TEST_CASE("aggregates inside expressions: CASE, ratios, HAVING", "[value_expressions]") {
    TempDataDir dir("vx_aggregates");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    REQUIRE(one(ex, "SELECT SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) * 100 + COUNT(*) FROM a") == "305");
    REQUIRE(is_number(one(ex, "SELECT SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) * 100.0 / COUNT(*) FROM a"), 60.0));
    REQUIRE(is_number(one(ex, "SELECT ROUND(100.0 * SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) / COUNT(*), 1) FROM a"), 60.0));
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g HAVING SUM(CASE WHEN v > 15 THEN 1 ELSE 0 END) > 0") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g HAVING COUNT(CASE WHEN v > 5 THEN 1 END) = 1 ORDER BY g") == Rows{{"1"}, {"2"}, {"3"}});
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g HAVING SUM(CASE WHEN v > 5 THEN v ELSE 0 END) > 8 ORDER BY g") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT g, SUM(CASE WHEN v > 5 THEN v ELSE 0 END) + COUNT(*) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "12"}, {"2", "32"}, {"3", "8"}});
    REQUIRE(q(ex, "SELECT g, CASE WHEN SUM(v) > 20 THEN 'hi' ELSE 'lo' END FROM a GROUP BY g ORDER BY g") == Rows{{"1", "lo"}, {"2", "hi"}, {"3", "lo"}});
    REQUIRE(q(ex, "SELECT g, COALESCE(SUM(CASE WHEN v > 20 THEN 1 END), 0) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "0"}, {"2", "1"}, {"3", "0"}});
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g HAVING CASE WHEN SUM(v) > 20 THEN 1 ELSE 0 END = 1") == Rows{{"2"}});
    // the old forms (the aggregate on its own) answer as before
    REQUIRE(q(ex, "SELECT SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END), COUNT(CASE WHEN v > 5 THEN 1 END), SUM(v > 5) FROM a") == Rows{{"3", "3", "3"}});
    REQUIRE(q(ex, "SELECT SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) + 0, COUNT(CASE WHEN v > 5 THEN 1 END) + 0, SUM(v > 5) + 0 FROM a") == Rows{{"3", "3", "3"}});
    // a condition with a list, a BETWEEN, strings and AND / OR inside an aggregate's CASE
    REQUIRE(one(ex, "SELECT SUM(CASE WHEN v IN (7, 10) AND w BETWEEN 1 AND 9 OR s = 'z' THEN 1 ELSE 0 END) + 0 FROM a") == "3");
    REQUIRE(one(ex, "SELECT SUM(CASE WHEN s LIKE 'a%' OR s = 'it''s' THEN 1 ELSE 0 END) + 0 FROM a") == "1");
    REQUIRE(one(ex, "SELECT MAX(CASE WHEN v IS NULL THEN -1 ELSE v END) + 0 FROM a") == "30");
    // a subquery inside the CASE of an aggregate expression cannot be written back as text: an error, never a wrong number
    const std::string error = error_of(ex, "SELECT SUM(CASE WHEN v > (SELECT 1) THEN 1 ELSE 0 END) + 0 FROM a");
    REQUIRE(error.find("subquery") != std::string::npos);
}

TEST_CASE("INSERT values and UPDATE assignments are expressions", "[value_expressions]") {
    TempDataDir dir("vx_dml");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, v INT, s VARCHAR(30), d DOUBLE)");
    ok(ex, "INSERT INTO c VALUES (1, 1 + 2, UPPER('x'), ROUND(2.7))");
    ok(ex, "INSERT INTO c VALUES (2, (3 * 4), CONCAT('a', 'b'), 1.5 * 2)");
    ok(ex, "INSERT INTO c VALUES (3, -5, 'q', -1.5), (4, 2 * (1 + 2), 'it''s', 0)");
    ok(ex, "INSERT INTO c VALUES (5, CASE WHEN 1 < 2 THEN 7 ELSE 8 END, IF(2 > 1, 'yes', 'no'), NULL)");
    REQUIRE(q(ex, "SELECT * FROM c ORDER BY id") ==
            Rows{{"1", "3", "X", "3"}, {"2", "12", "ab", "3"}, {"3", "-5", "q", "-1.5"}, {"4", "6", "it's", "0"}, {"5", "7", "yes", N}});
    // a variable in the expression
    ok(ex, "SET @k = 5");
    ok(ex, "INSERT INTO c VALUES (6, @k * 2 + 1, 'v', @k)");
    REQUIRE(q(ex, "SELECT v, d FROM c WHERE id = 6") == Rows{{"11", "5"}});
    // the date and time functions, with and without parentheses
    ok(ex, "CREATE TABLE e (id INT PRIMARY KEY, day DATE, at DATETIME)");
    ok(ex, "INSERT INTO e VALUES (1, CURRENT_DATE, NOW()), (2, CURDATE(), CURRENT_TIMESTAMP)");
    REQUIRE(q(ex, "SELECT id, LENGTH(day), LENGTH(at) FROM e ORDER BY id") == Rows{{"1", "10", "19"}, {"2", "10", "19"}});
    // REPLACE INTO
    ok(ex, "REPLACE INTO c VALUES (1, 10 * 10, 'r', 0)");
    REQUIRE(q(ex, "SELECT v, s FROM c WHERE id = 1") == Rows{{"100", "r"}});
    // UPDATE ... SET
    ok(ex, "UPDATE c SET v = v * 2 + 1 WHERE id = 2");
    REQUIRE(one(ex, "SELECT v FROM c WHERE id = 2") == "25");
    ok(ex, "UPDATE c SET v = CASE WHEN v > 20 THEN 1 ELSE 2 END WHERE id <= 2");
    REQUIRE(q(ex, "SELECT id, v FROM c WHERE id <= 2 ORDER BY id") == Rows{{"1", "1"}, {"2", "1"}});
    ok(ex, "UPDATE c SET s = CONCAT(s, '!'), d = ROUND(d) WHERE id = 3");
    REQUIRE(q(ex, "SELECT s, d FROM c WHERE id = 3") == Rows{{"q!", "-2"}});
    ok(ex, "UPDATE c SET v = IF(v > 5, v - 5, 0), d = (v > 5) WHERE id >= 4 AND id <= 5");
    REQUIRE(q(ex, "SELECT id, v, d FROM c WHERE id >= 4 AND id <= 5 ORDER BY id") == Rows{{"4", "1", "1"}, {"5", "2", "1"}});
    // ON DUPLICATE KEY UPDATE
    ok(ex, "INSERT INTO c VALUES (1, 0, 'x', 0) ON DUPLICATE KEY UPDATE v = CASE WHEN VALUES(v) = 0 THEN 100 ELSE 200 END, s = UPPER(s) || '?'");
    REQUIRE(q(ex, "SELECT v, s FROM c WHERE id = 1") == Rows{{"100", "R?"}});
    // an expression that cannot be a value of the column is refused like any other value, and nothing is inserted
    const std::string refused = error_of(ex, "INSERT INTO c VALUES (9, 'abc' || 'def', 'x', 0)");
    REQUIRE(!refused.empty());
    REQUIRE(one(ex, "SELECT COUNT(*) FROM c WHERE id = 9") == "0");
}

TEST_CASE("expressions in triggers, procedures, views and after a restart", "[value_expressions][persistence]") {
    TempDataDir dir("vx_persist");
    {
        Executor ex(dir.path);
        open_db(ex);
        table_a(ex);
        ok(ex, "CREATE TABLE lg (id INT, note VARCHAR(20), n INT)");
        ok(ex, "CREATE TRIGGER trg AFTER INSERT ON a FOR EACH ROW INSERT INTO lg VALUES (NEW.id, CASE WHEN NEW.v > 5 THEN 'big' ELSE 'small' END, NEW.v * 2 + 1)");
        ok(ex, "CREATE PROCEDURE p_add(IN pid INT, IN pv INT) BEGIN INSERT INTO a VALUES (pid, pv, pv + 1, NULL, UPPER('p'), pid % 2); END");
        ok(ex, "CREATE VIEW vw AS SELECT id, CASE WHEN v > 5 THEN 'big' ELSE 'small' END AS size, (v + w) * 2 AS t, SUM(v) OVER (PARTITION BY g) AS gs FROM a");
        ok(ex, "CREATE VIEW vw2 AS SELECT g, SUM(CASE WHEN v > 5 THEN 1 ELSE 0 END) * 100 + COUNT(*) AS score FROM a GROUP BY g");
        ok(ex, "INSERT INTO a VALUES (6, 12, 1, NULL, 'q', 3)");
        ok(ex, "CALL p_add(7, 3)");
    }
    Executor ex(dir.path);
    ok(ex, "USE d");
    REQUIRE(q(ex, "SELECT id, note, n FROM lg ORDER BY id") == Rows{{"6", "big", "25"}, {"7", "small", "7"}});
    REQUIRE(q(ex, "SELECT id, s, g FROM a WHERE id = 7") == Rows{{"7", "P", "1"}});
    REQUIRE(q(ex, "SELECT id, size, t FROM vw WHERE id <= 3 ORDER BY id") == Rows{{"1", "big", "24"}, {"2", "small", N}, {"3", "big", N}});
    REQUIRE(q(ex, "SELECT g, score FROM vw2 ORDER BY g") == Rows{{"1", "103"}, {"2", "102"}, {"3", "202"}});
    ok(ex, "CALL p_add(8, 40)");
    REQUIRE(q(ex, "SELECT note, n FROM lg WHERE id = 8") == Rows{{"big", "81"}});
}

TEST_CASE("an expression is written back so that it reads the same", "[value_expressions][parser]") {
    // the text of an aggregate's argument is read again by the executor: every kind of expression has to survive the round trip
    const char* texts[] = {
        "CASE WHEN a > 1 AND b BETWEEN 1 AND 5 THEN a * 2 ELSE NULL END",
        "CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'it''s' ELSE 'many' END",
        "COALESCE(a, b) + (c - d) * 2",
        "CASE WHEN a IN (1, 2, NULL) OR NOT (b IS NULL) THEN 1 END",
        "CAST(a + 1 AS SIGNED) - 3",
        "a - (b - c) - -5",
        "(a > 1) + (b > 2)",
        "CASE WHEN s LIKE 'a%' OR s NOT LIKE '%z' THEN 1 ELSE 0 END",
        "a / (b * c)",
    };
    for (const char* original : texts) {
        INFO(original);
        const ArithExpr first = Parser::str_to_arith(original);
        REQUIRE(!std::holds_alternative<ArithExpr::Col>(first.data)); // (read as an expression, not as a name)
        const std::string written = Parser::arith_to_string(first);
        const ArithExpr second = Parser::str_to_arith(written);
        REQUIRE(Parser::arith_to_string(second) == written);
    }
    // text that is not all one expression is not read as part of one: it stays a name (the executor then says it cannot read it)
    for (const char* text : {"a b", "a +", "a + b )", "CASE WHEN a THEN 1"}) {
        INFO(text);
        const ArithExpr e = Parser::str_to_arith(text);
        REQUIRE(std::holds_alternative<ArithExpr::Col>(e.data));
        REQUIRE(std::get<ArithExpr::Col>(e.data).name == text);
    }
}

// ---- random expressions against a reference ----------------------------------------------------------------------------------------------------

namespace {
using Cell = std::optional<long long>;
using Env = std::array<Cell, 3>; // x, y, z
using Truth = std::optional<bool>; // nullopt: unknown

// how tightly the outermost operator binds: OR 1, AND 2, NOT 3, a predicate 4, + and - 5, * 6, unary minus 7, anything else 8
struct IntExpr {
    std::string sql;
    int prec;
    std::function<Cell(const Env&)> eval;
};
struct BoolExpr {
    std::string sql;
    int prec;
    std::function<Truth(const Env&)> eval;
    bool bare = false; // a number used as a condition (`x + 1`): as a value it is the number, not 1 or 0
};

std::string wrap(const std::string& sql, int prec, int needs) { return prec < needs ? "(" + sql + ")" : sql; }

IntExpr gen_int(std::mt19937& rng, int depth, bool columns);
BoolExpr gen_bool(std::mt19937& rng, int depth, bool columns);

IntExpr gen_int(std::mt19937& rng, int depth, bool columns) {
    const int kinds = depth <= 0 ? (columns ? 3 : 2) : 15;
    switch (static_cast<int>(rng() % static_cast<unsigned>(kinds))) {
        case 0: { // a literal
            const long long k = static_cast<long long>(rng() % 12) - 2;
            return {std::to_string(k), 8, [k](const Env&) { return Cell(k); }};
        }
        case 1: { // NULL, or a literal
            if (rng() % 3 == 0) return {"NULL", 8, [](const Env&) { return Cell(); }};
            const long long k = static_cast<long long>(rng() % 8);
            return {std::to_string(k), 8, [k](const Env&) { return Cell(k); }};
        }
        case 2: { // a column
            if (!columns) return {"7", 8, [](const Env&) { return Cell(7); }};
            const std::size_t c = rng() % 3;
            const char* names[] = {"x", "y", "z"};
            return {names[c], 8, [c](const Env& e) { return e[c]; }};
        }
        case 3: { // -e
            IntExpr e = gen_int(rng, depth - 1, columns);
            return {"-" + (e.sql[0] == '-' ? "(" + e.sql + ")" : wrap(e.sql, e.prec, 8)), 7, [e](const Env& env) -> Cell {
                        Cell v = e.eval(env);
                        if (!v) return v;
                        return -*v;
                    }};
        }
        case 4: case 5: case 6: { // + - *
            IntExpr l = gen_int(rng, depth - 1, columns), r = gen_int(rng, depth - 1, columns);
            const char op = "+-*"[rng() % 3];
            const int prec = op == '*' ? 6 : 5;
            return {wrap(l.sql, l.prec, prec) + " " + op + " " + wrap(r.sql, r.prec, prec + 1), prec, [l, r, op](const Env& e) -> Cell {
                        Cell a = l.eval(e), b = r.eval(e);
                        if (!a || !b) return std::nullopt;
                        return op == '+' ? *a + *b : (op == '-' ? *a - *b : *a * *b);
                    }};
        }
        case 7: { // ABS(e), CAST(e AS SIGNED)
            IntExpr e = gen_int(rng, depth - 1, columns);
            if (rng() % 2 == 0) {
                return {"ABS(" + e.sql + ")", 8, [e](const Env& env) -> Cell {
                            Cell v = e.eval(env);
                            if (!v) return v;
                            return *v < 0 ? -*v : *v;
                        }};
            }
            return {"CAST(" + e.sql + " AS SIGNED)", 8, e.eval};
        }
        case 8: { // COALESCE / IFNULL
            IntExpr l = gen_int(rng, depth - 1, columns), r = gen_int(rng, depth - 1, columns);
            const bool coalesce = rng() % 2 == 0;
            return {std::string(coalesce ? "COALESCE(" : "IFNULL(") + l.sql + ", " + r.sql + ")", 8, [l, r](const Env& e) -> Cell {
                        Cell a = l.eval(e);
                        return a ? a : r.eval(e);
                    }};
        }
        case 9: { // NULLIF
            IntExpr l = gen_int(rng, depth - 1, columns), r = gen_int(rng, depth - 1, columns);
            return {"NULLIF(" + l.sql + ", " + r.sql + ")", 8, [l, r](const Env& e) -> Cell {
                        Cell a = l.eval(e), b = r.eval(e);
                        if (a && b && *a == *b) return std::nullopt;
                        return a;
                    }};
        }
        case 10: case 11: { // CASE WHEN .. THEN .. [WHEN ..] [ELSE ..] END
            const int arms = 1 + static_cast<int>(rng() % 3);
            std::vector<BoolExpr> whens;
            std::vector<IntExpr> thens;
            std::string sql = "CASE";
            for (int i = 0; i < arms; i++) {
                whens.push_back(gen_bool(rng, depth - 1, columns));
                thens.push_back(gen_int(rng, depth - 1, columns));
                sql += " WHEN " + whens.back().sql + " THEN " + thens.back().sql;
            }
            std::optional<IntExpr> otherwise;
            if (rng() % 3 != 0) {
                otherwise = gen_int(rng, depth - 1, columns);
                sql += " ELSE " + otherwise->sql;
            }
            return {sql + " END", 8, [whens, thens, otherwise](const Env& e) -> Cell {
                        for (std::size_t i = 0; i < whens.size(); i++) {
                            Truth t = whens[i].eval(e);
                            if (t && *t) return thens[i].eval(e);
                        }
                        return otherwise ? otherwise->eval(e) : Cell();
                    }};
        }
        case 12: { // CASE operand WHEN literal THEN ..
            IntExpr operand = gen_int(rng, depth - 1, columns);
            const int arms = 1 + static_cast<int>(rng() % 3);
            std::vector<long long> keys;
            std::vector<IntExpr> thens;
            std::string sql = "CASE " + operand.sql;
            for (int i = 0; i < arms; i++) {
                keys.push_back(static_cast<long long>(rng() % 10) - 2);
                thens.push_back(gen_int(rng, depth - 1, columns));
                sql += " WHEN " + std::to_string(keys.back()) + " THEN " + thens.back().sql;
            }
            std::optional<IntExpr> otherwise;
            if (rng() % 2 == 0) {
                otherwise = gen_int(rng, depth - 1, columns);
                sql += " ELSE " + otherwise->sql;
            }
            return {sql + " END", 8, [operand, keys, thens, otherwise](const Env& e) -> Cell {
                        Cell v = operand.eval(e);
                        if (v) {
                            for (std::size_t i = 0; i < keys.size(); i++) {
                                if (*v == keys[i]) return thens[i].eval(e);
                            }
                        }
                        return otherwise ? otherwise->eval(e) : Cell();
                    }};
        }
        case 13: { // IF(cond, a, b)
            BoolExpr c = gen_bool(rng, depth - 1, columns);
            IntExpr a = gen_int(rng, depth - 1, columns), b = gen_int(rng, depth - 1, columns);
            return {"IF(" + c.sql + ", " + a.sql + ", " + b.sql + ")", 8, [c, a, b](const Env& e) -> Cell {
                        Truth t = c.eval(e);
                        return t && *t ? a.eval(e) : b.eval(e);
                    }};
        }
        default: { // a condition as a number: 1, 0 or NULL
            BoolExpr c = gen_bool(rng, depth - 1, columns);
            // (a number used as a condition is itself where a value is read: AND TRUE makes it 1 or 0)
            return {c.bare ? "((" + c.sql + ") AND TRUE)" : "(" + c.sql + ")", 8, [c](const Env& e) -> Cell {
                        Truth t = c.eval(e);
                        if (!t) return std::nullopt;
                        return *t ? 1 : 0;
                    }};
        }
    }
}

BoolExpr gen_bool(std::mt19937& rng, int depth, bool columns) {
    const int kinds = depth <= 0 ? 4 : 13;
    switch (static_cast<int>(rng() % static_cast<unsigned>(kinds))) {
        case 0: case 1: case 2: { // a comparison
            IntExpr l = gen_int(rng, depth - 1, columns), r = gen_int(rng, depth - 1, columns);
            const int op = static_cast<int>(rng() % 6);
            const char* names[] = {"=", "<>", "<", "<=", ">", ">="};
            return {wrap(l.sql, l.prec, 5) + " " + names[op] + " " + wrap(r.sql, r.prec, 5), 4, [l, r, op](const Env& e) -> Truth {
                        Cell a = l.eval(e), b = r.eval(e);
                        if (!a || !b) return std::nullopt;
                        switch (op) {
                            case 0: return *a == *b;
                            case 1: return *a != *b;
                            case 2: return *a < *b;
                            case 3: return *a <= *b;
                            case 4: return *a > *b;
                            default: return *a >= *b;
                        }
                    }};
        }
        case 3: { // IS [NOT] NULL, IS [NOT] TRUE / FALSE
            IntExpr e = gen_int(rng, depth - 1, columns);
            const int form = static_cast<int>(rng() % 6);
            const char* tails[] = {" IS NULL", " IS NOT NULL", " IS TRUE", " IS NOT TRUE", " IS FALSE", " IS NOT FALSE"};
            return {wrap(e.sql, e.prec, 5) + tails[form], 4, [e, form](const Env& env) -> Truth {
                        Cell v = e.eval(env);
                        switch (form) {
                            case 0: return !v;
                            case 1: return static_cast<bool>(v);
                            case 2: return v && *v != 0;
                            case 3: return !(v && *v != 0);
                            case 4: return v && *v == 0;
                            default: return !(v && *v == 0);
                        }
                    }};
        }
        case 4: case 5: { // [NOT] BETWEEN lo AND hi
            IntExpr e = gen_int(rng, depth - 1, columns), lo = gen_int(rng, depth - 1, columns), hi = gen_int(rng, depth - 1, columns);
            const bool negated = rng() % 2 == 0;
            return {wrap(e.sql, e.prec, 5) + (negated ? " NOT BETWEEN " : " BETWEEN ") + wrap(lo.sql, lo.prec, 5) + " AND " + wrap(hi.sql, hi.prec, 5), 4,
                    [e, lo, hi, negated](const Env& env) -> Truth {
                        Cell v = e.eval(env), a = lo.eval(env), b = hi.eval(env);
                        Truth left = v && a ? Truth(*v >= *a) : Truth();
                        Truth right = v && b ? Truth(*v <= *b) : Truth();
                        Truth inside;
                        if ((left && !*left) || (right && !*right)) inside = false;
                        else if (left && right) inside = true;
                        if (!negated || !inside) return inside;
                        return !*inside;
                    }};
        }
        case 6: { // [NOT] IN (literals)
            IntExpr e = gen_int(rng, depth - 1, columns);
            std::vector<Cell> list;
            std::string items;
            const int n = 1 + static_cast<int>(rng() % 4);
            for (int i = 0; i < n; i++) {
                if (rng() % 6 == 0) {
                    list.push_back(std::nullopt);
                    items += (i ? ", " : "") + std::string("NULL");
                } else {
                    list.push_back(static_cast<long long>(rng() % 10) - 2);
                    items += (i ? ", " : "") + std::to_string(*list.back());
                }
            }
            const bool negated = rng() % 2 == 0;
            return {wrap(e.sql, e.prec, 5) + (negated ? " NOT IN (" : " IN (") + items + ")", 4, [e, list, negated](const Env& env) -> Truth {
                        Cell v = e.eval(env);
                        if (!v) return std::nullopt;
                        bool has_null = false, found = false;
                        for (auto& item : list) {
                            if (!item) has_null = true;
                            else if (*item == *v) found = true;
                        }
                        Truth in;
                        if (found) in = true;
                        else if (!has_null) in = false;
                        if (!negated || !in) return in;
                        return !*in;
                    }};
        }
        case 7: case 8: { // AND / OR
            BoolExpr l = gen_bool(rng, depth - 1, columns), r = gen_bool(rng, depth - 1, columns);
            const bool is_and = rng() % 2 == 0;
            const int prec = is_and ? 2 : 1;
            return {wrap(l.sql, l.prec, prec) + (is_and ? " AND " : " OR ") + wrap(r.sql, r.prec, prec + 1), prec, [l, r, is_and](const Env& e) -> Truth {
                        Truth a = l.eval(e), b = r.eval(e);
                        if (is_and) {
                            if ((a && !*a) || (b && !*b)) return false;
                            if (a && b) return true;
                            return std::nullopt;
                        }
                        if ((a && *a) || (b && *b)) return true;
                        if (a && b) return false;
                        return std::nullopt;
                    }};
        }
        case 9: { // NOT
            BoolExpr e = gen_bool(rng, depth - 1, columns);
            return {"NOT " + wrap(e.sql, e.prec, 3), 3, [e](const Env& env) -> Truth {
                        Truth t = e.eval(env);
                        if (!t) return t;
                        return !*t;
                    }};
        }
        default: { // a number as a condition
            IntExpr e = gen_int(rng, depth - 1, columns);
            return {wrap(e.sql, e.prec, 5), 5, [e](const Env& env) -> Truth {
                        Cell v = e.eval(env);
                        if (!v) return std::nullopt;
                        return *v != 0;
                    }, true};
        }
    }
}

std::string cell_text(const Cell& c) { return c ? std::to_string(*c) : N; }
std::string truth_text(const Truth& t) { return t ? (*t ? "1" : "0") : N; }
} // namespace

TEST_CASE("random expressions match a reference in every place a value goes", "[value_expressions][random]") {
    unsigned seed_count = 6; // RUSQL_FUZZ_SEEDS=60 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        TempDataDir dir("vx_random");
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, g INT, x INT, y INT, z INT)");
        ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, g INT, x INT, y INT, z INT, v INT)");
        ok(ex, "CREATE TABLE w (id INT PRIMARY KEY, v INT)");
        struct Line { int g; Env env; };
        std::vector<Line> table;
        std::string values;
        for (int i = 0; i < 40; i++) {
            Line r;
            r.g = static_cast<int>(rng() % 4);
            std::string line = "(" + std::to_string(i) + ", " + std::to_string(r.g);
            for (std::size_t c = 0; c < 3; c++) {
                if (rng() % 6 == 0) {
                    r.env[c] = std::nullopt;
                    line += ", NULL";
                } else {
                    r.env[c] = static_cast<long long>(rng() % 15) - 4;
                    line += ", " + std::to_string(*r.env[c]);
                }
            }
            table.push_back(r);
            values += (i ? ", " : "") + line + ")";
        }
        ok(ex, "INSERT INTO t VALUES " + values);
        ok(ex, "INSERT INTO u (id, g, x, y, z) VALUES " + values);
        for (int round = 0; round < 10; round++) {
            IntExpr e = gen_int(rng, 3, true);
            BoolExpr b = gen_bool(rng, 3, true);
            INFO("integer expression " << e.sql);
            INFO("condition " << b.sql);
            // as a column of the select list
            Rows ints, bools, true_ids, false_ids;
            for (std::size_t i = 0; i < table.size(); i++) {
                const std::string id = std::to_string(i);
                ints.push_back({id, cell_text(e.eval(table[i].env))});
                const Truth t = b.eval(table[i].env);
                bools.push_back({id, truth_text(t)});
                if (t && *t) true_ids.push_back({id});
                if (t && !*t) false_ids.push_back({id});
            }
            REQUIRE(q(ex, "SELECT id, " + e.sql + " FROM t ORDER BY id") == ints);
            REQUIRE(q(ex, "SELECT id, " + (b.bare ? "(" + b.sql + ") AND TRUE" : b.sql) + " FROM t ORDER BY id") == bools);
            // as a condition
            REQUIRE(q(ex, "SELECT id FROM t WHERE " + b.sql + " ORDER BY id") == true_ids);
            REQUIRE(q(ex, "SELECT id FROM t WHERE NOT (" + b.sql + ") ORDER BY id") == false_ids);
            REQUIRE(q(ex, "SELECT id FROM t WHERE (" + b.sql + ") IS NOT TRUE AND (" + b.sql + ") IS NOT FALSE ORDER BY id").size() ==
                    table.size() - true_ids.size() - false_ids.size());
            // inside a function, and compared
            {
                Rows want, want_abs;
                for (std::size_t i = 0; i < table.size(); i++) {
                    Cell v = e.eval(table[i].env);
                    if (v && *v > 5) want.push_back({std::to_string(i)});
                    if (v && std::llabs(*v) > 5) want_abs.push_back({std::to_string(i)});
                }
                REQUIRE(q(ex, "SELECT id FROM t WHERE " + e.sql + " > 5 ORDER BY id") == want);
                REQUIRE(q(ex, "SELECT id FROM t WHERE ABS(" + e.sql + ") + 0 > 5 ORDER BY id") == want_abs);
            }
            // aggregates of the expression, of a CASE over the condition, and expressions of aggregates
            {
                long long sum = 0, count = 0;
                Cell lo, hi;
                long long true_count = 0, case_sum = 0, case_count = 0;
                for (auto& r : table) {
                    Cell v = e.eval(r.env);
                    if (v) {
                        sum += *v;
                        count++;
                        lo = lo ? std::min(*lo, *v) : *v;
                        hi = hi ? std::max(*hi, *v) : *v;
                    }
                    Truth t = b.eval(r.env);
                    if (t && *t) {
                        true_count++;
                        if (v) { case_sum += *v; case_count++; }
                    }
                }
                REQUIRE(q(ex, "SELECT SUM(" + e.sql + "), COUNT(" + e.sql + "), MIN(" + e.sql + "), MAX(" + e.sql + ") FROM t") ==
                        Rows{{count ? std::to_string(sum) : N, std::to_string(count), cell_text(lo), cell_text(hi)}});
                REQUIRE(q(ex, "SELECT SUM(CASE WHEN " + b.sql + " THEN " + e.sql + " END), COUNT(CASE WHEN " + b.sql + " THEN " + e.sql + " END), COUNT(*) FROM t") ==
                        Rows{{case_count ? std::to_string(case_sum) : N, std::to_string(case_count), std::to_string(table.size())}});
                REQUIRE(q(ex, "SELECT COALESCE(SUM(CASE WHEN " + b.sql + " THEN 1 ELSE 0 END), 0) * 100 + COUNT(*) FROM t") ==
                        Rows{{std::to_string(true_count * 100 + static_cast<long long>(table.size()))}});
            }
            // per group, and the groups HAVING keeps
            {
                const long long threshold = static_cast<long long>(rng() % 20) - 5;
                Rows per_group, kept;
                for (int g = 0; g < 4; g++) {
                    long long sum = 0, count = 0, rows = 0, true_rows = 0;
                    for (auto& r : table) {
                        if (r.g != g) continue;
                        rows++;
                        Truth t = b.eval(r.env);
                        if (t && *t) true_rows++;
                        Cell v = e.eval(r.env);
                        if (t && *t && v) { sum += *v; count++; }
                    }
                    if (!rows) continue;
                    per_group.push_back({std::to_string(g), std::to_string(true_rows * 10 + rows)});
                    if (count && sum > threshold) kept.push_back({std::to_string(g)});
                }
                REQUIRE(q(ex, "SELECT g, SUM(CASE WHEN " + b.sql + " THEN 1 ELSE 0 END) * 10 + COUNT(*) FROM t GROUP BY g ORDER BY g") == per_group);
                REQUIRE(q(ex, "SELECT g FROM t GROUP BY g HAVING SUM(CASE WHEN " + b.sql + " THEN " + e.sql + " END) > " + std::to_string(threshold) + " ORDER BY g") == kept);
            }
            // UPDATE ... SET
            {
                Rows want, flag;
                for (std::size_t i = 0; i < table.size(); i++) {
                    want.push_back({std::to_string(i), cell_text(e.eval(table[i].env))});
                    Truth t = b.eval(table[i].env);
                    flag.push_back({std::to_string(i), t && *t ? "1" : "0"});
                }
                ok(ex, "UPDATE u SET v = " + e.sql);
                REQUIRE(q(ex, "SELECT id, v FROM u ORDER BY id") == want);
                ok(ex, "UPDATE u SET v = CASE WHEN " + b.sql + " THEN 1 ELSE 0 END");
                REQUIRE(q(ex, "SELECT id, v FROM u ORDER BY id") == flag);
            }
            // INSERT of an expression of constants
            {
                IntExpr k = gen_int(rng, 3, false);
                INFO("constant expression " << k.sql);
                ok(ex, "DELETE FROM w");
                ok(ex, "INSERT INTO w VALUES (1, " + k.sql + "), (2, " + k.sql + " + 1)");
                const Cell value = k.eval(Env{});
                Rows want = {{"1", cell_text(value)}, {"2", cell_text(value ? Cell(*value + 1) : Cell())}};
                REQUIRE(q(ex, "SELECT id, v FROM w ORDER BY id") == want);
            }
        }
    }
}
