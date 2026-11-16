// R90: the columns of a derived table / CTE / view whose body is a UNION | INTERSECT | EXCEPT | WITH are known (an unknown column over it is MySQL 1054, it used to
// answer no rows), the two queries of a set operation must give the same number of columns (1222), the type of a UNION column comes from both queries, `FROM (WITH ...) d`
// parses, NOT REGEXP and REGEXP_LIKE() in a CHECK are written the way MySQL writes them, and the tables of information_schema.tables say UPDATE_TIME (the time of the last
// commit that wrote rows of the table). The answers were measured on MySQL 8.4.9; code/test/diff/corpus/union_derived_r90.sql and check_text_r90.sql repeat them against the server.

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

std::string error_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_err());
    return r.error();
}

bool says(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// the rows of an answer table as "a,b;c,d" (every cell trimmed)
std::string flat(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) continue;
        std::string row;
        std::size_t from = 1;
        while (from < line.size()) {
            const std::size_t end = line.find('|', from);
            if (end == std::string::npos) break;
            std::string cell = line.substr(from, end - from);
            const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
            row += (row.empty() ? "" : ",") + (a == std::string::npos ? std::string() : cell.substr(a, b - a + 1));
            from = end + 1;
        }
        out += (out.empty() ? "" : ";") + row;
    }
    return out;
}

// the header of an answer table, cells trimmed and joined by a comma
std::string header(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        std::string out;
        std::size_t from = 1;
        while (from < line.size()) {
            const std::size_t end = line.find('|', from);
            if (end == std::string::npos) break;
            std::string cell = line.substr(from, end - from);
            const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
            out += (out.empty() ? "" : ",") + (a == std::string::npos ? std::string() : cell.substr(a, b - a + 1));
            from = end + 1;
        }
        return out;
    }
    return std::string();
}

void make_tables(Executor& ex) {
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, i INT, s VARCHAR(20))");
    ok(ex, "INSERT INTO t VALUES (1, 10, 'apple'), (2, NULL, 'Apple'), (3, 30, '10'), (9, 7, '9')");
    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, k INT, name VARCHAR(10))");
    ok(ex, "INSERT INTO u VALUES (1, 10, 'one'), (2, 20, 'Two'), (3, NULL, 'three')");
}
} // namespace

TEST_CASE("R90: a derived table over a set operation is named by its first query: an unknown column is 1054", "[r90]") {
    TempDataDir dir("r90_data_derived");
    Executor ex(dir.path);
    make_tables(ex);
    for (const char* body : {"SELECT id FROM t UNION ALL SELECT k FROM u", "SELECT id FROM t UNION SELECT k FROM u", "SELECT id, i FROM t INTERSECT SELECT id, k FROM u",
                             "SELECT id FROM t EXCEPT SELECT id FROM u", "SELECT id FROM t UNION SELECT id FROM u UNION SELECT k FROM u"}) {
        const std::string from = std::string(" FROM (") + body + ") s ";
        CHECK(says(error_of(ex, "SELECT *" + from + "WHERE nosuch = 1"), "Unknown column 'nosuch' in 'where clause'"));
        CHECK(says(error_of(ex, "SELECT s.nosuch" + from), "Unknown column 's.nosuch' in 'field list'"));
        CHECK(says(error_of(ex, "SELECT *" + from + "ORDER BY nosuch"), "Unknown column 'nosuch' in 'order clause'"));
        CHECK(says(error_of(ex, "SELECT id, COUNT(*)" + from + "GROUP BY nosuch"), "Unknown column 'nosuch' in 'group statement'"));
        CHECK(says(error_of(ex, "SELECT id, COUNT(*)" + from + "GROUP BY id HAVING nosuch > 1"), "Unknown column 'nosuch' in 'having clause'"));
        ok(ex, "SELECT *" + from + "WHERE id = 1");
    }
    // the names of the first query are the columns; the second query's own names are not
    CHECK(says(error_of(ex, "SELECT * FROM (SELECT id FROM t UNION ALL SELECT k FROM u) s WHERE k = 10"), "Unknown column 'k'"));
    CHECK(says(error_of(ex, "SELECT * FROM (SELECT id AS a FROM t UNION SELECT k FROM u) s WHERE id = 1"), "Unknown column 'id'"));
    CHECK(flat(ex, "SELECT * FROM (SELECT id AS a FROM t UNION SELECT k FROM u) s WHERE a = 10") == "10");
    CHECK(header(ex, "SELECT * FROM (SELECT id AS a FROM t UNION SELECT k FROM u) s") == "a");
    // the same in a join, in an ON clause, in a subquery, over an answer with no rows
    CHECK(says(error_of(ex, "SELECT * FROM t JOIN (SELECT id AS x FROM u UNION SELECT 9) d ON d.nosuch = t.id"), "Unknown column 'd.nosuch' in 'on clause'"));
    CHECK(flat(ex, "SELECT t.id, d.x FROM t JOIN (SELECT id AS x FROM u UNION SELECT 9) d ON d.x = t.id ORDER BY t.id") == "1,1;2,2;3,3;9,9");
    CHECK(says(error_of(ex, "SELECT id FROM t WHERE id IN (SELECT x FROM (SELECT id AS x FROM u UNION SELECT 9) d WHERE nosuch > 1)"), "Unknown column 'nosuch'"));
    CHECK(says(error_of(ex, "SELECT * FROM (SELECT id FROM t WHERE id < 0 UNION SELECT k FROM u WHERE k < 0) s WHERE nosuch = 1"), "Unknown column 'nosuch'"));
    CHECK(header(ex, "SELECT * FROM (SELECT id AS q FROM t WHERE id < 0 UNION ALL SELECT k FROM u WHERE k < 0) s") == "q");
    // `*` of the first query
    CHECK(says(error_of(ex, "SELECT * FROM (SELECT * FROM u UNION SELECT id, k, name FROM u) s WHERE nosuch = 'one'"), "Unknown column 'nosuch'"));
    CHECK(flat(ex, "SELECT id FROM (SELECT * FROM u UNION SELECT id, k, name FROM u) s WHERE name = 'one'") == "1");
}

TEST_CASE("R90: a CTE and a view over a set operation are named by their first query", "[r90]") {
    TempDataDir dir("r90_data_cte_view");
    Executor ex(dir.path);
    make_tables(ex);
    CHECK(says(error_of(ex, "WITH c AS (SELECT id FROM t UNION ALL SELECT id FROM u) SELECT * FROM c WHERE nosuch = 1"), "Unknown column 'nosuch'"));
    CHECK(says(error_of(ex, "WITH c AS (SELECT id AS a FROM t UNION ALL SELECT id FROM u) SELECT * FROM c WHERE id > 8"), "Unknown column 'id'"));
    CHECK(flat(ex, "WITH c AS (SELECT id AS a FROM t UNION ALL SELECT id FROM u) SELECT a FROM c WHERE a > 8 ORDER BY a") == "9");
    CHECK(says(error_of(ex, "WITH c AS (SELECT id FROM t INTERSECT SELECT id FROM u) SELECT * FROM c WHERE nosuch = 1"), "Unknown column 'nosuch'"));
    CHECK(header(ex, "WITH c AS (SELECT id FROM t WHERE id < 0 UNION SELECT id FROM u WHERE id < 0) SELECT * FROM c") == "id");
    ok(ex, "CREATE VIEW vu AS SELECT id AS a FROM u UNION ALL SELECT k FROM u");
    CHECK(says(error_of(ex, "SELECT * FROM vu WHERE nosuch = 1"), "Unknown column 'nosuch'"));
    CHECK(says(error_of(ex, "SELECT * FROM vu WHERE id = 1"), "Unknown column 'id'"));
    CHECK(flat(ex, "SELECT a FROM vu WHERE a > 3 ORDER BY a") == "10;20");
    ok(ex, "CREATE VIEW vu2 AS SELECT id, name FROM u WHERE id < 3 UNION SELECT id, name FROM u WHERE id > 2");
    CHECK(says(error_of(ex, "SELECT * FROM vu2 ORDER BY nosuch"), "Unknown column 'nosuch' in 'order clause'"));
    CHECK(flat(ex, "SELECT id FROM vu2 WHERE name = 'three'") == "3");
    // a recursive CTE still runs
    CHECK(flat(ex, "WITH RECURSIVE r AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM r WHERE n < 3) SELECT * FROM r") == "1;2;3");
}

TEST_CASE("R90: `FROM (WITH ...) d` is a derived table", "[r90]") {
    TempDataDir dir("r90_data_with_derived");
    Executor ex(dir.path);
    make_tables(ex);
    CHECK(flat(ex, "SELECT * FROM (WITH c AS (SELECT 1 AS a) SELECT a FROM c) s WHERE a = 1") == "1");
    CHECK(flat(ex, "SELECT * FROM (WITH c AS (SELECT 1 AS a UNION ALL SELECT 2) SELECT a FROM c) s WHERE a = 2") == "2");
    CHECK(says(error_of(ex, "SELECT * FROM (WITH c AS (SELECT 1 AS a) SELECT a FROM c) s WHERE nosuch = 1"), "Unknown column 'nosuch'"));
    CHECK(flat(ex, "SELECT t.id, d.a FROM t JOIN (WITH c AS (SELECT id AS a FROM u) SELECT a FROM c) d ON d.a = t.id ORDER BY t.id") == "1,1;2,2;3,3");
    // the list of tables in brackets is still a list of tables
    CHECK(flat(ex, "SELECT t.id, u.k FROM (t JOIN u ON t.id = u.id) ORDER BY t.id") == "1,10;2,20;3,NULL");
}

TEST_CASE("R90: the queries of a set operation must give the same number of columns (1222)", "[r90]") {
    TempDataDir dir("r90_data_columns");
    Executor ex(dir.path);
    make_tables(ex);
    const std::string expected = "different number of columns";
    for (const char* sql : {"SELECT id, i FROM t UNION SELECT k FROM u", "SELECT id FROM t UNION SELECT id, k FROM u", "SELECT id FROM t INTERSECT SELECT id, k FROM u",
                            "SELECT id FROM t EXCEPT SELECT id, k FROM u", "(SELECT id FROM t) UNION (SELECT id, k FROM u)",
                            "SELECT 1 UNION SELECT 1, 2", "SELECT 1, 2 UNION ALL SELECT 3", "SELECT 1, 2 UNION ALL SELECT 3, 4 UNION ALL SELECT 5",
                            "SELECT id FROM t WHERE id IN (SELECT id FROM u UNION SELECT id, k FROM u)", "SELECT * FROM (SELECT id, i FROM t UNION SELECT k FROM u) s",
                            "WITH c AS (SELECT id, i FROM t UNION SELECT k FROM u) SELECT * FROM c", "SELECT * FROM u UNION SELECT 1, 2",
                            "SELECT * FROM u UNION SELECT 1, 2, 'x', 4", "SELECT * FROM (SELECT * FROM u) d UNION SELECT 1", "SELECT t.*, u.* FROM t JOIN u ON t.id = u.id UNION SELECT 1, 2",
                            "SELECT * FROM t JOIN u USING (id) UNION SELECT 1, 2", "SELECT * FROM (SELECT * FROM t JOIN u USING (id)) d UNION SELECT 1, 2",
                            "SELECT * FROM (SELECT 1 AS a, 2 AS b UNION SELECT 3, 4) d UNION SELECT 5"}) {
        INFO(sql);
        CHECK(says(error_of(ex, sql), expected));
    }
    // nothing was written by the statements that failed
    CHECK(says(error_of(ex, "INSERT INTO u SELECT id, i FROM t UNION SELECT k FROM u"), expected));
    CHECK(flat(ex, "SELECT COUNT(*) FROM u") == "3");
    CHECK(says(error_of(ex, "CREATE TABLE zz AS SELECT id, i FROM t UNION SELECT k FROM u"), expected));
    CHECK(says(error_of(ex, "SELECT * FROM zz"), "zz"));
    // the shapes that are right are not refused
    ok(ex, "SELECT * FROM u UNION SELECT 1, 2, 'x'");
    ok(ex, "SELECT * FROM (SELECT * FROM u) d UNION SELECT 1, 2, 'x'");
    ok(ex, "SELECT * FROM t JOIN u USING (id) UNION SELECT 1, 2, 'x', 4, 'y'"); // (id, i, s, k, name: the USING column once)
    ok(ex, "SELECT * FROM (SELECT * FROM t JOIN u USING (id)) d UNION SELECT 1, 2, 'x', 4, 'y'");
    ok(ex, "SELECT u.* FROM t JOIN u ON t.id = u.id UNION SELECT 1, 2, 'y'");
    ok(ex, "SELECT t.*, u.* FROM t JOIN u ON t.id = u.id UNION SELECT 1, 2, 'a', 4, 5, 'b'");
    ok(ex, "WITH c AS (SELECT * FROM u) SELECT * FROM c UNION SELECT 1, 2, 'x'");
    ok(ex, "SELECT * FROM (SELECT 1 AS a, 2 AS b UNION SELECT 3, 4) d UNION SELECT 5, 6");
    ok(ex, "SELECT id FROM t WHERE id IN (SELECT id FROM u UNION SELECT k FROM u)");
}

TEST_CASE("R90: a UNION column of a number and a string is a string column", "[r90]") {
    TempDataDir dir("r90_data_union_type");
    Executor ex(dir.path);
    make_tables(ex);
    // MySQL types the column by both queries: INT and VARCHAR make a VARCHAR (compared and sorted as text)
    CHECK(flat(ex, "SELECT * FROM (SELECT 7 AS n UNION ALL SELECT '007') x WHERE n = '7'") == "7");
    CHECK(flat(ex, "SELECT * FROM (SELECT 7 AS n UNION ALL SELECT '007') x WHERE n = '007'") == "007");
    CHECK(flat(ex, "SELECT * FROM (SELECT '007' AS n UNION ALL SELECT 7) x WHERE n = '7'") == "7");
    CHECK(flat(ex, "SELECT * FROM (SELECT id FROM t UNION ALL SELECT name FROM u) x ORDER BY id") == "1;2;3;9;one;three;Two");
    CHECK(flat(ex, "SELECT * FROM (SELECT 9 AS n UNION ALL SELECT '10') x ORDER BY n") == "10;9"); // (as text: '10' < '9')
    // two numbers stay numbers
    CHECK(flat(ex, "SELECT * FROM (SELECT 1 AS n UNION ALL SELECT 2.5) x WHERE n = 2.5") == "2.5");
}

TEST_CASE("R90: NOT REGEXP and REGEXP_LIKE() in a CHECK are written as MySQL writes them", "[r90]") {
    TempDataDir dir("r90_data_check_regexp");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE ck (qty INT, name VARCHAR(20), CONSTRAINT c1 CHECK (name NOT REGEXP '^a'), CONSTRAINT c2 CHECK (name NOT RLIKE 'x'), "
           "CONSTRAINT c3 CHECK (REGEXP_LIKE(name, 'a')), CONSTRAINT c4 CHECK (NOT REGEXP_LIKE(name, 'a')), CONSTRAINT c5 CHECK (REGEXP_LIKE(name, 'a', 'i')), "
           "CONSTRAINT c6 CHECK (name NOT REGEXP 'a' OR qty > 1), CONSTRAINT c7 CHECK (NOT (name REGEXP 'a' AND qty > 1)), CONSTRAINT c8 CHECK (REGEXP_LIKE(name, '^a') AND qty > 1))");
    auto created = ex.show_create_table_mysql("ck");
    REQUIRE(created.is_ok());
    const std::string shown = created.value();
    CHECK(says(shown, "CONSTRAINT `c1` CHECK ((not(regexp_like(`name`,_utf8mb4'^a'))))"));
    CHECK(says(shown, "CONSTRAINT `c2` CHECK ((not(regexp_like(`name`,_utf8mb4'x'))))"));
    CHECK(says(shown, "CONSTRAINT `c3` CHECK (regexp_like(`name`,_utf8mb4'a'))"));
    CHECK(says(shown, "CONSTRAINT `c4` CHECK ((not(regexp_like(`name`,_utf8mb4'a'))))"));
    CHECK(says(shown, "CONSTRAINT `c5` CHECK (regexp_like(`name`,_utf8mb4'a',_utf8mb4'i'))"));
    CHECK(says(shown, "CONSTRAINT `c6` CHECK (((not(regexp_like(`name`,_utf8mb4'a'))) or (`qty` > 1)))"));
    CHECK(says(shown, "CONSTRAINT `c7` CHECK (((not(regexp_like(`name`,_utf8mb4'a'))) or (`qty` <= 1)))"));
    CHECK(says(shown, "CONSTRAINT `c8` CHECK ((regexp_like(`name`,_utf8mb4'^a') and (`qty` > 1)))"));
    // and the table still checks
    ok(ex, "DROP TABLE ck");
    ok(ex, "CREATE TABLE ck (name VARCHAR(20), CONSTRAINT c1 CHECK ((not(regexp_like(`name`,_utf8mb4'^a')))), CONSTRAINT c3 CHECK (regexp_like(`name`,_utf8mb4'b')))");
    ok(ex, "INSERT INTO ck VALUES ('xb')");
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES ('ab')"), "CHECK"));
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES ('xx')"), "CHECK"));
}

namespace {
// the UPDATE_TIME of a table ("NULL" when it has none)
std::string update_time(Executor& ex, const std::string& table) {
    return flat(ex, "SELECT update_time FROM information_schema.tables WHERE table_schema = 'v' AND table_name = '" + table + "'");
}
} // namespace

TEST_CASE("R90: UPDATE_TIME is the time of the last commit that wrote rows of the table", "[r90]") {
    TempDataDir dir("r90_data_update_time");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE ut (id INT PRIMARY KEY, v INT)");
    CHECK(update_time(ex, "ut") == "NULL"); // (made: not written to)
    ok(ex, "INSERT INTO ut VALUES (1, 1)");
    const std::string first = update_time(ex, "ut");
    CHECK(first != "NULL");
    CHECK(first.size() == 19); // (YYYY-MM-DD HH:MM:SS)
    CHECK(flat(ex, "SELECT update_time IS NOT NULL FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'ut'") == "1");
    // a read, a statement that changes no row and a rolled-back transaction leave a table that was never written to without one
    ok(ex, "CREATE TABLE ut_idle (id INT PRIMARY KEY, v INT)");
    ok(ex, "SELECT * FROM ut_idle");
    ok(ex, "UPDATE ut_idle SET v = 5 WHERE id = 99");
    ok(ex, "DELETE FROM ut_idle WHERE id = 99");
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO ut_idle VALUES (2, 2)");
    CHECK(update_time(ex, "ut_idle") == "NULL"); // (not before the commit)
    ok(ex, "ROLLBACK");
    CHECK(update_time(ex, "ut_idle") == "NULL");
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO ut_idle VALUES (3, 3)");
    ok(ex, "COMMIT");
    CHECK(update_time(ex, "ut_idle") != "NULL"); // (the commit)
    // a table that is only made has none, also when it is a copy of a table with rows without rows
    ok(ex, "CREATE TABLE ut_like LIKE ut");
    CHECK(update_time(ex, "ut_like") == "NULL");
    ok(ex, "CREATE TABLE ut_empty AS SELECT * FROM ut WHERE id < 0");
    CHECK(update_time(ex, "ut_empty") == "NULL");
    ok(ex, "CREATE TABLE ut_copy AS SELECT * FROM ut");
    CHECK(update_time(ex, "ut_copy") != "NULL");
    ok(ex, "CREATE TABLE ut_none (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO ut_none SELECT id FROM ut WHERE id < 0");
    CHECK(update_time(ex, "ut_none") == "NULL");
    // a committed transaction stamps every table it wrote, and only those
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO ut_none VALUES (7)");
    ok(ex, "COMMIT");
    CHECK(update_time(ex, "ut_none") != "NULL");
    // a rename keeps it; DDL that makes the table again (ALTER, CREATE INDEX, TRUNCATE) and DROP + CREATE take it
    ok(ex, "RENAME TABLE ut_copy TO ut_moved");
    CHECK(update_time(ex, "ut_moved") != "NULL");
    CHECK(update_time(ex, "ut_copy") == "");
    ok(ex, "ALTER TABLE ut_moved ADD COLUMN w INT");
    CHECK(update_time(ex, "ut_moved") == "NULL");
    ok(ex, "INSERT INTO ut VALUES (3, 3)");
    CHECK(update_time(ex, "ut") != "NULL");
    ok(ex, "CREATE INDEX ix ON ut (v)");
    CHECK(update_time(ex, "ut") == "NULL");
    ok(ex, "INSERT INTO ut VALUES (4, 4)");
    ok(ex, "TRUNCATE TABLE ut");
    CHECK(update_time(ex, "ut") == "NULL");
    ok(ex, "INSERT INTO ut VALUES (5, 5)");
    ok(ex, "DROP TABLE ut");
    ok(ex, "CREATE TABLE ut (id INT PRIMARY KEY, v INT)");
    CHECK(update_time(ex, "ut") == "NULL");
}

TEST_CASE("R90: a cascade stamps the child tables too", "[r90]") {
    TempDataDir dir("r90_data_update_time_fk");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE)");
    ok(ex, "INSERT INTO p VALUES (1), (2)");
    ok(ex, "INSERT INTO c VALUES (1, 1), (2, 2)");
    CHECK(update_time(ex, "p") != "NULL");
    CHECK(update_time(ex, "c") != "NULL");
}

TEST_CASE("R90: SIGNAL raises the error of a stored program (the number, the state, the message)", "[r90]") {
    TempDataDir dir("r90_data_signal");
    Executor ex(dir.path);
    make_tables(ex);
    ok(ex, "CREATE PROCEDURE sp1(IN x INT) BEGIN IF x < 0 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'negative not allowed'; END IF; SELECT x; END");
    CHECK(flat(ex, "CALL sp1(5)") == "5");
    CHECK(says(error_of(ex, "CALL sp1(-1)"), "ERROR 1644 (45000): negative not allowed"));
    ok(ex, "CREATE PROCEDURE sp2() BEGIN SIGNAL SQLSTATE '45000'; END");
    CHECK(says(error_of(ex, "CALL sp2()"), "ERROR 1644 (45000): Unhandled user-defined exception condition"));
    ok(ex, "CREATE PROCEDURE sp3() BEGIN SIGNAL SQLSTATE VALUE '45001' SET MESSAGE_TEXT = 'value form'; END");
    CHECK(says(error_of(ex, "CALL sp3()"), "ERROR 1644 (45001): value form"));
    ok(ex, "CREATE PROCEDURE sp4() BEGIN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'custom errno', MYSQL_ERRNO = 1234; END");
    CHECK(says(error_of(ex, "CALL sp4()"), "ERROR 1234 (45000): custom errno"));
    ok(ex, "CREATE PROCEDURE sp5(IN x INT) BEGIN DECLARE msg VARCHAR(100); SET msg = CONCAT('bad value ', x); IF x > 10 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = msg; END IF; SELECT 'fine'; END");
    CHECK(flat(ex, "CALL sp5(3)") == "fine");
    CHECK(says(error_of(ex, "CALL sp5(30)"), "ERROR 1644 (45000): bad value 30"));
    // class 01 is a warning: the program goes on and no result row of the SIGNAL itself is sent
    ok(ex, "CREATE PROCEDURE sp6() BEGIN SIGNAL SQLSTATE '01000' SET MESSAGE_TEXT = 'only a warning'; SELECT 'after the warning'; END");
    CHECK(flat(ex, "CALL sp6()") == "after the warning");
    // class 02 is the "not found" error
    ok(ex, "CREATE PROCEDURE sp7() BEGIN SIGNAL SQLSTATE '02000' SET MESSAGE_TEXT = 'not found class'; END");
    CHECK(says(error_of(ex, "CALL sp7()"), "ERROR 1643 (02000): not found class"));
    ok(ex, "CREATE PROCEDURE sp9() BEGIN SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 1500; END");
    CHECK(says(error_of(ex, "CALL sp9()"), "ERROR 1500 (45000): Unhandled user-defined exception condition"));
    // a trigger refuses the row, and a statement of several rows leaves none of them
    ok(ex, "CREATE TABLE sg (id INT PRIMARY KEY, v INT)");
    ok(ex, "CREATE TRIGGER sg_bi BEFORE INSERT ON sg FOR EACH ROW BEGIN IF NEW.v < 0 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'v must not be negative'; END IF; END");
    ok(ex, "INSERT INTO sg VALUES (10, 5)");
    CHECK(says(error_of(ex, "INSERT INTO sg VALUES (11, -5)"), "v must not be negative"));
    CHECK(says(error_of(ex, "INSERT INTO sg VALUES (12, 1), (13, -1)"), "ERROR 1644 (45000)"));
    CHECK(flat(ex, "SELECT id FROM sg ORDER BY id") == "10");
    ok(ex, "CREATE TRIGGER sg_bd BEFORE DELETE ON sg FOR EACH ROW BEGIN IF OLD.id = 99 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'row 99 is protected'; END IF; END");
    ok(ex, "INSERT INTO sg VALUES (99, 1)");
    CHECK(says(error_of(ex, "DELETE FROM sg WHERE id IN (10, 99)"), "row 99 is protected"));
    CHECK(flat(ex, "SELECT id FROM sg ORDER BY id") == "10;99");
}

TEST_CASE("R90: a loop label closes with END LOOP | WHILE | REPEAT label", "[r90]") {
    TempDataDir dir("r90_data_labels");
    Executor ex(dir.path);
    make_tables(ex);
    ok(ex, "CREATE PROCEDURE lp1() BEGIN DECLARE i INT DEFAULT 0; lp: LOOP SET i = i + 1; IF i >= 3 THEN LEAVE lp; END IF; END LOOP lp; SELECT i; END");
    CHECK(flat(ex, "CALL lp1()") == "3");
    ok(ex, "CREATE PROCEDURE lp2() BEGIN DECLARE i INT DEFAULT 0; DECLARE s INT DEFAULT 0; w: WHILE i < 5 DO SET i = i + 1; IF i = 2 THEN ITERATE w; END IF; SET s = s + i; END WHILE w; SELECT s; END");
    CHECK(flat(ex, "CALL lp2()") == "13");
    ok(ex, "CREATE PROCEDURE lp3() BEGIN DECLARE i INT DEFAULT 0; r: REPEAT SET i = i + 1; UNTIL i >= 4 END REPEAT r; SELECT i; END");
    CHECK(flat(ex, "CALL lp3()") == "4");
    ok(ex, "CREATE PROCEDURE lp4() BEGIN DECLARE i INT DEFAULT 0; outer_l: LOOP SET i = i + 1; inner_l: LOOP SET i = i + 10; LEAVE inner_l; END LOOP inner_l; IF i > 20 THEN LEAVE outer_l; END IF; END LOOP outer_l; SELECT i; END");
    CHECK(flat(ex, "CALL lp4()") == "22");
    // a loop with no label still ends the old way
    ok(ex, "CREATE PROCEDURE lp5() BEGIN DECLARE i INT DEFAULT 0; WHILE i < 3 DO SET i = i + 1; END WHILE; SELECT i; END");
    CHECK(flat(ex, "CALL lp5()") == "3");
}

TEST_CASE("R90: INSERT of no value gives every column its default", "[r90]") {
    TempDataDir dir("r90_data_empty_insert");
    Executor ex(dir.path);
    make_tables(ex);
    ok(ex, "CREATE TABLE em1 (id INT PRIMARY KEY AUTO_INCREMENT, v INT DEFAULT 7, s VARCHAR(10) DEFAULT 'x', d DATE)");
    ok(ex, "INSERT INTO em1 () VALUES ()");
    ok(ex, "INSERT INTO em1 VALUES ()");
    ok(ex, "INSERT INTO em1 () VALUES (), ()");
    ok(ex, "INSERT INTO em1 VALUES (), (), ()");
    CHECK(says(error_of(ex, "INSERT INTO em1 (v) VALUES (), ()"), "doesn't match")); // (a named column wants a value)
    CHECK(flat(ex, "SELECT id, v, s, d FROM em1 ORDER BY id") == "1,7,x,NULL;2,7,x,NULL;3,7,x,NULL;4,7,x,NULL;5,7,x,NULL;6,7,x,NULL;7,7,x,NULL");
    ok(ex, "CREATE TABLE em2 (id INT NOT NULL, v INT)");
    CHECK(says(error_of(ex, "INSERT INTO em2 () VALUES ()"), "default value"));
    CHECK(says(error_of(ex, "INSERT INTO em2 VALUES ()"), "default value"));
    ok(ex, "CREATE TABLE em3 (id INT PRIMARY KEY AUTO_INCREMENT)");
    ok(ex, "INSERT INTO em3 VALUES ()");
    ok(ex, "INSERT INTO em3 () VALUES ()");
    CHECK(flat(ex, "SELECT id FROM em3 ORDER BY id") == "1;2");
    // a column list and a row that do not match are still an error
    CHECK(says(error_of(ex, "INSERT INTO em3 (id) VALUES ()"), "doesn't match"));
}

TEST_CASE("R90: index hints after a table are read and change nothing", "[r90]") {
    TempDataDir dir("r90_data_hints");
    Executor ex(dir.path);
    make_tables(ex);
    ok(ex, "CREATE INDEX ik ON u (k)");
    for (const char* hint : {"USE INDEX (ik)", "FORCE INDEX (ik)", "IGNORE INDEX (ik)", "USE KEY (ik)", "FORCE KEY (ik, ik)", "USE INDEX ()", "USE INDEX FOR ORDER BY (ik)",
                             "FORCE INDEX FOR JOIN (ik)", "IGNORE INDEX FOR GROUP BY (ik)", "USE INDEX (ik) USE INDEX (PRIMARY)", "force index (ik)"}) {
        INFO(hint);
        CHECK(flat(ex, std::string("SELECT id FROM u ") + hint + " WHERE k = 10 ORDER BY id") == "1");
    }
    CHECK(flat(ex, "SELECT x.id FROM u AS x FORCE INDEX (ik) WHERE x.k = 10") == "1");
    CHECK(flat(ex, "SELECT x.id FROM u x FORCE INDEX (ik) WHERE x.k = 10") == "1");
    CHECK(flat(ex, "SELECT t.id, u.name FROM t FORCE INDEX (PRIMARY) JOIN u USE INDEX (ik) ON t.id = u.id ORDER BY t.id") == "1,one;2,Two;3,three");
    CHECK(flat(ex, "SELECT t.id, u.name FROM t USE INDEX () LEFT JOIN u IGNORE INDEX (ik) ON t.i = u.k ORDER BY t.id") == "1,one;2,NULL;3,NULL;9,NULL");
    ok(ex, "UPDATE u USE INDEX (ik) SET name = 'uno' WHERE k = 10");
    CHECK(flat(ex, "SELECT name FROM u WHERE id = 1") == "uno");
    // a table called force or use is still a table, and an alias is still an alias
    ok(ex, "CREATE TABLE force (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO force VALUES (5)");
    CHECK(flat(ex, "SELECT id FROM force") == "5");
    CHECK(flat(ex, "SELECT f.id FROM force f") == "5");
    CHECK(flat(ex, "SELECT id FROM t AS ff WHERE ff.id = 1") == "1");
}
