#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Wrong answers found by comparing with a real MySQL 8.4 (R42), the ones whose exact TEXT matters (the conformance corpora compare numbers by value):
// the text of a DOUBLE and a FLOAT, the types a stored routine declares (kept after a restart too), and a division by zero that must not write a NULL.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
}

std::string fails(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

Rows q(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    Rows rows;
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        rows.push_back(result_text::split_row(line));
    }
    return rows;
}
} // namespace

TEST_CASE("a DOUBLE and a FLOAT column are written the way MySQL writes them", "[r42][double]") {
    TempDataDir dir("r42_double");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE dd (id INT PRIMARY KEY, d DOUBLE, f FLOAT)");
    ok(ex, "INSERT INTO dd VALUES (1, 1000000, 1000000), (2, 1e10, 1e10), (3, 123456789012, 16777216), (4, 1e15, 1e15), (5, 1e16, 1e16), (6, 0.0001, 0.0001),"
           " (7, 0.00001, 0.00001), (8, 1234567.891, 1234567.891), (9, 100, 100), (10, 1e-7, 1e-7), (11, 2000000.5, 2000000.5), (12, -1500000, -1500000),"
           " (13, 0.1, 0.1), (14, 3.14159265358979, 3.14159265358979)");
    // (a DOUBLE is written as MySQL writes it: a number from 1e15 on has an exponent. A FLOAT keeps what a single-precision number holds, in the fewest digits that
    // read back as it: 3.14159265358979 is 3.1415927 -- MySQL shows six digits, 3.14159, but the text here is the value, so arithmetic on it is as exact as the float)
    REQUIRE(q(ex, "SELECT id, d, f FROM dd ORDER BY id") ==
            Rows{{"1", "1000000", "1000000"},        {"2", "10000000000", "10000000000"}, {"3", "123456789012", "16777216"}, {"4", "1e15", "1e15"},
                 {"5", "1e16", "1e16"},              {"6", "0.0001", "0.0001"},           {"7", "0.00001", "0.00001"},      {"8", "1234567.891", "1234567.9"},
                 {"9", "100", "100"},                {"10", "0.0000001", "0.0000001"},    {"11", "2000000.5", "2000000.5"}, {"12", "-1500000", "-1500000"},
                 {"13", "0.1", "0.1"},               {"14", "3.14159265358979", "3.1415927"}});
}

TEST_CASE("a lone ! is NOT", "[r42][lexer]") {
    TempDataDir dir("r42_not");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE n (id INT PRIMARY KEY, i INT)");
    ok(ex, "INSERT INTO n VALUES (1, 10), (2, 0), (3, NULL), (4, 3)");
    REQUIRE(q(ex, "SELECT !0, !5, !NULL, !'a', ! ! 3, !(2 > 1)") == Rows{{"1", "0", "NULL", "1", "1", "0"}});
    REQUIRE(q(ex, "SELECT id FROM n WHERE !(i > 5) ORDER BY id") == Rows{{"2"}, {"4"}});
    REQUIRE(q(ex, "SELECT id FROM n WHERE ! i ORDER BY id") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id FROM n WHERE i != 3 ORDER BY id") == Rows{{"1"}, {"2"}});
}

TEST_CASE("the types of a stored function are kept after a restart", "[r42][routines]") {
    TempDataDir dir("r42_function_types");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE FUNCTION f_total(p_qty INT, p_price DECIMAL(8,2)) RETURNS DECIMAL(10,2) DETERMINISTIC RETURN p_qty * IFNULL(p_price, 0)");
        ok(ex, "CREATE FUNCTION f_half(x INT) RETURNS INT DETERMINISTIC RETURN x / 2");
        ok(ex, "CREATE FUNCTION f_up(x INT UNSIGNED) RETURNS BIGINT UNSIGNED DETERMINISTIC RETURN x + 1");
        REQUIRE(q(ex, "SELECT f_total(2, 1.005), f_total(NULL, 1), f_half(7), f_half('9'), f_half(7.5)") == Rows{{"2.02", "NULL", "4", "5", "4"}});
    }
    Executor ex(dir.path);
    ok(ex, "USE d");
    REQUIRE(q(ex, "SELECT f_total(2, 1.005), f_total(3, 2), f_half(7), f_half(8)") == Rows{{"2.02", "6.00", "4", "4"}});
    REQUIRE(q(ex, "SELECT f_up(5), f_up(4294967295)") == Rows{{"6", "4294967296"}});
    REQUIRE(fails(ex, "SELECT f_up(-1)").find("Out of range value for column 'x'") != std::string::npos);
    REQUIRE(fails(ex, "SELECT f_half('abc')").find("Incorrect integer value: 'abc' for column 'x'") != std::string::npos);
}

TEST_CASE("a procedure's parameters and variables hold values of their declared type", "[r42][routines]") {
    TempDataDir dir("r42_procedure_types");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE pr (id INT PRIMARY KEY AUTO_INCREMENT, v DECIMAL(8,2), n INT, s VARCHAR(10))");
    ok(ex, "CREATE PROCEDURE p_calc(IN a DECIMAL(5,2), IN b INT, OUT r DECIMAL(10,3)) BEGIN DECLARE t DECIMAL(5,2) DEFAULT 0; DECLARE i INT DEFAULT 0;"
           " SET t = a * 3; SET i = b / 3; SET r = t + i; INSERT INTO pr (v, n, s) VALUES (t, i, 'x'); END");
    ok(ex, "CALL p_calc(1.005, 10, @r)");
    REQUIRE(q(ex, "SELECT @r") == Rows{{"6.030"}}); // (a is 1.01, t is 3.03, i is 3, r is 6.030)
    REQUIRE(q(ex, "SELECT v, n FROM pr") == Rows{{"3.03", "3"}});
    REQUIRE(fails(ex, "CALL p_calc(1)").find("Incorrect number of arguments for PROCEDURE d.p_calc; expected 3, got 1") != std::string::npos);
    REQUIRE(fails(ex, "CALL p_calc(1, 2, @a, @b)").find("expected 3, got 4") != std::string::npos);
    REQUIRE(fails(ex, "CALL p_calc(100000, 1, @r)").find("Out of range value for column 'a'") != std::string::npos);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM pr") == Rows{{"1"}}); // (the failed calls wrote nothing)
}

TEST_CASE("a division by zero is an error where a value is written, a NULL where it is shown", "[r42][division]") {
    TempDataDir dir("r42_division");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE dz (id INT PRIMARY KEY, a INT, b INT)");
    ok(ex, "INSERT INTO dz VALUES (0, 8, 2), (1, 9, 0)");
    REQUIRE(q(ex, "SELECT 1 / 0, 5 DIV 0, 5 % 0, MOD(5, 0), a / b FROM dz WHERE id = 1") == Rows{{"NULL", "NULL", "NULL", "NULL", "NULL"}});
    REQUIRE(fails(ex, "INSERT INTO dz VALUES (2, 1 / 0, 5)") == "Division by 0");
    REQUIRE(fails(ex, "INSERT INTO dz SELECT 3, a / b, 1 FROM dz") == "Division by 0");
    REQUIRE(fails(ex, "UPDATE dz SET a = a / b") == "Division by 0");
    REQUIRE(q(ex, "SELECT id, a, b FROM dz ORDER BY id") == Rows{{"0", "8", "2"}, {"1", "9", "0"}}); // (nothing was written)
    ok(ex, "INSERT IGNORE INTO dz VALUES (4, 1 / 0, 1)");
    ok(ex, "UPDATE dz SET a = CASE WHEN b = 0 THEN 0 ELSE a / b END");
    ok(ex, "INSERT INTO dz VALUES (5, IF(1 = 1, 5, 1 / 0), COALESCE(7, 1 / 0))"); // (a branch that is not taken is not worked out)
    REQUIRE(q(ex, "SELECT id, a, b FROM dz ORDER BY id") == Rows{{"0", "4", "2"}, {"1", "0", "0"}, {"4", "NULL", "1"}, {"5", "5", "7"}});
    ok(ex, "CREATE FUNCTION f_div(a INT, b INT) RETURNS DECIMAL(10,3) DETERMINISTIC RETURN a / b");
    REQUIRE(q(ex, "SELECT f_div(1, 3), f_div(10, 4), f_div(NULL, 2)") == Rows{{"0.333", "2.500", "NULL"}});
    REQUIRE(fails(ex, "SELECT f_div(1, 0)") == "Division by 0");
    ok(ex, "CREATE PROCEDURE p_set() BEGIN DECLARE x INT DEFAULT 5; SET @u = 1 / 0; SET x = 5 DIV 0; END");
    REQUIRE(fails(ex, "CALL p_set()") == "Division by 0");
    REQUIRE(q(ex, "SELECT @u") == Rows{{"NULL"}});
}

TEST_CASE("the type a routine declares is read, and a type kept without its places is not applied", "[r42][routines]") {
    REQUIRE_FALSE(Parser::declared_column("x", "DECIMAL").has_value()); // (a catalog from before the types were kept holds DECIMAL(10,2) as DECIMAL)
    REQUIRE_FALSE(Parser::declared_column("x", "").has_value());
    REQUIRE_FALSE(Parser::declared_column("x", "NOT A TYPE").has_value());
    REQUIRE(Parser::declared_column("x", "DECIMAL(10,0)").has_value());
    auto unsigned_int = Parser::declared_column("x", "INT UNSIGNED");
    REQUIRE(unsigned_int.has_value());
    REQUIRE(unsigned_int->is_unsigned);
    REQUIRE_FALSE(Parser::declared_column("x", "INT")->is_unsigned);
    // a DECIMAL written with no places is DECIMAL(10,0): a value is rounded to a whole number
    TempDataDir dir("r42_bare_decimal");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE FUNCTION f_bare(x DECIMAL) RETURNS DECIMAL DETERMINISTIC RETURN x");
    ok(ex, "CREATE FUNCTION f_bare_u(x DECIMAL UNSIGNED) RETURNS DECIMAL DETERMINISTIC RETURN x");
    REQUIRE(q(ex, "SELECT f_bare(3.7), f_bare(12.5), f_bare(-2.5), f_bare(NULL)") == Rows{{"4", "13", "-3", "NULL"}});
    REQUIRE(fails(ex, "SELECT f_bare_u(-1)").find("Out of range value for column 'x'") != std::string::npos);
}

TEST_CASE("an index finds the rows with the columns they have after ALTER TABLE", "[r42][alter]") {
    TempDataDir dir("r42_alter_index");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE m (id INT PRIMARY KEY, a INT, b INT, INDEX ia (a))");
    ok(ex, "INSERT INTO m VALUES (1, 10, 100), (2, 20, 200)");
    // MODIFY converts the values, and the index copies of the rows say so (the text, not only the number: 20.00)
    ok(ex, "ALTER TABLE m MODIFY a DECIMAL(6,2)");
    REQUIRE(q(ex, "SELECT a FROM m WHERE id = 2") == Rows{{"20.00"}});
    REQUIRE(q(ex, "SELECT id, a FROM m WHERE a = 20") == Rows{{"2", "20.00"}});
    // a column dropped and added again is empty, in a row the index finds too
    ok(ex, "ALTER TABLE m DROP COLUMN b");
    ok(ex, "ALTER TABLE m ADD COLUMN b INT");
    REQUIRE(q(ex, "SELECT b FROM m WHERE id = 2") == Rows{{"NULL"}});
    REQUIRE(q(ex, "SELECT id, b FROM m WHERE a = 20") == Rows{{"2", "NULL"}});
    ok(ex, "ALTER TABLE m ADD COLUMN c INT DEFAULT 7");
    REQUIRE(q(ex, "SELECT c FROM m WHERE id = 1") == Rows{{"7"}});
    REQUIRE(q(ex, "SELECT id FROM m WHERE c = 7 ORDER BY id") == Rows{{"1"}, {"2"}});
    // a value that does not fit changes nothing
    REQUIRE(fails(ex, "ALTER TABLE m MODIFY a DECIMAL(3,2)").find("Out of range value for column 'a'") != std::string::npos);
    REQUIRE(q(ex, "SELECT a FROM m WHERE id = 2") == Rows{{"20.00"}});
    // an ENUM that lacks a value the column holds is refused, and the column stays as it was
    ok(ex, "CREATE TABLE e (id INT PRIMARY KEY, v VARCHAR(5))");
    ok(ex, "INSERT INTO e VALUES (1, 'a'), (2, 'x')");
    REQUIRE_FALSE(fails(ex, "ALTER TABLE e MODIFY v ENUM('a','b')").empty());
    REQUIRE(q(ex, "SELECT v FROM e WHERE id = 2") == Rows{{"x"}});
    ok(ex, "UPDATE e SET v = 'b' WHERE id = 2");
    ok(ex, "ALTER TABLE e MODIFY v ENUM('a','b')");
    REQUIRE(q(ex, "SELECT id, v FROM e ORDER BY id") == Rows{{"1", "a"}, {"2", "b"}});
}

TEST_CASE("a CALL answers with the last result table, and LAST_INSERT_ID() follows an INSERT in its body", "[r42][routines]") {
    TempDataDir dir("r42_call_result");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE pt (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(20), qty INT)");
    ok(ex, "INSERT INTO pt (name, qty) VALUES ('a', 5), ('b', 0)");
    ok(ex, "CREATE PROCEDURE pt_add() BEGIN INSERT INTO pt (name, qty) VALUES ('e', 1); SELECT LAST_INSERT_ID(); END");
    REQUIRE(q(ex, "CALL pt_add()") == Rows{{"3"}});
    REQUIRE(q(ex, "SELECT LAST_INSERT_ID()") == Rows{{"3"}});
    ok(ex, "CREATE PROCEDURE pt_say(IN s VARCHAR(10)) BEGIN SELECT CONCAT(s, '!'), UPPER(s); SET @out = CONCAT('got ', s); END");
    REQUIRE(q(ex, "CALL pt_say('hi')") == Rows{{"hi!", "HI"}});
    REQUIRE(q(ex, "SELECT @out") == Rows{{"got hi"}});
    ok(ex, "CREATE PROCEDURE pt_count() BEGIN SELECT COUNT(*) FROM pt; INSERT INTO pt (name, qty) VALUES ('f', 2); END");
    REQUIRE(q(ex, "CALL pt_count()") == Rows{{"3"}});
    REQUIRE(q(ex, "SELECT LAST_INSERT_ID()") == Rows{{"4"}});
    ok(ex, "CREATE PROCEDURE pt_sel() BEGIN INSERT INTO pt (name, qty) SELECT 'g', 9; SELECT LAST_INSERT_ID(); END");
    REQUIRE(q(ex, "CALL pt_sel()") == Rows{{"5"}});
}

TEST_CASE("NEW of a BEFORE INSERT trigger holds the values as the columns will hold them", "[r42][routines][trigger]") {
    TempDataDir dir("r42_trigger_new");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE li (id INT PRIMARY KEY AUTO_INCREMENT, qty INT, price DECIMAL(8,2), total DECIMAL(8,2), note VARCHAR(5))");
    ok(ex, "CREATE TRIGGER li_bi BEFORE INSERT ON li FOR EACH ROW SET NEW.total = NEW.qty * NEW.price");
    ok(ex, "INSERT INTO li (qty, price) VALUES (3, 1.005), ('2', 10.555)");
    REQUIRE(q(ex, "SELECT id, qty, price, total FROM li ORDER BY id") == Rows{{"1", "3", "1.01", "3.03"}, {"2", "2", "10.56", "21.12"}}); // (3 * 1.005 was 3.02)
    REQUIRE_FALSE(fails(ex, "INSERT INTO li (qty, price) VALUES (100000, 9999.99)").empty()); // (the total does not fit)
    REQUIRE(q(ex, "SELECT COUNT(*) FROM li") == Rows{{"2"}});
    REQUIRE_FALSE(fails(ex, "INSERT INTO li (qty, price) VALUES ('abc', 1)").empty()); // (a value that does not fit is refused by the INSERT, as without a trigger)
}

TEST_CASE("the numbers a trigger gives out are not what the statement that fired it reports", "[r42][routines][trigger]") {
    TempDataDir dir("r42_trigger_ids");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(5))");
    ok(ex, "CREATE TABLE lg (id INT PRIMARY KEY AUTO_INCREMENT, what VARCHAR(10))");
    ok(ex, "INSERT INTO lg (what) VALUES ('1'), ('2'), ('3'), ('4'), ('5')"); // (the next number of lg is 6, of a is 1)
    ok(ex, "CREATE TRIGGER a_bi BEFORE INSERT ON a FOR EACH ROW INSERT INTO lg (what) VALUES ('before')");
    ok(ex, "CREATE TRIGGER a_ai AFTER INSERT ON a FOR EACH ROW INSERT INTO lg (what) VALUES ('after')");
    ok(ex, "INSERT INTO a (v) VALUES ('x'), ('y')");
    REQUIRE(ex.statement_insert_id() == 1); // (what the statement reports, as the OK packet of the MySQL protocol: a's first number, not the 6 of lg)
    REQUIRE(q(ex, "SELECT LAST_INSERT_ID()") == Rows{{"1"}});
    ok(ex, "INSERT INTO a (v) VALUES ('z')");
    REQUIRE(ex.statement_insert_id() == 3);
    REQUIRE(q(ex, "SELECT LAST_INSERT_ID()") == Rows{{"3"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM lg") == Rows{{"11"}}); // (five, and a before and an after for each of the three rows)
    // a statement that gives no number out reports none, and leaves LAST_INSERT_ID() as it was, whatever its trigger numbered
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, v VARCHAR(5))");
    ok(ex, "CREATE TRIGGER b_bi BEFORE INSERT ON b FOR EACH ROW INSERT INTO lg (what) VALUES ('b')");
    ok(ex, "INSERT INTO b VALUES (1, 'x')");
    REQUIRE(ex.statement_insert_id() == 0);
    REQUIRE(q(ex, "SELECT LAST_INSERT_ID()") == Rows{{"3"}});
}
