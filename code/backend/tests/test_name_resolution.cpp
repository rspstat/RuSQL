#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Names and groups, the way MySQL reads them: an unqualified column that two tables of the query have is ambiguous (1052: it used to be the first
// table's), a qualifier that is no table or alias of the query (or one around it) is an unknown column even inside a subquery, HAVING and ORDER BY of an
// aggregating query may name only what is grouped (ONLY_FULL_GROUP_BY), `x IN (empty)` is false and `x NOT IN (empty)` true whatever x is (a NULL too),
// `SET @x = (SELECT ...)` is a scalar subquery (one row, one column). Also the gaps that were parse errors: CREATE OR REPLACE VIEW, a SELECT without FROM
// followed by UNION / INTERSECT / EXCEPT, `a DIV b`, `1e3`.

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

bool fails_with(Executor& ex, const std::string& sql, const std::string& part) { return error_of(ex, sql).find(part) != std::string::npos; }

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

// a: (id, v, w, g) (1, 10, 2, 1) (2, NULL, 3, 1) (3, 30, NULL, 2) (4, 0, 0, 2) (5, 7, 7, 3)     b: (id, k, a_id) (1, 3, 1) (2, 8, 1) (3, 1, 3) (4, 5, 9)
void tables(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, v INT, w INT, g INT)");
    ok(ex, "INSERT INTO a VALUES (1, 10, 2, 1), (2, NULL, 3, 1), (3, 30, NULL, 2), (4, 0, 0, 2), (5, 7, 7, 3)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, k INT, a_id INT)");
    ok(ex, "INSERT INTO b VALUES (1, 3, 1), (2, 8, 1), (3, 1, 3), (4, 5, 9)");
}
} // namespace

TEST_CASE("a column two tables have is ambiguous", "[name_resolution]") {
    TempDataDir dir("nr_ambiguous");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    const std::string join = " FROM a JOIN b ON b.a_id = a.id";
    REQUIRE(fails_with(ex, "SELECT id" + join, "Column 'id' in field list is ambiguous"));
    REQUIRE(fails_with(ex, "SELECT k" + join + " WHERE id = 1", "Column 'id' in where clause is ambiguous"));
    REQUIRE(fails_with(ex, "SELECT k" + join + " ORDER BY id", "Column 'id' in order clause is ambiguous"));
    REQUIRE(fails_with(ex, "SELECT * " + join + " ORDER BY id", "Column 'id' in order clause is ambiguous"));
    REQUIRE(fails_with(ex, "SELECT COUNT(*)" + join + " GROUP BY id", "Column 'id' in group statement is ambiguous"));
    REQUIRE(fails_with(ex, "SELECT k FROM a JOIN b ON id = a_id", "Column 'id' in on clause is ambiguous"));
    REQUIRE(fails_with(ex, "SELECT COUNT(id)" + join, "ambiguous"));
    REQUIRE(fails_with(ex, "SELECT k + id" + join, "ambiguous"));
    REQUIRE(fails_with(ex, "SELECT a.id FROM a, b WHERE a.id = b.a_id AND id > 0", "Column 'id' in where clause is ambiguous"));
    REQUIRE(fails_with(ex, "SELECT a.id FROM a JOIN b ON b.a_id = id", "ambiguous")); // (the bare word on the right is a column)
    REQUIRE(fails_with(ex, "SELECT x.v FROM a x JOIN a y ON x.id = y.id WHERE v > 1", "Column 'v' in where clause is ambiguous")); // (a table twice)
    // what is not ambiguous
    REQUIRE(q(ex, "SELECT a.id, k" + join + " ORDER BY id") == Rows{{"1", "3"}, {"1", "8"}, {"3", "1"}}); // (ORDER BY reads the select list's id first)
    REQUIRE(q(ex, "SELECT a.id AS id, k" + join + " ORDER BY id DESC") == Rows{{"3", "1"}, {"1", "3"}, {"1", "8"}});
    REQUIRE(q(ex, "SELECT k" + join + " WHERE a.id = 1 AND b.id = 2") == Rows{{"8"}});
    REQUIRE(q(ex, "SELECT k, v" + join + " ORDER BY k") == Rows{{"1", "30"}, {"3", "10"}, {"8", "10"}}); // (a column only one table has)
    REQUIRE(q(ex, "SELECT id, k FROM a JOIN b USING (id) ORDER BY id") == Rows{{"1", "3"}, {"2", "8"}, {"3", "1"}, {"4", "5"}}); // (USING makes it one)
    REQUIRE(q(ex, "SELECT id, k FROM a NATURAL JOIN b WHERE id = 2") == Rows{{"2", "8"}});
    REQUIRE(q(ex, "SELECT x.id FROM a x JOIN a y ON x.id = y.id WHERE x.id = 2") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id, n FROM a CROSS JOIN LATERAL (SELECT a.v + 1 AS n) x WHERE id = 1") == Rows{{"1", "11"}}); // (a LATERAL table's columns cannot be told: it takes any name)
    // an inner query's own table comes first, whatever the query around it has
    REQUIRE(q(ex, "SELECT id FROM a WHERE id IN (SELECT id FROM b) ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}, {"4"}});
    REQUIRE(q(ex, "SELECT (SELECT id FROM b WHERE b.id = 1) FROM a WHERE id = 1") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM a JOIN b ON b.a_id = a.id GROUP BY g HAVING g > 0 ORDER BY g") == Rows{{"1", "2"}, {"2", "1"}});
    // HAVING reads the select list's id first, too
    REQUIRE(q(ex, "SELECT a.id, COUNT(*)" + join + " GROUP BY a.id HAVING id > 0 ORDER BY id") == Rows{{"1", "2"}, {"3", "1"}});
    REQUIRE(fails_with(ex, "SELECT k" + join + " GROUP BY k HAVING id > 0", "ambiguous"));
    REQUIRE(q(ex, "SELECT a.g AS grp, COUNT(*) AS id" + join + " GROUP BY a.g HAVING id > 1") == Rows{{"1", "2"}}); // (a name the select list gives wins)
}

TEST_CASE("a qualifier that names nothing is an unknown column inside a subquery too", "[name_resolution]") {
    TempDataDir dir("nr_qualifier");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(fails_with(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE zz.id = 1)", "Unknown column 'zz.id'"));
    REQUIRE(fails_with(ex, "SELECT id FROM a WHERE id IN (SELECT zz.k FROM b)", "Unknown column 'zz.k'"));
    REQUIRE(fails_with(ex, "SELECT id FROM a x WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = x.nosuch)", "Unknown column"));
    // the table or alias of a query around it is a table
    REQUIRE(q(ex, "SELECT x.id FROM a x WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = x.id) ORDER BY x.id") == Rows{{"1"}, {"3"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id) ORDER BY id") == Rows{{"1"}, {"3"}});
    REQUIRE(q(ex, "SELECT x.id, (SELECT COUNT(*) FROM b WHERE b.a_id = x.id) FROM a x ORDER BY x.id") == Rows{{"1", "2"}, {"2", "0"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
    ok(ex, "UPDATE a x SET w = (SELECT COUNT(*) FROM b WHERE b.a_id = x.id)");
    REQUIRE(q(ex, "SELECT id, w FROM a ORDER BY id") == Rows{{"1", "2"}, {"2", "0"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
    ok(ex, "DELETE FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = a.id AND b.k > 5)");
    REQUIRE(q(ex, "SELECT id FROM a ORDER BY id") == Rows{{"2"}, {"3"}, {"4"}, {"5"}});
}

TEST_CASE("an aliased UPDATE or DELETE names its row by the alias in a subquery", "[name_resolution]") {
    TempDataDir dir("nr_write_alias");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // b has rows for a 1 (k 3, 8) and 3 (k 1)
    ok(ex, "UPDATE a x SET w = 99 WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = x.id)");
    REQUIRE(q(ex, "SELECT id, w FROM a ORDER BY id") == Rows{{"1", "99"}, {"2", "3"}, {"3", "99"}, {"4", "0"}, {"5", "7"}});
    ok(ex, "UPDATE a x SET w = (SELECT COUNT(*) FROM b WHERE b.a_id = x.id)");
    REQUIRE(q(ex, "SELECT id, w FROM a ORDER BY id") == Rows{{"1", "2"}, {"2", "0"}, {"3", "1"}, {"4", "0"}, {"5", "0"}});
    ok(ex, "UPDATE a x JOIN b y ON y.a_id = x.id SET x.v = (SELECT SUM(k) FROM b z WHERE z.a_id = x.id)");
    REQUIRE(q(ex, "SELECT id, v FROM a ORDER BY id") == Rows{{"1", "11"}, {"2", N}, {"3", "1"}, {"4", "0"}, {"5", "7"}});
    ok(ex, "DELETE FROM a x WHERE EXISTS (SELECT 1 FROM b WHERE b.a_id = x.id AND b.k > 5)");
    REQUIRE(q(ex, "SELECT id FROM a ORDER BY id") == Rows{{"2"}, {"3"}, {"4"}, {"5"}});
    ok(ex, "DELETE x FROM a x JOIN b y ON y.a_id = x.id WHERE EXISTS (SELECT 1 FROM b z WHERE z.a_id = x.id AND z.k < 2)");
    REQUIRE(q(ex, "SELECT id FROM a ORDER BY id") == Rows{{"2"}, {"4"}, {"5"}});
    // an alias of the subquery's own table is the subquery's
    ok(ex, "DELETE FROM a x WHERE x.id IN (SELECT x.a_id FROM b x WHERE x.k = 5)");
    REQUIRE(q(ex, "SELECT id FROM a ORDER BY id") == Rows{{"2"}, {"4"}, {"5"}}); // (b's a_id of k = 5 is 9: no such a)
    // inside a stored procedure (the statement is kept as a tree)
    ok(ex, "CREATE PROCEDURE clean() BEGIN DELETE FROM a x WHERE NOT EXISTS (SELECT 1 FROM b WHERE b.a_id = x.id); END");
    ok(ex, "CALL clean()");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a") == Rows{{"0"}});
}

TEST_CASE("IN and NOT IN over a subquery that gives no row", "[name_resolution]") {
    TempDataDir dir("nr_empty_in");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // v is NULL for id 2
    REQUIRE(q(ex, "SELECT id FROM a WHERE v NOT IN (SELECT k FROM b WHERE id = 99) ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}, {"4"}, {"5"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE v IN (SELECT k FROM b WHERE id = 99) ORDER BY id").empty());
    REQUIRE(q(ex, "SELECT id FROM a WHERE NOT (v IN (SELECT k FROM b WHERE id = 99)) ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}, {"4"}, {"5"}});
    REQUIRE(q(ex, "SELECT id, v IN (SELECT k FROM b WHERE id = 99), v NOT IN (SELECT k FROM b WHERE id = 99) FROM a WHERE id = 2") == Rows{{"2", "0", "1"}});
    // correlated: b has rows for a 1 and 3 only (10 is not among 3 and 8, 30 not 1; 2, 4 and 5 find no row: the NULL v too)
    REQUIRE(q(ex, "SELECT id FROM a WHERE v NOT IN (SELECT k FROM b WHERE b.a_id = a.id) ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}, {"4"}, {"5"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE v IN (SELECT k FROM b WHERE b.a_id = a.id) ORDER BY id").empty());
    // with rows a NULL on the left is unknown, as it was
    REQUIRE(q(ex, "SELECT id FROM a WHERE v NOT IN (SELECT k FROM b) ORDER BY id") == Rows{{"1"}, {"3"}, {"4"}, {"5"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE v IN (SELECT k FROM b) ORDER BY id").empty());
    REQUIRE(q(ex, "SELECT id FROM a WHERE w IN (SELECT k FROM b) ORDER BY id") == Rows{{"2"}}); // (w = 3 is a k)
    // a write decides the same way
    ok(ex, "DELETE FROM a WHERE v NOT IN (SELECT k FROM b WHERE id = 99)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a") == Rows{{"0"}});
}

TEST_CASE("HAVING and ORDER BY of an aggregating query name what is grouped", "[name_resolution]") {
    TempDataDir dir("nr_grouping");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(fails_with(ex, "SELECT g, COUNT(*) FROM a GROUP BY g HAVING v > 3", "Non-grouping field 'v' is used in HAVING clause"));
    REQUIRE(fails_with(ex, "SELECT COUNT(*) FROM a HAVING v > 3", "Non-grouping field 'v' is used in HAVING clause"));
    REQUIRE(fails_with(ex, "SELECT g, COUNT(*) FROM a GROUP BY g ORDER BY v", "Expression #1 of ORDER BY clause is not in GROUP BY clause"));
    REQUIRE(fails_with(ex, "SELECT g, COUNT(*) FROM a GROUP BY g ORDER BY g, w", "Expression #2 of ORDER BY clause is not in GROUP BY clause"));
    REQUIRE(fails_with(ex, "SELECT COUNT(*) FROM a ORDER BY v", "In aggregated query without GROUP BY, expression #1 of ORDER BY"));
    // what is allowed
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM a GROUP BY g HAVING g > 1 ORDER BY g") == Rows{{"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) AS total FROM a GROUP BY g HAVING total > 5 ORDER BY total DESC") == Rows{{"2", "30"}, {"1", "10"}, {"3", "7"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) AS v FROM a GROUP BY g HAVING v > 5 ORDER BY g") == Rows{{"1", "10"}, {"2", "30"}, {"3", "7"}}); // (the alias wins over the column)
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM a GROUP BY g HAVING MAX(v) > 8 AND SUM(w) >= 0 ORDER BY COUNT(*) DESC, g") == Rows{{"1", "2"}, {"2", "2"}});
    REQUIRE(q(ex, "SELECT id, v FROM a GROUP BY id HAVING v >= 0 ORDER BY v, id") == Rows{{"4", "0"}, {"5", "7"}, {"1", "10"}, {"3", "30"}}); // (the primary key is grouped by)
    REQUIRE(q(ex, "SELECT g FROM a GROUP BY g ORDER BY g DESC") == Rows{{"3"}, {"2"}, {"1"}});
    REQUIRE(q(ex, "SELECT DISTINCT g FROM a ORDER BY g") == Rows{{"1"}, {"2"}, {"3"}}); // (no aggregation: the rule does not apply)
}

TEST_CASE("SET @x = (SELECT ...) is a scalar subquery", "[name_resolution]") {
    TempDataDir dir("nr_set");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    ok(ex, "SET @one = (SELECT k FROM b WHERE id = 2)");
    REQUIRE(q(ex, "SELECT @one") == Rows{{"8"}});
    ok(ex, "SET @none = (SELECT k FROM b WHERE id = 99)");
    REQUIRE(q(ex, "SELECT @none") == Rows{{N}});
    ok(ex, "SET @twice = (SELECT MAX(k) FROM b) * 2");
    REQUIRE(q(ex, "SELECT @twice") == Rows{{"16"}});
    REQUIRE(fails_with(ex, "SET @many = (SELECT k FROM b)", "more than 1 row"));
    REQUIRE(fails_with(ex, "SET @wide = (SELECT k, id FROM b WHERE id = 1)", "Operand should contain 1 column"));
    REQUIRE(q(ex, "SELECT @one") == Rows{{"8"}}); // (a failed SET leaves the variable)
    ok(ex, "CREATE PROCEDURE pick() BEGIN DECLARE m INT DEFAULT 0; SET m = (SELECT MAX(k) FROM b); SELECT m; END");
    REQUIRE(q(ex, "CALL pick()") == Rows{{"8"}});
    ok(ex, "CREATE PROCEDURE pick_many() BEGIN DECLARE m INT DEFAULT 0; SET m = (SELECT k FROM b); SELECT m; END");
    REQUIRE(fails_with(ex, "CALL pick_many()", "more than 1 row"));
    // a subquery in an IF / WHILE condition reads the variables as they are in each round
    ok(ex, "CREATE PROCEDURE climb() BEGIN DECLARE i INT DEFAULT 0; WHILE (SELECT COUNT(*) FROM b WHERE k > i) > 0 DO SET i = i + 1; END WHILE; SELECT i; END");
    REQUIRE(q(ex, "CALL climb()") == Rows{{"8"}});
    ok(ex, "CREATE PROCEDURE above(IN n INT) BEGIN DECLARE r VARCHAR(5) DEFAULT 'no'; IF EXISTS (SELECT 1 FROM b WHERE k > n) THEN SET r = 'yes'; END IF; SELECT r; END");
    REQUIRE(q(ex, "CALL above(7)") == Rows{{"yes"}});
    REQUIRE(q(ex, "CALL above(8)") == Rows{{"no"}});
}

TEST_CASE("syntax that was a parse error", "[name_resolution]") {
    TempDataDir dir("nr_syntax");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // CREATE OR REPLACE VIEW
    ok(ex, "CREATE OR REPLACE VIEW vw AS SELECT id FROM a WHERE id < 3");
    REQUIRE(q(ex, "SELECT * FROM vw ORDER BY id") == Rows{{"1"}, {"2"}});
    ok(ex, "CREATE OR REPLACE VIEW vw AS SELECT id FROM a WHERE id > 3");
    REQUIRE(q(ex, "SELECT * FROM vw ORDER BY id") == Rows{{"4"}, {"5"}});
    // a SELECT without FROM and a set operator
    REQUIRE(q(ex, "SELECT 1 UNION SELECT 2") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT 1 UNION SELECT 1").size() == 1);
    REQUIRE(q(ex, "SELECT 1 UNION ALL SELECT 1").size() == 2);
    REQUIRE(q(ex, "SELECT 1 UNION SELECT 2 UNION SELECT 3").size() == 3);
    REQUIRE(q(ex, "SELECT 1, 'a' UNION SELECT 2, 'b'") == Rows{{"1", "a"}, {"2", "b"}});
    REQUIRE(q(ex, "SELECT 1 INTERSECT SELECT 1") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT 1 EXCEPT SELECT 1").empty());
    REQUIRE(q(ex, "SELECT id FROM a WHERE id = 1 UNION SELECT 9") == Rows{{"1"}, {"9"}});
    // DIV
    REQUIRE(q(ex, "SELECT 7 DIV 2, -7 DIV 2, 7 DIV -2, 6 DIV 3") == Rows{{"3", "-3", "-3", "2"}});
    REQUIRE(q(ex, "SELECT 7 DIV 0") == Rows{{N}});
    REQUIRE(q(ex, "SELECT id, v DIV 3 FROM a ORDER BY id") == Rows{{"1", "3"}, {"2", N}, {"3", "10"}, {"4", "0"}, {"5", "2"}});
    REQUIRE(q(ex, "SELECT 2 + 9 DIV 2 * 3") == Rows{{"14"}}); // (DIV binds like * and /)
    // a column called div is still a column
    ok(ex, "CREATE TABLE dv (id INT PRIMARY KEY, div INT)");
    ok(ex, "INSERT INTO dv VALUES (1, 5)");
    REQUIRE(q(ex, "SELECT div FROM dv") == Rows{{"5"}});
    // numbers with an exponent
    REQUIRE(q(ex, "SELECT 1e3, 2.5E-1, 1E2 + 1, 5e0") == Rows{{"1000", "0.25", "101", "5"}});
    REQUIRE(q(ex, "SELECT id FROM a WHERE v > 1e1 ORDER BY id") == Rows{{"3"}});
}
