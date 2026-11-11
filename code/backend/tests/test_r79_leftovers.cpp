// R79: a text against a number in IN (subquery) and CHECK, ORDER BY / LIMIT after a SELECT without FROM, CREATE DATABASE and CREATE TABLE ... SELECT, and the privilege
// leftovers (DROP INDEX without ON, the columns DESCRIBE and information_schema show to an account that holds a column privilege). The answers were measured on
// MySQL 8.4; code/test/diff/corpus/{text_number_in,dual_order_limit,create_database,privileges}.sql repeat them against the server.

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

// the number of an error "ERROR 1142 (42000): ...", 0 for no error, -1 for an error without a number
int errno_of(const StringResult& r) {
    if (r.is_ok()) return 0;
    const std::string& e = r.error();
    if (e.compare(0, 6, "ERROR ") != 0) return -1;
    return std::stoi(e.substr(6, 4));
}

// a CHECK that refuses the statement: the error says so (the wire layer gives it MySQL's number 3819); a statement that goes through
bool violates(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    return r.is_err() && r.error().find("CHECK constraint") != std::string::npos;
}
bool passes(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    return r.is_ok();
}

// a database with a root session in the database v
struct World {
    TempDataDir dir;
    Executor root;
    explicit World(const std::string& name) : dir(name), root(dir.path) {
        ok(root, "CREATE DATABASE v");
        ok(root, "USE v");
    }
    Executor session(const std::string& user) {
        ok(root, "CREATE USER IF NOT EXISTS '" + user + "'@'%' IDENTIFIED BY 'pw'");
        Executor ex = Executor::new_session(root.get_shared());
        ex.auth_user = user;
        ex.activate_default_roles();
        ex.current_db = "v";
        return ex;
    }
};
} // namespace

TEST_CASE("IN (subquery) compares a text with a number as numbers", "[r79]") {
    World w("r79_data_in");
    Executor& ex = w.root;
    ok(ex, "CREATE TABLE txt (id INT PRIMARY KEY, s VARCHAR(10))");
    ok(ex, "INSERT INTO txt VALUES (1,'007'),(2,'7'),(3,'7.0'),(4,'abc'),(5,'0'),(6,' 7'),(7,NULL),(8,'10'),(9,'9')");
    ok(ex, "CREATE TABLE num (id INT PRIMARY KEY, n INT, d DECIMAL(5,2))");
    ok(ex, "INSERT INTO num VALUES (1,7,7.00),(2,0,0.50),(3,10,NULL),(4,NULL,9.00)");

    // a number on the other side: every text that reads as that number is in
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s IN (SELECT 7) ORDER BY id") == "1,2,3,6");
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s NOT IN (SELECT 7) ORDER BY id") == "4,5,8,9"); // (the NULL row is neither)
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s IN (SELECT n FROM num) ORDER BY id") == "1,2,3,4,5,6,8"); // ('abc' reads as 0, which n holds)
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s IN (SELECT 7.0)") == "1,2,3,6");
    // ... and the other way round: a number against the texts of a column
    CHECK(first_column(ex, "SELECT id FROM num WHERE n IN (SELECT s FROM txt) ORDER BY id") == "1,2,3");
    CHECK(first_column(ex, "SELECT '007' IN (SELECT 7), 'abc' IN (SELECT 0), 7 IN (SELECT '007'), 0 IN (SELECT 'abc')") == "1");
    // a subquery that is run for each row compares the same way
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s IN (SELECT n FROM num WHERE n >= txt.id) ORDER BY id") == "1,2,3,6,8");
    // two texts stay texts
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s IN (SELECT '7') ORDER BY id") == "2");
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s IN (SELECT 'ABC') ORDER BY id") == "4");
    // a row against a row: each pair compares by its own types
    CHECK(first_column(ex, "SELECT id FROM txt WHERE (s, id) IN (SELECT n, 1 FROM num) ORDER BY id") == "1");
    CHECK(first_column(ex, "SELECT id FROM txt WHERE (id, s) IN (SELECT 1, 7) ORDER BY id") == "1");
    CHECK(first_column(ex, "SELECT id FROM txt WHERE (id, s) IN (SELECT 1, '7') ORDER BY id") == "");
    // the writes read the subquery the same way: a text that is no number makes UPDATE and DELETE fail (1292, a plain comparison does too) and nothing changes
    auto refused = ex.execute_sql("UPDATE txt SET s = 'x' WHERE s IN (SELECT 7)");
    REQUIRE(refused.is_err());
    CHECK(refused.error().find("Truncated incorrect DOUBLE value") != std::string::npos);
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s = 'x' ORDER BY id") == "");
    ok(ex, "DELETE FROM txt WHERE id = 4");
    ok(ex, "UPDATE txt SET s = 'x' WHERE s IN (SELECT 7)");
    CHECK(first_column(ex, "SELECT id FROM txt WHERE s = 'x' ORDER BY id") == "1,2,3,6");
}

TEST_CASE("CHECK compares a text column as text and a number as a number", "[r79]") {
    World w("r79_data_check");
    Executor& ex = w.root;
    ok(ex, "CREATE TABLE ck1 (s VARCHAR(10), CHECK (s = '7'))");
    ok(ex, "CREATE TABLE ck2 (s VARCHAR(10), CHECK (s = 7))");
    ok(ex, "CREATE TABLE ck3 (s VARCHAR(10), CHECK (s > '10'))");
    ok(ex, "CREATE TABLE ck4 (s VARCHAR(10) CHECK (s <> '7'))");
    ok(ex, "CREATE TABLE ck6 (s VARCHAR(10), CHECK (s IN ('7', '8')))");
    ok(ex, "CREATE TABLE ck7 (s VARCHAR(10), CHECK (s BETWEEN '1' AND '3'))");

    CHECK(violates(ex, "INSERT INTO ck1 VALUES ('007')"));
    CHECK(passes(ex, "INSERT INTO ck1 VALUES ('7')"));
    CHECK(passes(ex, "INSERT INTO ck2 VALUES ('007')")); // (a number on the right: the text is read as one)
    CHECK(violates(ex, "INSERT INTO ck2 VALUES ('abc')"));
    CHECK(passes(ex, "INSERT INTO ck3 VALUES ('9')")); // ('9' > '10' as texts)
    CHECK(violates(ex, "INSERT INTO ck3 VALUES ('10')"));
    CHECK(passes(ex, "INSERT INTO ck4 VALUES ('007')"));
    CHECK(violates(ex, "INSERT INTO ck4 VALUES ('7')"));
    CHECK(violates(ex, "INSERT INTO ck6 VALUES ('007')"));
    CHECK(passes(ex, "INSERT INTO ck6 VALUES ('8')"));
    CHECK(passes(ex, "INSERT INTO ck7 VALUES ('15')")); // ('1' <= '15' <= '3' as texts)
    CHECK(violates(ex, "INSERT INTO ck7 VALUES ('4')"));
    // an update meets the same condition, and a refused row leaves the table as it was
    CHECK(violates(ex, "UPDATE ck1 SET s = '007'"));
    CHECK(first_column(ex, "SELECT s FROM ck1") == "7");
    CHECK(passes(ex, "INSERT INTO ck1 VALUES ('7') ON DUPLICATE KEY UPDATE s = '007'")); // (no unique key: a second row)
    CHECK(first_column(ex, "SELECT COUNT(*) FROM ck1") == "2");

    // ADD CHECK looks at the rows the table has
    ok(ex, "CREATE TABLE ck8 (s VARCHAR(10))");
    ok(ex, "INSERT INTO ck8 VALUES ('007'), ('7')");
    CHECK(violates(ex, "ALTER TABLE ck8 ADD CONSTRAINT c8 CHECK (s = '7')"));
    CHECK(passes(ex, "ALTER TABLE ck8 ADD CONSTRAINT c8 CHECK (s = 7)"));
    CHECK(violates(ex, "INSERT INTO ck8 VALUES ('abc')"));
    CHECK(passes(ex, "INSERT INTO ck8 VALUES ('007.0')"));
}

TEST_CASE("CHECK of a number column against a text, and the kept condition follows the table", "[r79]") {
    World w("r79_data_check_number");
    Executor& ex = w.root;
    ok(ex, "CREATE TABLE cu (n INT, CHECK (n = '7'))");
    CHECK(passes(ex, "INSERT INTO cu VALUES (7)"));
    CHECK(violates(ex, "INSERT INTO cu VALUES (8)"));
    // one statement of many rows evaluates the same condition for each: the second row is the one that fails and nothing is written
    CHECK(violates(ex, "INSERT INTO cu VALUES (7), (9)"));
    CHECK(first_column(ex, "SELECT COUNT(*) FROM cu") == "1");
    // a text that is no number, read as one, is a violation (MySQL 3819)
    ok(ex, "CREATE TABLE ck5 (s VARCHAR(10), n INT, CHECK (s = n))");
    CHECK(violates(ex, "INSERT INTO ck5 VALUES ('abc', 0)"));
    CHECK(passes(ex, "INSERT INTO ck5 VALUES ('007', 7)"));
    CHECK(violates(ex, "INSERT INTO ck5 VALUES ('7', 8)"));
    // a table of the same name with another type (dropped and made again) is checked by its own columns
    ok(ex, "DROP TABLE cu");
    ok(ex, "CREATE TABLE cu (n VARCHAR(10), CHECK (n = '7'))");
    CHECK(violates(ex, "INSERT INTO cu VALUES ('007')"));
}

TEST_CASE("a SELECT without FROM takes ORDER BY, LIMIT and OFFSET", "[r79]") {
    World w("r79_data_dual");
    Executor& ex = w.root;
    CHECK(first_column(ex, "SELECT 5 UNION SELECT 3 ORDER BY 1") == "3,5");
    CHECK(first_column(ex, "SELECT 5 UNION SELECT 3 ORDER BY 1 DESC") == "5,3");
    CHECK(first_column(ex, "SELECT 5 UNION SELECT 3 ORDER BY 1 DESC LIMIT 1") == "5");
    CHECK(first_column(ex, "SELECT 5 UNION SELECT 3 UNION SELECT 4 ORDER BY 1") == "3,4,5");
    CHECK(first_column(ex, "SELECT 5 UNION ALL SELECT 3 UNION ALL SELECT 5 ORDER BY 1") == "3,5,5");
    CHECK(first_column(ex, "SELECT 5 AS a UNION SELECT 3 ORDER BY a") == "3,5");
    CHECK(first_column(ex, "SELECT 3 UNION SELECT 1 WHERE 1 = 1 ORDER BY 1") == "1,3");
    CHECK(first_column(ex, "SELECT 5 UNION SELECT 3 LIMIT 1 OFFSET 1") == "3");
    CHECK(first_column(ex, "SELECT 1 ORDER BY 1") == "1");
    CHECK(first_column(ex, "SELECT 1 LIMIT 1") == "1");
    CHECK(first_column(ex, "SELECT 1 LIMIT 0") == "");
    CHECK(first_column(ex, "SELECT 1 LIMIT 1 OFFSET 1") == "");
    CHECK(first_column(ex, "SELECT 1 WHERE 1 = 0 ORDER BY 1") == "");
    CHECK(first_column(ex, "SELECT 5 EXCEPT SELECT 3 ORDER BY 1") == "5");
    CHECK(first_column(ex, "SELECT * FROM (SELECT 5 UNION SELECT 3 ORDER BY 1 DESC) d") == "5,3");
    // a chain is read from the left, and INTERSECT binds tighter than UNION and EXCEPT; the ORDER BY / LIMIT at its end are for the whole result
    CHECK(first_column(ex, "SELECT 5 UNION ALL SELECT 5 UNION SELECT 5") == "5"); // (the UNION takes the duplicates out of all that is before it)
    CHECK(first_column(ex, "SELECT 5 UNION SELECT 5 UNION ALL SELECT 5") == "5,5");
    CHECK(first_column(ex, "SELECT 1 UNION ALL SELECT 1 EXCEPT SELECT 1") == "");
    CHECK(first_column(ex, "SELECT 1 UNION SELECT 2 INTERSECT SELECT 2 ORDER BY 1") == "1,2"); // (1 UNION (2 INTERSECT 2))
    CHECK(first_column(ex, "SELECT 1 UNION SELECT 2 INTERSECT SELECT 3 ORDER BY 1") == "1");
    CHECK(first_column(ex, "SELECT 2 INTERSECT SELECT 2 UNION SELECT 7 ORDER BY 1 DESC") == "7,2");
    CHECK(first_column(ex, "(SELECT 1) UNION (SELECT 2) UNION (SELECT 3) ORDER BY 1 DESC") == "3,2,1");
    CHECK(first_column(ex, "SELECT * FROM (SELECT 3 UNION SELECT 1 UNION SELECT 2 ORDER BY 1 DESC LIMIT 2) d") == "3,2");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO t VALUES (1), (2)");
    CHECK(first_column(ex, "SELECT id FROM t UNION SELECT 9 ORDER BY 1 DESC") == "9,2,1");
    CHECK(first_column(ex, "SELECT id FROM t WHERE id IN (SELECT 1 UNION SELECT 2 ORDER BY 1 DESC LIMIT 1)") == "2");
}

TEST_CASE("CREATE DATABASE leaves the session where it is", "[r79]") {
    World w("r79_data_create_db");
    Executor& ex = w.root;
    auto created = ex.execute_sql("CREATE DATABASE other");
    REQUIRE(created.is_ok());
    CHECK(ex.statement_affected_rows(created.value()) == 1); // (the OK packet of CREATE DATABASE says 1)
    CHECK(first_column(ex, "SELECT DATABASE()") == "v");
    ok(ex, "CREATE TABLE here (a INT)");
    ok(ex, "CREATE TABLE other.there (a INT)");
    CHECK(first_column(ex, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'here'") == "1");
    CHECK(first_column(ex, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'other' AND table_name = 'there'") == "1");
    ok(ex, "USE other");
    CHECK(first_column(ex, "SELECT DATABASE()") == "other");
    ok(ex, "CREATE VIEW vw AS SELECT 1 AS one");
    auto dropped = ex.execute_sql("DROP DATABASE other");
    REQUIRE(dropped.is_ok());
    CHECK(ex.statement_affected_rows(dropped.value()) == 2); // (the tables and views it took away)
}

TEST_CASE("CREATE TABLE ... SELECT reports the rows it copied", "[r79]") {
    World w("r79_data_ctas");
    Executor& ex = w.root;
    ok(ex, "CREATE TABLE s (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO s VALUES (1), (2), (3)");
    auto made = ex.execute_sql("CREATE TABLE c AS SELECT id FROM s");
    REQUIRE(made.is_ok());
    CHECK(ex.statement_affected_rows(made.value()) == 3);
    CHECK(first_column(ex, "SELECT ROW_COUNT()") == "3");
    auto none = ex.execute_sql("CREATE TABLE d AS SELECT id FROM s WHERE id > 100");
    REQUIRE(none.is_ok());
    CHECK(ex.statement_affected_rows(none.value()) == 0);
    auto plain = ex.execute_sql("CREATE TABLE e (a INT)");
    REQUIRE(plain.is_ok());
    CHECK(ex.statement_affected_rows(plain.value()) == 0);
}

TEST_CASE("DROP INDEX without ON needs the INDEX privilege on the table that has the index", "[r79][privileges]") {
    World w("r79_data_drop_index");
    Executor& root = w.root;
    ok(root, "CREATE TABLE t (id INT PRIMARY KEY, i INT)");
    ok(root, "CREATE TABLE u (id INT PRIMARY KEY, j INT)");
    ok(root, "CREATE INDEX ix_i ON t (i)");
    ok(root, "CREATE INDEX ix_j ON u (j)");
    ok(root, "CREATE USER 'alice'@'%' IDENTIFIED BY 'pw'");
    ok(root, "GRANT INDEX ON v.t TO 'alice'@'%'");
    Executor alice = w.session("alice");
    CHECK(errno_of(alice.execute_sql("DROP INDEX ix_j")) == 1142); // (the index is on u)
    CHECK(errno_of(alice.execute_sql("DROP INDEX ix_j ON u")) == 1142);
    CHECK(first_column(root, "SELECT index_name FROM information_schema.statistics WHERE table_name = 'u' AND index_name = 'ix_j'") == "ix_j");
    CHECK(errno_of(alice.execute_sql("DROP INDEX ix_i")) == 0);
    CHECK(first_column(root, "SELECT index_name FROM information_schema.statistics WHERE table_name = 't' AND index_name = 'ix_i'") == "");
    // an index of another database is no business of an account that holds INDEX here only
    ok(root, "CREATE DATABASE w");
    ok(root, "CREATE TABLE w.x (id INT PRIMARY KEY, k INT)");
    ok(root, "CREATE INDEX ix_k ON w.x (k)");
    CHECK(errno_of(alice.execute_sql("DROP INDEX ix_k")) == 1142);
    CHECK(first_column(root, "SELECT index_name FROM information_schema.statistics WHERE table_schema = 'w' AND index_name = 'ix_k'") == "ix_k");
}

TEST_CASE("an account that holds a column privilege sees those columns only", "[r79][privileges]") {
    World w("r79_data_columns");
    Executor& root = w.root;
    ok(root, "CREATE TABLE pc (a INT PRIMARY KEY, b INT, c INT)");
    ok(root, "CREATE USER 'colonly'@'%' IDENTIFIED BY 'pw'");
    ok(root, "GRANT SELECT (a) ON v.pc TO 'colonly'@'%'");
    ok(root, "CREATE USER 'twocol'@'%' IDENTIFIED BY 'pw'");
    ok(root, "GRANT SELECT (a), INSERT (c) ON v.pc TO 'twocol'@'%'");
    ok(root, "CREATE USER 'tblsel'@'%' IDENTIFIED BY 'pw'");
    ok(root, "GRANT SELECT ON v.pc TO 'tblsel'@'%'");

    Executor colonly = w.session("colonly");
    auto described = colonly.execute_sql("DESCRIBE pc");
    REQUIRE(described.is_ok());
    CHECK(described.value().find("| a ") != std::string::npos);
    CHECK(described.value().find("| b ") == std::string::npos);
    CHECK(described.value().find("| c ") == std::string::npos);
    CHECK(first_column(colonly, "SELECT column_name FROM information_schema.columns WHERE table_name = 'pc' ORDER BY ordinal_position") == "a");

    Executor twocol = w.session("twocol");
    CHECK(first_column(twocol, "SELECT column_name FROM information_schema.columns WHERE table_name = 'pc' ORDER BY ordinal_position") == "a,c");

    Executor tblsel = w.session("tblsel"); // (a privilege on the table covers every column)
    CHECK(first_column(tblsel, "SELECT column_name FROM information_schema.columns WHERE table_name = 'pc' ORDER BY ordinal_position") == "a,b,c");
    CHECK(first_column(root, "SELECT column_name FROM information_schema.columns WHERE table_name = 'pc' ORDER BY ordinal_position") == "a,b,c");
}
