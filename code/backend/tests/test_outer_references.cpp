#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A subquery that names a column of the query around it (`WHERE b.a_id = a.id`) is answered for every row of that query with the row's value. It used
// to be found by the shape of the text -- a dotted name on the right of a comparison -- so a column on the left (`WHERE a.id = b.a_id`) was true for every
// row (a DELETE with it deleted every row), and so were a column inside a function, in the select list, an alias of the outer table, a column named
// without its table, and a subquery inside a subquery.

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

std::vector<std::string> first_column(Executor& ex, const std::string& sql) {
    std::vector<std::string> out;
    for (auto& r : q(ex, sql)) out.push_back(r[0]);
    return out;
}

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

// a: ids 1..5; v: 10, NULL, 30, 0, 7; w: 2, 3, NULL, 0, 7; g: 1, 1, 2, 2, 3     b: (id, k, a_id) = (1, 3, 1) (2, 8, 1) (3, 1, 3) (4, 5, 9)
void tables_ab(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, v INT, w INT, d DOUBLE, s VARCHAR(20), g INT)");
    ok(ex, "INSERT INTO a VALUES (1, 10, 2, 1.5, 'x', 1), (2, NULL, 3, NULL, NULL, 1), (3, 30, NULL, 2.5, 'z', 2), (4, 0, 0, 0, '12', 2), (5, 7, 7, 3.25, 'abc', 3)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, k INT, a_id INT)");
    ok(ex, "INSERT INTO b VALUES (1, 3, 1), (2, 8, 1), (3, 1, 3), (4, 5, 9)");
}
using V = std::vector<std::string>;
} // namespace

TEST_CASE("an outer column on either side of a comparison", "[outer_references]") {
    TempDataDir dir("or_sides");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    for (const char* cond : {"a.id = b.a_id", "b.a_id = a.id"}) {
        INFO(cond);
        REQUIRE(first_column(ex, std::string("SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE ") + cond + ") ORDER BY id") == V{"1", "3"});
        REQUIRE(first_column(ex, std::string("SELECT id FROM a WHERE NOT EXISTS (SELECT 1 FROM b WHERE ") + cond + ") ORDER BY id") == V{"2", "4", "5"});
        REQUIRE(q(ex, std::string("SELECT id, (SELECT COUNT(*) FROM b WHERE ") + cond + ") FROM a ORDER BY id") ==
                Rows{{"1", "2"}, {"2", "0"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
        REQUIRE(first_column(ex, std::string("SELECT id FROM a WHERE v > (SELECT MIN(k) FROM b WHERE ") + cond + ") ORDER BY id") == V{"1", "3"});
    }
    // an inequality, the outer column on either side
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE id IN (SELECT a_id FROM b WHERE b.k < a.v) ORDER BY id") == V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE id IN (SELECT a_id FROM b WHERE a.v > b.k) ORDER BY id") == V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.v <= b.k) ORDER BY id") == V{"4", "5"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.v <> b.k AND a.id = b.a_id) ORDER BY id") == V{"1", "3"});
    // both columns are the outer ones: the subquery is the same for the whole row
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.id = a.g AND b.id = 1) ORDER BY id") == V{"1"});
    // an outer NULL compares as unknown: no row
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.w = b.k) ORDER BY id") == V{"2"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = a.w) ORDER BY id") == V{"2"});
}

TEST_CASE("an outer column inside a function, an expression, a CASE, a select list, HAVING, ON", "[outer_references]") {
    TempDataDir dir("or_expressions");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    for (const char* cond : {"b.a_id = ABS(a.id)", "ABS(a.id) = b.a_id", "a.id + 0 = b.a_id", "b.a_id = a.id * 1", "b.a_id = CASE WHEN a.id > 0 THEN a.id ELSE 0 END",
                             "CASE WHEN a.id > 0 THEN a.id ELSE 0 END = b.a_id", "b.a_id = CASE WHEN a.v > 5 THEN a.id ELSE 0 END",
                             "CASE WHEN a.v > 5 THEN a.id ELSE 0 END = b.a_id", "b.a_id = IF(a.v > 5, a.id, 0)"}) {
        INFO(cond);
        REQUIRE(first_column(ex, std::string("SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE ") + cond + ") ORDER BY id") == V{"1", "3"});
    }
    // a NULL passes through the function (a3's w is NULL: COALESCE gives 1, and b3 has k = 1)
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = COALESCE(a.w, 1)) ORDER BY id") == V{"2", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE COALESCE(a.w, 1) = b.k) ORDER BY id") == V{"2", "3"});
    // the select list of the subquery
    REQUIRE(q(ex, "SELECT id, (SELECT a.v + b.k FROM b WHERE b.id = 1) FROM a ORDER BY id") == Rows{{"1", "13"}, {"2", N}, {"3", "33"}, {"4", "3"}, {"5", "10"}});
    REQUIRE(q(ex, "SELECT id, (SELECT a.v FROM b WHERE b.id = 1) FROM a ORDER BY id") == Rows{{"1", "10"}, {"2", N}, {"3", "30"}, {"4", "0"}, {"5", "7"}});
    REQUIRE(q(ex, "SELECT id, (SELECT a.v * 2 + b.k FROM b WHERE b.id = 2) AS t FROM a ORDER BY id") == Rows{{"1", "28"}, {"2", N}, {"3", "68"}, {"4", "8"}, {"5", "22"}});
    // HAVING: the groups of b by a_id have 2, 1, 1 rows
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b GROUP BY b.a_id HAVING COUNT(*) >= a.w) ORDER BY id") == V{"1", "4"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b GROUP BY b.a_id HAVING a.w <= COUNT(*)) ORDER BY id") == V{"1", "4"});
    // ON and WHERE of a join inside the subquery
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b JOIN a x ON x.id = b.a_id WHERE x.g = a.g AND b.k > 2) ORDER BY id") == V{"1", "2"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b JOIN a x ON x.g = a.g AND x.id = b.a_id WHERE b.k > 2) ORDER BY id") == V{"1", "2"});
}

TEST_CASE("which table an outer column belongs to", "[outer_references]") {
    TempDataDir dir("or_names");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    // the same column name inside and outside: the qualifier tells them apart
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.id = a.id AND b.k > 2) ORDER BY id") == V{"1", "2", "4"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.id = b.id AND b.k > 2) ORDER BY id") == V{"1", "2", "4"});
    // a column named without its table: the subquery's own tables come first, then the query around it
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = w) ORDER BY id") == V{"2"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE w = b.k) ORDER BY id") == V{"2"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE id = 3 AND k = 1) ORDER BY id") == V{"1", "2", "3", "4", "5"}); // (`id` is b's own)
    // an alias of the outer table
    REQUIRE(first_column(ex, "SELECT x.id FROM a x WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = x.id) ORDER BY x.id") == V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT x.id FROM a x WHERE EXISTS (SELECT 1 FROM b WHERE x.id = b.a_id) ORDER BY x.id") == V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT x.id FROM a AS x WHERE EXISTS (SELECT 1 FROM b y WHERE x.id = y.a_id AND y.k > 2) ORDER BY x.id") == V{"1"});
    // the same table inside and outside, in every combination of aliases
    ok(ex, "CREATE TABLE e (id INT PRIMARY KEY, dept INT, sal INT)");
    ok(ex, "INSERT INTO e VALUES (1, 10, 100), (2, 10, 300), (3, 20, 200), (4, 20, 200), (5, 30, 500)");
    for (const char* query : {"SELECT id FROM e WHERE sal > (SELECT AVG(sal) FROM e e2 WHERE e2.dept = e.dept) ORDER BY id",
                              "SELECT id FROM e WHERE sal > (SELECT AVG(sal) FROM e e2 WHERE e.dept = e2.dept) ORDER BY id",
                              "SELECT id FROM e x WHERE sal > (SELECT AVG(sal) FROM e WHERE dept = x.dept) ORDER BY id",
                              "SELECT id FROM e x WHERE sal > (SELECT AVG(sal) FROM e WHERE x.dept = e.dept) ORDER BY id",
                              "SELECT x.id FROM e x WHERE x.sal > (SELECT AVG(y.sal) FROM e y WHERE y.dept = x.dept) ORDER BY x.id",
                              "SELECT e1.id FROM e e1 WHERE e1.sal > (SELECT AVG(e2.sal) FROM e e2 WHERE e2.dept = e1.dept) ORDER BY e1.id",
                              "SELECT id FROM e WHERE EXISTS (SELECT 1 FROM e e2 WHERE e2.dept = e.dept AND e2.sal < e.sal) ORDER BY id"}) {
        INFO(query);
        // (the department's average is 200: only 300 is above it; the average of everyone, 260, would add 500)
        REQUIRE(first_column(ex, query) == V{"2"});
    }
    // a row with a colleague who earns more

    REQUIRE(first_column(ex, "SELECT id FROM e WHERE EXISTS (SELECT 1 FROM e e2 WHERE e2.dept = e.dept AND e2.sal > e.sal) ORDER BY id") == V{"1"});
    // the subquery's table is the outer one: the inner name is the subquery's, so `e.dept = e.dept` is not correlated, and the alias is
    REQUIRE(first_column(ex, "SELECT id FROM e WHERE sal > (SELECT AVG(sal) FROM e WHERE e.dept = e.dept) ORDER BY id") == V{"2", "5"});
}

TEST_CASE("an outer column in the select list of the query, and next to a date unit", "[outer_references]") {
    TempDataDir dir("or_select_list");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    // (the subquery comes before the FROM list of the query around it)
    REQUIRE(q(ex, "SELECT id, (SELECT COUNT(*) FROM a a2 WHERE a2.g = a.g) FROM a ORDER BY id") == Rows{{"1", "2"}, {"2", "2"}, {"3", "2"}, {"4", "2"}, {"5", "1"}});
    REQUIRE(q(ex, "SELECT x.id, (SELECT COUNT(*) FROM a WHERE a.g = x.g) FROM a x ORDER BY x.id") == Rows{{"1", "2"}, {"2", "2"}, {"3", "2"}, {"4", "2"}, {"5", "1"}});
    REQUIRE(q(ex, "SELECT id, (SELECT COUNT(*) FROM a a2 WHERE a.g = a2.g AND a2.id < a.id) FROM a ORDER BY id") == Rows{{"1", "0"}, {"2", "1"}, {"3", "0"}, {"4", "1"}, {"5", "0"}});
    // a word that is the unit of a date function is a unit, not the column of that name of the query around
    ok(ex, "CREATE TABLE dd (id INT PRIMARY KEY, MONTH INT, DAY INT)");
    ok(ex, "INSERT INTO dd VALUES (1, 5, 5), (2, 6, 6)");
    ok(ex, "CREATE TABLE ev (id INT PRIMARY KEY, d DATE)");
    ok(ex, "INSERT INTO ev VALUES (1, '2024-01-15'), (2, '2024-02-15')");
    REQUIRE(first_column(ex, "SELECT id FROM dd WHERE EXISTS (SELECT 1 FROM ev WHERE ev.id = dd.id AND DATE_ADD(ev.d, INTERVAL 1 MONTH) = '2024-02-15') ORDER BY id") == V{"1"});
    REQUIRE(first_column(ex, "SELECT id FROM dd WHERE EXISTS (SELECT 1 FROM ev WHERE ev.id = dd.id AND DATE_SUB(ev.d, INTERVAL 1 DAY) = '2024-01-14') ORDER BY id") == V{"1"});
    // ... and the amount may be the outer column
    REQUIRE(first_column(ex, "SELECT id FROM dd WHERE EXISTS (SELECT 1 FROM ev WHERE ev.id = dd.id AND DATE_ADD(ev.d, INTERVAL dd.DAY DAY) = '2024-01-20') ORDER BY id") == V{"1"});
}

TEST_CASE("subqueries inside subqueries", "[outer_references]") {
    TempDataDir dir("or_levels");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    // a2 is reached from the innermost query
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id AND EXISTS (SELECT 1 FROM a y WHERE y.id = b.a_id AND y.v > a.w)) ORDER BY id") == V{"1"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.id = b.a_id AND EXISTS (SELECT 1 FROM a y WHERE b.a_id = y.id AND a.w < y.v)) ORDER BY id") == V{"1"});
    // three levels, each naming the one before and the first
    REQUIRE(first_column(ex,
                         "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id AND EXISTS (SELECT 1 FROM a y WHERE y.id = b.a_id AND "
                         "EXISTS (SELECT 1 FROM b z WHERE z.a_id = y.id AND z.k >= y.id - 2 AND z.id <= a.id + 1))) ORDER BY id") == V{"1", "3"});
    // an uncorrelated subquery inside a correlated one is the same for every row; one that names the outer row is not
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id AND b.k > (SELECT MIN(k) FROM b b3)) ORDER BY id") == V{"1"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id AND b.k >= (SELECT MAX(b3.k) FROM b b3 WHERE b3.a_id = a.id)) ORDER BY id") == V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k > (SELECT MIN(b3.k) FROM b b3 WHERE b3.a_id = a.id)) ORDER BY id") == V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE 0 < (SELECT COUNT(*) FROM b WHERE b.a_id = a.id AND b.k > (SELECT AVG(b3.k) FROM b b3)) ORDER BY id") == V{"1"});
    // two sibling subqueries with a subquery each (their copies must not answer for each other)
    REQUIRE(first_column(ex,
                         "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id AND b.k > (SELECT MIN(b3.k) FROM b b3 WHERE b3.a_id = a.id)) "
                         "AND EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id AND b.k < (SELECT MAX(b4.k) FROM b b4 WHERE b4.a_id = a.id)) ORDER BY id") == V{"1"});
}

TEST_CASE("UPDATE and DELETE with a subquery that names the row", "[outer_references]") {
    TempDataDir dir("or_writes");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    // (this used to delete every row)
    ok(ex, "DELETE FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.id = b.a_id)");
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY id") == V{"2", "4", "5"});
    ok(ex, "DROP TABLE a");
    ok(ex, "DROP TABLE b");
    tables_ab(ex);
    ok(ex, "DELETE FROM a WHERE NOT EXISTS (SELECT 1 FROM b WHERE a.id = b.a_id)");
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY id") == V{"1", "3"});
    ok(ex, "DROP TABLE a");
    ok(ex, "DROP TABLE b");
    tables_ab(ex);
    ok(ex, "UPDATE a SET w = 100 WHERE EXISTS (SELECT 1 FROM b WHERE a.id = b.a_id AND b.k > 5)");
    REQUIRE(q(ex, "SELECT id, w FROM a ORDER BY id") == Rows{{"1", "100"}, {"2", "3"}, {"3", N}, {"4", "0"}, {"5", "7"}});
    ok(ex, "UPDATE a SET w = 0 WHERE a.id IN (SELECT a_id FROM b WHERE a.v > b.k)");
    REQUIRE(q(ex, "SELECT id, w FROM a ORDER BY id") == Rows{{"1", "0"}, {"2", "3"}, {"3", "0"}, {"4", "0"}, {"5", "7"}});
    ok(ex, "UPDATE a SET v = v + 1 WHERE v > (SELECT MIN(k) FROM b WHERE a.id = b.a_id)");
    REQUIRE(q(ex, "SELECT id, v FROM a ORDER BY id") == Rows{{"1", "11"}, {"2", N}, {"3", "31"}, {"4", "0"}, {"5", "7"}});
    // the rows the DELETE keeps are the ones the same SELECT does not return
    ok(ex, "DELETE FROM a WHERE a.g = (SELECT MIN(b.k) FROM b WHERE b.a_id = a.id)");
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY id") == V{"1", "2", "3", "4", "5"}); // (no row's g is the smallest k of its b rows)
    ok(ex, "DELETE FROM a WHERE a.v = (SELECT MAX(b.k) + 3 FROM b WHERE b.a_id = a.id)");
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY id") == V{"2", "3", "4", "5"}); // (a1: v is 11 now, the largest k of its rows 8)
}

TEST_CASE("an outer column of another type", "[outer_references]") {
    TempDataDir dir("or_types");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE t1 (id INT PRIMARY KEY, code VARCHAR(10))");
    ok(ex, "CREATE TABLE t2 (id INT PRIMARY KEY, code VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO t1 VALUES (1, '007'), (2, '7'), (3, 'abc'), (4, NULL)");
    ok(ex, "INSERT INTO t2 VALUES (1, '007', 1), (2, '7', 2), (3, '7.0', 2), (4, 'abc', 9)");
    // two strings compare as strings: '007', '7' and '7.0' are three different keys
    for (const char* cond : {"t2.code = t1.code", "t1.code = t2.code"}) {
        INFO(cond);
        REQUIRE(first_column(ex, std::string("SELECT id FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE ") + cond + ") ORDER BY id") == V{"1", "2", "3"});
        REQUIRE(q(ex, std::string("SELECT id, (SELECT COUNT(*) FROM t2 WHERE ") + cond + ") FROM t1 ORDER BY id") == Rows{{"1", "1"}, {"2", "1"}, {"3", "1"}, {"4", "0"}});
    }
    // two numbers as numbers; a number and a string as numbers
    for (const char* cond : {"t2.n = t1.id", "t1.id = t2.n"}) {
        INFO(cond);
        REQUIRE(first_column(ex, std::string("SELECT id FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE ") + cond + ") ORDER BY id") == V{"1", "2"});
    }
    REQUIRE(first_column(ex, "SELECT id FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t1.id + 6 = t2.code) ORDER BY id") == V{"1"});
    REQUIRE(first_column(ex, "SELECT id FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.code = t1.id + 6) ORDER BY id") == V{"1"});
    // a string of the outer row that looks like a column name is a string
    ok(ex, "CREATE TABLE t3 (id INT PRIMARY KEY, code VARCHAR(10), word VARCHAR(10))");
    ok(ex, "INSERT INTO t3 VALUES (1, 'code', 'x'), (2, 'word', 'word'), (3, 'zzz', 'q')");
    REQUIRE(first_column(ex, "SELECT id FROM t3 WHERE EXISTS (SELECT 1 FROM t1 WHERE t1.code = t3.code) ORDER BY id") == V{});
    REQUIRE(first_column(ex, "SELECT t1.id FROM t1 WHERE EXISTS (SELECT 1 FROM t3 WHERE t3.word = t1.code OR t3.code = t1.code) ORDER BY t1.id") == V{});
    REQUIRE(first_column(ex, "SELECT id FROM t3 WHERE EXISTS (SELECT 1 FROM t3 t WHERE t.word = t3.code AND t.id <> t3.id) ORDER BY id") == V{});
    REQUIRE(first_column(ex, "SELECT id FROM t3 WHERE EXISTS (SELECT 1 FROM t3 t WHERE t.word = t3.code) ORDER BY id") == V{"2"});
}

TEST_CASE("a place that cannot hold an outer value says so", "[outer_references]") {
    TempDataDir dir("or_refused");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    REQUIRE(error_of(ex, "SELECT id, (SELECT SUM(b.k * a.w) FROM b) FROM a").find("not supported") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id, (SELECT SUM(a.w) FROM b) FROM a").find("not supported") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b ORDER BY a.w)").find("not supported") != std::string::npos);
    // a column that is nobody's is still an unknown column
    REQUIRE(error_of(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = a.nosuch)").find("Unknown column") != std::string::npos);
}

TEST_CASE("subqueries of views, LATERAL joins and set operations", "[outer_references]") {
    TempDataDir dir("or_forms");
    {
        Executor ex(dir.path);
        open_db(ex);
        tables_ab(ex);
        ok(ex, "CREATE VIEW has_b AS SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.id = b.a_id)");
        ok(ex, "CREATE VIEW no_b AS SELECT x.id AS id FROM a x WHERE NOT EXISTS (SELECT 1 FROM b WHERE x.id = b.a_id)");
        ok(ex, "CREATE VIEW same_t AS SELECT id FROM a WHERE v > (SELECT AVG(v) FROM a a2 WHERE a2.g = a.g)");
        REQUIRE(first_column(ex, "SELECT id FROM has_b ORDER BY id") == V{"1", "3"});
    }
    Executor ex(dir.path);
    ok(ex, "USE d");
    REQUIRE(first_column(ex, "SELECT id FROM has_b ORDER BY id") == V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM no_b ORDER BY id") == V{"2", "4", "5"});
    REQUIRE(first_column(ex, "SELECT id FROM same_t ORDER BY id") == V{"3"});
    ok(ex, "INSERT INTO b VALUES (5, 2, 2)");
    REQUIRE(first_column(ex, "SELECT id FROM has_b ORDER BY id") == V{"1", "2", "3"});
    // LATERAL: the subquery of every row of the left side
    REQUIRE(q(ex, "SELECT a.id, d.c FROM a JOIN LATERAL (SELECT COUNT(*) AS c FROM b WHERE b.a_id = a.id) d ON 1 = 1 ORDER BY a.id") ==
            Rows{{"1", "2"}, {"2", "1"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
    REQUIRE(q(ex, "SELECT a.id, d.m FROM a JOIN LATERAL (SELECT MAX(k) AS m FROM b WHERE a.id = b.a_id) d ON 1 = 1 WHERE a.id < 4 ORDER BY a.id") ==
            Rows{{"1", "8"}, {"2", "2"}, {"3", "1"}});
    // a subquery that is a UNION (it used to answer false)
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE id IN (SELECT a_id FROM b WHERE k > 5 UNION SELECT 4) ORDER BY id") == V{"1", "4"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a_id = a.id AND k > 5 UNION SELECT 1 FROM b WHERE a_id = a.id AND k = 1) ORDER BY id") ==
            V{"1", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE a.id = a_id AND k = 1 UNION ALL SELECT 1 FROM b WHERE a.id = a_id AND k = 2) ORDER BY id") ==
            V{"2", "3"});
}

TEST_CASE("a view is read again after the tables it reads change", "[outer_references][views]") {
    // (the answer of a SELECT is kept until a table it reads is written; the tables of a view's body used to be left out)
    TempDataDir dir("or_view_cache");
    Executor ex(dir.path);
    open_db(ex);
    tables_ab(ex);
    ok(ex, "CREATE VIEW big AS SELECT id, v FROM a WHERE v > 5");
    ok(ex, "CREATE VIEW joined AS SELECT a.id, b.k FROM a JOIN b ON a.id = b.a_id");
    ok(ex, "CREATE VIEW of_big AS SELECT id FROM big WHERE v < 50");
    ok(ex, "CREATE VIEW with_sub AS SELECT id FROM a WHERE id IN (SELECT a_id FROM b)");
    auto ids = [&](const char* view) { return first_column(ex, std::string("SELECT id FROM ") + view + " ORDER BY id"); };
    REQUIRE(ids("big") == V{"1", "3", "5"});
    REQUIRE(ids("joined") == V{"1", "1", "3"});
    REQUIRE(ids("of_big") == V{"1", "3", "5"});
    REQUIRE(ids("with_sub") == V{"1", "3"});
    ok(ex, "INSERT INTO a VALUES (9, 99, 1, 1.0, 'n', 7), (8, 40, 1, 1.0, 'n', 7)");
    REQUIRE(ids("big") == V{"1", "3", "5", "8", "9"});
    REQUIRE(ids("of_big") == V{"1", "3", "5", "8"});
    REQUIRE(ids("joined") == V{"1", "1", "3", "9"}); // (b4 is a row of a9)
    ok(ex, "INSERT INTO b VALUES (7, 7, 9), (8, 1, 2)");
    REQUIRE(ids("joined") == V{"1", "1", "2", "3", "9", "9"});
    REQUIRE(ids("with_sub") == V{"1", "2", "3", "9"});
    ok(ex, "UPDATE a SET v = 0 WHERE id = 9");
    REQUIRE(ids("big") == V{"1", "3", "5", "8"});
    ok(ex, "DELETE FROM b WHERE a_id = 1");
    REQUIRE(ids("joined") == V{"2", "3", "9", "9"});
    // a view that is dropped and made again is the new one
    ok(ex, "DROP VIEW big");
    ok(ex, "CREATE VIEW big AS SELECT id, v FROM a WHERE v > 20");
    REQUIRE(ids("big") == V{"3", "8"});
    REQUIRE(ids("of_big") == V{"3", "8"});
    ok(ex, "DROP VIEW big");
    ok(ex, "CREATE VIEW big AS SELECT id, v FROM a WHERE v < 20");
    REQUIRE(ids("big") == V{"1", "4", "5", "9"});
    REQUIRE(ids("of_big") == V{"1", "4", "5", "9"});
    // a view made again under its name, and one that is gone
    ok(ex, "CREATE VIEW big AS SELECT id, v FROM a WHERE v > 20");
    REQUIRE(ids("big") == V{"3", "8"});
    REQUIRE(ids("with_sub") == V{"2", "3", "9"});
    ok(ex, "DROP VIEW with_sub");
    REQUIRE(error_of(ex, "SELECT id FROM with_sub ORDER BY id").find("not found") != std::string::npos);
}

// ---- random correlated subqueries against a reference ------------------------------------------------------------------------------------------

namespace {
using Cell = std::optional<long long>;
using Env = std::array<Cell, 3>; // g, x, y (z for the inner table)
using Truth = std::optional<bool>;

struct Operand {
    std::string sql;
    std::function<Cell(const Env&)> eval;
};

// `qualifier` is how a column of the table is written ("" = unqualified)
Operand make_operand(std::mt19937& rng, const std::vector<std::string>& cols, const std::string& qualifier) {
    const std::size_t c = rng() % 3;
    const std::string col = (qualifier.empty() ? "" : qualifier + ".") + cols[c];
    const long long k = static_cast<long long>(rng() % 5) - 1;
    switch (rng() % 5) {
        case 0: return {col, [c](const Env& e) { return e[c]; }};
        case 1: return {"ABS(" + col + ")", [c](const Env& e) { return e[c] ? Cell(std::llabs(*e[c])) : Cell(); }};
        case 2: return {col + " + " + std::to_string(k), [c, k](const Env& e) { return e[c] ? Cell(*e[c] + k) : Cell(); }};
        case 3: return {"COALESCE(" + col + ", " + std::to_string(k) + ")", [c, k](const Env& e) { return e[c] ? e[c] : Cell(k); }};
        default: return {col + " * 2", [c](const Env& e) { return e[c] ? Cell(*e[c] * 2) : Cell(); }};
    }
}

struct Comparison {
    Operand outer_side, inner_side;
    std::string op;
    bool outer_left;
    Truth eval(const Env& o, const Env& i) const {
        const Cell a = outer_side.eval(o), b = inner_side.eval(i);
        if (!a || !b) return std::nullopt;
        const Cell l = outer_left ? a : b, r = outer_left ? b : a;
        if (op == "=") return *l == *r;
        if (op == "<>") return *l != *r;
        if (op == "<") return *l < *r;
        if (op == "<=") return *l <= *r;
        if (op == ">") return *l > *r;
        return *l >= *r;
    }
    std::string sql() const { return outer_left ? "(" + outer_side.sql + ") " + op + " (" + inner_side.sql + ")" : "(" + inner_side.sql + ") " + op + " (" + outer_side.sql + ")"; }
};

struct Term { // one comparison, or two joined by OR
    std::vector<Comparison> parts;
    Truth eval(const Env& o, const Env& i) const {
        Truth result = parts[0].eval(o, i);
        for (std::size_t p = 1; p < parts.size(); p++) {
            const Truth next = parts[p].eval(o, i);
            if (result == true || next == true) result = true;
            else if (!result || !next) result = std::nullopt;
            else result = false;
        }
        return result;
    }
    std::string sql() const {
        std::string out;
        for (std::size_t p = 0; p < parts.size(); p++) out += (p ? " OR " : "") + parts[p].sql();
        return parts.size() > 1 ? "(" + out + ")" : out;
    }
};

struct Table {
    std::string name;
    std::vector<std::string> cols; // the three columns after id
    std::vector<Env> rows;
};
} // namespace

TEST_CASE("random correlated subqueries match a reference", "[outer_references][random]") {
    unsigned seed_count = 6; // RUSQL_FUZZ_SEEDS=60 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        TempDataDir dir("or_random");
        Executor ex(dir.path);
        open_db(ex);
        auto fill = [&](Table& t, std::size_t count) {
            ok(ex, "CREATE TABLE " + t.name + " (id INT PRIMARY KEY, " + t.cols[0] + " INT, " + t.cols[1] + " INT, " + t.cols[2] + " INT)");
            std::string values;
            for (std::size_t r = 0; r < count; r++) {
                Env env;
                std::string line = "(" + std::to_string(r);
                for (std::size_t c = 0; c < 3; c++) {
                    if (rng() % 6 == 0) {
                        line += ", NULL";
                    } else {
                        env[c] = static_cast<long long>(rng() % 7) - 2;
                        line += ", " + std::to_string(*env[c]);
                    }
                }
                t.rows.push_back(env);
                values += (r ? ", " : "") + line + ")";
            }
            ok(ex, "INSERT INTO " + t.name + " VALUES " + values);
        };
        Table outer{"ot", {"g", "x", "y"}, {}}, other{"it", {"g", "x", "z"}, {}};
        fill(outer, 12);
        fill(other, 14);
        for (int round = 0; round < 40; round++) {
            const bool self = rng() % 4 == 0; // the subquery reads the same table as the query around it
            const Table& inner = self ? outer : other;
            const std::string outer_alias = rng() % 2 ? "p" : "";
            const std::string inner_alias = self ? "q" : (rng() % 2 ? "q" : "");
            const std::string outer_name = outer_alias.empty() ? "ot" : outer_alias;
            const std::string inner_name = inner_alias.empty() ? inner.name : inner_alias;
            // the comparisons of the outer row with a row of the subquery's table, written with `qualifier` for the inner columns
            auto build_terms = [&](const std::string& qualifier) {
                std::vector<Term> built;
                const std::size_t count = 1 + rng() % 3;
                for (std::size_t t = 0; t < count; t++) {
                    Term term;
                    const std::size_t parts = rng() % 4 == 0 ? 2 : 1;
                    for (std::size_t p = 0; p < parts; p++) {
                        static const char* ops[] = {"=", "<>", "<", "<=", ">", ">="};
                        // an outer column is written without its table only when no table of the subquery has it
                        const bool outer_bare = !self && rng() % 3 == 0;
                        Comparison cmp{make_operand(rng, outer.cols, outer_bare ? "" : outer_name), make_operand(rng, inner.cols, qualifier), ops[rng() % 6], rng() % 2 == 0};
                        if (outer_bare) { // (only y is the outer table's alone)
                            const long long k = static_cast<long long>(rng() % 5) - 1;
                            cmp.outer_side = {"y + " + std::to_string(k), [k](const Env& e) { return e[2] ? Cell(*e[2] + k) : Cell(); }};
                        }
                        term.parts.push_back(std::move(cmp));
                    }
                    built.push_back(std::move(term));
                }
                return built;
            };
            auto sql_of = [](const std::vector<Term>& list) {
                std::string s;
                for (std::size_t t = 0; t < list.size(); t++) s += (t ? " AND " : "") + list[t].sql();
                return s;
            };
            // the rows of the subquery's table the terms keep for one outer row
            auto rows_kept = [&](const std::vector<Term>& list, const Env& o) {
                std::vector<const Env*> out;
                for (auto& i : inner.rows) {
                    bool all = true;
                    for (auto& t : list) all = all && t.eval(o, i) == true;
                    if (all) out.push_back(&i);
                }
                return out;
            };
            const std::vector<Term> terms = build_terms(inner_name);
            auto pred_sql = [&] { return sql_of(terms); };
            auto kept = [&](const Env& o) { return rows_kept(terms, o); };
            const std::string from_outer = "ot" + (outer_alias.empty() ? std::string() : " " + outer_alias);
            const std::string from_inner = inner.name + (inner_alias.empty() ? std::string() : " " + inner_alias);
            const std::string id_col = outer_name + ".id";
            const std::string sub = "SELECT 1 FROM " + from_inner + " WHERE " + pred_sql();
            INFO("round " << round << " predicate " << pred_sql() << " self=" << self);
            switch (rng() % 6) {
                case 5: { // a subquery inside the subquery, which names the outer row too
                    const std::vector<Term> nested_terms = build_terms("r");
                    const std::string nested_from = inner.name + " r";
                    std::vector<std::string> expected;
                    for (std::size_t r = 0; r < outer.rows.size(); r++) {
                        Cell smallest;
                        for (const Env* n : rows_kept(nested_terms, outer.rows[r])) {
                            if ((*n)[1] && (!smallest || *(*n)[1] < *smallest)) smallest = (*n)[1];
                        }
                        bool hit = false;
                        for (const Env* i : kept(outer.rows[r])) hit = hit || ((*i)[1] && smallest && *(*i)[1] > *smallest);
                        if (hit) expected.push_back(std::to_string(r));
                    }
                    REQUIRE(first_column(ex, "SELECT " + id_col + " FROM " + from_outer + " WHERE EXISTS (SELECT 1 FROM " + from_inner + " WHERE " + pred_sql() + " AND " + inner_name +
                                                 ".x > (SELECT MIN(r.x) FROM " + nested_from + " WHERE " + sql_of(nested_terms) + ")) ORDER BY " + id_col) == expected);
                    break;
                }
                case 0:
                case 1: {
                    const bool negate = rng() % 2 == 0;
                    std::vector<std::string> expected;
                    for (std::size_t r = 0; r < outer.rows.size(); r++) {
                        if (kept(outer.rows[r]).empty() == negate) expected.push_back(std::to_string(r));
                    }
                    REQUIRE(first_column(ex, "SELECT " + id_col + " FROM " + from_outer + " WHERE " + (negate ? "NOT " : "") + "EXISTS (" + sub + ") ORDER BY " + id_col) == expected);
                    break;
                }
                case 2: {
                    Rows expected;
                    for (std::size_t r = 0; r < outer.rows.size(); r++) expected.push_back({std::to_string(r), std::to_string(kept(outer.rows[r]).size())});
                    REQUIRE(q(ex, "SELECT " + id_col + ", (SELECT COUNT(*) FROM " + from_inner + " WHERE " + pred_sql() + ") FROM " + from_outer + " ORDER BY " + id_col) == expected);
                    break;
                }
                case 3: {
                    std::vector<std::string> expected;
                    for (std::size_t r = 0; r < outer.rows.size(); r++) {
                        const Cell x = outer.rows[r][1];
                        bool hit = false;
                        for (const Env* i : kept(outer.rows[r])) hit = hit || (x && (*i)[1] && *x == *(*i)[1]);
                        if (hit) expected.push_back(std::to_string(r));
                    }
                    REQUIRE(first_column(ex, "SELECT " + id_col + " FROM " + from_outer + " WHERE " + outer_name + ".x IN (SELECT " + inner_name + ".x FROM " + from_inner + " WHERE " +
                                                 pred_sql() + ") ORDER BY " + id_col) == expected);
                    break;
                }
                default: {
                    Rows expected;
                    for (std::size_t r = 0; r < outer.rows.size(); r++) {
                        Cell sum;
                        for (const Env* i : kept(outer.rows[r])) {
                            if ((*i)[2]) sum = sum ? Cell(*sum + *(*i)[2]) : Cell(*(*i)[2]);
                        }
                        expected.push_back({std::to_string(r), sum ? std::to_string(*sum) : N});
                    }
                    REQUIRE(q(ex, "SELECT " + id_col + ", (SELECT SUM(" + inner_name + "." + inner.cols[2] + ") FROM " + from_inner + " WHERE " + pred_sql() + ") FROM " + from_outer +
                                      " ORDER BY " + id_col) == expected);
                }
            }
        }
    }
}
