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

using namespace engine;
namespace fs = std::filesystem;

// A scalar subquery is a value wherever a value goes: an operand of + - * /, an argument of a function, a CASE's result, either side of a comparison,
// a BETWEEN bound, an item of IN (...), the pattern of LIKE, a value of INSERT / UPDATE / SET, a procedure's SET / IF / WHILE. IN lists and LIKE patterns
// take any expression. A condition that is only a value (a CASE's WHEN, IF(), a select item, the ON of a JOIN) is answered when it holds a subquery
// (`CASE WHEN EXISTS (...)`, `JOIN ... ON x IN (SELECT ...)` were always false). What a subquery answered is kept for the statement and the data it was
// asked of (a procedure's loop built the next round's statement where the last one was and read the old answer).

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

std::string text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return r.value();
}

void ok(Executor& ex, const std::string& sql) { text(ex, sql); }

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
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

// the ids a query gives, one column
std::vector<std::string> ids(Executor& ex, const std::string& sql) {
    std::vector<std::string> out;
    for (auto& r : q(ex, sql)) out.push_back(r[0]);
    return out;
}
using Ids = std::vector<std::string>;

// a: (id, v, w, s, g) (1, 10, 2, 'x', 1) (2, NULL, 3, NULL, 1) (3, 30, NULL, 'z', 2) (4, 0, 0, '12', 2) (5, 7, 7, 'abc', 3)     b: (id, k, a_id) (1, 3, 1) (2, 8, 1) (3, 1, 3) (4, 5, 9)
void tables(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, v INT, w INT, s VARCHAR(20), g INT)");
    ok(ex, "INSERT INTO a VALUES (1, 10, 2, 'x', 1), (2, NULL, 3, NULL, 1), (3, 30, NULL, 'z', 2), (4, 0, 0, '12', 2), (5, 7, 7, 'abc', 3)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, k INT, a_id INT)");
    ok(ex, "INSERT INTO b VALUES (1, 3, 1), (2, 8, 1), (3, 1, 3), (4, 5, 9)");
}
} // namespace

TEST_CASE("a scalar subquery is an operand of an expression", "[expression_subqueries]") {
    TempDataDir dir("es_operand");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(q(ex, "SELECT id, (SELECT MAX(k) FROM b) + 1 FROM a ORDER BY id") == Rows{{"1", "9"}, {"2", "9"}, {"3", "9"}, {"4", "9"}, {"5", "9"}});
    REQUIRE(q(ex, "SELECT id, v + (SELECT MIN(k) FROM b) FROM a ORDER BY id") == Rows{{"1", "11"}, {"2", N}, {"3", "31"}, {"4", "1"}, {"5", "8"}});
    REQUIRE(q(ex, "SELECT id, (SELECT MAX(k) FROM b) - v FROM a ORDER BY id") == Rows{{"1", "-2"}, {"2", N}, {"3", "-22"}, {"4", "8"}, {"5", "1"}});
    REQUIRE(q(ex, "SELECT id, (SELECT MAX(k) FROM b) * w FROM a ORDER BY id") == Rows{{"1", "16"}, {"2", "24"}, {"3", N}, {"4", "0"}, {"5", "56"}});
    // no row is NULL, and NULL goes through the arithmetic
    REQUIRE(q(ex, "SELECT (SELECT k FROM b WHERE id = 99) + 1") == Rows{{N}});
    // two of them, one inside another, a subquery with an operator on each side
    REQUIRE(q(ex, "SELECT (SELECT MAX(k) FROM b) + (SELECT MIN(k) FROM b)") == Rows{{"9"}});
    REQUIRE(q(ex, "SELECT (SELECT MAX(k) + (SELECT COUNT(*) FROM b) FROM b) + 1") == Rows{{"13"}});
    REQUIRE(q(ex, "SELECT id, v + w * (SELECT MIN(k) FROM b) - (SELECT MAX(k) FROM b) FROM a ORDER BY id") ==
            Rows{{"1", "4"}, {"2", N}, {"3", N}, {"4", "-8"}, {"5", "6"}});
    // one that names the row (through the table, an alias, or an unqualified name that is the inner table's own)
    REQUIRE(q(ex, "SELECT id, v + (SELECT COUNT(*) FROM b WHERE b.a_id = a.id) FROM a ORDER BY id") == Rows{{"1", "12"}, {"2", N}, {"3", "31"}, {"4", "0"}, {"5", "7"}});
    REQUIRE(q(ex, "SELECT x.id, (SELECT SUM(k) FROM b WHERE b.a_id = x.id) * 2 FROM a x ORDER BY x.id") == Rows{{"1", "22"}, {"2", N}, {"3", "2"}, {"4", N}, {"5", N}});
    // aggregates around them
    REQUIRE(q(ex, "SELECT g, SUM(v) + (SELECT MAX(k) FROM b) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "18"}, {"2", "38"}, {"3", "15"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) FROM a GROUP BY g HAVING SUM(v) > (SELECT AVG(k) FROM b) * 2 ORDER BY g") == Rows{{"1", "10"}, {"2", "30"}});
    REQUIRE(q(ex, "SELECT COUNT(*) + (SELECT COUNT(*) FROM b) FROM a") == Rows{{"9"}});
    REQUIRE(q(ex, "SELECT g, (SELECT COUNT(*) FROM b WHERE b.a_id = a.g) + COUNT(*) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "4"}, {"2", "2"}, {"3", "2"}});
}

TEST_CASE("a scalar subquery inside functions, CASE and conditions", "[expression_subqueries]") {
    TempDataDir dir("es_functions");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(q(ex, "SELECT id, COALESCE((SELECT MAX(k) FROM b WHERE b.a_id = a.id), 0) FROM a ORDER BY id") == Rows{{"1", "8"}, {"2", "0"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
    REQUIRE(q(ex, "SELECT ROUND((SELECT AVG(k) FROM b), 1), UPPER((SELECT s FROM a WHERE id = 5)), CONCAT('n=', (SELECT COUNT(*) FROM b))") == Rows{{"4.3", "ABC", "n=4"}});
    REQUIRE(q(ex, "SELECT CASE WHEN (SELECT COUNT(*) FROM b) > 3 THEN 'many' ELSE 'few' END") == Rows{{"many"}});
    // either side of a comparison, BETWEEN bounds, a value that is a condition
    REQUIRE(ids(ex, "SELECT id FROM a WHERE (SELECT MAX(k) FROM b) > v ORDER BY id") == Ids{"4", "5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE v > (SELECT AVG(k) FROM b) * 2 ORDER BY id") == Ids{"1", "3"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE (SELECT COUNT(*) FROM b WHERE b.a_id = a.id) > 0 ORDER BY id") == Ids{"1", "3"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE NOT ((SELECT MAX(k) FROM b WHERE b.a_id = a.id) > 3) ORDER BY id") == Ids{"3"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE v BETWEEN (SELECT MIN(k) FROM b) AND (SELECT MAX(k) FROM b) ORDER BY id") == Ids{"5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE (SELECT k FROM b WHERE id = 99) IS NULL ORDER BY id").size() == 5);
    REQUIRE(ids(ex, "SELECT id FROM a WHERE (SELECT MAX(k) FROM b) IN (SELECT k FROM b) ORDER BY id").size() == 5);
    REQUIRE(ids(ex, "SELECT id FROM a WHERE (SELECT k FROM b WHERE id = 1) IN (1, 2, 3) AND id < 3 ORDER BY id") == Ids{"1", "2"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE (SELECT MAX(k) FROM b) ORDER BY id").size() == 5);
}

TEST_CASE("a condition that is only a value answers its subquery", "[expression_subqueries]") {
    TempDataDir dir("es_conditions");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(q(ex, "SELECT id, CASE WHEN EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id) THEN 'y' ELSE 'n' END FROM a ORDER BY id") ==
            Rows{{"1", "y"}, {"2", "n"}, {"3", "y"}, {"4", "n"}, {"5", "n"}});
    REQUIRE(q(ex, "SELECT id, CASE WHEN id IN (SELECT a_id FROM b) THEN 'y' ELSE 'n' END FROM a ORDER BY id") == Rows{{"1", "y"}, {"2", "n"}, {"3", "y"}, {"4", "n"}, {"5", "n"}});
    REQUIRE(q(ex, "SELECT id, CASE WHEN v > (SELECT AVG(k) FROM b) THEN 'hi' ELSE 'lo' END FROM a ORDER BY id") == Rows{{"1", "hi"}, {"2", "lo"}, {"3", "hi"}, {"4", "lo"}, {"5", "hi"}});
    REQUIRE(q(ex, "SELECT id, IF(id IN (SELECT a_id FROM b), 1, 0) FROM a ORDER BY id") == Rows{{"1", "1"}, {"2", "0"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
    REQUIRE(q(ex, "SELECT id, id IN (SELECT a_id FROM b) FROM a ORDER BY id") == Rows{{"1", "1"}, {"2", "0"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE CASE WHEN id IN (SELECT a_id FROM b) THEN 1 ELSE 0 END = 1 ORDER BY id") == Ids{"1", "3"});
    // the ON of a join (a plain equality plus a condition on a subquery)
    REQUIRE(q(ex, "SELECT a.id, b.k FROM a JOIN b ON b.a_id = a.id AND b.k > (SELECT AVG(k) FROM b) ORDER BY a.id") == Rows{{"1", "8"}});
    REQUIRE(q(ex, "SELECT a.id, b.k FROM a JOIN b ON b.a_id = a.id AND b.k IN (SELECT k FROM b WHERE k > 4) ORDER BY a.id") == Rows{{"1", "8"}});
    REQUIRE(q(ex, "SELECT a.id, b.k FROM a JOIN b ON b.a_id = a.id AND EXISTS (SELECT 1 FROM b x WHERE x.id = b.id AND x.k > 4) ORDER BY a.id") == Rows{{"1", "8"}});
    REQUIRE(q(ex, "SELECT a.id, b.k FROM a LEFT JOIN b ON b.a_id = a.id AND b.k > (SELECT AVG(k) FROM b) ORDER BY a.id") ==
            Rows{{"1", "8"}, {"2", N}, {"3", N}, {"4", N}, {"5", N}});
}

TEST_CASE("IN takes a list of expressions", "[expression_subqueries]") {
    TempDataDir dir("es_in");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(ids(ex, "SELECT id FROM a WHERE w IN (v, 3) ORDER BY id") == Ids{"2", "4", "5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE id IN (g, w + 1) ORDER BY id") == Ids{"1"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE id NOT IN (g, 2) ORDER BY id") == Ids{"3", "4", "5"});
    // a NULL makes NOT IN unknown unless a value is found, and IN true only when one matches
    REQUIRE(ids(ex, "SELECT id FROM a WHERE w NOT IN (v, 3) ORDER BY id") == Ids{"1"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE v NOT IN (w, 100) ORDER BY id") == Ids{"1"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE id NOT IN (1, (SELECT k FROM b WHERE id = 99)) ORDER BY id").empty());
    REQUIRE(ids(ex, "SELECT id FROM a WHERE id NOT IN (1, (SELECT k FROM b WHERE id = 1)) ORDER BY id") == Ids{"2", "4", "5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE id IN (1, 2, (SELECT MAX(k) FROM b)) ORDER BY id") == Ids{"1", "2"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE v IN ((SELECT MAX(k) FROM b), 10, (SELECT MIN(k) FROM b) + 6) ORDER BY id") == Ids{"1", "5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s IN ('x', CONCAT('a', 'bc')) ORDER BY id") == Ids{"1", "5"});
    // a list of plain values is what it was
    REQUIRE(ids(ex, "SELECT id FROM a WHERE v IN (10, 30, NULL) ORDER BY id") == Ids{"1", "3"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE v NOT IN (10, 30) ORDER BY id") == Ids{"4", "5"});
}

TEST_CASE("LIKE and REGEXP take an expression as the pattern", "[expression_subqueries]") {
    TempDataDir dir("es_like");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s LIKE CONCAT('a', '%') ORDER BY id") == Ids{"5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s NOT LIKE CONCAT('%', 'b', '%') ORDER BY id") == Ids{"1", "3", "4"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s REGEXP CONCAT('^', 'a') ORDER BY id") == Ids{"5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s LIKE s ORDER BY id") == Ids{"1", "3", "4", "5"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s LIKE (SELECT s FROM a WHERE id = 3) ORDER BY id") == Ids{"3"});
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s LIKE CONCAT((SELECT s FROM a WHERE id = 5), '%') ORDER BY id") == Ids{"5"});
    // a quoted pattern is a string, never the column of that name (w is a column)
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s LIKE 'w' ORDER BY id").empty());
    REQUIRE(ids(ex, "SELECT id FROM a WHERE s LIKE 'a%' ORDER BY id") == Ids{"5"});
}

TEST_CASE("subqueries in the values of INSERT, UPDATE, SET and DELETE", "[expression_subqueries]") {
    TempDataDir dir("es_write");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, n INT, m INT)");
    ok(ex, "INSERT INTO t VALUES (1, (SELECT MAX(k) FROM b), 2)");
    ok(ex, "INSERT INTO t VALUES (2, (SELECT MAX(k) FROM b) + 1, (SELECT COUNT(*) FROM b)), (3, COALESCE((SELECT k FROM b WHERE id = 99), -1), 0)");
    ok(ex, "INSERT INTO t VALUES (4, (SELECT MAX(n) FROM t) + 1, 0)"); // (reads the table it writes)
    REQUIRE(q(ex, "SELECT id, n, m FROM t ORDER BY id") == Rows{{"1", "8", "2"}, {"2", "9", "4"}, {"3", "-1", "0"}, {"4", "10", "0"}});
    // the same value for every row, and the row's own
    ok(ex, "UPDATE t SET n = (SELECT MAX(k) FROM b) + id WHERE id < 3");
    ok(ex, "UPDATE t SET m = (SELECT COUNT(*) FROM b WHERE b.a_id = t.id)");
    REQUIRE(q(ex, "SELECT id, n, m FROM t ORDER BY id") == Rows{{"1", "9", "2"}, {"2", "10", "0"}, {"3", "-1", "1"}, {"4", "10", "0"}});
    ok(ex, "UPDATE t SET n = (SELECT MAX(n) FROM t) + 1"); // (every row reads the rows as they were)
    REQUIRE(q(ex, "SELECT id, n FROM t ORDER BY id") == Rows{{"1", "11"}, {"2", "11"}, {"3", "11"}, {"4", "11"}});
    ok(ex, "DELETE FROM t WHERE (SELECT COUNT(*) FROM b WHERE b.a_id = t.id) > 0");
    REQUIRE(ids(ex, "SELECT id FROM t ORDER BY id") == Ids{"2", "4"});
    // a trigger's SET NEW.x, a user variable
    ok(ex, "CREATE TRIGGER trg BEFORE INSERT ON t FOR EACH ROW SET NEW.m = NEW.n + (SELECT COUNT(*) FROM b)");
    ok(ex, "INSERT INTO t VALUES (10, 100, 0)");
    REQUIRE(q(ex, "SELECT m FROM t WHERE id = 10") == Rows{{"104"}});
    ok(ex, "SET @sv = (SELECT MAX(k) FROM b) * 2");
    REQUIRE(q(ex, "SELECT @sv") == Rows{{"16"}});
    // the answer to the cases that were never an answer
    REQUIRE(error_of(ex, "INSERT INTO t VALUES (20, (SELECT k FROM b), 0)").find("more than 1 row") != std::string::npos);
    REQUIRE(error_of(ex, "INSERT INTO t VALUES (21, (SELECT k, id FROM b WHERE id = 1), 0)").find("Operand should contain 1 column") != std::string::npos);
    REQUIRE(error_of(ex, "INSERT INTO t VALUES (22, (SELECT nosuch FROM b WHERE id = 1), 0)").find("Unknown column") != std::string::npos);
    REQUIRE(error_of(ex, "INSERT INTO t VALUES (23, (SELECT k FROM nosuch), 0)").find("not found") != std::string::npos);
    REQUIRE(error_of(ex, "UPDATE t SET n = (SELECT k FROM b WHERE b.a_id = 1)").find("more than 1 row") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, n FROM t ORDER BY id") == Rows{{"2", "11"}, {"4", "11"}, {"10", "100"}}); // (nothing changed)
}

TEST_CASE("errors of a subquery used as a value", "[expression_subqueries]") {
    TempDataDir dir("es_errors");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(error_of(ex, "SELECT id, (SELECT k FROM b) + 1 FROM a").find("more than 1 row") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id, (SELECT k, id FROM b WHERE id = 1) + 1 FROM a").find("Operand should contain 1 column") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id, (SELECT nosuch FROM b WHERE id = 1) + 1 FROM a").find("Unknown column") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id, (SELECT k FROM nosuch) + 1 FROM a").find("not found") != std::string::npos);
    // where the expression is kept as text (GROUP BY, ORDER BY, the argument of an aggregate) it is refused, never a wrong answer
    REQUIRE(error_of(ex, "SELECT g, SUM(v + (SELECT MAX(k) FROM b)) FROM a GROUP BY g").find("not supported") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id FROM a ORDER BY v + (SELECT MAX(k) FROM b)").find("not supported") != std::string::npos);
}

TEST_CASE("subqueries in stored routines, views and after a restart", "[expression_subqueries]") {
    TempDataDir dir("es_routines");
    {
        Executor ex(dir.path);
        open_db(ex);
        tables(ex);
        ok(ex, "CREATE VIEW va AS SELECT id, v + (SELECT MAX(k) FROM b) AS vk FROM a");
        REQUIRE(q(ex, "SELECT id, vk FROM va ORDER BY id") == Rows{{"1", "18"}, {"2", N}, {"3", "38"}, {"4", "8"}, {"5", "15"}});
        ok(ex, "INSERT INTO b VALUES (9, 100, 5)"); // (a view shows the rows as they are)
        REQUIRE(q(ex, "SELECT id, vk FROM va ORDER BY id") == Rows{{"1", "110"}, {"2", N}, {"3", "130"}, {"4", "100"}, {"5", "107"}});
        ok(ex, "DELETE FROM b WHERE id = 9");
        ok(ex, "CREATE FUNCTION plus_max(x) RETURNS INT RETURN x + (SELECT MAX(k) FROM b)");
        REQUIRE(q(ex, "SELECT id, plus_max(id) FROM a WHERE id < 3 ORDER BY id") == Rows{{"1", "9"}, {"2", "10"}});
        // each round of a loop reads the data as it is, whatever the last round's statement left (the writes of the body, the values in it)
        ok(ex, "CREATE PROCEDURE grow() BEGIN DECLARE i INT DEFAULT 0; DECLARE c INT DEFAULT 0; DECLARE total INT DEFAULT 0; "
               "WHILE i < 3 DO INSERT INTO b VALUES (200 + i, 1, 2 + i); SELECT COUNT(*) INTO c FROM a WHERE id IN (SELECT a_id FROM b); "
               "SET total = total * 10 + c; SET i = i + 1; END WHILE; SELECT total; END");
        REQUIRE(q(ex, "CALL grow()") == Rows{{"334"}});
        ok(ex, "CREATE PROCEDURE steps() BEGIN DECLARE i INT DEFAULT 0; DECLARE c INT DEFAULT 0; DECLARE total INT DEFAULT 0; "
               "WHILE i < 3 DO SELECT COUNT(*) INTO c FROM a WHERE id IN (SELECT a_id FROM b WHERE k >= i * 4); "
               "SET total = total * 10 + c; SET i = i + 1; END WHILE; SELECT total; END");
        REQUIRE(q(ex, "CALL steps()") == Rows{{"411"}}); // (k >= 0: a_id 1, 3, 9, 2, 3, 4 -> 4 ids of a; k >= 4: 1 and 9 -> 1; k >= 8: 1)
        ok(ex, "CREATE PROCEDURE drain() BEGIN DECLARE i INT DEFAULT 0; WHILE EXISTS (SELECT 1 FROM b WHERE k > 3) DO UPDATE b SET k = k - 1 WHERE k > 3; "
               "SET i = i + 1; END WHILE; SELECT i; END");
        REQUIRE(q(ex, "CALL drain()") == Rows{{"5"}});
        ok(ex, "CREATE PROCEDURE size_of() BEGIN DECLARE i INT DEFAULT 0; WHILE i < (SELECT COUNT(*) FROM b) DO SET i = i + 1; END WHILE; SELECT i; END");
        REQUIRE(q(ex, "CALL size_of()") == Rows{{"7"}});
        ok(ex, "CREATE PROCEDURE pick() BEGIN DECLARE r VARCHAR(10) DEFAULT 'none'; IF (SELECT COUNT(*) FROM b) > 100 THEN SET r = 'big'; "
               "ELSEIF EXISTS (SELECT 1 FROM b WHERE k = 1) THEN SET r = 'one'; ELSE SET r = 'other'; END IF; SELECT r; END");
        REQUIRE(q(ex, "CALL pick()") == Rows{{"one"}});
        ok(ex, "CREATE PROCEDURE plus() BEGIN DECLARE m INT DEFAULT 0; SET m = (SELECT MAX(k) FROM b) + 10; SELECT m; END");
        REQUIRE(q(ex, "CALL plus()") == Rows{{"13"}});
    }
    Executor ex(dir.path);
    ok(ex, "USE d");
    REQUIRE(q(ex, "SELECT id, vk FROM va WHERE id < 3 ORDER BY id") == Rows{{"1", "13"}, {"2", N}});
    REQUIRE(q(ex, "CALL plus()") == Rows{{"13"}});
    REQUIRE(q(ex, "CALL size_of()") == Rows{{"7"}});
    REQUIRE(q(ex, "SELECT plus_max(1)") == Rows{{"4"}});
}

TEST_CASE("a scan over many rows with subqueries in its condition", "[expression_subqueries]") {
    TempDataDir dir("es_parallel");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, v INT)");
    std::string values;
    for (int i = 1; i <= 12000; i++) values += (values.empty() ? "" : ", ") + std::string("(") + std::to_string(i) + ", " + std::to_string(i % 10) + ")";
    ok(ex, "INSERT INTO big VALUES " + values);
    // v is 0..9 (1200 rows of each): v >= 7 is 3 of 10 rows, v in (1, 3, 9) is 3 of 10, v + 1 > 8 is 2 of 10
    REQUIRE(q(ex, "SELECT COUNT(*) FROM big WHERE v >= (SELECT MAX(k) FROM b) - 1") == Rows{{"3600"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM big WHERE CASE WHEN v IN (SELECT a_id FROM b) THEN 1 ELSE 0 END = 1") == Rows{{"3600"}}); // (a_id: 1, 1, 3, 9)
    REQUIRE(q(ex, "SELECT COUNT(*) FROM big WHERE v + (SELECT MIN(k) FROM b) > 8") == Rows{{"2400"}});
}

// ---- random expressions against a reference ------------------------------------------------------------------------------------------------

namespace {
using Cell = std::optional<long long>;
using Truth = std::optional<bool>;

struct Env { // one row of a, and the rows of b
    long long id;
    Cell v, w;
    const std::vector<std::array<Cell, 3>>* b; // (id, k, a_id)
};

struct Expr {
    std::string sql;
    std::function<Cell(const Env&)> value;
};

std::string cell_sql(const Cell& c) { return c ? std::to_string(*c) : std::string("NULL"); }

Expr random_expr(std::mt19937& rng, int depth) {
    const unsigned kind = rng() % (depth <= 0 ? 4 : 7);
    switch (kind) {
        case 0: return {"v", [](const Env& e) { return e.v; }};
        case 1: return {"w", [](const Env& e) { return e.w; }};
        case 2: {
            const long long k = static_cast<long long>(rng() % 10);
            return {std::to_string(k), [k](const Env&) { return Cell(k); }};
        }
        case 3: {
            switch (rng() % 6) {
                case 0: return {"(SELECT MAX(k) FROM b)", [](const Env& e) {
                                    Cell m;
                                    for (auto& r : *e.b) if (r[1] && (!m || *r[1] > *m)) m = r[1];
                                    return m;
                                }};
                case 1: return {"(SELECT MIN(k) FROM b)", [](const Env& e) {
                                    Cell m;
                                    for (auto& r : *e.b) if (r[1] && (!m || *r[1] < *m)) m = r[1];
                                    return m;
                                }};
                case 2: return {"(SELECT COUNT(*) FROM b)", [](const Env& e) { return Cell(static_cast<long long>(e.b->size())); }};
                case 3: return {"(SELECT COUNT(*) FROM b WHERE b.a_id = a.id)", [](const Env& e) {
                                    long long n = 0;
                                    for (auto& r : *e.b) if (r[2] && *r[2] == e.id) n++;
                                    return Cell(n);
                                }};
                case 4: return {"(SELECT SUM(k) FROM b WHERE b.a_id = a.id)", [](const Env& e) {
                                    Cell sum;
                                    for (auto& r : *e.b) {
                                        if (r[2] && *r[2] == e.id && r[1]) sum = sum ? Cell(*sum + *r[1]) : r[1];
                                    }
                                    return sum;
                                }};
                default: return {"(SELECT k FROM b WHERE id = 99)", [](const Env&) { return Cell(); }};
            }
        }
        default: {
            Expr a = random_expr(rng, depth - 1), b = random_expr(rng, depth - 1);
            switch (rng() % 4) {
                case 0: return {"(" + a.sql + " + " + b.sql + ")", [a, b](const Env& e) { Cell x = a.value(e), y = b.value(e); return x && y ? Cell(*x + *y) : Cell(); }};
                case 1: return {"(" + a.sql + " - " + b.sql + ")", [a, b](const Env& e) { Cell x = a.value(e), y = b.value(e); return x && y ? Cell(*x - *y) : Cell(); }};
                case 2: return {"(" + a.sql + " * " + b.sql + ")", [a, b](const Env& e) { Cell x = a.value(e), y = b.value(e); return x && y ? Cell(*x * *y) : Cell(); }};
                default: return {"COALESCE(" + a.sql + ", " + b.sql + ")", [a, b](const Env& e) { Cell x = a.value(e); return x ? x : b.value(e); }};
            }
        }
    }
}

Truth compare(const std::string& op, const Cell& x, const Cell& y) {
    if (!x || !y) return std::nullopt;
    if (op == "=") return *x == *y;
    if (op == "<>") return *x != *y;
    if (op == "<") return *x < *y;
    if (op == "<=") return *x <= *y;
    if (op == ">") return *x > *y;
    return *x >= *y;
}
} // namespace

TEST_CASE("random expressions with subqueries match a reference", "[expression_subqueries][random]") {
    unsigned seed_count = 6; // RUSQL_FUZZ_SEEDS=60 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        TempDataDir dir("es_random");
        Executor ex(dir.path);
        open_db(ex);
        for (int round = 0; round < 12; round++) {
            ok(ex, "DROP TABLE IF EXISTS a");
            ok(ex, "DROP TABLE IF EXISTS b");
            ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, v INT, w INT)");
            ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, k INT, a_id INT)");
            std::vector<Env> rows;
            std::vector<std::array<Cell, 3>> brows;
            std::string avalues, bvalues;
            for (long long id = 1; id <= 8; id++) {
                Cell v = rng() % 5 == 0 ? Cell() : Cell(static_cast<long long>(rng() % 12)), w = rng() % 5 == 0 ? Cell() : Cell(static_cast<long long>(rng() % 12));
                rows.push_back(Env{id, v, w, &brows});
                avalues += (avalues.empty() ? "" : ", ") + std::string("(") + std::to_string(id) + ", " + cell_sql(v) + ", " + cell_sql(w) + ")";
            }
            for (long long id = 1; id <= static_cast<long long>(rng() % 6); id++) {
                std::array<Cell, 3> r{Cell(id), rng() % 6 == 0 ? Cell() : Cell(static_cast<long long>(rng() % 12)), Cell(static_cast<long long>(1 + rng() % 8))};
                brows.push_back(r);
                bvalues += (bvalues.empty() ? "" : ", ") + std::string("(") + cell_sql(r[0]) + ", " + cell_sql(r[1]) + ", " + cell_sql(r[2]) + ")";
            }
            ok(ex, "INSERT INTO a VALUES " + avalues);
            if (!bvalues.empty()) ok(ex, "INSERT INTO b VALUES " + bvalues);

            // values in the select list
            Expr e = random_expr(rng, 2);
            Rows want;
            for (auto& r : rows) want.push_back({std::to_string(r.id), cell_sql(e.value(r))});
            INFO(e.sql);
            REQUIRE(q(ex, "SELECT id, " + e.sql + " FROM a ORDER BY id") == want);

            // a comparison of two of them, either side may be a subquery
            static const char* ops[] = {"=", "<>", "<", "<=", ">", ">="};
            Expr x = random_expr(rng, 2), y = random_expr(rng, 2);
            const std::string op = ops[rng() % 6];
            Ids expected;
            for (auto& r : rows) {
                if (compare(op, x.value(r), y.value(r)) == true) expected.push_back(std::to_string(r.id));
            }
            INFO(x.sql << " " << op << " " << y.sql);
            REQUIRE(ids(ex, "SELECT id FROM a WHERE " + x.sql + " " + op + " " + y.sql + " ORDER BY id") == expected);

            // IN and NOT IN over expressions (the answer of three comparisons)
            Expr l1 = random_expr(rng, 1), l2 = random_expr(rng, 1), l3 = random_expr(rng, 1);
            for (bool negated : {false, true}) {
                Ids in_expected;
                for (auto& r : rows) {
                    Truth t = negated ? Truth(true) : Truth(false);
                    for (const Expr* item : {&l1, &l2, &l3}) {
                        Truth c = compare(negated ? "<>" : "=", x.value(r), item->value(r));
                        if (negated) t = (t == false || c == false) ? Truth(false) : ((!t || !c) ? Truth() : Truth(true));
                        else t = (t == true || c == true) ? Truth(true) : ((!t || !c) ? Truth() : Truth(false));
                    }
                    if (t == true) in_expected.push_back(std::to_string(r.id));
                }
                const std::string sql = "SELECT id FROM a WHERE " + x.sql + (negated ? " NOT IN (" : " IN (") + l1.sql + ", " + l2.sql + ", " + l3.sql + ") ORDER BY id";
                INFO(sql);
                REQUIRE(ids(ex, sql) == in_expected);
            }

            // UPDATE SET to the value
            Expr u = random_expr(rng, 2);
            ok(ex, "UPDATE a SET w = " + u.sql);
            Rows after;
            for (auto& r : rows) after.push_back({std::to_string(r.id), cell_sql(u.value(r))});
            INFO(u.sql);
            REQUIRE(q(ex, "SELECT id, w FROM a ORDER BY id") == after);
        }
    }
}
