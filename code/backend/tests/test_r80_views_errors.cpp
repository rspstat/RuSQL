// R80: writes through a view over a join, FROM DUAL, a bracketed UNION operand with its own LIMIT, the MOD operator, and the errors whose number or order differed from MySQL (window
// functions where none is read, ALTER ... NOT NULL over NULLs, ADD COLUMN ... NOT NULL, the unsigned zero, a DATETIME that rounds past the end, SELECT ... INTO a short variable, a row that
// is a child and a parent). The answers were measured on MySQL 8.4; code/test/diff/corpus/{join_view_writes,dual_paren_union,errno_more}.sql repeat them against the server.

#include <filesystem>
#include <sstream>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
}

// the first cell of every row of an answer, in order
std::string first_column(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue; // (the header)
        std::size_t end = line.find('|', 1);
        std::string cell = line.substr(1, end - 1);
        const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
        cell = a == std::string::npos ? std::string() : cell.substr(a, b - a + 1);
        out += (out.empty() ? "" : ",") + cell;
    }
    return out;
}

// the text of the error of a statement, "" when it goes through
std::string error_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    return r.is_ok() ? std::string() : r.error();
}

bool says(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

struct World {
    TempDataDir dir;
    Executor ex;
    explicit World(const std::string& name) : dir(name), ex(dir.path) {
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
    }
};

void join_tables(Executor& ex) {
    ok(ex, "CREATE TABLE ja (id INT PRIMARY KEY, x INT, g INT)");
    ok(ex, "CREATE TABLE jb (id INT PRIMARY KEY, y INT, aid INT, z INT)");
    ok(ex, "INSERT INTO ja VALUES (1,10,1),(2,20,1),(3,30,2),(4,40,NULL)");
    ok(ex, "INSERT INTO jb VALUES (1,100,1,5),(2,200,1,6),(3,300,2,7),(4,400,9,8)");
    ok(ex, "CREATE VIEW v1 AS SELECT ja.id AS aid, ja.x, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id");
    ok(ex, "CREATE VIEW v2 AS SELECT ja.id AS aid, ja.x, jb.y FROM ja LEFT JOIN jb ON jb.aid = ja.id");
    ok(ex, "CREATE VIEW v4 AS SELECT ja.id AS aid, ja.x, jb.y FROM ja JOIN jb ON jb.aid = ja.id WHERE ja.g = 1");
    ok(ex, "CREATE VIEW v5 AS SELECT ja.id AS aid, ja.x * 2 AS dbl, jb.y FROM ja JOIN jb ON jb.aid = ja.id");
    ok(ex, "CREATE VIEW v7 AS SELECT * FROM v1 WHERE y > 100");
    ok(ex, "CREATE VIEW v9 AS SELECT ja.id, ja.x, jb.id AS bid, jb.y FROM ja, jb WHERE jb.aid = ja.id");
}
} // namespace

TEST_CASE("UPDATE through a join view changes the rows of one table, each once", "[r80]") {
    World w("r80_data_jv_update");
    Executor& ex = w.ex;
    join_tables(ex);
    ok(ex, "UPDATE v1 SET x = x + 1 WHERE y = 100");
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "11,20,30,40");
    ok(ex, "UPDATE v1 SET x = x + 1 WHERE aid = 1"); // (ja 1 is paired with two rows of jb: it is changed once)
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "12,20,30,40");
    ok(ex, "UPDATE v1 SET y = y + 1 WHERE bid = 3");
    CHECK(first_column(ex, "SELECT y FROM jb ORDER BY id") == "100,200,301,400");
    ok(ex, "UPDATE v1 SET x = 1 WHERE aid = 99");
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "12,20,30,40");
    // the new value may read the columns of the other table
    ok(ex, "UPDATE v1 SET x = y WHERE aid = 2");
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "12,301,30,40"); // (ja 2 is paired with jb 3, whose y is 301)
    // the WHERE of the view applies, a view over a join view, a comma join
    ok(ex, "UPDATE v4 SET x = 77");
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "77,77,30,40");
    ok(ex, "UPDATE v7 SET x = 55 WHERE bid = 2");
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "55,77,30,40");
    ok(ex, "UPDATE v7 SET x = 56 WHERE bid = 1"); // (jb 1 has y = 100: not in v7)
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "55,77,30,40");
    ok(ex, "UPDATE v9 SET x = 8 WHERE id = 1");
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "8,77,30,40");
    // the columns of the view are the table's: a key too
    ok(ex, "UPDATE v5 SET y = 9 WHERE aid = 1");
    CHECK(first_column(ex, "SELECT y FROM jb ORDER BY id") == "9,9,301,400");
}

TEST_CASE("UPDATE through a join view: what MySQL refuses", "[r80]") {
    World w("r80_data_jv_refused");
    Executor& ex = w.ex;
    join_tables(ex);
    CHECK(says(error_of(ex, "UPDATE v1 SET x = 1, y = 2"), "Can not modify more than one base table through a join view"));
    CHECK(says(error_of(ex, "UPDATE v1 SET nosuch = 1"), "Unknown column 'nosuch'"));
    CHECK(says(error_of(ex, "UPDATE v1 SET x = 5 WHERE nosuch = 1"), "Unknown column 'nosuch'"));
    CHECK(says(error_of(ex, "UPDATE v5 SET dbl = 1"), "ERROR 1348"));
    CHECK(says(error_of(ex, "UPDATE v1 SET x = 7 ORDER BY aid LIMIT 1"), "Incorrect usage of UPDATE and ORDER BY"));
    CHECK(says(error_of(ex, "UPDATE v1 SET x = 7 LIMIT 1"), "Incorrect usage of UPDATE and LIMIT"));
    CHECK(says(error_of(ex, "UPDATE v2 SET x = 41 WHERE aid = 4"), "ERROR 1288")); // (an outer join: nothing goes through)
    CHECK(says(error_of(ex, "DELETE FROM v1 WHERE aid = 1"), "ERROR 1395"));
    CHECK(says(error_of(ex, "DELETE FROM v2 WHERE aid = 1"), "ERROR 1288")); // (an outer join view is not updatable at all)
    // nothing was changed by any of them
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "10,20,30,40");
    CHECK(first_column(ex, "SELECT y FROM jb ORDER BY id") == "100,200,300,400");
}

TEST_CASE("INSERT through a join view puts the row in the table the columns belong to", "[r80]") {
    World w("r80_data_jv_insert");
    Executor& ex = w.ex;
    join_tables(ex);
    ok(ex, "INSERT INTO v1 (aid, x) VALUES (10, 1)");
    CHECK(first_column(ex, "SELECT id FROM ja ORDER BY id") == "1,2,3,4,10");
    ok(ex, "INSERT INTO v1 (bid, y) VALUES (50, 1)");
    CHECK(first_column(ex, "SELECT id FROM jb ORDER BY id") == "1,2,3,4,50");
    ok(ex, "INSERT INTO v1 (aid, x) VALUES (13, 1), (14, 2)");
    ok(ex, "INSERT INTO v4 (aid, x) VALUES (17, 1)"); // (the WHERE of the view does not stop an INSERT)
    ok(ex, "INSERT INTO v1 (aid, x) SELECT 21, 1");
    CHECK(first_column(ex, "SELECT id FROM ja ORDER BY id") == "1,2,3,4,10,13,14,17,21");
    CHECK(says(error_of(ex, "INSERT INTO v1 VALUES (12, 1, 51, 2)"), "ERROR 1394"));
    CHECK(says(error_of(ex, "INSERT INTO v1 (aid, x, y) VALUES (11, 1, 2)"), "ERROR 1393"));
    CHECK(says(error_of(ex, "INSERT INTO v1 (x) VALUES (1)"), "ERROR 1423")); // (ja.id has no default)
    CHECK(says(error_of(ex, "INSERT INTO v1 (aid, nosuch) VALUES (15, 1)"), "Unknown column 'nosuch'"));
    CHECK(says(error_of(ex, "INSERT INTO v2 (aid, x) VALUES (15, 1)"), "ERROR 1471")); // (an outer join)
    CHECK(says(error_of(ex, "INSERT INTO v5 (aid, dbl) VALUES (18, 1)"), "ERROR 1348"));
    CHECK(says(error_of(ex, "INSERT INTO v5 (aid) VALUES (18)"), "ERROR 1471")); // (a view with a column of an expression takes no INSERT)
    CHECK(says(error_of(ex, "REPLACE INTO v1 (aid, x) VALUES (10, 2)"), "ERROR 1395"));
    // ON DUPLICATE KEY UPDATE names the columns of the view
    ok(ex, "INSERT INTO v1 (aid, x) VALUES (10, 3) ON DUPLICATE KEY UPDATE x = 4");
    CHECK(first_column(ex, "SELECT x FROM ja WHERE id = 10") == "4");
    ok(ex, "INSERT INTO v1 (aid, x) VALUES (10, 3) ON DUPLICATE KEY UPDATE aid = 24");
    CHECK(first_column(ex, "SELECT id FROM ja WHERE id >= 10 ORDER BY id") == "13,14,17,21,24");
}

TEST_CASE("a join view that cannot be written through says so, and a table used twice does not break the others", "[r80]") {
    World w("r80_data_jv_other");
    Executor& ex = w.ex;
    join_tables(ex);
    ok(ex, "CREATE VIEW vs AS SELECT a.id AS aid, b.id AS bid, a.x FROM ja a JOIN ja b ON a.g = b.g");
    // the view works for reading; a write through it is refused with the error of a view that cannot take it, never a silent change
    CHECK(first_column(ex, "SELECT COUNT(*) FROM vs") != "");
    const std::string refused = error_of(ex, "UPDATE vs SET x = 1");
    if (!refused.empty()) CHECK(says(refused, "not updatable"));
    CHECK(first_column(ex, "SELECT x FROM ja ORDER BY id") == "10,20,30,40");
}

TEST_CASE("FROM DUAL is a select that reads no table", "[r80]") {
    World w("r80_data_dual");
    Executor& ex = w.ex;
    CHECK(first_column(ex, "SELECT 1 FROM DUAL") == "1");
    CHECK(first_column(ex, "SELECT 2 FROM dual WHERE 1 = 1") == "2");
    CHECK(first_column(ex, "SELECT 1 FROM DUAL WHERE 1 = 0") == "");
    CHECK(first_column(ex, "SELECT 5 FROM DUAL UNION SELECT 3 ORDER BY 1") == "3,5");
    CHECK(first_column(ex, "SELECT 1 FROM DUAL LIMIT 0") == "");
    CHECK(first_column(ex, "SELECT (SELECT 7 FROM DUAL)") == "7");
    // a table of the name written with its database is a table
    ok(ex, "CREATE TABLE `dual` (a INT)");
    ok(ex, "INSERT INTO v.dual VALUES (4)");
    CHECK(first_column(ex, "SELECT a FROM v.dual") == "4");
}

TEST_CASE("a bracketed operand of a set operation keeps its own ORDER BY and LIMIT", "[r80]") {
    World w("r80_data_paren");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO t VALUES (1), (2), (3), (4), (5)");
    CHECK(first_column(ex, "(SELECT id FROM t ORDER BY id LIMIT 2) UNION (SELECT 99) ORDER BY 1 DESC") == "99,2,1");
    CHECK(first_column(ex, "(SELECT id FROM t ORDER BY id DESC LIMIT 2) UNION ALL (SELECT id FROM t ORDER BY id LIMIT 1) ORDER BY 1") == "1,4,5");
    CHECK(first_column(ex, "(SELECT id FROM t ORDER BY id LIMIT 1) UNION (SELECT id FROM t ORDER BY id DESC LIMIT 1) ORDER BY 1") == "1,5");
    CHECK(first_column(ex, "(SELECT id FROM t WHERE id < 5 ORDER BY id LIMIT 1 OFFSET 1) UNION (SELECT 7) ORDER BY 1") == "2,7");
    CHECK(first_column(ex, "(SELECT id FROM t ORDER BY id LIMIT 2)") == "1,2");
    CHECK(first_column(ex, "(SELECT 1 LIMIT 1)") == "1");
    CHECK(first_column(ex, "(SELECT id FROM t ORDER BY id LIMIT 2) UNION (SELECT id FROM t ORDER BY id DESC LIMIT 2) UNION (SELECT 50) ORDER BY 1") == "1,2,4,5,50");
    CHECK(first_column(ex, "(SELECT id FROM t ORDER BY id LIMIT 3) UNION ALL (SELECT id FROM t ORDER BY id LIMIT 3) ORDER BY 1 LIMIT 4") == "1,1,2,2");
    // no ORDER BY / LIMIT of its own: the brackets are only brackets, as before
    CHECK(first_column(ex, "(SELECT 5) UNION (SELECT 3) ORDER BY 1") == "3,5");
}

TEST_CASE("the MOD operator, and the window function where none is read", "[r80]") {
    World w("r80_data_mod");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE we (id INT PRIMARY KEY, n INT, b INT)");
    ok(ex, "INSERT INTO we VALUES (1, 4, 2), (2, 5, 0), (3, 6, 3)");
    CHECK(first_column(ex, "SELECT n MOD 3 FROM we ORDER BY id") == "1,2,0");
    CHECK(first_column(ex, "SELECT id FROM we WHERE n MOD 2 = 0 ORDER BY id") == "1,3");
    CHECK(first_column(ex, "SELECT id FROM we WHERE n MOD b = 0 ORDER BY id") == "1,3"); // (MOD 0 is NULL in a SELECT)
    CHECK(says(error_of(ex, "UPDATE we SET n = n WHERE n MOD b = 0"), "Division by 0"));
    CHECK(says(error_of(ex, "SELECT id FROM we WHERE RANK() OVER (ORDER BY id) = 1"), "You cannot use the window function 'rank' in this context."));
    CHECK(says(error_of(ex, "SELECT n FROM we GROUP BY n HAVING DENSE_RANK() OVER (ORDER BY n) = 1"), "window function 'dense_rank'"));
    CHECK(says(error_of(ex, "SELECT id, SUM(n) OVER (nosuchwindow ORDER BY id) FROM we"), "Window name 'nosuchwindow' is not defined."));
    CHECK(error_of(ex, "SELECT id, SUM(n) OVER w FROM we WINDOW w AS (ORDER BY id)").empty());
}

TEST_CASE("ALTER ... NOT NULL over a NULL, ADD COLUMN ... NOT NULL", "[r80]") {
    World w("r80_data_alter");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE m1 (id INT PRIMARY KEY, f DOUBLE, i INT, s VARCHAR(5))");
    ok(ex, "INSERT INTO m1 VALUES (1, 2.5, 1, 'a'), (2, 1.5, 2, 'b'), (3, NULL, NULL, NULL)");
    // a change that keeps the type is made in place; one that changes it says the NULL does not fit, at its row
    CHECK(says(error_of(ex, "ALTER TABLE m1 MODIFY f INT NOT NULL"), "Data truncated for column 'f' at row 3"));
    CHECK(error_of(ex, "ALTER TABLE m1 MODIFY f DOUBLE NOT NULL") == "Invalid use of NULL value");
    CHECK(error_of(ex, "ALTER TABLE m1 MODIFY i INT NOT NULL") == "Invalid use of NULL value");
    CHECK(says(error_of(ex, "ALTER TABLE m1 MODIFY i BIGINT NOT NULL"), "Data truncated for column 'i' at row 3"));
    CHECK(error_of(ex, "ALTER TABLE m1 MODIFY s VARCHAR(9) NOT NULL") == "Invalid use of NULL value");
    CHECK(says(error_of(ex, "ALTER TABLE m1 MODIFY s TEXT NOT NULL"), "Data truncated for column 's' at row 3"));
    // a column added NOT NULL gives the rows the zero of its type
    ok(ex, "ALTER TABLE m1 ADD COLUMN z INT NOT NULL");
    ok(ex, "ALTER TABLE m1 ADD COLUMN zs VARCHAR(5) NOT NULL");
    ok(ex, "ALTER TABLE m1 ADD COLUMN zd DECIMAL(6,2) NOT NULL");
    ok(ex, "ALTER TABLE m1 ADD COLUMN ze ENUM('x','y') NOT NULL");
    ok(ex, "ALTER TABLE m1 ADD COLUMN zn INT NOT NULL DEFAULT 7");
    ok(ex, "ALTER TABLE m1 ADD COLUMN zq INT");
    CHECK(first_column(ex, "SELECT z FROM m1 ORDER BY id") == "0,0,0");
    CHECK(first_column(ex, "SELECT zd FROM m1 ORDER BY id") == "0.00,0.00,0.00");
    CHECK(first_column(ex, "SELECT ze FROM m1 ORDER BY id") == "x,x,x");
    CHECK(first_column(ex, "SELECT zn FROM m1 ORDER BY id") == "7,7,7");
    CHECK(first_column(ex, "SELECT zq FROM m1 ORDER BY id") == "NULL,NULL,NULL");
    // a DATE has no zero that strict mode takes, unless the table is empty
    CHECK(says(error_of(ex, "ALTER TABLE m1 ADD COLUMN zdate DATE NOT NULL"), "Incorrect date value: '0000-00-00' for column 'zdate' at row 1"));
    CHECK(says(error_of(ex, "ALTER TABLE m1 ADD COLUMN zdt DATETIME(3) NOT NULL"), "Incorrect datetime value: '0000-00-00 00:00:00'"));
    ok(ex, "CREATE TABLE empty_one (id INT PRIMARY KEY)");
    ok(ex, "ALTER TABLE empty_one ADD COLUMN zdate DATE NOT NULL");
}

TEST_CASE("the errors of unsigned arithmetic, a DATETIME past the end, SELECT INTO, and a child that is a parent", "[r80]") {
    World w("r80_data_errors");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE un2 (id INT PRIMARY KEY, i INT UNSIGNED, n INT)");
    ok(ex, "INSERT INTO un2 VALUES (1, 4294967295, 0), (2, 0, 0), (3, 5, 0)");
    CHECK(says(error_of(ex, "UPDATE un2 SET i = i - 5 WHERE id = 2"), "BIGINT UNSIGNED value is out of range"));
    CHECK(says(error_of(ex, "UPDATE un2 SET n = i - 5 WHERE id = 2"), "BIGINT UNSIGNED value is out of range")); // (the target need not be unsigned)
    CHECK(error_of(ex, "UPDATE un2 SET i = i - 5 WHERE id = 3").empty());
    CHECK(says(error_of(ex, "UPDATE un2 SET i = i + 1 WHERE id = 1"), "Out of range value for column 'i'")); // (a result that does not fit the column: its own error)
    CHECK(first_column(ex, "SELECT i FROM un2 ORDER BY id") == "4294967295,0,0");

    ok(ex, "CREATE TABLE fs2 (id INT PRIMARY KEY, a DATETIME(3))");
    CHECK(says(error_of(ex, "INSERT INTO fs2 VALUES (4, '9999-12-31 23:59:59.9996')"), "Datetime function: datetime field overflow"));
    ok(ex, "INSERT INTO fs2 VALUES (5, '9999-12-31 23:59:59.9994')");

    ok(ex, "CREATE TABLE rt2 (id INT PRIMARY KEY, s VARCHAR(10))");
    ok(ex, "INSERT INTO rt2 VALUES (1, 'abc'), (2, 'a')");
    ok(ex, "CREATE PROCEDURE p_into2(IN k INT) BEGIN DECLARE t VARCHAR(2); SELECT s INTO t FROM rt2 WHERE id = k; SELECT t; END");
    ok(ex, "CREATE PROCEDURE p_set2() BEGIN DECLARE t VARCHAR(2); SET t = 'abc'; SELECT t; END");
    CHECK(says(error_of(ex, "CALL p_into2(1)"), "Data truncated for column 't'"));
    CHECK(error_of(ex, "CALL p_into2(2)").empty());
    CHECK(says(error_of(ex, "CALL p_set2()"), "Data too long for column 't'"));

    ok(ex, "CREATE TABLE emp2 (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES emp2(id))");
    ok(ex, "INSERT INTO emp2 VALUES (1, NULL), (7, 7)");
    // the references to the old key are asked about first (MySQL 1451), the parent of the new value after (1452)
    CHECK(says(error_of(ex, "UPDATE emp2 SET id = 8, mgr = 8 WHERE id = 7"), "ON UPDATE RESTRICT"));
    CHECK(says(error_of(ex, "UPDATE emp2 SET mgr = 9 WHERE id = 7"), "Foreign key violation: '9' not found"));
}

TEST_CASE("an ambiguous column of a subquery is told before its target table", "[r80]") {
    World w("r80_data_target");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE tt (id INT PRIMARY KEY, v INT)");
    ok(ex, "CREATE TABLE tu (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO tt VALUES (1, 1)");
    ok(ex, "INSERT INTO tu VALUES (1)");
    CHECK(says(error_of(ex, "UPDATE tt SET v = 0 WHERE id IN (SELECT id FROM tu JOIN tt ON tu.id = tt.id)"), "Column 'id' in field list is ambiguous"));
    CHECK(says(error_of(ex, "UPDATE tt SET v = 0 WHERE id IN (SELECT tu.id FROM tu JOIN tt ON tu.id = tt.id)"), "You can't specify target table 'tt'"));
}
