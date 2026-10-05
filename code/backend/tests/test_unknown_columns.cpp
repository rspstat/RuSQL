#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A column name that no table of the statement has is an error (MySQL 1054, "Unknown column 'x' in 'where clause'"), not an empty value, no rows or a
// hidden key. The names that are not columns -- the quoted text on the right of a comparison, function calls, keywords like CURRENT_DATE -- are not
// looked up, and a table the catalog does not know (a view, a CTE) accepts any name.

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

// the statement has to fail with exactly this message
void unknown(Executor& ex, const std::string& sql, const std::string& column, const std::string& clause) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    REQUIRE(r.error() == "Unknown column '" + column + "' in '" + clause + "'");
}

void seed(Executor& ex) {
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT, w INT)");
    ok(ex, "INSERT INTO t VALUES (1, 10, 5), (2, 20, 6), (3, NULL, 7)");
    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, name VARCHAR(10), t_id INT)");
    ok(ex, "INSERT INTO u VALUES (1, 'a', 1), (2, 'b', 2)");
    ok(ex, "CREATE TABLE empty_t (id INT PRIMARY KEY, x INT)");
}
} // namespace

TEST_CASE("an unknown column in the select list is an error", "[unknown_column][select]") {
    TempDataDir dir("uc_select");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    unknown(ex, "SELECT nosuch FROM t", "nosuch", "field list");
    unknown(ex, "SELECT id, nosuch FROM t", "nosuch", "field list");
    unknown(ex, "SELECT nosuch FROM empty_t", "nosuch", "field list"); // a table without rows is no excuse
    unknown(ex, "SELECT t.nosuch FROM t", "t.nosuch", "field list");
    unknown(ex, "SELECT x.nosuch FROM t x", "t.nosuch", "field list"); // (the parser has replaced the alias by the table)
    unknown(ex, "SELECT z.id FROM t", "z.id", "field list");           // a table that is not in the query
    unknown(ex, "SELECT u.id, u.nosuch FROM t JOIN u ON t.id = u.t_id", "u.nosuch", "field list");
    unknown(ex, "SELECT v + nosuch FROM t", "nosuch", "field list");
    unknown(ex, "SELECT SUM(nosuch) FROM t", "nosuch", "field list");
    unknown(ex, "SELECT COUNT(nosuch) FROM t", "nosuch", "field list");
    unknown(ex, "SELECT nosuch AS a FROM t", "nosuch", "field list");
    unknown(ex, "SELECT CASE WHEN nosuch > 1 THEN 'a' ELSE 'b' END FROM t", "nosuch", "field list");
    unknown(ex, "SELECT ROW_NUMBER() OVER (ORDER BY nosuch) FROM t", "nosuch", "field list");
    unknown(ex, "SELECT (SELECT nosuch FROM u WHERE u.id = 1) FROM t", "nosuch", "field list");
    unknown(ex, "SELECT DISTINCT nosuch FROM t", "nosuch", "field list");
    unknown(ex, "SELECT d.nosuch FROM (SELECT id, v FROM t) AS d", "d.nosuch", "field list");
    // a statement that reads only what exists is as before
    REQUIRE(text(ex, "SELECT id, v FROM t WHERE id = 2").find("1 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT d.id FROM (SELECT id, v FROM t) AS d WHERE d.v > 15").find("1 row(s) returned.") != std::string::npos);
}

TEST_CASE("an unknown column in WHERE, ON, GROUP BY, HAVING or ORDER BY is an error", "[unknown_column][clauses]") {
    TempDataDir dir("uc_clauses");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    unknown(ex, "SELECT id FROM t WHERE nosuch > 1", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE t.nosuch = 1", "t.nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE v > 1 AND nosuch = 2", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE v > 1 OR NOT (nosuch IS NULL)", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE v + nosuch > 1", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE nosuch IN (1, 2)", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE nosuch LIKE 'a%'", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE nosuch BETWEEN 1 AND 2", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE nosuch IS NULL", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE v = t.nosuch", "t.nosuch", "where clause"); // the right side, too, when it names a table of the query
    unknown(ex, "SELECT * FROM t JOIN u ON t.id = u.nosuch", "u.nosuch", "on clause");
    unknown(ex, "SELECT * FROM t JOIN u ON nosuch = u.t_id", "nosuch", "on clause");
    unknown(ex, "SELECT v, COUNT(*) FROM t GROUP BY nosuch", "nosuch", "group statement");
    unknown(ex, "SELECT v, COUNT(*) FROM t GROUP BY v HAVING nosuch > 1", "nosuch", "having clause");
    unknown(ex, "SELECT id FROM t ORDER BY nosuch", "nosuch", "order clause");
    unknown(ex, "SELECT id FROM t ORDER BY id, t.nosuch DESC", "t.nosuch", "order clause");
    // the names the select list gives may be used where MySQL allows them
    REQUIRE(text(ex, "SELECT v AS x FROM t WHERE v IS NOT NULL ORDER BY x DESC").find("2 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT w, COUNT(*) AS n FROM t GROUP BY w HAVING n > 0 ORDER BY n").find("3 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT v, COUNT(*) FROM t GROUP BY v HAVING COUNT(*) > 0").find("3 row(s) returned.") != std::string::npos);
}

TEST_CASE("UPDATE and DELETE name only columns of their table", "[unknown_column][write]") {
    TempDataDir dir("uc_write");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    unknown(ex, "UPDATE t SET v = nosuch", "nosuch", "field list");
    unknown(ex, "UPDATE t SET v = v + nosuch WHERE id = 1", "nosuch", "field list");
    unknown(ex, "UPDATE t SET v = 1 WHERE nosuch = 2", "nosuch", "where clause");
    unknown(ex, "DELETE FROM t WHERE nosuch = 1", "nosuch", "where clause");
    unknown(ex, "DELETE FROM t WHERE id = 1 AND t.nosuch = 1", "t.nosuch", "where clause");
    unknown(ex, "UPDATE t JOIN u ON t.id = u.t_id SET t.v = u.nosuch", "u.nosuch", "field list");
    unknown(ex, "UPDATE t JOIN u ON t.id = u.t_id SET t.nosuch = 1", "t.nosuch", "field list");
    unknown(ex, "UPDATE t JOIN u ON t.id = u.nosuch SET t.v = 1", "u.nosuch", "on clause");
    // nothing changed by the failed ones
    REQUIRE(q(ex, "SELECT id, v, w FROM t ORDER BY id") == Rows{{"1", "10", "5"}, {"2", "20", "6"}, {"3", "NULL", "7"}});
    // the aliases a write may use in its SET expressions
    ok(ex, "UPDATE t x SET v = x.v + 1 WHERE x.id = 1");
    REQUIRE(q(ex, "SELECT v FROM t WHERE id = 1") == Rows{{"11"}});
    ok(ex, "UPDATE t AS x JOIN u AS y ON x.id = y.t_id SET x.w = y.id + x.w WHERE y.id = 2");
    REQUIRE(q(ex, "SELECT w FROM t WHERE id = 2") == Rows{{"8"}});
    ok(ex, "DELETE FROM t AS x WHERE x.id = 3");
}

TEST_CASE("subqueries see the columns of the queries around them, and only those", "[unknown_column][subquery]") {
    TempDataDir dir("uc_subquery");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    unknown(ex, "SELECT id FROM t WHERE id IN (SELECT nosuch FROM u)", "nosuch", "field list");
    unknown(ex, "SELECT id FROM t WHERE id IN (SELECT t_id FROM u WHERE nosuch = 1)", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE nosuch = t.id)", "nosuch", "where clause");
    unknown(ex, "SELECT id FROM t WHERE v > (SELECT MAX(nosuch) FROM u)", "nosuch", "field list");
    unknown(ex, "SELECT id FROM (SELECT nosuch FROM t) AS d", "nosuch", "field list");
    unknown(ex, "SELECT * FROM t JOIN (SELECT nosuch FROM u) AS d ON d.id = t.id", "nosuch", "field list");
    unknown(ex, "SELECT id FROM t UNION SELECT nosuch FROM u", "nosuch", "field list");
    unknown(ex, "INSERT INTO empty_t SELECT nosuch, 1 FROM t", "nosuch", "field list");
    unknown(ex, "CREATE VIEW bad AS SELECT id, nosuch FROM t", "nosuch", "field list"); // refused when it is made, not an empty column later
    REQUIRE(ex.execute_sql("SELECT * FROM bad").is_err());
    // a correlated subquery reads the outer row's columns: bare, qualified, by the outer alias, inside an expression
    REQUIRE(text(ex, "SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.t_id = t.id)").find("2 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT id FROM t x WHERE EXISTS (SELECT 1 FROM u y WHERE y.t_id = x.id)").find("2 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT id FROM t x WHERE EXISTS (SELECT 1 FROM u y WHERE y.t_id = x.id + 0)").find("2 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT id, (SELECT COUNT(*) FROM u WHERE u.t_id = t.id) AS n FROM t ORDER BY id").find("3 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT id FROM t WHERE w > (SELECT MIN(t_id) FROM u WHERE u.t_id <= t.id)").find("3 row(s) returned.") != std::string::npos);
}

TEST_CASE("what is not a column name is not looked up", "[unknown_column][accepted]") {
    TempDataDir dir("uc_accepted");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    // the right side of a comparison is text when it is not `table.column` (the parser keeps it without quotes), keywords are not columns
    REQUIRE(text(ex, "SELECT id FROM u WHERE name = 'a'").find("1 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT id FROM u WHERE name = a").find("1 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT id FROM u WHERE name = 'e.g'").find("0 rows returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT id FROM u WHERE name = 'no.such'").find("0 rows returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT CURRENT_DATE() FROM u").find("2 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT 1, 'x', NULL FROM t").find("3 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT 1 + 1").find("1 row(s) returned.") != std::string::npos);
    ok(ex, "UPDATE t SET v = NULL WHERE id = 3");
    ok(ex, "UPDATE u SET name = NULL WHERE id = 2");
    ok(ex, "CREATE TABLE flags (id INT PRIMARY KEY, on_ BOOLEAN)");
    ok(ex, "INSERT INTO flags VALUES (1, FALSE)");
    ok(ex, "UPDATE flags SET on_ = TRUE WHERE id = 1"); // TRUE / FALSE / NULL are kept as names by the parser, and are not columns
    REQUIRE(text(ex, "SELECT id FROM flags WHERE on_ = TRUE").find("1 row(s) returned.") != std::string::npos);
    // a table the catalog does not know: a view and a WITH clause accept any column of their query
    ok(ex, "CREATE VIEW tv AS SELECT id AS key_, v FROM t");
    REQUIRE(text(ex, "SELECT key_ FROM tv").find("3 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "WITH c AS (SELECT id AS k FROM t) SELECT k FROM c ORDER BY k").find("3 row(s) returned.") != std::string::npos);
    // a derived table's own columns
    REQUIRE(text(ex, "SELECT x.k FROM (SELECT id AS k FROM t) AS x ORDER BY x.k").find("3 row(s) returned.") != std::string::npos);
    // a function's arguments are not looked at (they may name a unit or a type as well as a column)
    REQUIRE(text(ex, "SELECT CAST(v AS CHAR) FROM t").find("3 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT DATE_ADD('2024-01-01', INTERVAL 1 DAY) FROM t").find("3 row(s) returned.") != std::string::npos);
    // a column of the table is found by the exact name, whatever the query does with the table
    REQUIRE(text(ex, "SELECT x.id FROM t x JOIN u y ON x.id = y.t_id ORDER BY y.name").find("2 row(s) returned.") != std::string::npos);
    REQUIRE(text(ex, "SELECT a.id, b.id FROM t a JOIN t b ON a.id < b.id").find("3 row(s) returned.") != std::string::npos);
}

TEST_CASE("a trigger firing inside a statement does not change what the statement may name", "[unknown_column][trigger]") {
    TempDataDir dir("uc_proc");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    ok(ex, "CREATE TABLE audit (note VARCHAR(20))");
    ok(ex, "CREATE TRIGGER trg AFTER INSERT ON t FOR EACH ROW INSERT INTO audit VALUES ('new')");
    ok(ex, "INSERT INTO t VALUES (9, 90, 9)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM audit") == Rows{{"1"}});

}
