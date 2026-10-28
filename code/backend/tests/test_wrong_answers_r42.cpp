#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
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
    // (a DOUBLE is written as MySQL writes it: a number from 1e15 on has an exponent. A FLOAT holds a single-precision number, in the fewest digits that read back as
    // it -- 3.14159265358979 is 3.1415927 -- and is shown with six digits as MySQL shows it, 3.14159 (R44 7); the stored value keeps all of them)
    REQUIRE(q(ex, "SELECT id, d, f FROM dd ORDER BY id") ==
            Rows{{"1", "1000000", "1000000"},        {"2", "10000000000", "10000000000"}, {"3", "123456789012", "16777200"}, {"4", "1e15", "1e15"},
                 {"5", "1e16", "1e16"},              {"6", "0.0001", "0.0001"},           {"7", "0.00001", "0.00001"},      {"8", "1234567.891", "1234570"},
                 {"9", "100", "100"},                {"10", "0.0000001", "0.0000001"},    {"11", "2000000.5", "2000000"},   {"12", "-1500000", "-1500000"},
                 {"13", "0.1", "0.1"},               {"14", "3.14159265358979", "3.14159"}});
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

TEST_CASE(":= assigns where a value is assigned and is an error anywhere else", "[r43][lexer]") {
    TempDataDir dir("r43_colon_eq");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE x (id INT PRIMARY KEY, n INT)");
    ok(ex, "INSERT INTO x VALUES (1, 1), (2, 2)");
    ok(ex, "SET @a := 5");
    REQUIRE(q(ex, "SELECT @a") == Rows{{"5"}});
    ok(ex, "UPDATE x SET n := 9 WHERE id = 2");
    ok(ex, "INSERT INTO x SET id = 3, n := 4");
    ok(ex, "INSERT INTO x VALUES (3, 1) ON DUPLICATE KEY UPDATE n := 7");
    REQUIRE(q(ex, "SELECT id, n FROM x ORDER BY id") == Rows{{"1", "1"}, {"2", "9"}, {"3", "7"}});
    ok(ex, "CREATE TABLE y (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO y VALUES (1), (3)");
    ok(ex, "UPDATE x JOIN y ON y.id = x.id SET x.n := 100");
    REQUIRE(q(ex, "SELECT id, n FROM x ORDER BY id") == Rows{{"1", "100"}, {"2", "9"}, {"3", "100"}});
    ok(ex, "MERGE INTO x t USING y s ON t.id = s.id WHEN MATCHED THEN UPDATE SET n := 5");
    REQUIRE(q(ex, "SELECT id, n FROM x ORDER BY id") == Rows{{"1", "5"}, {"2", "9"}, {"3", "5"}});
    // as an operator inside an expression it is not supported -- and said so, where it used to be read as a comparison (NULL) with the colon dropped
    REQUIRE_FALSE(fails(ex, "SELECT @b := 7").empty());
    REQUIRE_FALSE(fails(ex, "SELECT @i := @i + 1").empty());
    REQUIRE(q(ex, "SELECT @b") == Rows{{"NULL"}});
    // a label (`lp:`) still is a label
    ok(ex, "CREATE PROCEDURE lab(IN n INT) BEGIN DECLARE i INT DEFAULT 0; lp: WHILE i < n DO SET i := i + 1; IF i > 5 THEN LEAVE lp; END IF; END WHILE; SELECT i; END");
    REQUIRE(q(ex, "CALL lab(3)") == Rows{{"3"}});
    REQUIRE(q(ex, "CALL lab(9)") == Rows{{"6"}});
}

TEST_CASE("a text that is not a number is refused where a value is written", "[r43][strict]") {
    TempDataDir dir("r43_text_number");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE src (id INT PRIMARY KEY, s VARCHAR(20), n INT)");
    ok(ex, "INSERT INTO src VALUES (1, 'abc', 1), (2, '12', 2), (3, '12abc', 3), (4, '', 4), (5, '0x10', 5), (6, ' 7 ', 6)");
    ok(ex, "CREATE TABLE dst (id INT PRIMARY KEY, v DOUBLE)");
    // a SELECT reads the number the text starts with
    REQUIRE(q(ex, "SELECT id, s + 1 FROM src ORDER BY id") == Rows{{"1", "1"}, {"2", "13"}, {"3", "13"}, {"4", "1"}, {"5", "1"}, {"6", "8"}});
    // what writes refuses the ones that are not wholly a number (a blank text is 0, a number may have blanks around it)
    REQUIRE(fails(ex, "INSERT INTO dst SELECT id, s + 1 FROM src WHERE id = 1") == "Truncated incorrect DOUBLE value: 'abc'");
    REQUIRE(fails(ex, "INSERT INTO dst SELECT id, s * 2 FROM src WHERE id = 3") == "Truncated incorrect DOUBLE value: '12abc'");
    REQUIRE(fails(ex, "INSERT INTO dst VALUES (7, 5 - 'abc')") == "Truncated incorrect DOUBLE value: 'abc'");
    REQUIRE(fails(ex, "UPDATE src SET n = s + 1 WHERE id = 5") == "Truncated incorrect DOUBLE value: '0x10'");
    ok(ex, "INSERT INTO dst SELECT id, s + 1 FROM src WHERE id IN (2, 4, 6)");
    ok(ex, "INSERT IGNORE INTO dst VALUES (8, 'abc' + 1)"); // (a warning there)
    REQUIRE(q(ex, "SELECT id, v FROM dst ORDER BY id") == Rows{{"2", "13"}, {"4", "1"}, {"6", "8"}, {"8", "1"}});
    REQUIRE(q(ex, "SELECT id, n FROM src WHERE id = 5") == Rows{{"5", "5"}}); // (the failed UPDATE changed nothing)
    // a date plus an interval is not a text read as a number
    ok(ex, "CREATE TABLE dt (d DATE)");
    ok(ex, "INSERT INTO dt VALUES ('2024-01-31' + INTERVAL 1 DAY)");
    REQUIRE(q(ex, "SELECT d FROM dt") == Rows{{"2024-02-01"}});
    // nor is a date or a time read as a number (MySQL turns it into 20240305; the number here is the one it starts with)
    ok(ex, "CREATE TABLE tm (id INT PRIMARY KEY, d DATE, t TIME, dt DATETIME)");
    ok(ex, "INSERT INTO tm VALUES (1, '2024-03-05', '12:30:00', '2024-03-05 12:30:00')");
    ok(ex, "INSERT INTO dst SELECT id + 100, d + 0 FROM tm");
    ok(ex, "INSERT INTO dst SELECT id + 101, t + 0 FROM tm");
    ok(ex, "INSERT INTO dst SELECT id + 102, dt + 0 FROM tm");
    ok(ex, "UPDATE src SET n = 1 + 1 WHERE id = 2");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM dst WHERE id >= 100") == Rows{{"3"}});
}

TEST_CASE("a negative number made unsigned: below the smallest BIGINT it is 2^63, the rest wraps round (exact text)", "[r44][unsigned]") {
    TempDataDir dir("r44_unsigned_negative");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // (the conformance corpus compares numbers as doubles, which cannot tell 9223372036854775807 from 9223372036854775808)
    REQUIRE(q(ex, "SELECT CAST('-18446744073709551615' AS UNSIGNED), CAST('-9223372036854775809' AS UNSIGNED), CAST('-9223372036854775808' AS UNSIGNED),"
                  " CAST('-9223372036854775807' AS UNSIGNED), CAST('-1' AS UNSIGNED), CAST('-1.5' AS UNSIGNED), CAST('-0.4' AS UNSIGNED),"
                  " CAST('-999999999999999999999' AS UNSIGNED)") ==
            Rows{{"9223372036854775808", "9223372036854775808", "9223372036854775808", "9223372036854775809", "18446744073709551615", "18446744073709551615", "0",
                  "9223372036854775808"}});
    REQUIRE(q(ex, "SELECT -9223372036854775809 | 0, -18446744073709551615 | 0, -9223372036854775808 | 0, -1 | 0, -2 & 255, ~-9223372036854775809,"
                  " -9223372036854775809 >> 1, -9223372036854775809 ^ 1, -999999999999999999999 | 0, '-999999999999999999999' | 0") ==
            Rows{{"9223372036854775808", "9223372036854775808", "9223372036854775808", "18446744073709551615", "254", "9223372036854775807", "4611686018427387904",
                  "9223372036854775809", "9223372036854775808", "9223372036854775808"}});
}

TEST_CASE("the rows a statement affects are the rows it changed; a client that asks for found rows gets them", "[r44][affected]") {
    TempDataDir dir("r44_affected");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ar (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(10), u INT UNIQUE, n INT DEFAULT 0)");
    ok(ex, "INSERT INTO ar (v, u) VALUES ('a', 1), ('b', 2), ('c', 3)");
    auto affected = [&](const std::string& sql) { // (what the OK packet of the MySQL protocol and ROW_COUNT() say)
        auto r = ex.execute_sql(sql);
        INFO(sql);
        REQUIRE(r.is_ok());
        return ex.statement_affected_rows(r.value());
    };
    REQUIRE(affected("UPDATE ar SET n = 5 WHERE u <= 3") == 3);
    REQUIRE(affected("UPDATE ar SET n = 5 WHERE u <= 3") == 0); // (found, not changed)
    REQUIRE(q(ex, "SELECT ROW_COUNT()") == Rows{{"0"}});
    ex.client_found_rows = true; // CLIENT_FOUND_ROWS: Connector/J, MySQLdb
    REQUIRE(affected("UPDATE ar SET n = 5 WHERE u <= 3") == 3);
    REQUIRE(affected("UPDATE ar SET n = 6 WHERE u <= 2") == 2);
    ex.client_found_rows = false;
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('p', 2), ('q', 20), ('r', 3) ON DUPLICATE KEY UPDATE v = CONCAT(v, '!')") == 5); // 2 + 1 + 2
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('p', 2), ('q', 20) ON DUPLICATE KEY UPDATE v = v") == 0);
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('a2', 1) ON DUPLICATE KEY UPDATE v = 'changed'") == 2);
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('a3', 1) ON DUPLICATE KEY UPDATE v = 'changed'") == 0);
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('x', 1), ('x', 1), ('y', 77) ON DUPLICATE KEY UPDATE n = n + 1") == 5); // 2 + 2 + 1
    REQUIRE(affected("REPLACE INTO ar (v, u) VALUES ('r1', 1), ('r2', 30)") == 3); // 2 + 1
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('m1', 40), ('m2', 41)") == 2);
    REQUIRE(affected("DELETE FROM ar WHERE u >= 40") == 3); // (the row 'y' with 77 too)
    REQUIRE(affected("INSERT IGNORE INTO ar (v, u) VALUES ('d', 2), ('e', 90)") == 1);
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('t1', 500), ('t1', 500), ('t2', 501) ON DUPLICATE KEY UPDATE n = n + 1") == 4); // 1 + (the 2nd row changes the 1st) 2 + 1
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('t3', 600), ('t3', 600) ON DUPLICATE KEY UPDATE n = n") == 1);
    // the statements of a trigger do not change what the statement that fired it affected
    ok(ex, "CREATE TABLE lg2 (id INT PRIMARY KEY AUTO_INCREMENT, k INT)");
    ok(ex, "INSERT INTO lg2 (k) VALUES (1), (1), (1)");
    ok(ex, "CREATE TRIGGER ar_ai AFTER INSERT ON ar FOR EACH ROW UPDATE lg2 SET k = k + 1");
    ok(ex, "CREATE TRIGGER ar_ad AFTER DELETE ON ar FOR EACH ROW UPDATE lg2 SET k = k + 1");
    ex.client_found_rows = true;
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('tr', 700)") == 1);
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('tr2', 701) ON DUPLICATE KEY UPDATE v = 'x'") == 1);
    REQUIRE(affected("INSERT INTO ar (v, u) VALUES ('tr2', 701) ON DUPLICATE KEY UPDATE v = 'y'") == 2);
    ok(ex, "CREATE TRIGGER ar_au AFTER UPDATE ON ar FOR EACH ROW UPDATE lg2 SET k = k + 1");
    REQUIRE(affected("UPDATE ar SET n = 9 WHERE u >= 700") == 2);
    ex.client_found_rows = false;
    REQUIRE(affected("UPDATE ar SET n = 9 WHERE u >= 700") == 0);
    REQUIRE(affected("UPDATE ar SET n = 10 WHERE u >= 700") == 2);
    ex.client_found_rows = true;
    REQUIRE(affected("DELETE FROM ar WHERE u >= 700") == 2);
    // ON DUPLICATE KEY UPDATE: a row it sets to what it holds is 0 changed rows, 1 found row
    for (const bool found_rows : {false, true}) {
        const std::string name = found_rows ? "afr_found" : "afr_changed";
        ex.client_found_rows = found_rows;
        ok(ex, "CREATE TABLE " + name + " (id INT PRIMARY KEY, n INT)");
        ok(ex, "INSERT INTO " + name + " VALUES (1, 0), (2, 0), (3, 0)");
        const std::string odku = "INSERT INTO " + name + " VALUES ";
        REQUIRE(affected(odku + "(1, 9) ON DUPLICATE KEY UPDATE n = 9") == 2);
        REQUIRE(affected(odku + "(1, 9) ON DUPLICATE KEY UPDATE n = 9") == (found_rows ? 1 : 0));
        REQUIRE(affected(odku + "(1, 9), (2, 9), (4, 9) ON DUPLICATE KEY UPDATE n = 9") == (found_rows ? 4 : 3));
        REQUIRE(affected(odku + "(5, 1), (5, 1) ON DUPLICATE KEY UPDATE n = n") == (found_rows ? 2 : 1));
        REQUIRE(affected(odku + "(6, 1), (6, 2) ON DUPLICATE KEY UPDATE n = VALUES(n)") == 3);
        REQUIRE(affected("REPLACE INTO " + name + " VALUES (1, 8), (7, 1)") == 3); // (2 for the row it replaces, 1 for the new one)
    }
    ex.client_found_rows = false;
}

TEST_CASE("the WHERE of UPDATE and DELETE fails on a division by zero or a text that is no number, whichever way the rows are read", "[r44][where_errors]") {
    TempDataDir dir("r44_where_errors");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE w (id INT PRIMARY KEY, n INT, s VARCHAR(5), b INT)");
    ok(ex, "CREATE TABLE wc (id INT PRIMARY KEY, w_id INT, FOREIGN KEY (w_id) REFERENCES w(id))"); // (a table a foreign key points at is deleted from another way)
    ok(ex, "INSERT INTO w VALUES (1, 10, 'abc', 2), (2, 20, '12', 0), (3, 30, '7', 5)");
    const Rows before = q(ex, "SELECT id, n, s, b FROM w ORDER BY id");
    const auto refused = [&](const std::string& sql, const std::string& text) {
        const std::string error = fails(ex, sql);
        INFO(error);
        REQUIRE(error.find(text) != std::string::npos);
        REQUIRE(q(ex, "SELECT id, n, s, b FROM w ORDER BY id") == before); // (nothing was written, nothing removed)
    };
    for (const bool transaction : {false, true}) {
        if (transaction) ok(ex, "BEGIN");
        refused("DELETE FROM w WHERE n / b > 100", "Division by 0");
        refused("DELETE FROM w WHERE s = 5", "Truncated incorrect DOUBLE value: 'abc'");
        refused("DELETE FROM w WHERE s IN (1, 2)", "Truncated incorrect DOUBLE value: 'abc'");
        refused("DELETE FROM w WHERE n > (SELECT 1 / 0)", "Division by 0");
        refused("UPDATE w SET n = n + 1 WHERE n / b > 3", "Division by 0");
        refused("UPDATE w SET n = n + 1 WHERE s > 0", "Truncated incorrect DOUBLE value: 'abc'");
        refused("UPDATE w SET n = n + 1 WHERE n = 'x'", "Truncated incorrect DOUBLE value: 'x'");
        // a guard in front of the division keeps the zero out; a SELECT reads the number a text starts with and gives NULL for a division by zero
        ok(ex, "UPDATE w SET n = n + 1 WHERE b > 0 AND n / b > 3");
        REQUIRE(q(ex, "SELECT id, n FROM w WHERE n / b > 3 OR s = 5 ORDER BY id").size() >= 1);
        ok(ex, "UPDATE w SET n = n - 1 WHERE b > 0 AND n / b > 3");
        if (transaction) ok(ex, "ROLLBACK");
    }
    // a table that a foreign key points at is deleted from the long way round
    ok(ex, "INSERT INTO wc VALUES (1, 3)");
    refused("DELETE FROM w WHERE n / b > 100", "Division by 0");
    refused("DELETE FROM w WHERE s = 5", "Truncated incorrect DOUBLE value: 'abc'");
    // a condition on an indexed column may read fewer rows in MySQL: no error is raised for what the other terms do to the rows left out
    ok(ex, "DELETE FROM w WHERE id = 99 AND s = 5");
    ok(ex, "UPDATE w SET n = n WHERE id > 0 OR s = 5");
    // a date, an ENUM and a SET column compared with a number: its number is not its text, so nothing is truncated
    ok(ex, "CREATE TABLE wk (id INT PRIMARY KEY, d DATE, e ENUM('red', 'green'), st SET('a', 'b'), f DOUBLE)");
    ok(ex, "INSERT INTO wk VALUES (1, '2024-01-02', 'green', 'a,b', 1.5)");
    ok(ex, "UPDATE wk SET f = f + 1 WHERE e = 2 OR st = 3 OR d = 20240102 OR d > 20240101 OR e > 0");
    ok(ex, "DELETE FROM wk WHERE e = 5 OR st = 7 OR d = 20990101");
    ok(ex, "UPDATE wk SET f = f + 1 WHERE 2 = e OR f = e OR f = (e) OR f + 0 = (st) OR 20240102 = d OR f = d");
}

TEST_CASE("a date, a time, an ENUM and a SET are read as the numbers MySQL reads them as, to the last digit", "[r44][arith]") {
    TempDataDir dir("r44_arith");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, d DATE, dt3 DATETIME(3), tm2 TIME(2), e ENUM('red', 'green', 'blue'), st SET('a', 'b', 'c'))");
    ok(ex, "INSERT INTO t VALUES (1, '2024-01-02', '2024-01-02 03:04:05.678', '10:20:30.45', 'green', 'a,c')");
    // (a date and a time are exact numbers, with the places of the fraction of a second the column keeps; an ENUM and a SET are read as a DOUBLE)
    REQUIRE(q(ex, "SELECT d + 0, dt3 + 0, tm2 + 0, d / 3, e / 3, st / 4, dt3 * 1, -tm2 FROM t") ==
            Rows{{"20240102", "20240102030405.678", "102030.45", "6746700.6667", "0.6666666666666666", "1.25", "20240102030405.678", "-102030.45"}});
    REQUIRE(q(ex, "SELECT e + 0, st + 1, e * 2, d + 0.5, CAST(dt3 AS DECIMAL(20,3)), ABS(tm2), e DIV 2, st & 4, ROUND(dt3) FROM t") ==
            Rows{{"2", "6", "4", "20240102.5", "20240102030405.678", "102030.45", "1", "4", "20240102030406"}});
}

TEST_CASE("a FLOAT column is shown with six significant digits, to the last character", "[r44][float]") {
    TempDataDir dir("r44_float");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fl (id INT PRIMARY KEY AUTO_INCREMENT, f FLOAT, d DOUBLE)");
    ok(ex, "INSERT INTO fl (f, d) VALUES (1.23456789, 1.23456789), (0.1, 0.1), (1e10, 1e10), (123456789, 123456789), (1234567.891, 1234567.891), (0.000012345678, 0.000012345678), "
           "(3.14159265358979, 3.14159265358979), (100, 100), (1.5, 1.5), (16777217, 16777217), (-2.5e-5, -2.5e-5), (99999.999, 99999.999), (1e-10, 1e-10), (123.456, 123.456)");
    // (the texts MySQL 8.4 gives; a DOUBLE keeps all its digits)
    REQUIRE(q(ex, "SELECT f FROM fl ORDER BY id") == Rows{{"1.23457"}, {"0.1"}, {"10000000000"}, {"123457000"}, {"1234570"}, {"0.0000123457"}, {"3.14159"}, {"100"}, {"1.5"},
                                                         {"16777200"}, {"-0.000025"}, {"100000"}, {"0.0000000001"}, {"123.456"}});
    REQUIRE(q(ex, "SELECT d FROM fl WHERE id IN (1, 4, 5) ORDER BY id") == Rows{{"1.23456789"}, {"123456789"}, {"1234567.891"}});
    REQUIRE(q(ex, "SELECT CAST(1.23456789 AS FLOAT), CAST(16777217 AS FLOAT), CAST(100 AS FLOAT), CAST(1.23456789 AS DOUBLE)") == Rows{{"1.23457", "16777200", "100", "1.23456789"}});
    // the stored number is the single-precision one: comparisons and arithmetic read it, not the six digits
    REQUIRE(q(ex, "SELECT id FROM fl WHERE f > 1.23456 AND f < 1.23458 ORDER BY id") == Rows{{"1"}});
}

TEST_CASE("EXPLAIN ANALYZE reports the steps of the statement with their rows and share of the time", "[r45][explain]") {
    TempDataDir dir("r45_explain");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, city VARCHAR(5))");
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, cid INT, amount INT)");
    ok(ex, "INSERT INTO c VALUES (1, 'x'), (2, 'y')");
    ok(ex, "INSERT INTO o VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30), (4, 2, 40)");
    auto plan = [&](const std::string& sql) {
        auto r = ex.execute_sql("EXPLAIN ANALYZE " + sql);
        INFO(sql);
        REQUIRE(r.is_ok());
        return r.value();
    };
    const std::string joined = plan("SELECT c.city, SUM(o.amount) FROM o JOIN c ON o.cid = c.id GROUP BY c.city ORDER BY c.city");
    REQUIRE(joined.find("Step 1 Scan d.o : 4 rows") != std::string::npos); // (R53: the join is a step of its own, with its algorithm)
    REQUIRE(joined.find("Step 2 Join d.c (inner, ") != std::string::npos);
    REQUIRE(joined.find("Group / aggregate : 2 rows") != std::string::npos);
    REQUIRE(joined.find("Sort groups : 2 rows") != std::string::npos);
    REQUIRE(plan("SELECT COUNT(*) FROM o").find("Aggregate : 1 rows") != std::string::npos);
    REQUIRE(plan("SELECT * FROM o WHERE id = 3").find("Step 1 Execute : 1 rows") != std::string::npos); // (an index answer is one step)
    // a subquery's own steps are part of the step that ran it
    const std::string with_subquery = plan("SELECT * FROM o WHERE cid IN (SELECT id FROM c WHERE city = 'x') ORDER BY amount DESC");
    REQUIRE(with_subquery.find("Step 1 Scan d.o : 2 rows") != std::string::npos);
    REQUIRE(with_subquery.find("Scan d.c") == std::string::npos);
}

TEST_CASE("information_schema shows the sessions, open transactions and row locks", "[r45][infoschema]") {
    TempDataDir dir("r45_infoschema");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO t VALUES (1, 0), (2, 0)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM information_schema.innodb_trx") == Rows{{"0"}});
    ok(ex, "BEGIN");
    ok(ex, "UPDATE t SET v = 1 WHERE id = 2");
    REQUIRE(q(ex, "SELECT TRX_STATE, TRX_ROWS_LOCKED, TRX_WAITING_FOR FROM information_schema.innodb_trx") == Rows{{"RUNNING", "1", "NULL"}});
    REQUIRE(q(ex, "SELECT LOCK_TABLE, LOCK_DATA FROM information_schema.innodb_locks") == Rows{{"d.t", "2"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM information_schema.innodb_lock_waits") == Rows{{"0"}});
    ok(ex, "COMMIT");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM information_schema.innodb_locks") == Rows{{"0"}});
}

TEST_CASE("ANY_VALUE lets a column the group does not fix be selected, and UPDATE / DELETE take ORDER BY through a view", "[r45][any_value]") {
    TempDataDir dir("r45_any_value");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE av (id INT PRIMARY KEY, grp INT, name VARCHAR(5), val INT)");
    ok(ex, "INSERT INTO av VALUES (1, 1, 'a', 10), (2, 1, 'a', 10), (3, 2, 'b', 20)");
    REQUIRE(q(ex, "SELECT grp, ANY_VALUE(name), ANY_VALUE(val) + 1 FROM av GROUP BY grp ORDER BY grp") == Rows{{"1", "a", "11"}, {"2", "b", "21"}});
    REQUIRE(q(ex, "SELECT COUNT(*), ANY_VALUE(grp) FROM av WHERE grp = 2") == Rows{{"1", "2"}});
    REQUIRE(q(ex, "SELECT ANY_VALUE(name) FROM av WHERE grp = 1") == Rows{{"a"}, {"a"}}); // (not an aggregate: one row per row)
    REQUIRE(fails(ex, "SELECT grp, name FROM av GROUP BY grp").find("only_full_group_by") != std::string::npos);
    ok(ex, "CREATE VIEW avv AS SELECT id, grp, val FROM av WHERE grp = 1");
    ok(ex, "UPDATE avv SET val = val + 1 ORDER BY id DESC LIMIT 1");
    REQUIRE(q(ex, "SELECT id, val FROM av ORDER BY id") == Rows{{"1", "10"}, {"2", "11"}, {"3", "20"}});
    ok(ex, "DELETE FROM avv ORDER BY id LIMIT 1");
    REQUIRE(q(ex, "SELECT id FROM av ORDER BY id") == Rows{{"2"}, {"3"}});
}

TEST_CASE("TRACE runs a statement and lists the steps of its path, and MAX_EXECUTION_TIME stops a SELECT that runs too long", "[r46][trace][deadline]") {
    TempDataDir dir("r46_trace");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO t VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30), (4, 2, 40), (5, 3, 50)");
    auto trace = [&](const std::string& sql) {
        auto r = ex.execute_sql("TRACE " + sql);
        INFO(sql);
        REQUIRE(r.is_ok());
        return r.value();
    };
    const std::string select_path = trace("SELECT g, SUM(v) FROM t WHERE v > 10 GROUP BY g ORDER BY g");
    for (const char* step : {"Parse", "Prepare (cache, locks)", "Table locks", "Scan d.t", "Group / aggregate", "Return the answer", "Total", "Statement: 3 row(s) returned."})
        REQUIRE(select_path.find(step) != std::string::npos);
    // a TRACE of a write writes, and its path ends with the redo log
    const std::string update_path = trace("UPDATE t SET v = v + 1 WHERE g = 1");
    REQUIRE(update_path.find("Commit (redo log, fsync)") != std::string::npos);
    REQUIRE(update_path.find("Statement: 2 row(s) updated.") != std::string::npos);
    REQUIRE(q(ex, "SELECT v FROM t WHERE g = 1 ORDER BY id") == Rows{{"11"}, {"21"}});
    REQUIRE(trace("SELECT nosuch FROM t").find("Statement failed: Unknown column 'nosuch'") != std::string::npos);

    // MAX_EXECUTION_TIME: the hint and the variable, SELECT only; the session goes on
    const std::string heavy = "SELECT a.id FROM t a, t b, t c, t d, t e, t f, t g, t h, t i, t j ORDER BY a.id LIMIT 3";
    REQUIRE(fails(ex, "SELECT /*+ MAX_EXECUTION_TIME(30) */ " + heavy.substr(7)).find("3024") != std::string::npos);
    REQUIRE(q(ex, "SELECT /*+ MAX_EXECUTION_TIME(60000) */ COUNT(*) FROM t a, t b") == Rows{{"25"}});
    ok(ex, "SET max_execution_time = 30");
    REQUIRE(q(ex, "SELECT @@max_execution_time") == Rows{{"30"}});
    REQUIRE(fails(ex, heavy).find("maximum statement execution time exceeded") != std::string::npos);
    ok(ex, "UPDATE t SET v = v + 1 WHERE id = 5"); // (a write is not timed)
    ok(ex, "SET max_execution_time = 0");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t a, t b, t c") == Rows{{"125"}});
}

TEST_CASE("a foreign key keeps its name: DROP FOREIGN KEY by name, a drop and an add in one ALTER, referential_constraints, a parent named without its database", "[r46][fk_names]") {
    TempDataDir dir("r46_fk_names");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR(10))");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, note VARCHAR(10))");
    ok(ex, "INSERT INTO p VALUES (1, 'a'), (2, 'b')");
    ok(ex, "INSERT INTO c VALUES (1, 1, 'x')");
    ok(ex, "ALTER TABLE c ADD CONSTRAINT fk_c_p FOREIGN KEY (pid) REFERENCES p (id) ON DELETE CASCADE"); // (the parent is `p`, not `d.p`)
    REQUIRE(fails(ex, "INSERT INTO c VALUES (2, 9, 'bad')").find("Foreign key violation") != std::string::npos);
    ok(ex, "INSERT INTO c VALUES (2, 2, 'ok')");
    REQUIRE(q(ex, "SELECT CONSTRAINT_NAME, UPDATE_RULE, DELETE_RULE, REFERENCED_TABLE_NAME FROM information_schema.referential_constraints WHERE TABLE_NAME = 'c'") ==
            Rows{{"fk_c_p", "NO ACTION", "CASCADE", "p"}}); // (a rule left out is NO ACTION: RESTRICT only when it is written)
    REQUIRE(q(ex, "SELECT CONSTRAINT_NAME FROM information_schema.key_column_usage WHERE TABLE_NAME = 'c' AND REFERENCED_TABLE_NAME IS NOT NULL") == Rows{{"fk_c_p"}});
    REQUIRE(fails(ex, "ALTER TABLE c ADD CONSTRAINT fk_c_p FOREIGN KEY (pid) REFERENCES p (id)").find("Duplicate foreign key constraint name") != std::string::npos);
    REQUIRE(fails(ex, "ALTER TABLE c DROP FOREIGN KEY nosuch").find("not found") != std::string::npos);
    // a change is one ALTER: the old key goes and the new one comes, or nothing happens
    ok(ex, "ALTER TABLE c DROP FOREIGN KEY fk_c_p, ADD CONSTRAINT fk_c_p2 FOREIGN KEY (pid) REFERENCES p (id) ON DELETE SET NULL ON UPDATE CASCADE");
    REQUIRE(q(ex, "SELECT CONSTRAINT_NAME, UPDATE_RULE, DELETE_RULE FROM information_schema.referential_constraints WHERE TABLE_NAME = 'c'") == Rows{{"fk_c_p2", "CASCADE", "SET NULL"}});
    ok(ex, "UPDATE p SET id = 20 WHERE id = 2");
    REQUIRE(q(ex, "SELECT id, pid FROM c ORDER BY id") == Rows{{"1", "1"}, {"2", "20"}});
    ok(ex, "DELETE FROM p WHERE id = 20");
    REQUIRE(q(ex, "SELECT id, pid FROM c WHERE id = 2") == Rows{{"2", "NULL"}});
    // the name survives a restart and is written by SHOW CREATE TABLE
    auto shown = ex.execute_sql("SHOW CREATE TABLE c");
    REQUIRE(shown.is_ok());
    REQUIRE(shown.value().find("CONSTRAINT `fk_c_p2` FOREIGN KEY (`pid`) REFERENCES `p`(`id`) ON DELETE SET NULL ON UPDATE CASCADE") != std::string::npos);
    ok(ex, "ALTER TABLE c DROP FOREIGN KEY fk_c_p2");
    ok(ex, "INSERT INTO c VALUES (3, 99, 'free now')");
    // a table created with a named constraint, and one with none (reported as <table>_ibfk_<n>)
    ok(ex, "CREATE TABLE e (id INT PRIMARY KEY, pid INT, CONSTRAINT my_fk FOREIGN KEY (pid) REFERENCES p (id))");
    ok(ex, "CREATE TABLE f (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p (id))");
    REQUIRE(q(ex, "SELECT TABLE_NAME, CONSTRAINT_NAME FROM information_schema.referential_constraints ORDER BY TABLE_NAME") == Rows{{"e", "my_fk"}, {"f", "f_ibfk_1"}});
    ok(ex, "ALTER TABLE e DROP FOREIGN KEY my_fk");
    ok(ex, "ALTER TABLE f DROP FOREIGN KEY f_ibfk_1");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM information_schema.referential_constraints") == Rows{{"0"}});
}

TEST_CASE("window functions run on the groups: over aggregates, inside expressions, with DISTINCT and ORDER BY, and rows that tie are peers", "[r47][window_agg]") {
    TempDataDir dir("r47_window_agg");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE s (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO s VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30), (4, 2, NULL), (5, 3, 5)");
    // the window sees the groups: an aggregate as its argument or as what it orders by
    REQUIRE(q(ex, "SELECT g, SUM(v), SUM(SUM(v)) OVER (ORDER BY g) FROM s GROUP BY g ORDER BY g") == Rows{{"1", "30", "30"}, {"2", "30", "60"}, {"3", "5", "65"}});
    REQUIRE(q(ex, "SELECT g, RANK() OVER (ORDER BY SUM(v) DESC) FROM s GROUP BY g ORDER BY g") == Rows{{"1", "1"}, {"2", "1"}, {"3", "3"}});
    REQUIRE(q(ex, "SELECT g, LAG(SUM(v)) OVER (ORDER BY g) FROM s GROUP BY g ORDER BY g") == Rows{{"1", "NULL"}, {"2", "30"}, {"3", "30"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) OVER (ORDER BY g) FROM s GROUP BY g, id ORDER BY g, id") == Rows{{"1", "30"}, {"1", "30"}, {"2", "60"}, {"2", "60"}, {"3", "65"}}); // (the blank this used to give)
    REQUIRE(q(ex, "SELECT SUM(v), SUM(SUM(v)) OVER () FROM s") == Rows{{"65", "65"}});
    REQUIRE(q(ex, "SELECT COUNT(*), SUM(SUM(v) + 1) OVER () FROM s") == Rows{{"5", "66"}}); // (an expression of aggregates that only the window reads)
    REQUIRE(q(ex, "SELECT g, SUM(v) FROM s GROUP BY g HAVING SUM(v) > 10 ORDER BY g") == Rows{{"1", "30"}, {"2", "30"}}); // (HAVING comes before the window)
    REQUIRE(q(ex, "SELECT g, RANK() OVER (ORDER BY SUM(v) DESC) FROM s GROUP BY g HAVING SUM(v) < 30 ORDER BY g") == Rows{{"3", "1"}});
    // a window function inside an expression, in DISTINCT and in ORDER BY
    REQUIRE(q(ex, "SELECT g, ROUND(SUM(v) / SUM(SUM(v)) OVER (), 2) FROM s GROUP BY g ORDER BY g") == Rows{{"1", "0.46"}, {"2", "0.46"}, {"3", "0.08"}});
    REQUIRE(q(ex, "SELECT DISTINCT SUM(SUM(v)) OVER () FROM s GROUP BY g") == Rows{{"65"}});
    REQUIRE(q(ex, "SELECT DISTINCT SUM(v) AS total FROM s GROUP BY g ORDER BY total") == Rows{{"5"}, {"30"}}); // (a grouped query takes DISTINCT too)
    REQUIRE(q(ex, "SELECT g FROM s GROUP BY g ORDER BY RANK() OVER (ORDER BY SUM(v) DESC), g DESC") == Rows{{"2"}, {"1"}, {"3"}});
    REQUIRE(q(ex, "SELECT id, v - LAG(v) OVER (ORDER BY id) FROM s ORDER BY id") == Rows{{"1", "NULL"}, {"2", "10"}, {"3", "10"}, {"4", "NULL"}, {"5", "NULL"}});
    REQUIRE(q(ex, "SELECT id, ROW_NUMBER() OVER (ORDER BY id % 2, id) FROM s ORDER BY id") == Rows{{"1", "3"}, {"2", "1"}, {"3", "4"}, {"4", "2"}, {"5", "5"}}); // (an expression as a window item)
    // the hidden column of the window shows in no result, and the item is called by its text
    auto shown = ex.execute_sql("SELECT g, SUM(v) / SUM(SUM(v)) OVER () FROM s GROUP BY g");
    REQUIRE(shown.is_ok());
    REQUIRE(shown.value().find("__win") == std::string::npos);
    REQUIRE(shown.value().find("SUM(v) / SUM(SUM(v)) OVER ()") != std::string::npos);
    ok(ex, "CREATE VIEW sv AS SELECT g, SUM(v) / SUM(SUM(v)) OVER () AS share FROM s GROUP BY g");
    REQUIRE(q(ex, "SELECT * FROM sv ORDER BY g")[0].size() == 2);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM sv") == Rows{{"3"}});
    // errors as MySQL gives them
    REQUIRE(fails(ex, "SELECT g, SUM(v) OVER (ORDER BY g) FROM s GROUP BY g").find("only_full_group_by") != std::string::npos);
    REQUIRE(fails(ex, "SELECT g, SUM(SUM(SUM(v))) OVER () FROM s GROUP BY g").find("Invalid use of group function") != std::string::npos);
    REQUIRE(fails(ex, "SELECT id FROM s WHERE RANK() OVER (ORDER BY id) = 1").size() > 0);
    // rows that tie in the ORDER BY are peers: the default frame and RANGE frames take them along, ROWS frames do not
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (ORDER BY g) FROM s ORDER BY id") == Rows{{"1", "30"}, {"2", "30"}, {"3", "60"}, {"4", "60"}, {"5", "65"}});
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (ORDER BY g ROWS UNBOUNDED PRECEDING) FROM s ORDER BY id") == Rows{{"1", "10"}, {"2", "30"}, {"3", "60"}, {"4", "60"}, {"5", "65"}});
    REQUIRE(q(ex, "SELECT id, COUNT(*) OVER (ORDER BY g RANGE BETWEEN CURRENT ROW AND CURRENT ROW) FROM s ORDER BY id") == Rows{{"1", "2"}, {"2", "2"}, {"3", "2"}, {"4", "2"}, {"5", "1"}});
}

TEST_CASE("information_schema.row_versions lists every stored version of a row and which of them a session sees", "[r49][mvcc]") {
    TempDataDir dir("r49_row_versions");
    Executor a(dir.path);
    ok(a, "CREATE DATABASE d");
    ok(a, "USE d");
    ok(a, "CREATE TABLE acct (id INT PRIMARY KEY, bal INT)");
    ok(a, "INSERT INTO acct VALUES (1, 100), (2, 200)");
    Executor b = Executor::new_session(a.get_shared());
    ok(b, "USE d");
    const std::string versions = "SELECT ROW_KEY, ROW_DATA, STATE, VISIBLE_TO_ME FROM information_schema.row_versions WHERE TABLE_NAME = 'acct' ORDER BY ROW_KEY, XMIN";
    REQUIRE(q(a, versions) == Rows{{"1", "id=1, bal=100", "LIVE", "YES"}, {"2", "id=2, bal=200", "LIVE", "YES"}});
    // A updates row 1 and has not committed: the old version is being ended, the new one is not final; A sees the new, B the old
    ok(a, "BEGIN");
    ok(a, "UPDATE acct SET bal = 150 WHERE id = 1");
    REQUIRE(q(a, versions) == Rows{{"1", "id=1, bal=100", "ENDING", "NO"}, {"1", "id=1, bal=150", "UNCOMMITTED", "YES"}, {"2", "id=2, bal=200", "LIVE", "YES"}});
    REQUIRE(q(b, versions) == Rows{{"1", "id=1, bal=100", "ENDING", "YES"}, {"1", "id=1, bal=150", "UNCOMMITTED", "NO"}, {"2", "id=2, bal=200", "LIVE", "YES"}});
    // B under READ UNCOMMITTED sees the new version too: a dirty read
    ok(b, "SET ISOLATION LEVEL READ UNCOMMITTED");
    REQUIRE(q(b, "SELECT bal FROM acct WHERE id = 1") == Rows{{"150"}});
    ok(b, "SET ISOLATION LEVEL READ COMMITTED");
    ok(a, "COMMIT");
    REQUIRE(q(b, versions) == Rows{{"1", "id=1, bal=100", "DEAD", "NO"}, {"1", "id=1, bal=150", "LIVE", "YES"}, {"2", "id=2, bal=200", "LIVE", "YES"}});
    // a snapshot keeps the dead version in sight until its transaction ends, and VACUUM then removes it
    ok(b, "SET ISOLATION LEVEL REPEATABLE READ");
    ok(b, "BEGIN");
    REQUIRE(q(b, "SELECT bal FROM acct WHERE id = 1") == Rows{{"150"}});
    ok(a, "UPDATE acct SET bal = 175 WHERE id = 1");
    REQUIRE(q(b, "SELECT bal FROM acct WHERE id = 1") == Rows{{"150"}}); // (the snapshot from BEGIN)
    REQUIRE(q(b, "SELECT COUNT(*) FROM information_schema.row_versions WHERE TABLE_NAME = 'acct' AND ROW_KEY = '1'") == Rows{{"3"}});
    ok(b, "COMMIT");
    ok(a, "VACUUM acct");
    REQUIRE(q(a, "SELECT COUNT(*) FROM information_schema.row_versions WHERE TABLE_NAME = 'acct' AND ROW_KEY = '1'") == Rows{{"1"}});
}

TEST_CASE("R50: RANGE offsets, frames that hold no row, dates as numbers, FLOAT as single precision, error 1093, VERSION()", "[r50]") {
    TempDataDir dir("r50_misc");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE rf (id INT PRIMARY KEY, v INT, dt DATE)");
    ok(ex, "INSERT INTO rf VALUES (1,10,'2024-01-01'),(2,15,'2024-01-03'),(3,30,'2024-01-08'),(4,NULL,NULL),(5,15,'2024-02-01')");
    // RANGE n PRECEDING is a range of VALUES (10 and 15 are within 10 of 15), not the 10 rows before; the NULL row has only the NULL rows
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (ORDER BY v RANGE BETWEEN 10 PRECEDING AND CURRENT ROW) FROM rf ORDER BY id") ==
            Rows{{"1", "10"}, {"2", "40"}, {"3", "30"}, {"4", "NULL"}, {"5", "40"}});
    REQUIRE(q(ex, "SELECT id, COUNT(*) OVER (ORDER BY v RANGE BETWEEN 5 PRECEDING AND 5 FOLLOWING) FROM rf ORDER BY id") ==
            Rows{{"1", "3"}, {"2", "3"}, {"3", "1"}, {"4", "1"}, {"5", "3"}});
    // a DESC order runs the offsets the other way, a date takes an INTERVAL (a month is a calendar month)
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (ORDER BY v DESC RANGE BETWEEN 10 PRECEDING AND CURRENT ROW) FROM rf ORDER BY id") ==
            Rows{{"1", "40"}, {"2", "30"}, {"3", "30"}, {"4", "NULL"}, {"5", "30"}});
    REQUIRE(q(ex, "SELECT id, COUNT(*) OVER (ORDER BY dt RANGE BETWEEN INTERVAL 1 MONTH PRECEDING AND CURRENT ROW) FROM rf ORDER BY id") ==
            Rows{{"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "1"}, {"5", "4"}});
    REQUIRE(fails(ex, "SELECT SUM(v) OVER (ORDER BY v, id RANGE BETWEEN 5 PRECEDING AND CURRENT ROW) FROM rf").find("exactly one ORDER BY expression") != std::string::npos);
    REQUIRE(fails(ex, "SELECT SUM(v) OVER (ORDER BY v RANGE BETWEEN INTERVAL 1 DAY PRECEDING AND CURRENT ROW) FROM rf").find("INTERVAL bound value not allowed") != std::string::npos);
    REQUIRE(fails(ex, "SELECT SUM(v) OVER (ORDER BY dt RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM rf").find("Only INTERVAL bound value allowed") != std::string::npos);
    REQUIRE(fails(ex, "SELECT SUM(v) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND CURRENT ROW) FROM rf").find("negative, NULL or of non-integral type") != std::string::npos);
    REQUIRE(fails(ex, "SELECT SUM(v) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED FOLLOWING AND CURRENT ROW) FROM rf").find("cannot be UNBOUNDED FOLLOWING") != std::string::npos);
    // a frame that holds no row: COUNT 0, the others NULL (it used to be clamped onto the last row)
    REQUIRE(q(ex, "SELECT id, COUNT(*) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING) FROM rf ORDER BY id") ==
            Rows{{"1", "2"}, {"2", "2"}, {"3", "2"}, {"4", "1"}, {"5", "0"}});
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 1 PRECEDING) FROM rf ORDER BY id") ==
            Rows{{"1", "NULL"}, {"2", "10"}, {"3", "25"}, {"4", "45"}, {"5", "30"}});
    // what a date stands for as a number: a function's result too (the column of it was R44), and SUM / AVG of dates
    REQUIRE(q(ex, "SELECT DATE('2024-03-05') + 0, CAST('2024-03-05 12:30:45' AS DATETIME) + 0, TIME('12:30:45') * 2") == Rows{{"20240305", "20240305123045", "246090"}});
    REQUIRE(q(ex, "SELECT SUM(dt), AVG(dt), MAX(dt) + 0 FROM rf") == Rows{{"80960513", "20240128.2500", "20240201"}});
    REQUIRE(q(ex, "SELECT DATE('2024-03-05') > 20240101, DATE('2024-03-05') = '2024-03-05'") == Rows{{"1", "1"}});
    // a FLOAT column is its single-precision number: 0.1 is not the 0.1 of a DOUBLE or of a literal
    ok(ex, "CREATE TABLE fl (id INT PRIMARY KEY, f FLOAT, d DOUBLE)");
    ok(ex, "INSERT INTO fl VALUES (1, 0.1, 0.1), (2, 0.5, 0.5)");
    REQUIRE(q(ex, "SELECT id FROM fl WHERE f = d ORDER BY id") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id FROM fl WHERE f = 0.1").empty());
    REQUIRE(q(ex, "SELECT f + 0 FROM fl WHERE id = 1") == Rows{{"0.10000000149011612"}});
    REQUIRE(q(ex, "SELECT f FROM fl WHERE id = 1") == Rows{{"0.1"}}); // (what a FLOAT column shows is unchanged)
    // the table an UPDATE or DELETE changes cannot be read by a subquery of it; a derived table may
    ok(ex, "CREATE TABLE tt (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO tt VALUES (1, 10), (2, 20)");
    REQUIRE(fails(ex, "UPDATE tt SET v = 1 WHERE id IN (SELECT id FROM tt WHERE v > 5)") == "You can't specify target table 'tt' for update in FROM clause");
    REQUIRE(fails(ex, "UPDATE tt a SET v = 1 WHERE id IN (SELECT id FROM tt)") == "You can't specify target table 'a' for update in FROM clause");
    REQUIRE(fails(ex, "INSERT INTO tt (id, v) VALUES (3, (SELECT MAX(v) FROM tt))") == "You can't specify target table 'tt' for update in FROM clause");
    ok(ex, "UPDATE tt SET v = 1 WHERE id IN (SELECT id FROM (SELECT id FROM tt WHERE v > 15) x)");
    REQUIRE(q(ex, "SELECT v FROM tt ORDER BY id") == Rows{{"10"}, {"1"}});
    // ON DUPLICATE KEY UPDATE may read another table (its tables used to be looked up without the database)
    ok(ex, "CREATE TABLE other (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO other VALUES (1, 99)");
    ok(ex, "INSERT INTO tt (id, v) VALUES (1, 0) ON DUPLICATE KEY UPDATE v = (SELECT MAX(v) FROM other)");
    REQUIRE(q(ex, "SELECT v FROM tt WHERE id = 1") == Rows{{"99"}});
    // a month offset is a calendar month: 2024-03-31 less a month is 2024-02-29, not the 31st of a month with 29 days
    ok(ex, "CREATE TABLE ends (dt DATE)");
    ok(ex, "INSERT INTO ends VALUES ('2024-02-29'), ('2024-03-31')");
    REQUIRE(q(ex, "SELECT dt, COUNT(*) OVER (ORDER BY dt RANGE BETWEEN INTERVAL 1 MONTH PRECEDING AND CURRENT ROW) FROM ends ORDER BY dt") ==
            Rows{{"2024-02-29", "1"}, {"2024-03-31", "2"}});
    REQUIRE(q(ex, "SELECT dt, COUNT(*) OVER (ORDER BY dt DESC RANGE BETWEEN INTERVAL 1 MONTH PRECEDING AND CURRENT ROW) FROM ends ORDER BY dt") ==
            Rows{{"2024-02-29", "1"}, {"2024-03-31", "1"}}); // (DESC: the rows before are the later dates, up to a month on)
    // HAVING reads ANY_VALUE(x) as a plain x: a column of the select list
    ok(ex, "CREATE TABLE av (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO av VALUES (1, 1, 10), (2, 1, 10), (3, 2, 20)");
    REQUIRE(fails(ex, "SELECT g FROM av GROUP BY g HAVING ANY_VALUE(v) > 5") == "Unknown column 'v' in 'having clause'");
    REQUIRE(q(ex, "SELECT g, v FROM av GROUP BY g, v HAVING ANY_VALUE(v) > 15") == Rows{{"2", "20"}});
    // a routine's SELECT names its column by the expression as written, not by the values the variables held
    ok(ex, "CREATE PROCEDURE add_two() BEGIN DECLARE a INT DEFAULT 2; DECLARE b INT DEFAULT 6; SELECT a + b; END");
    auto called = ex.execute_sql("CALL add_two()");
    REQUIRE(called.is_ok());
    REQUIRE(called.value().find("| a + b") != std::string::npos);
    REQUIRE(called.value().find("2+6") == std::string::npos);
    // VERSION() is what @@version and the handshake say
    REQUIRE(q(ex, "SELECT VERSION() = @@version") == Rows{{"1"}});
}

TEST_CASE("R51: ORDER BY and HAVING read ANY_VALUE of a column the groups do not key by", "[r51]") {
    TempDataDir dir("r51_any_value");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE av (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO av VALUES (1, 1, 30), (2, 1, 30), (3, 2, 10), (4, 3, 20), (5, 3, 20)");
    // ORDER BY ANY_VALUE(v) sorts the groups by the v of a row of each (it used to sort by NULL: the groups came out in the order of g)
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM av GROUP BY g ORDER BY ANY_VALUE(v)") == Rows{{"2", "1"}, {"3", "2"}, {"1", "2"}});
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM av GROUP BY g ORDER BY ANY_VALUE(v) DESC, g") == Rows{{"1", "2"}, {"3", "2"}, {"2", "1"}});
    REQUIRE(q(ex, "SELECT g FROM av GROUP BY g ORDER BY ANY_VALUE(v) + g") == Rows{{"2"}, {"3"}, {"1"}});
    // HAVING names the alias of an expression of the select list: it used to see no column of that name and keep every group or none
    REQUIRE(q(ex, "SELECT g, ANY_VALUE(v) AS a FROM av GROUP BY g HAVING a > 15 ORDER BY g") == Rows{{"1", "30"}, {"3", "20"}});
    REQUIRE(q(ex, "SELECT g, ANY_VALUE(v) * 2 AS d FROM av GROUP BY g HAVING d = 20") == Rows{{"2", "20"}});
    REQUIRE(q(ex, "SELECT g, ANY_VALUE(v) AS a FROM av GROUP BY g HAVING a > 15 AND g > 1") == Rows{{"3", "20"}});
    // the aggregate beside it still works, and a plain select list is unchanged
    REQUIRE(q(ex, "SELECT g, SUM(v) AS s, ANY_VALUE(v) AS a FROM av GROUP BY g HAVING s > 20 ORDER BY ANY_VALUE(v) DESC, g") == Rows{{"1", "60", "30"}, {"3", "40", "20"}});
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM av GROUP BY g ORDER BY g DESC") == Rows{{"3", "2"}, {"2", "1"}, {"1", "2"}});
}

TEST_CASE("R52: HAVING reads a select-list alias before a column of the same name", "[r52]") {
    TempDataDir dir("r52_having_alias");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ha (id INT PRIMARY KEY, g INT, name VARCHAR(10), v INT, d DATE)");
    ok(ex, "INSERT INTO ha VALUES (1,1,'a',10,'2024-01-01'),(2,1,'a',10,'2024-01-01'),(3,2,'b',20,'2024-02-02'),(4,3,'c',NULL,NULL),(5,3,'c',5,'2024-03-03')");
    // an alias that is also a column of the table (d is a DATE column here): HAVING reads the alias, the select list keeps reading the column
    REQUIRE(q(ex, "SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d = 40 ORDER BY g") == Rows{{"1", "40"}, {"2", "40"}});
    REQUIRE(q(ex, "SELECT g, COUNT(*) * 2 AS name FROM ha GROUP BY g HAVING name = 4 ORDER BY g") == Rows{{"1", "4"}, {"3", "4"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d IS NULL").empty());
    REQUIRE(q(ex, "SELECT g, ANY_VALUE(d) AS first_d, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d = 40 ORDER BY g") ==
            Rows{{"1", "2024-01-01", "40"}, {"2", "2024-02-02", "40"}});
    // the alias of a plain column (it was never looked up: no group passed)
    REQUIRE(q(ex, "SELECT g, name AS n FROM ha GROUP BY g, name HAVING n = 'a'") == Rows{{"1", "a"}});
    REQUIRE(q(ex, "SELECT g, name AS d FROM ha GROUP BY g, name HAVING d = 'b'") == Rows{{"2", "b"}});
    REQUIRE(q(ex, "SELECT g AS grp_id, COUNT(*) AS c FROM ha GROUP BY g HAVING grp_id = 1") == Rows{{"1", "2"}});
    // HAVING without GROUP BY
    REQUIRE(q(ex, "SELECT SUM(v) * 2 AS d FROM ha HAVING d = 90") == Rows{{"90"}});
    REQUIRE(q(ex, "SELECT SUM(v) * 2 AS d FROM ha HAVING d = 91").empty());
    REQUIRE(q(ex, "SELECT COUNT(*) AS c FROM ha HAVING c > 3") == Rows{{"5"}});
    // the aliases that never collided, and a name that is not an alias of the select list but a column
    REQUIRE(q(ex, "SELECT g, SUM(v) AS s FROM ha GROUP BY g HAVING s * 2 > 30 ORDER BY g") == Rows{{"1", "20"}, {"2", "20"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) * 2 AS dbl FROM ha GROUP BY g HAVING DBL = 40 ORDER BY g") == Rows{{"1", "40"}, {"2", "40"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) AS s FROM ha GROUP BY g HAVING g = 2") == Rows{{"2", "20"}});
}

TEST_CASE("R52: a table joined with itself in UPDATE / DELETE, the SET value that names a table by its alias, ORDER BY and DISTINCT on a scalar subquery", "[r52]") {
    TempDataDir dir("r52_self_join");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE up (id INT PRIMARY KEY, mgr INT, a INT, b INT, g INT)");
    ok(ex, "INSERT INTO up VALUES (1,NULL,11,11,1),(2,1,22,22,1),(3,1,30,3,2),(4,2,40,4,2),(5,2,50,5,3)");
    // the second use of the table goes by its alias: these used to change no row (and say so: 0 row(s) updated)
    REQUIRE(ex.execute_sql("UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 SET u1.b = u2.a").value().find("4 row(s) updated") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, b FROM up ORDER BY id") == Rows{{"1", "22"}, {"2", "30"}, {"3", "40"}, {"4", "50"}, {"5", "5"}});
    ok(ex, "UPDATE up e JOIN up m ON e.mgr = m.id SET e.b = m.a");
    REQUIRE(q(ex, "SELECT id, b FROM up ORDER BY id") == Rows{{"1", "22"}, {"2", "11"}, {"3", "11"}, {"4", "22"}, {"5", "22"}});
    ok(ex, "UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 SET u2.a = u1.a + 100 WHERE u1.id > 1"); // (the target is the second use)
    REQUIRE(q(ex, "SELECT id, a FROM up ORDER BY id") == Rows{{"1", "11"}, {"2", "22"}, {"3", "122"}, {"4", "130"}, {"5", "140"}});
    ok(ex, "UPDATE up a, up b SET a.b = b.a WHERE a.id = b.id - 1"); // (the comma form)
    REQUIRE(q(ex, "SELECT id, b FROM up ORDER BY id") == Rows{{"1", "22"}, {"2", "122"}, {"3", "130"}, {"4", "140"}, {"5", "22"}});
    REQUIRE(fails(ex, "UPDATE up JOIN up ON up.id = up.id SET up.b = 0").find("Not unique table/alias") != std::string::npos);
    ok(ex, "DELETE u2 FROM up u1 JOIN up u2 ON u1.id = u2.id - 1 WHERE u1.id = 1"); // (the alias of the table to delete from is named with the database in front of it)
    REQUIRE(q(ex, "SELECT id FROM up ORDER BY id") == Rows{{"1"}, {"3"}, {"4"}, {"5"}});
    ok(ex, "DELETE e FROM up e JOIN up m ON e.mgr = m.id WHERE m.id = 1");
    REQUIRE(q(ex, "SELECT id FROM up ORDER BY id") == Rows{{"1"}, {"4"}, {"5"}});
    // the SET value reads the column of the other table by its alias (two tables with a column of the same name: it read the first table's)
    ok(ex, "CREATE TABLE t1 (id INT PRIMARY KEY, v INT, w INT)");
    ok(ex, "CREATE TABLE t2 (id INT PRIMARY KEY, v INT, k INT)");
    ok(ex, "INSERT INTO t1 VALUES (1,10,100),(2,20,200)");
    ok(ex, "INSERT INTO t2 VALUES (1,111,1),(2,222,2)");
    ok(ex, "UPDATE t1 x JOIN t2 y ON x.id = y.id SET x.w = y.v + x.v WHERE y.k = 1");
    REQUIRE(q(ex, "SELECT id, v, w FROM t1 ORDER BY id") == Rows{{"1", "10", "121"}, {"2", "20", "200"}});
    ok(ex, "UPDATE t1 x LEFT JOIN t2 y ON x.id = y.id SET x.v = y.v");
    REQUIRE(q(ex, "SELECT id, v FROM t1 ORDER BY id") == Rows{{"1", "111"}, {"2", "222"}});
    // ORDER BY the alias or the position of a scalar subquery of the select list: the rows came out in table order
    ok(ex, "CREATE TABLE ee (id INT PRIMARY KEY, name VARCHAR(10), mgr INT, dept INT)");
    ok(ex, "INSERT INTO ee VALUES (1,'Ann',NULL,1),(2,'Bob',1,1),(3,'Cid',1,2),(4,'Dee',2,2),(5,'Eve',3,3),(6,'Fay',3,NULL)");
    REQUIRE(q(ex, "SELECT e.name, (SELECT COUNT(*) FROM ee x WHERE x.mgr = e.id) AS reports FROM ee e ORDER BY reports DESC, e.name") ==
            Rows{{"Ann", "2"}, {"Cid", "2"}, {"Bob", "1"}, {"Dee", "0"}, {"Eve", "0"}, {"Fay", "0"}});
    REQUIRE(q(ex, "SELECT e.name, (SELECT COUNT(*) FROM ee x WHERE x.mgr = e.id) AS reports FROM ee e ORDER BY 2 DESC, 1") ==
            Rows{{"Ann", "2"}, {"Cid", "2"}, {"Bob", "1"}, {"Dee", "0"}, {"Eve", "0"}, {"Fay", "0"}});
    REQUIRE(q(ex, "SELECT e.name, (SELECT COUNT(*) FROM ee x WHERE x.mgr = e.id) r FROM ee e ORDER BY r, e.name LIMIT 2") == Rows{{"Dee", "0"}, {"Eve", "0"}});
    REQUIRE(q(ex, "SELECT name, (SELECT COUNT(*) FROM ee x WHERE x.dept = ee.dept) AS cnt FROM ee ORDER BY cnt, name") ==
            Rows{{"Fay", "0"}, {"Eve", "1"}, {"Ann", "2"}, {"Bob", "2"}, {"Cid", "2"}, {"Dee", "2"}});
    // DISTINCT over a scalar subquery of the select list collapsed every row into one
    REQUIRE(q(ex, "SELECT DISTINCT (SELECT COUNT(*) FROM ee x WHERE x.dept = e.dept) AS n FROM ee e ORDER BY n") == Rows{{"0"}, {"1"}, {"2"}});
}

// ---------------------------------------------------------------------------------------------------------------------
// R53: KILL [QUERY] n, SHOW STATUS with real counters, EXPLAIN ANALYZE that names the join algorithm and the subquery time.

namespace {
long long status_value(Executor& ex, const std::string& scope, const std::string& name) {
    auto rows = q(ex, "SHOW " + scope + " STATUS LIKE '" + name + "'");
    REQUIRE(rows.size() == 1);
    return std::stoll(rows[0][1]);
}

// the id of the session of a user (the processlist is read without the lock of the database)
std::string session_of(Executor& ex, const std::string& user) {
    auto rows = q(ex, "SELECT ID FROM information_schema.processlist WHERE USER = '" + user + "'");
    REQUIRE(rows.size() == 1);
    return rows[0][0];
}

// waits (at most a few seconds) until a session shows the command in the processlist
void wait_for_command(Executor& ex, const std::string& user, const std::string& command) {
    for (int i = 0; i < 300; i++) {
        if (q(ex, "SELECT ID FROM information_schema.processlist WHERE USER = '" + user + "' AND COMMAND = '" + command + "'").size() == 1) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL("the session of " << user << " never showed the command " << command);
}

std::size_t characters(const std::string& text) {
    std::size_t n = 0;
    for (unsigned char c : text) n += (c & 0xC0) != 0x80;
    return n;
}
} // namespace

TEST_CASE("R53: SHOW STATUS counts the statements by kind, per session and for the server, and the sessions", "[r53][status]") {
    TempDataDir dir("r53_status");
    Executor ex(dir.path);
    ex.register_process("root", "localhost");
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE s (id INT PRIMARY KEY, v INT)");
    const long long inserts0 = status_value(ex, "GLOBAL", "Com_insert");
    const long long questions0 = status_value(ex, "SESSION", "Questions");
    ok(ex, "INSERT INTO s VALUES (1, 10)");
    ok(ex, "INSERT INTO s VALUES (2, 20)");
    ok(ex, "INSERT INTO s VALUES (3, 30)");
    ok(ex, "UPDATE s SET v = v + 1 WHERE id = 1");
    ok(ex, "DELETE FROM s WHERE id = 3");
    q(ex, "SELECT * FROM s");
    q(ex, "SELECT * FROM s WHERE id = 1");
    // the kinds are counted for the session and the server alike
    REQUIRE(status_value(ex, "SESSION", "Com_insert") == 3);
    REQUIRE(status_value(ex, "SESSION", "Com_update") == 1);
    REQUIRE(status_value(ex, "SESSION", "Com_delete") == 1);
    REQUIRE(status_value(ex, "SESSION", "Com_select") == 2);
    REQUIRE(status_value(ex, "SESSION", "Com_create_table") == 1);
    REQUIRE(status_value(ex, "GLOBAL", "Com_insert") == inserts0 + 3);
    REQUIRE(status_value(ex, "SESSION", "Questions") >= questions0 + 7); // (every statement the client sent, the SHOW ones too)
    // SHOW STATUS without a scope is the session's, and a failed statement is still a statement that was sent
    REQUIRE(q(ex, "SHOW STATUS LIKE 'Com_insert'") == Rows{{"Com_insert", "3"}});
    REQUIRE(ex.execute_sql("INSERT INTO s VALUES (1, 99)").is_err());
    REQUIRE(status_value(ex, "SESSION", "Com_insert") == 4);
    // LIKE: the names that match, with % and _
    auto com = q(ex, "SHOW STATUS LIKE 'Com_%'");
    REQUIRE(com.size() >= 10);
    for (auto& row : com) REQUIRE(row[0].rfind("Com_", 0) == 0);
    REQUIRE(q(ex, "SHOW STATUS LIKE 'no_such_counter'").empty());
    // another session: its own statements are its own, the server's are everybody's, and the sessions are counted
    auto shared = ex.get_shared();
    Executor other = Executor::new_session(shared);
    other.auth_user = "bob";
    other.register_process("bob", "localhost");
    ok(other, "USE d");
    REQUIRE(status_value(other, "SESSION", "Com_insert") == 0);
    REQUIRE(status_value(other, "GLOBAL", "Com_insert") == inserts0 + 4);
    ok(other, "INSERT INTO s VALUES (10, 1)");
    REQUIRE(status_value(other, "SESSION", "Com_insert") == 1);
    REQUIRE(status_value(ex, "SESSION", "Com_insert") == 4);
    REQUIRE(status_value(ex, "GLOBAL", "Com_insert") == inserts0 + 5);
    REQUIRE(status_value(ex, "GLOBAL", "Threads_connected") == 2);
    REQUIRE(status_value(ex, "GLOBAL", "Connections") >= 2);
    REQUIRE(status_value(ex, "GLOBAL", "Max_used_connections") >= 2);
    other.deregister_process();
    REQUIRE(status_value(ex, "GLOBAL", "Threads_connected") == 1);
    REQUIRE(status_value(ex, "GLOBAL", "Max_used_connections") >= 2); // (the most there were, not the number now)
    // the buffer pool: the size it was made with; the redo log: what the committed inserts above wrote and no checkpoint has emptied
    REQUIRE(status_value(ex, "GLOBAL", "Innodb_buffer_pool_pages_total") == 64);
    REQUIRE(status_value(ex, "GLOBAL", "Innodb_redo_log_bytes") > 0);
    REQUIRE(status_value(ex, "GLOBAL", "Uptime") >= 0);
    // the same numbers as a table, to query
    REQUIRE(q(ex, "SELECT VARIABLE_VALUE FROM information_schema.global_status WHERE VARIABLE_NAME = 'Com_insert'") == Rows{{std::to_string(inserts0 + 5)}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM information_schema.global_status WHERE VARIABLE_NAME LIKE 'Innodb%'") == Rows{{"8"}});
}

TEST_CASE("R53: KILL QUERY stops a SELECT that is running and a wait for a row lock; KILL CONNECTION ends the session", "[r53][kill]") {
    TempDataDir dir("r53_kill");
    Executor ex(dir.path);
    ex.register_process("root", "localhost");
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, g INT, v INT)");
    for (int start = 1; start <= 40000; start += 1000) {
        std::string sql = "INSERT INTO big VALUES ";
        for (int i = start; i < start + 1000; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 100) + "," + std::to_string(i % 7) + ")";
        ok(ex, sql);
    }
    ok(ex, "CREATE TABLE acct (id INT PRIMARY KEY, bal INT)");
    ok(ex, "INSERT INTO acct VALUES (1, 100), (2, 200)");

    auto shared = ex.get_shared();
    Executor victim = Executor::new_session(shared);
    victim.auth_user = "bob";
    victim.register_process("bob", "localhost");
    ok(victim, "USE d");
    const std::string victim_id = session_of(ex, "bob");

    // a SELECT that would run for a long time (a correlated subquery for every row) is stopped at the next row, and the session goes on
    {
        std::string answer;
        bool failed = false;
        std::thread runner([&] {
            auto r = victim.execute_sql("SELECT COUNT(*) FROM big a WHERE EXISTS (SELECT 1 FROM big b WHERE b.v = a.v AND b.g = a.g + 1000)");
            failed = r.is_err();
            answer = failed ? r.error() : r.value();
        });
        wait_for_command(ex, "bob", "Query");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(q(ex, "SELECT COMMAND FROM information_schema.processlist WHERE USER = 'bob'") == Rows{{"Query"}});
        const auto killed_at = std::chrono::steady_clock::now();
        REQUIRE(ex.execute_sql("KILL QUERY " + victim_id).value() == "Query " + victim_id + " interrupted.");
        runner.join();
        REQUIRE(std::chrono::steady_clock::now() - killed_at < std::chrono::seconds(5)); // (it stops at the next row, it does not run on to its end)
        REQUIRE(failed);
        REQUIRE(answer.find("ERROR 1317 (70100): Query execution was interrupted") != std::string::npos);
        REQUIRE(q(ex, "SELECT COMMAND FROM information_schema.processlist WHERE USER = 'bob'") == Rows{{"Sleep"}});
        REQUIRE(q(victim, "SELECT COUNT(*) FROM acct") == Rows{{"2"}}); // (the session is whole, and the request to stop does not wait for its next statement)
        REQUIRE_FALSE(victim.connection_killed());
    }

    // KILL QUERY of a session that is asleep does nothing to its next statement
    REQUIRE(ex.execute_sql("KILL QUERY " + victim_id).is_ok());
    REQUIRE(q(victim, "SELECT COUNT(*) FROM acct") == Rows{{"2"}});

    // a wait for a row lock ends when its statement is killed; the row is the holder's again and nothing was written
    {
        Executor holder = Executor::new_session(shared);
        holder.auth_user = "carl";
        holder.register_process("carl", "localhost");
        ok(holder, "USE d");
        ok(holder, "BEGIN");
        ok(holder, "UPDATE acct SET bal = bal + 1 WHERE id = 1");
        const long long waits0 = status_value(ex, "GLOBAL", "Innodb_row_lock_waits");
        std::string answer;
        bool failed = false;
        std::thread waiter([&] {
            auto r = victim.execute_sql("UPDATE acct SET bal = bal + 5 WHERE id = 1");
            failed = r.is_err();
            answer = failed ? r.error() : r.value();
        });
        wait_for_command(ex, "bob", "Query");
        for (int i = 0; i < 300 && status_value(ex, "GLOBAL", "Innodb_row_lock_current_waits") == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        REQUIRE(status_value(ex, "GLOBAL", "Innodb_row_lock_current_waits") == 1);
        REQUIRE(status_value(ex, "GLOBAL", "Innodb_row_lock_waits") == waits0 + 1);
        const auto killed_at = std::chrono::steady_clock::now();
        REQUIRE(ex.execute_sql("KILL QUERY " + victim_id).is_ok());
        waiter.join();
        REQUIRE(std::chrono::steady_clock::now() - killed_at < std::chrono::seconds(5)); // (the wait ends at once, not when the lock wait times out after 50 seconds)
        REQUIRE(failed);
        REQUIRE(answer.find("ERROR 1317 (70100): Query execution was interrupted") != std::string::npos);
        REQUIRE(status_value(ex, "GLOBAL", "Innodb_row_lock_current_waits") == 0);
        ok(holder, "ROLLBACK");
        REQUIRE(q(ex, "SELECT bal FROM acct WHERE id = 1") == Rows{{"100"}});

        // KILL CONNECTION: the session is marked as killed (the server closes its connection), and only the owner or root may do it
        const std::string holder_id = session_of(ex, "carl");
        REQUIRE(fails(victim, "KILL " + holder_id).find("ERROR 1095 (HY000): You are not owner of thread " + holder_id) != std::string::npos);
        REQUIRE_FALSE(holder.connection_killed());
        REQUIRE(ex.execute_sql("KILL CONNECTION " + holder_id).value() == "Connection " + holder_id + " killed.");
        REQUIRE(holder.connection_killed());
        holder.deregister_process();
    }

    // the errors
    REQUIRE(fails(ex, "KILL 999999").find("ERROR 1094 (HY000): Unknown thread id: 999999") != std::string::npos);
    REQUIRE(fails(ex, "KILL QUERY abc").find("ERROR 1064 (42000)") != std::string::npos);
    REQUIRE(fails(ex, "KILL").find("ERROR 1064 (42000)") != std::string::npos);
    REQUIRE(fails(ex, "KILL QUERY " + std::to_string(ex.session_id)).find("ERROR 1317 (70100)") != std::string::npos); // (a session that kills its own query)
    REQUIRE(status_value(ex, "GLOBAL", "Com_kill") >= 4);
    REQUIRE(ex.execute_sql("KILL " + std::to_string(ex.session_id)).value() == "Connection " + std::to_string(ex.session_id) + " killed.");
    REQUIRE(ex.connection_killed());
}

TEST_CASE("R53: EXPLAIN ANALYZE names the way each join was done and tells the time of the subqueries apart from the steps", "[r53][explain]") {
    TempDataDir dir("r53_explain");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ea (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "CREATE TABLE eb (id INT PRIMARY KEY, a_id INT, w INT)");
    ok(ex, "INSERT INTO ea VALUES (1,1,10),(2,1,20),(3,2,30),(4,2,40)");
    ok(ex, "INSERT INTO eb VALUES (1,1,5),(2,2,6),(3,3,7),(4,3,8),(5,4,9)");
    auto text = [&](const std::string& sql) {
        auto r = ex.execute_sql(sql);
        REQUIRE(r.is_ok());
        return r.value();
    };
    // a join is a step of its own, with its kind and the algorithm that did it
    std::string inner = text("EXPLAIN ANALYZE SELECT * FROM ea JOIN eb ON eb.a_id = ea.id WHERE ea.v > 10");
    REQUIRE(inner.find("Step 1 Scan d.ea : ") != std::string::npos);
    REQUIRE(inner.find("Step 2 Join d.eb (inner, ") != std::string::npos);
    REQUIRE(inner.find("Step 3 Filter (WHERE) and expressions : 4 rows") != std::string::npos);
    std::string left = text("EXPLAIN ANALYZE SELECT * FROM ea LEFT JOIN eb ON eb.a_id = ea.id");
    REQUIRE(left.find("Join d.eb (left, ") != std::string::npos);
    REQUIRE((left.find("hash join)") != std::string::npos || left.find("nested loop)") != std::string::npos || left.find("sort-merge join)") != std::string::npos));
    REQUIRE(text("EXPLAIN ANALYZE SELECT * FROM ea CROSS JOIN eb").find("Join d.eb (cross, ") != std::string::npos);
    // one table: a scan
    REQUIRE(text("EXPLAIN ANALYZE SELECT * FROM ea WHERE v > 10").find("Step 1 Scan d.ea : 3 rows") != std::string::npos);
    // a subquery that names nothing of the outer query runs once; one that does, for every row. Their time is told apart, as it is inside the steps
    std::string once = text("EXPLAIN ANALYZE SELECT * FROM ea WHERE v > (SELECT AVG(v) FROM ea)");
    REQUIRE(once.find("Subqueries not correlated, run once : 1 run(s), ") != std::string::npos);
    REQUIRE(once.find("(inside the steps below)") != std::string::npos);
    REQUIRE(once.find("Subqueries correlated") == std::string::npos);
    std::string per_row = text("EXPLAIN ANALYZE SELECT id, (SELECT COUNT(*) FROM eb WHERE eb.a_id = ea.id) FROM ea");
    REQUIRE(per_row.find("Subqueries correlated, run per row : 4 run(s), ") != std::string::npos);
    REQUIRE(per_row.find("Subqueries not correlated") == std::string::npos);
    REQUIRE(text("EXPLAIN ANALYZE SELECT * FROM ea").find("Subqueries") == std::string::npos);
    // a long label widens the box: every line has the width of the top line, whatever its length
    for (const std::string& sql : {std::string("EXPLAIN ANALYZE SELECT * FROM ea JOIN eb ON eb.a_id = ea.id WHERE ea.v > 10"),
                                   std::string("EXPLAIN ANALYZE SELECT id, (SELECT COUNT(*) FROM eb WHERE eb.a_id = ea.id) FROM ea")}) {
        std::istringstream in(text(sql));
        std::string line;
        std::size_t width = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (width == 0) width = characters(line);
            INFO(line);
            REQUIRE(characters(line) == width);
        }
        REQUIRE(width > 70);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// R54: LIKE's escape character, USE that leaves the redo log alone, the parent side of a foreign key that does not scan the child table.

namespace {
// RUSQL_PROBE_CHECK=1 for the engines made while it lives: every use of the cache of values counts the rows again and compares (the engine throws when they differ)
struct ProbeCheck {
    ProbeCheck() { _putenv_s("RUSQL_PROBE_CHECK", "1"); }
    ~ProbeCheck() { _putenv_s("RUSQL_PROBE_CHECK", ""); }
};

std::uint64_t probe_skipped(Executor& ex) { return ex.get_shared()->read()->unique_probes->skipped.load(); }
std::uint64_t probe_builds(Executor& ex) { return ex.get_shared()->read()->unique_probes->builds.load(); }

std::string multi_insert(const std::string& table, int from, int to, const std::function<std::string(int)>& values) {
    std::string sql = "INSERT INTO " + table + " VALUES ";
    for (int i = from; i <= to; i++) sql += std::string(i == from ? "" : ",") + "(" + values(i) + ")";
    return sql;
}
} // namespace

TEST_CASE("R54: LIKE reads \\% and \\_ as the characters themselves, ESCAPE names another escape character", "[r54][like]") {
    TempDataDir dir("r54_like");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // `\%` and `\_` stay two characters in a string, as in MySQL (any other `\x` is x)
    REQUIRE(q(ex, "SELECT LENGTH('a\\_b'), LENGTH('a\\%'), LENGTH('a\\qb'), 'a\\qb'") == Rows{{"4", "3", "3", "aqb"}});
    REQUIRE(q(ex, "SELECT 'a%b' LIKE 'a\\%b', 'axb' LIKE 'a\\%b', 'a_b' LIKE 'a\\_b', 'axb' LIKE 'a\\_b', '%' LIKE '\\%', 'x' LIKE '\\%', 'a%' LIKE 'a%\\%', 'ab' LIKE 'a%\\%'") ==
            Rows{{"1", "0", "1", "0", "1", "0", "1", "0"}});
    // the escaped backslash, and a backslash that ends the pattern is a backslash
    REQUIRE(q(ex, "SELECT 'a\\\\' LIKE 'a\\\\\\\\', 'a\\\\' LIKE 'a\\\\', 'a' LIKE 'a\\\\', 'a\\\\b' LIKE 'a\\\\\\\\b', 'ab' LIKE 'a\\\\b'") == Rows{{"1", "1", "0", "1", "1"}}); // (the last: the backslash escapes the b)
    // ESCAPE 'c': c + x is x; a backslash is an ordinary character; an escape character that ends the pattern is itself
    REQUIRE(q(ex, "SELECT 'a%b' LIKE 'a!%b' ESCAPE '!', 'axb' LIKE 'a!%b' ESCAPE '!', 'a!b' LIKE 'a!!b' ESCAPE '!', 'a_b' LIKE 'a!_b' ESCAPE '!', 'axb' LIKE 'a!_b' ESCAPE '!'") ==
            Rows{{"1", "0", "1", "1", "0"}});
    REQUIRE(q(ex, "SELECT 'a\\\\b' LIKE 'a\\\\b' ESCAPE '!', 'a\\\\%b' LIKE 'a\\%b' ESCAPE '!', 'axb' LIKE 'a\\%b' ESCAPE '!', 'a%b' LIKE 'a\\%b' ESCAPE '!'") == Rows{{"1", "1", "0", "0"}});
    REQUIRE(q(ex, "SELECT 'a!' LIKE 'a!' ESCAPE '!', 'a' LIKE 'a!' ESCAPE '!', 'a#' LIKE 'a##' ESCAPE '#', 'a#b' LIKE 'a#%b' ESCAPE '#'") == Rows{{"1", "0", "1", "0"}});
    REQUIRE(q(ex, "SELECT 'a\\\\b' LIKE 'a\\\\b' ESCAPE '', 'ab' LIKE 'a\\\\b' ESCAPE '', 'a%b' LIKE 'a\\\\%b' ESCAPE '\\\\'") == Rows{{"1", "0", "1"}});
    REQUIRE(q(ex, "SELECT 'axb' NOT LIKE 'a!%b' ESCAPE '!', 'a%b' NOT LIKE 'a!%b' ESCAPE '!'") == Rows{{"1", "0"}});
    REQUIRE(q(ex, "SELECT 'a_' LIKE 'a\xE2\x86\x92_' ESCAPE '\xE2\x86\x92', 'ab' LIKE 'a\xE2\x86\x92_' ESCAPE '\xE2\x86\x92'") == Rows{{"1", "0"}}); // (an escape character of several bytes)
    // the same next to a character that is no ASCII (another walk over the text)
    REQUIRE(q(ex, "SELECT '\xC3\xA9_' LIKE '\xC3\xA9\\_', '\xC3\xA9x' LIKE '\xC3\xA9\\_', '\xC3\xA9%' LIKE '\xC3\xA9\\%', '\xC3\x89_' LIKE '\xC3\xA9\\_', '\xC3\xA9_x' LIKE '\xC3\xA9\\_%'") ==
            Rows{{"1", "0", "1", "1", "1"}});
    // what the engine cannot do is said
    REQUIRE(fails(ex, "SELECT 'a' LIKE 'a' ESCAPE '!!'").find("Incorrect arguments to ESCAPE") != std::string::npos);
    ok(ex, "CREATE TABLE pat (id INT PRIMARY KEY, p VARCHAR(10))");
    ok(ex, "INSERT INTO pat VALUES (1, 'a!_')");
    // a pattern read from a column has its escapes too, and ESCAPE works on it
    ok(ex, "INSERT INTO pat VALUES (2, 'a\\_')");
    REQUIRE(q(ex, "SELECT id FROM pat WHERE 'a_' LIKE p AND 'ab' NOT LIKE p ORDER BY id") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id, 'a_' LIKE p ESCAPE '!' FROM pat ORDER BY id") == Rows{{"1", "1"}, {"2", "0"}});
    REQUIRE(q(ex, "SELECT id FROM pat WHERE 'a_' LIKE CONCAT('a', '!', '_') ESCAPE '!'") == Rows{{"1"}, {"2"}});
    ok(ex, "INSERT INTO pat VALUES (3, NULL)");
    REQUIRE(q(ex, "SELECT id, 'a_' LIKE p ESCAPE '!' FROM pat WHERE id = 3") == Rows{{"3", "NULL"}});
    // the same matcher everywhere a pattern is read: SHOW ... LIKE and the rows of a table
    ok(ex, "CREATE TABLE xak (a INT)");
    ok(ex, "CREATE TABLE x_k (a INT)");
    REQUIRE(q(ex, "SHOW TABLES LIKE 'x\\_k'") == Rows{{"x_k"}});
    REQUIRE(q(ex, "SHOW TABLES LIKE 'x_k'") == Rows{{"x_k"}, {"xak"}});
    REQUIRE(q(ex, "SHOW STATUS LIKE 'Com\\_sel%'").size() == 1);
    ok(ex, "CREATE TABLE lk (id INT PRIMARY KEY, name VARCHAR(20))");
    ok(ex, "CREATE INDEX ix_name ON lk (name)");
    ok(ex, "INSERT INTO lk VALUES (1,'a_1'),(2,'ab1'),(3,'a_2'),(4,'a%x'),(5,'axx'),(6,'b_1'),(7,'a\\\\b')");
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a\\_%' ORDER BY id") == Rows{{"1"}, {"3"}});
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a\\%%' ORDER BY id") == Rows{{"4"}});
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a!_%' ESCAPE '!' ORDER BY id") == Rows{{"1"}, {"3"}});
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a%' AND name NOT LIKE 'a\\_%' ORDER BY id") == Rows{{"2"}, {"4"}, {"5"}, {"7"}});
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a%' ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}, {"4"}, {"5"}, {"7"}}); // (a plain prefix still reads the index)
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a\\\\\\\\b'") == Rows{{"7"}});
    // a pattern with a backslash and a % at its end is no plain prefix: the index may not be searched for it as it is written ('a\\b%' reads as 'ab%')
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a\\\\b%'") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'a\\\\\\\\b%'") == Rows{{"7"}});
    REQUIRE(q(ex, "SELECT id FROM lk WHERE name LIKE 'ab\\\\%'").empty());
    ok(ex, "UPDATE lk SET name = 'z' WHERE name LIKE 'a\\_1'");
    ok(ex, "DELETE FROM lk WHERE name LIKE 'b\\_%'");
    REQUIRE(q(ex, "SELECT id, name FROM lk ORDER BY id") == Rows{{"1", "z"}, {"2", "ab1"}, {"3", "a_2"}, {"4", "a%x"}, {"5", "axx"}, {"7", "a\\b"}});
}

TEST_CASE("R54: USE changes the current database and leaves the redo log (and every running statement) alone", "[r54][use]") {
    TempDataDir dir("r54_use");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "CREATE DATABASE e");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
        ok(ex, "INSERT INTO t VALUES (1, 10)");
        ok(ex, "INSERT INTO t VALUES (2, 20)");
        auto redo_bytes = [&] { return std::stoll(q(ex, "SHOW GLOBAL STATUS LIKE 'Innodb_redo_log_bytes'")[0][1]); };
        const long long before = redo_bytes();
        REQUIRE(before > 0);
        ok(ex, "USE e");
        ok(ex, "USE d");
        REQUIRE(redo_bytes() == before); // (a USE used to checkpoint it: the table files were rewritten and the log emptied)
        // the SET statements that change only this session's variables are the same
        ok(ex, "SET @x = 5");
        ok(ex, "SET autocommit = 1");
        ok(ex, "SET sql_mode = 'ANSI', @y = @x + 1");
        ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
        ok(ex, "SET NAMES utf8mb4");
        REQUIRE(redo_bytes() == before);
        REQUIRE(q(ex, "SELECT @x, @y") == Rows{{"5", "6"}});
        REQUIRE(fails(ex, "USE nowhere").find("Unknown database") != std::string::npos);
        REQUIRE(q(ex, "SELECT DATABASE()") == Rows{{"d"}});
        ok(ex, "USE e");
        REQUIRE(q(ex, "SELECT DATABASE()") == Rows{{"e"}});
        // inside a transaction too, and the transaction goes on
        ok(ex, "USE d");
        ok(ex, "BEGIN");
        ok(ex, "INSERT INTO t VALUES (3, 30)");
        ok(ex, "USE e");
        ok(ex, "USE d");
        ok(ex, "COMMIT");
        ok(ex, "INSERT INTO t VALUES (4, 40)");
        ok(ex, "USE e");
        REQUIRE(redo_bytes() > before);
        // another session's USE does not wait for this one's statements and does not touch its database
        Executor other = Executor::new_session(ex.get_shared());
        ok(other, "USE e");
        REQUIRE(q(ex, "SELECT DATABASE()") == Rows{{"e"}});
        ok(ex, "USE d");
        REQUIRE(q(other, "SELECT DATABASE()") == Rows{{"e"}});
    } // (a crash: nothing was checkpointed, the redo log is what holds the rows)
    Executor again(dir.path);
    ok(again, "USE d");
    REQUIRE(q(again, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "10"}, {"2", "20"}, {"3", "30"}, {"4", "40"}});
    REQUIRE(q(again, "SELECT REDO_OPS > 0 FROM information_schema.recovery") == Rows{{"1"}});
    // a SET whose value reads a table through a subquery still works (it keeps the exclusive path)
    ok(again, "SET @n = (SELECT COUNT(*) FROM t)");
    REQUIRE(q(again, "SELECT @n") == Rows{{"4"}});
}

TEST_CASE("R54: a parent row that nothing refers to is deleted without scanning the child table", "[r54][fk]") {
    ProbeCheck check;
    TempDataDir dir("r54_fk");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR(10))");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id))");
    ok(ex, "CREATE TABLE pc (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE cc (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES pc(id) ON DELETE CASCADE ON UPDATE CASCADE)");
    ok(ex, "CREATE TABLE pn (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE cn (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES pn(id) ON DELETE SET NULL)");
    // 3000 parents; the first 1500 are each referred to by two children (ids i and i + 1500)
    const auto parents = [](int i) { return std::to_string(i) + ",'n" + std::to_string(i) + "'"; };
    const auto children = [](int i) { return std::to_string(i) + "," + std::to_string((i - 1) % 1500 + 1); };
    const auto ids = [](int i) { return std::to_string(i); };
    for (int from = 1; from <= 3000; from += 500) {
        ok(ex, multi_insert("p", from, from + 499, parents));
        ok(ex, multi_insert("c", from, from + 499, children));
        ok(ex, multi_insert("pc", from, from + 499, ids));
        ok(ex, multi_insert("cc", from, from + 499, children));
        ok(ex, multi_insert("pn", from, from + 499, ids));
        ok(ex, multi_insert("cn", from, from + 499, children));
    }
    auto count = [&](const std::string& table) { return std::stoll(q(ex, "SELECT COUNT(*) FROM " + table)[0][0]); };

    // restrict: the first statement scans, the second makes the cache, then the child table is not read for a parent that nothing refers to
    ok(ex, "DELETE FROM p WHERE id = 3000");
    const auto builds0 = probe_builds(ex);
    ok(ex, "DELETE FROM p WHERE id = 2999");
    REQUIRE(probe_builds(ex) > builds0);
    const auto skipped0 = probe_skipped(ex);
    for (int id = 2998; id >= 2900; id--) ok(ex, "DELETE FROM p WHERE id = " + std::to_string(id));
    REQUIRE(probe_skipped(ex) >= skipped0 + 99);
    REQUIRE(count("p") == 2899);
    // a parent that a child refers to is still refused, by every way of finding it
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 5").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 1500").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "DELETE FROM p WHERE id BETWEEN 1495 AND 1505").find("Foreign key violation") != std::string::npos);
    REQUIRE(count("p") == 2899);
    // a child written after the cache was made counts: INSERT, UPDATE, DELETE, a transaction that is rolled back
    ok(ex, "INSERT INTO c VALUES (10001, 2000)");
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 2000").find("Foreign key violation") != std::string::npos);
    ok(ex, "DELETE FROM c WHERE id = 10001");
    ok(ex, "DELETE FROM p WHERE id = 2000");
    ok(ex, "UPDATE c SET pid = 2001 WHERE id = 7"); // (parent 7 keeps its second child, 1507; 2001 gets one)
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 2001").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 7").find("Foreign key violation") != std::string::npos);
    ok(ex, "UPDATE c SET pid = 2001 WHERE id = 1507");
    ok(ex, "DELETE FROM p WHERE id = 7"); // (no child is left on it)
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 2001").find("Foreign key violation") != std::string::npos);
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO c VALUES (10002, 2002)");
    ok(ex, "ROLLBACK");
    ok(ex, "DELETE FROM p WHERE id = 2002"); // (the child that was rolled back refers to nothing)
    ok(ex, "BEGIN");
    ok(ex, "DELETE FROM c WHERE id = 8"); // (parent 8's other child, 1508, stays)
    ok(ex, "DELETE FROM c WHERE id = 1508");
    ok(ex, "ROLLBACK");
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 8").find("Foreign key violation") != std::string::npos);
    // a TRUNCATE, a restart of the counters by DDL: the cache is made again
    ok(ex, "ALTER TABLE c ADD COLUMN note INT");
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 9").find("Foreign key violation") != std::string::npos);
    ok(ex, "DELETE FROM p WHERE id = 2003");
    ok(ex, "DELETE FROM p WHERE id = 2004");
    ok(ex, "DELETE FROM p WHERE id = 2005");
    // the same for a change of the key of the parent (ON UPDATE RESTRICT)
    ok(ex, "UPDATE p SET id = 90001 WHERE id = 2500");
    ok(ex, "UPDATE p SET id = 90002 WHERE id = 2501");
    REQUIRE(fails(ex, "UPDATE p SET id = 90003 WHERE id = 6").find("Foreign key violation") != std::string::npos);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM p WHERE id = 6") == Rows{{"1"}});
    // (a statement that only looks at the child table leaves the cache current: the next one does not scan either, and the cache is not made again)
    ok(ex, "DELETE FROM p WHERE id = 2504");
    ok(ex, "DELETE FROM p WHERE id = 2505");
    const auto builds1 = probe_builds(ex), skipped1 = probe_skipped(ex);
    for (int id = 2510; id < 2515; id++) ok(ex, "UPDATE p SET id = " + std::to_string(id + 80000) + " WHERE id = " + std::to_string(id));
    ok(ex, "DELETE FROM p WHERE id = 2503");
    REQUIRE(probe_builds(ex) == builds1);
    REQUIRE(probe_skipped(ex) >= skipped1 + 6);

    // cascade: the unreferenced parents go without a scan, a referenced one takes its children with it, and the next parent finds the cache current
    ok(ex, "DELETE FROM pc WHERE id = 3000");
    const auto skipped2 = probe_skipped(ex);
    ok(ex, "DELETE FROM pc WHERE id = 2999"); // (the second statement makes the cache, and a parent that nothing refers to has no child to find)
    REQUIRE(probe_skipped(ex) > skipped2);
    ok(ex, "DELETE FROM pc WHERE id = 5"); // (children 5 and 1505 go)
    REQUIRE(count("cc") == 2998);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM cc WHERE pid = 5") == Rows{{"0"}});
    ok(ex, "DELETE FROM pc WHERE id = 2998");
    ok(ex, "DELETE FROM pc WHERE id BETWEEN 6 AND 8"); // (six children)
    REQUIRE(count("cc") == 2992);
    ok(ex, "UPDATE pc SET id = 70001 WHERE id = 9"); // (children 9 and 1509 follow)
    REQUIRE(q(ex, "SELECT COUNT(*) FROM cc WHERE pid = 70001") == Rows{{"2"}});
    const auto skipped3 = probe_skipped(ex);
    ok(ex, "UPDATE pc SET id = 70002 WHERE id = 2900"); // (no child to follow, and the cache that the cascades kept current says so)
    REQUIRE(probe_skipped(ex) > skipped3);
    REQUIRE(count("cc") == 2992);
    // set null
    ok(ex, "DELETE FROM pn WHERE id = 3000");
    ok(ex, "DELETE FROM pn WHERE id = 2999");
    ok(ex, "DELETE FROM pn WHERE id = 10");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM cn WHERE pid IS NULL") == Rows{{"2"}});
    ok(ex, "DELETE FROM pn WHERE id = 11");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM cn WHERE pid IS NULL") == Rows{{"4"}});
    REQUIRE(count("cn") == 3000);
}

TEST_CASE("R54: a NULL in the referenced column of a parent is referred to by nothing", "[r54][fk]") {
    TempDataDir dir("r54_fk_null");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, code VARCHAR(5) UNIQUE)");
    ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY, pcode VARCHAR(5), FOREIGN KEY (pcode) REFERENCES par(code))");
    ok(ex, "INSERT INTO par VALUES (1, NULL), (2, 'a'), (3, NULL)");
    ok(ex, "INSERT INTO chi VALUES (1, NULL), (2, 'a')");
    ok(ex, "DELETE FROM par WHERE id = 1"); // (the child with a NULL refers to no parent; MySQL deletes the row too)
    REQUIRE(fails(ex, "DELETE FROM par WHERE id = 2").find("Foreign key violation") != std::string::npos);
    ok(ex, "UPDATE par SET code = 'z' WHERE id = 3"); // (the value that changes is a NULL)
    REQUIRE(q(ex, "SELECT id FROM par ORDER BY id") == Rows{{"2"}, {"3"}});
}

TEST_CASE("R54: parents are deleted while children are written", "[r54][fk][concurrency]") {
    ProbeCheck check;
    TempDataDir dir("r54_fk_threads");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id))");
    const auto ids = [](int i) { return std::to_string(i); };
    for (int from = 1; from <= 2000; from += 500) ok(ex, multi_insert("p", from, from + 499, ids));
    ok(ex, multi_insert("c", 1, 1500, [](int i) { return std::to_string(i) + "," + std::to_string(i); })); // (parents 1..1500 have a child each)
    auto shared = ex.get_shared();
    std::atomic<bool> failed{false};
    std::atomic<int> deleted{0};
    std::thread deleter([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        for (int id = 2000; id > 1500; id--) {
            auto r = s.execute_sql("DELETE FROM p WHERE id = " + std::to_string(id));
            if (r.is_ok()) deleted++;
            else if (r.error().find("Foreign key violation") == std::string::npos) failed = true; // (a parent a child has just taken is refused: that is right)
        }
    });
    std::thread writer([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        int next = 10000;
        for (int id = 1501; id <= 2000; id += 3) {
            auto r = s.execute_sql("INSERT INTO c VALUES (" + std::to_string(next++) + ", " + std::to_string(id) + ")");
            if (r.is_err() && r.error().find("Foreign key violation") == std::string::npos) failed = true; // (the parent may be gone already)
            if (id % 9 == 0) s.execute_sql("DELETE FROM c WHERE id = " + std::to_string(next - 1));
        }
    });
    deleter.join();
    writer.join();
    REQUIRE_FALSE(failed);
    REQUIRE(deleted > 0);
    // no child is left without its parent
    REQUIRE(q(ex, "SELECT COUNT(*) FROM c LEFT JOIN p ON p.id = c.pid WHERE p.id IS NULL") == Rows{{"0"}});
}

namespace {
// the text of the answer of a DELETE / UPDATE: "<n> row(s) deleted."
std::string answer(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return r.value();
}

Rows emp_rows(Executor& ex) { return q(ex, "SELECT id, mgr FROM emp ORDER BY id"); }
} // namespace

TEST_CASE("R55: a DELETE of a row that its own table refers to (emp.mgr -> emp.id) is refused row by row under RESTRICT", "[r55][self_fk]") {
    TempDataDir dir("r55_restrict");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES emp(id))");
    const Rows all = {{"1", "NULL"}, {"2", "1"}, {"3", "2"}, {"4", "1"}, {"5", "NULL"}};
    ok(ex, "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 2), (4, 1), (5, NULL)");
    // (the row went whatever referred to it: no child was looked for in the table itself)
    REQUIRE(fails(ex, "DELETE FROM emp WHERE id = 1").find("Foreign key violation") != std::string::npos);
    REQUIRE(emp_rows(ex) == all);
    REQUIRE(answer(ex, "DELETE FROM emp WHERE id = 5") == "1 row(s) deleted.");
    REQUIRE(answer(ex, "DELETE FROM emp WHERE id = 3") == "1 row(s) deleted.");
    REQUIRE(emp_rows(ex) == (Rows{{"1", "NULL"}, {"2", "1"}, {"4", "1"}}));
    // the rows go in the order of the key: 1 is first and has children, so the statement is refused whole, whatever is named after it
    REQUIRE_FALSE(ex.execute_sql("DELETE FROM emp WHERE id IN (4, 2, 1)").is_ok());
    REQUIRE_FALSE(ex.execute_sql("DELETE FROM emp").is_ok());
    REQUIRE(emp_rows(ex) == (Rows{{"1", "NULL"}, {"2", "1"}, {"4", "1"}}));
    // ... but in the order of an ORDER BY the children are gone before their parent is reached
    REQUIRE(answer(ex, "DELETE FROM emp ORDER BY id DESC") == "3 row(s) deleted.");
    REQUIRE(emp_rows(ex).empty());
    // a row that is its own parent is its own child, until it is gone
    ok(ex, "INSERT INTO emp VALUES (1, NULL), (7, 7)");
    REQUIRE_FALSE(ex.execute_sql("DELETE FROM emp WHERE id = 7").is_ok());
    ok(ex, "UPDATE emp SET mgr = 1 WHERE id = 7");
    ok(ex, "DELETE FROM emp WHERE id = 7");
    REQUIRE(emp_rows(ex) == (Rows{{"1", "NULL"}}));
    // the order of the key, not the order the rows were written in: 3 is stored after 5 but is read before it
    ok(ex, "DELETE FROM emp");
    ok(ex, "INSERT INTO emp VALUES (5, NULL), (3, 5), (4, 5)");
    REQUIRE_FALSE(ex.execute_sql("DELETE FROM emp WHERE id IN (5, 4)").is_ok());
    REQUIRE(answer(ex, "DELETE FROM emp") == "3 row(s) deleted.");
}

TEST_CASE("R55: ON DELETE CASCADE / SET NULL through the table's own foreign key: the rows that go with a row, and the rows the statement counts", "[r55][self_fk]") {
    TempDataDir dir("r55_cascade");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES emp(id) ON DELETE CASCADE)");
        ok(ex, "CREATE TABLE sn (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES sn(id) ON DELETE SET NULL)");
        ok(ex, "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 2), (4, 1), (5, NULL)");
        ok(ex, "INSERT INTO sn VALUES (1, NULL), (2, 1), (3, 2), (4, 1), (5, NULL)");
        // the children go, and their children: one row is counted, the cascade is not
        REQUIRE(answer(ex, "DELETE FROM emp WHERE id = 1") == "1 row(s) deleted.");
        REQUIRE(emp_rows(ex) == (Rows{{"5", "NULL"}}));
        // a row that a cascade has deleted is not reached by the scan any more, so it is not counted
        ok(ex, "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 2), (4, 1)");
        REQUIRE(answer(ex, "DELETE FROM emp WHERE id IN (1, 2)") == "1 row(s) deleted.");
        REQUIRE(answer(ex, "DELETE FROM emp") == "1 row(s) deleted.");
        ok(ex, "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 2), (4, 1)");
        REQUIRE(answer(ex, "DELETE FROM emp ORDER BY id DESC") == "4 row(s) deleted."); // (the scan reaches every row before a cascade does)
        // a case-insensitive text key
        ok(ex, "CREATE TABLE tx (id VARCHAR(5) PRIMARY KEY, mgr VARCHAR(5), FOREIGN KEY (mgr) REFERENCES tx(id) ON DELETE CASCADE)");
        ok(ex, "INSERT INTO tx VALUES ('A', NULL), ('b', 'a'), ('C', 'B'), ('d', NULL)");
        ok(ex, "DELETE FROM tx WHERE id = 'A'");
        REQUIRE(q(ex, "SELECT id FROM tx") == Rows{{"d"}});
        // SET NULL: the children stay; a row that goes with its parent is not worth rewriting
        REQUIRE(answer(ex, "DELETE FROM sn WHERE id = 1") == "1 row(s) deleted.");
        REQUIRE(q(ex, "SELECT id, mgr FROM sn ORDER BY id") == (Rows{{"2", "NULL"}, {"3", "2"}, {"4", "NULL"}, {"5", "NULL"}}));
        ok(ex, "DELETE FROM sn");
        ok(ex, "INSERT INTO sn VALUES (1, NULL), (2, 1), (3, 2), (4, 1)");
        REQUIRE(answer(ex, "DELETE FROM sn WHERE id IN (1, 2)") == "2 row(s) deleted.");
        REQUIRE(q(ex, "SELECT id, mgr FROM sn ORDER BY id") == (Rows{{"3", "NULL"}, {"4", "NULL"}}));
        // the scan reads a row as an earlier SET NULL has left it: row 3 no longer matches `mgr IS NOT NULL` when it is reached
        ok(ex, "DELETE FROM sn");
        ok(ex, "INSERT INTO sn VALUES (1, NULL), (2, 1), (3, 2), (4, 1)");
        REQUIRE(answer(ex, "DELETE FROM sn WHERE mgr IS NOT NULL") == "2 row(s) deleted.");
        REQUIRE(q(ex, "SELECT id, mgr FROM sn ORDER BY id") == (Rows{{"1", "NULL"}, {"3", "NULL"}}));
        // (the rows that stay are still found through the primary key: the index lost only the rows that went)
        REQUIRE(q(ex, "SELECT id, mgr FROM sn WHERE id = 3") == (Rows{{"3", "NULL"}}));
        REQUIRE(q(ex, "SELECT id FROM sn WHERE id = 1") == Rows{{"1"}});
        REQUIRE(q(ex, "SELECT id FROM sn WHERE id = 2").empty());
        // a transaction that rolls the cascade back gets every row back
        ok(ex, "DELETE FROM emp");
        ok(ex, "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 2), (4, 1), (5, NULL)");
        ok(ex, "BEGIN");
        REQUIRE(answer(ex, "DELETE FROM emp WHERE id = 1") == "1 row(s) deleted.");
        REQUIRE(emp_rows(ex) == (Rows{{"5", "NULL"}}));
        ok(ex, "ROLLBACK");
        REQUIRE(emp_rows(ex) == (Rows{{"1", "NULL"}, {"2", "1"}, {"3", "2"}, {"4", "1"}, {"5", "NULL"}}));
        // a savepoint inside it
        ok(ex, "BEGIN");
        ok(ex, "SAVEPOINT s1");
        ok(ex, "DELETE FROM emp WHERE id = 2");
        ok(ex, "ROLLBACK TO SAVEPOINT s1");
        REQUIRE(emp_rows(ex).size() == 5);
        ok(ex, "DELETE FROM emp WHERE id = 1");
        ok(ex, "COMMIT");
        REQUIRE(emp_rows(ex) == (Rows{{"5", "NULL"}}));
    } // (no checkpoint: the redo log holds what the cascades did)
    Executor again(dir.path);
    ok(again, "USE d");
    REQUIRE(emp_rows(again) == (Rows{{"5", "NULL"}}));
    REQUIRE(q(again, "SELECT id, mgr FROM sn ORDER BY id") == (Rows{{"1", "NULL"}, {"3", "NULL"}}));
    REQUIRE(q(again, "SELECT id FROM tx") == Rows{{"d"}});
}

TEST_CASE("R55: a cascade of a table's own key reaches the foreign keys of other tables, and stops at 14 levels", "[r55][self_fk]") {
    TempDataDir dir("r55_levels");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES emp(id) ON DELETE CASCADE)");
    ok(ex, "CREATE TABLE badge (id INT PRIMARY KEY, emp INT, FOREIGN KEY (emp) REFERENCES emp(id))");
    ok(ex, "INSERT INTO emp VALUES (1, NULL), (2, 1), (3, 2), (4, 1), (5, NULL)");
    ok(ex, "INSERT INTO badge VALUES (1, 3)");
    // employee 3 goes with 1, and a badge refers to 3: nothing of the statement happens
    REQUIRE(fails(ex, "DELETE FROM emp WHERE id = 1").find("Foreign key violation") != std::string::npos);
    REQUIRE(emp_rows(ex).size() == 5);
    ok(ex, "DELETE FROM badge");
    ok(ex, "DROP TABLE badge");
    ok(ex, "CREATE TABLE badge (id INT PRIMARY KEY, emp INT, FOREIGN KEY (emp) REFERENCES emp(id) ON DELETE CASCADE)");
    ok(ex, "INSERT INTO badge VALUES (1, 3), (2, 5)");
    ok(ex, "DELETE FROM emp WHERE id = 1");
    REQUIRE(q(ex, "SELECT id FROM badge") == Rows{{"2"}}); // (the badge of a cascaded employee goes too)
    // a chain: 15 rows cascade, a 16th level does not (MySQL: error 3008)
    ok(ex, "DELETE FROM emp");
    const auto chain = [](int n) {
        std::string sql = "INSERT INTO emp VALUES (1, NULL)";
        for (int i = 2; i <= n; i++) sql += ", (" + std::to_string(i) + ", " + std::to_string(i - 1) + ")";
        return sql;
    };
    ok(ex, chain(15));
    ok(ex, "DELETE FROM emp WHERE id = 1");
    REQUIRE(emp_rows(ex).empty());
    ok(ex, chain(16));
    REQUIRE(fails(ex, "DELETE FROM emp WHERE id = 1").find("max depth of 15") != std::string::npos);
    REQUIRE(emp_rows(ex).size() == 16);
    ok(ex, "DELETE FROM emp WHERE id = 2");
    REQUIRE(emp_rows(ex) == Rows{{"1", "NULL"}});
}

TEST_CASE("R55: two foreign keys of a table to itself: a cascade is refused by the other one while a child is there to see", "[r55][self_fk]") {
    TempDataDir dir("r55_two");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, mgr INT, buddy INT, FOREIGN KEY (mgr) REFERENCES emp(id) ON DELETE CASCADE, FOREIGN KEY (buddy) REFERENCES emp(id))");
    ok(ex, "INSERT INTO emp VALUES (1, NULL, NULL), (2, 1, NULL), (3, NULL, 2), (4, NULL, NULL), (5, NULL, NULL)");
    REQUIRE_FALSE(ex.execute_sql("DELETE FROM emp WHERE id = 1").is_ok()); // (2 goes with 1, and 3 has 2 as a buddy)
    REQUIRE(q(ex, "SELECT COUNT(*) FROM emp") == Rows{{"5"}});
    ok(ex, "DELETE FROM emp WHERE id = 5");
    ok(ex, "UPDATE emp SET buddy = NULL WHERE id = 3");
    ok(ex, "DELETE FROM emp WHERE id = 1");
    REQUIRE(q(ex, "SELECT id FROM emp ORDER BY id") == (Rows{{"3"}, {"4"}}));
}

TEST_CASE("R55: ON UPDATE CASCADE / SET NULL of a table's own key acts as RESTRICT", "[r55][self_fk]") {
    TempDataDir dir("r55_update");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES c(id) ON UPDATE CASCADE)");
    ok(ex, "CREATE TABLE n (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES n(id) ON UPDATE SET NULL)");
    for (const char* t : {"c", "n"}) ok(ex, std::string("INSERT INTO ") + t + " VALUES (1, NULL), (2, 1), (3, 2), (4, 1), (5, NULL)");
    for (const char* t : {"c", "n"}) {
        const std::string name = t;
        INFO(name);
        // (a key that has children: nothing is cascaded into the table that is being updated)
        REQUIRE_FALSE(ex.execute_sql("UPDATE " + name + " SET id = 10 WHERE id = 1").is_ok());
        REQUIRE_FALSE(ex.execute_sql("UPDATE " + name + " SET id = id + 10").is_ok());
        REQUIRE(q(ex, "SELECT id, mgr FROM " + name + " ORDER BY id") == (Rows{{"1", "NULL"}, {"2", "1"}, {"3", "2"}, {"4", "1"}, {"5", "NULL"}}));
        // a key that has none is changed
        ok(ex, "UPDATE " + name + " SET id = 50 WHERE id = 5");
        ok(ex, "UPDATE " + name + " SET id = 30 WHERE id = 3");
        REQUIRE(q(ex, "SELECT id, mgr FROM " + name + " ORDER BY id") == (Rows{{"1", "NULL"}, {"2", "1"}, {"4", "1"}, {"30", "2"}, {"50", "NULL"}}));
    }
}

TEST_CASE("R55: parents of a table's own key are deleted while children are written", "[r55][self_fk][concurrency]") {
    TempDataDir dir("r55_threads");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES emp(id) ON DELETE CASCADE)");
    ok(ex, multi_insert("emp", 1, 400, [](int i) { return std::to_string(i) + ",NULL"; }));
    ok(ex, multi_insert("emp", 401, 800, [](int i) { return std::to_string(i) + "," + std::to_string(i - 400); }));
    auto shared = ex.get_shared();
    std::atomic<bool> failed{false};
    std::atomic<int> deleted{0};
    std::thread deleter([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        for (int id = 400; id >= 1; id -= 2) {
            auto r = s.execute_sql("DELETE FROM emp WHERE id = " + std::to_string(id));
            if (r.is_ok()) deleted++;
            else failed = true;
        }
    });
    std::thread writer([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        int next = 10000;
        for (int id = 1; id <= 400; id += 2) {
            auto r = s.execute_sql("INSERT INTO emp VALUES (" + std::to_string(next++) + ", " + std::to_string(id) + ")");
            if (r.is_err() && r.error().find("Foreign key violation") == std::string::npos) failed = true; // (the parent may be gone already)
        }
    });
    deleter.join();
    writer.join();
    REQUIRE_FALSE(failed);
    REQUIRE(deleted == 200);
    // no row is left without its parent
    REQUIRE(q(ex, "SELECT COUNT(*) FROM emp e LEFT JOIN emp m ON m.id = e.mgr WHERE e.mgr IS NOT NULL AND m.id IS NULL") == Rows{{"0"}});
    // the even parents are gone, with everything below them
    REQUIRE(q(ex, "SELECT COUNT(*) FROM emp WHERE id <= 400 AND id % 2 = 0") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM emp WHERE id > 400 AND id <= 800 AND mgr % 2 = 0") == Rows{{"0"}});
}

TEST_CASE("R55: a cascade waits for a row of its own table that another transaction holds", "[r55][self_fk][concurrency]") {
    TempDataDir dir("r55_claims");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, mgr INT, v INT, FOREIGN KEY (mgr) REFERENCES emp(id) ON DELETE CASCADE)");
    ok(ex, "INSERT INTO emp VALUES (1, NULL, 0), (2, 1, 0), (3, 2, 0)");
    ok(ex, "BEGIN");
    ok(ex, "UPDATE emp SET v = 1 WHERE id = 3"); // (this transaction holds row 3, which the cascade of row 1 reaches two levels down)
    std::atomic<bool> finished{false}, failed{false};
    auto shared = ex.get_shared();
    std::thread deleter([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok() || !s.execute_sql("DELETE FROM emp WHERE id = 1").is_ok()) failed = true;
        finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE_FALSE(finished); // (it waits for the row, as a DELETE of that row would)
    ok(ex, "COMMIT");
    deleter.join();
    REQUIRE_FALSE(failed);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM emp") == Rows{{"0"}});
}

TEST_CASE("R55: a statement that only reads (WITH, a derived table, a prepared SELECT, SET with a subquery, LOCK TABLES, ANALYZE, a read-only CALL) leaves the redo log", "[r55][redo]") {
    TempDataDir dir("r55_redo");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
        ok(ex, "CREATE PROCEDURE rd() BEGIN SELECT COUNT(*) FROM t; END");
        ok(ex, "CREATE PROCEDURE wr() BEGIN INSERT INTO t VALUES (99, 1); END");
        ok(ex, "INSERT INTO t VALUES (1, 10)");
        ok(ex, "INSERT INTO t VALUES (2, 20)");
        auto redo_bytes = [&] { return std::stoll(q(ex, "SHOW GLOBAL STATUS LIKE 'Innodb_redo_log_bytes'")[0][1]); };
        const long long before = redo_bytes();
        REQUIRE(before > 0);
        REQUIRE(q(ex, "WITH c AS (SELECT id FROM t) SELECT * FROM c ORDER BY id") == (Rows{{"1"}, {"2"}}));
        REQUIRE(q(ex, "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 3) SELECT n FROM r") == (Rows{{"1"}, {"2"}, {"3"}}));
        REQUIRE(q(ex, "SELECT * FROM (SELECT id FROM t) d ORDER BY id") == (Rows{{"1"}, {"2"}}));
        ok(ex, "PREPARE st FROM 'SELECT v FROM t WHERE id = ?'");
        ok(ex, "SET @i = 2");
        REQUIRE(q(ex, "EXECUTE st USING @i") == Rows{{"20"}});
        ok(ex, "DEALLOCATE PREPARE st");
        ok(ex, "SET @n = (SELECT COUNT(*) FROM t)");
        REQUIRE(q(ex, "SELECT @n") == Rows{{"2"}});
        ok(ex, "LOCK TABLES t READ");
        ok(ex, "UNLOCK TABLES");
        ok(ex, "ANALYZE TABLE t");
        REQUIRE(q(ex, "CALL rd()") == Rows{{"2"}});
        REQUIRE(redo_bytes() == before); // (every one of them used to checkpoint it: the table files were rewritten and the log emptied)
        // inside a transaction they do not make it flush the tables at COMMIT either
        ok(ex, "BEGIN");
        ok(ex, "INSERT INTO t VALUES (3, 30)");
        REQUIRE(q(ex, "WITH c AS (SELECT id FROM t) SELECT COUNT(*) FROM c") == Rows{{"3"}});
        ok(ex, "COMMIT");
        REQUIRE(redo_bytes() > before);
        // EXECUTE takes the path of the statement that was prepared: an INSERT is logged like any other
        ok(ex, "PREPARE ins FROM 'INSERT INTO t VALUES (?, ?)'");
        ok(ex, "SET @a = 4, @b = 40");
        const long long mid = redo_bytes();
        ok(ex, "EXECUTE ins USING @a, @b");
        REQUIRE(redo_bytes() > mid);
        REQUIRE(fails(ex, "EXECUTE ins USING @a, @b").find("Duplicate") != std::string::npos);
        REQUIRE(fails(ex, "EXECUTE nothing").find("Unknown prepared statement") != std::string::npos);
        ok(ex, "CALL wr()"); // (a procedure that writes still has its own path)
        REQUIRE(q(ex, "SELECT COUNT(*) FROM t") == Rows{{"5"}});
    } // (a crash: whatever was not checkpointed comes back from the log)
    Executor again(dir.path);
    ok(again, "USE d");
    REQUIRE(q(again, "SELECT id, v FROM t ORDER BY id") == (Rows{{"1", "10"}, {"2", "20"}, {"3", "30"}, {"4", "40"}, {"99", "1"}}));
}

namespace {
// the rows of a table in key order, as text: "id|a_id|..." joined by "," (NULL as the word)
std::string dump(Executor& ex, const std::string& table) {
    std::string out;
    for (auto& row : q(ex, "SELECT * FROM " + table + " ORDER BY id")) {
        if (!out.empty()) out += ",";
        for (std::size_t i = 0; i < row.size(); i++) out += (i ? "|" : "") + row[i];
    }
    return out;
}

// three tables a <- b <- c with the actions the caller gives for b's and c's foreign key
void chain(Executor& ex, const std::string& suffix, const std::string& b_action, const std::string& c_action) {
    ok(ex, "CREATE TABLE a" + suffix + " (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE b" + suffix + " (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES a" + suffix + "(id) ON DELETE " + b_action + ")");
    ok(ex, "CREATE TABLE c" + suffix + " (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b" + suffix + "(id) ON DELETE " + c_action + ")");
    ok(ex, "INSERT INTO a" + suffix + " VALUES (1), (2)");
    ok(ex, "INSERT INTO b" + suffix + " VALUES (10, 1), (20, 2), (11, 1)");
    ok(ex, "INSERT INTO c" + suffix + " VALUES (100, 10), (200, 20), (101, 11), (102, 10)");
}
} // namespace

TEST_CASE("R56: a cascade goes down the whole chain of tables (a <- b <- c) and a RESTRICT anywhere below refuses the statement", "[r56][chain]") {
    TempDataDir dir("r56_chain");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        // CASCADE all the way: the grandchildren go with the children
        chain(ex, "1", "CASCADE", "CASCADE");
        REQUIRE(answer(ex, "DELETE FROM a1 WHERE id = 1") == "1 row(s) deleted.");
        REQUIRE(dump(ex, "a1") == "2");
        REQUIRE(dump(ex, "b1") == "20|2");
        REQUIRE(dump(ex, "c1") == "200|20");
        // RESTRICT below a CASCADE: nothing of the statement happens, for either row of a
        chain(ex, "2", "CASCADE", "RESTRICT");
        REQUIRE(fails(ex, "DELETE FROM a2 WHERE id = 1").find("Foreign key violation") != std::string::npos);
        REQUIRE_FALSE(ex.execute_sql("DELETE FROM a2 WHERE id = 2").is_ok());
        REQUIRE(dump(ex, "a2") == "1,2");
        REQUIRE(dump(ex, "b2") == "10|1,11|1,20|2");
        REQUIRE(dump(ex, "c2") == "100|10,101|11,102|10,200|20");
        // SET NULL one level further down, and SET NULL above a CASCADE (the children stay, the grandchildren with them)
        chain(ex, "3", "CASCADE", "SET NULL");
        ok(ex, "DELETE FROM a3 WHERE id = 1");
        REQUIRE(dump(ex, "c3") == "100|NULL,101|NULL,102|NULL,200|20");
        chain(ex, "4", "SET NULL", "CASCADE");
        ok(ex, "DELETE FROM a4 WHERE id = 1");
        REQUIRE(dump(ex, "b4") == "10|NULL,11|NULL,20|2");
        REQUIRE(dump(ex, "c4") == "100|10,101|11,102|10,200|20");
        // four levels
        chain(ex, "5", "CASCADE", "CASCADE");
        ok(ex, "CREATE TABLE d5 (id INT PRIMARY KEY, r INT, FOREIGN KEY (r) REFERENCES c5(id) ON DELETE CASCADE)");
        ok(ex, "INSERT INTO d5 VALUES (1000, 100), (2000, 200)");
        ok(ex, "DELETE FROM a5 WHERE id = 1");
        REQUIRE(dump(ex, "d5") == "2000|200");
        // one row of a with children in two tables, one of them RESTRICT, and a grandchild: refused
        ok(ex, "CREATE TABLE a6 (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE b6 (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES a6(id) ON DELETE CASCADE)");
        ok(ex, "CREATE TABLE b6r (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES a6(id) ON DELETE RESTRICT)");
        ok(ex, "CREATE TABLE c6 (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b6(id) ON DELETE CASCADE)");
        ok(ex, "INSERT INTO a6 VALUES (1)");
        ok(ex, "INSERT INTO b6 VALUES (10, 1)");
        ok(ex, "INSERT INTO b6r VALUES (30, 1)");
        ok(ex, "INSERT INTO c6 VALUES (100, 10)");
        REQUIRE_FALSE(ex.execute_sql("DELETE FROM a6 WHERE id = 1").is_ok());
        REQUIRE(dump(ex, "b6") == "10|1");
        REQUIRE(dump(ex, "c6") == "100|10");
        // a diamond: c refers to a (CASCADE) and to b (RESTRICT): the b row goes first (tables are asked in the order of their names), and c is still there to see
        ok(ex, "CREATE TABLE a7 (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE b7 (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES a7(id) ON DELETE CASCADE)");
        ok(ex, "CREATE TABLE c7 (id INT PRIMARY KEY, ap INT, bp INT, FOREIGN KEY (ap) REFERENCES a7(id) ON DELETE CASCADE, FOREIGN KEY (bp) REFERENCES b7(id) ON DELETE RESTRICT)");
        ok(ex, "INSERT INTO a7 VALUES (1)");
        ok(ex, "INSERT INTO b7 VALUES (10, 1)");
        ok(ex, "INSERT INTO c7 VALUES (100, 1, 10)");
        REQUIRE_FALSE(ex.execute_sql("DELETE FROM a7 WHERE id = 1").is_ok());
        REQUIRE(dump(ex, "c7") == "100|1|10");
        // the whole table: the first row of a (id 1) has b 10 with a RESTRICT grandchild, whatever the order the rows are read in
        chain(ex, "8", "CASCADE", "RESTRICT");
        REQUIRE_FALSE(ex.execute_sql("DELETE FROM a8").is_ok());
        REQUIRE_FALSE(ex.execute_sql("DELETE FROM a8 ORDER BY id DESC").is_ok());
        REQUIRE(dump(ex, "a8") == "1,2");
        // a cycle across two tables
        ok(ex, "CREATE TABLE a9 (id INT PRIMARY KEY, bp INT)");
        ok(ex, "CREATE TABLE b9 (id INT PRIMARY KEY, ap INT)");
        ok(ex, "INSERT INTO a9 VALUES (1, NULL), (2, NULL)");
        ok(ex, "INSERT INTO b9 VALUES (10, 1), (20, 2)");
        ok(ex, "UPDATE a9 SET bp = 10 WHERE id = 1");
        ok(ex, "ALTER TABLE b9 ADD FOREIGN KEY (ap) REFERENCES a9(id) ON DELETE CASCADE");
        ok(ex, "ALTER TABLE a9 ADD FOREIGN KEY (bp) REFERENCES b9(id) ON DELETE CASCADE");
        REQUIRE(answer(ex, "DELETE FROM a9 WHERE id = 1") == "1 row(s) deleted.");
        REQUIRE(dump(ex, "a9") == "2|NULL");
        REQUIRE(dump(ex, "b9") == "20|2");
        // SET NULL on a column that grandchildren refer to: they follow it (ON UPDATE CASCADE) to NULL
        ok(ex, "CREATE TABLE a10 (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE b10 (id INT PRIMARY KEY, p INT UNIQUE, FOREIGN KEY (p) REFERENCES a10(id) ON DELETE SET NULL)");
        ok(ex, "CREATE TABLE c10 (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b10(p) ON UPDATE CASCADE ON DELETE CASCADE)");
        ok(ex, "INSERT INTO a10 VALUES (1)");
        ok(ex, "INSERT INTO b10 VALUES (10, 1)");
        ok(ex, "INSERT INTO c10 VALUES (100, 1)");
        ok(ex, "DELETE FROM a10 WHERE id = 1");
        REQUIRE(dump(ex, "b10") == "10|NULL");
        REQUIRE(dump(ex, "c10") == "100|NULL");
        // a transaction that rolls the chain back gets every row of every table back, with the index
        chain(ex, "11", "CASCADE", "CASCADE");
        ok(ex, "BEGIN");
        ok(ex, "DELETE FROM a11 WHERE id = 1");
        REQUIRE(dump(ex, "c11") == "200|20");
        ok(ex, "ROLLBACK");
        REQUIRE(dump(ex, "c11") == "100|10,101|11,102|10,200|20");
        REQUIRE(q(ex, "SELECT q FROM c11 WHERE id = 102") == Rows{{"10"}});
        ok(ex, "BEGIN");
        ok(ex, "SAVEPOINT s1");
        ok(ex, "DELETE FROM a11 WHERE id = 2");
        ok(ex, "ROLLBACK TO SAVEPOINT s1");
        ok(ex, "DELETE FROM a11 WHERE id = 1");
        ok(ex, "COMMIT");
        REQUIRE(dump(ex, "b11") == "20|2");
        REQUIRE(dump(ex, "c11") == "200|20");
    } // (no checkpoint: the redo log holds what the chains did)
    Executor again(dir.path);
    ok(again, "USE d");
    REQUIRE(dump(again, "c1") == "200|20");
    REQUIRE(dump(again, "c3") == "100|NULL,101|NULL,102|NULL,200|20");
    REQUIRE(dump(again, "d5") == "2000|200");
    REQUIRE(dump(again, "c10") == "100|NULL");
    REQUIRE(dump(again, "c11") == "200|20");
    REQUIRE(dump(again, "b9") == "20|2");
}

TEST_CASE("R56: a key that changes goes down the chain too (ON UPDATE CASCADE through a column another table refers to), and a RESTRICT below refuses", "[r56][chain]") {
    TempDataDir dir("r56_update_chain");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE a (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, p INT UNIQUE, FOREIGN KEY (p) REFERENCES a(id) ON UPDATE CASCADE)");
        ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b(p) ON UPDATE CASCADE)");
        ok(ex, "INSERT INTO a VALUES (1), (2)");
        ok(ex, "INSERT INTO b VALUES (10, 1), (20, 2)");
        ok(ex, "INSERT INTO c VALUES (100, 1), (200, 2)");
        ok(ex, "UPDATE a SET id = 7 WHERE id = 1");
        REQUIRE(dump(ex, "a") == "2,7");
        REQUIRE(dump(ex, "b") == "10|7,20|2");
        REQUIRE(dump(ex, "c") == "100|7,200|2");
        // the new values are found through the primary key and the foreign key columns
        REQUIRE(q(ex, "SELECT id FROM b WHERE p = 7") == Rows{{"10"}});
        REQUIRE(fails(ex, "INSERT INTO c VALUES (300, 1)").find("Foreign key violation") != std::string::npos);
        ok(ex, "INSERT INTO c VALUES (300, 7)");
        // ... and a RESTRICT two levels down refuses the statement, which changes nothing (the other row of a can move)
        ok(ex, "CREATE TABLE a2 (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE b2 (id INT PRIMARY KEY, p INT UNIQUE, FOREIGN KEY (p) REFERENCES a2(id) ON UPDATE CASCADE)");
        ok(ex, "CREATE TABLE c2 (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b2(p) ON UPDATE RESTRICT)");
        ok(ex, "INSERT INTO a2 VALUES (1), (2)");
        ok(ex, "INSERT INTO b2 VALUES (10, 1), (20, 2)");
        ok(ex, "INSERT INTO c2 VALUES (100, 1)");
        REQUIRE_FALSE(ex.execute_sql("UPDATE a2 SET id = 7 WHERE id = 1").is_ok());
        REQUIRE(dump(ex, "a2") == "1,2");
        REQUIRE(dump(ex, "b2") == "10|1,20|2");
        ok(ex, "UPDATE a2 SET id = 8 WHERE id = 2");
        REQUIRE(dump(ex, "a2") == "1,8");
        REQUIRE(dump(ex, "b2") == "10|1,20|8");
        REQUIRE(dump(ex, "c2") == "100|1");
        // a cascade that comes back to the table it started in acts as RESTRICT: b.p follows a.id, and a.bk refers to b.p
        ok(ex, "CREATE TABLE a3 (id INT PRIMARY KEY, bk INT UNIQUE)");
        ok(ex, "CREATE TABLE b3 (id INT PRIMARY KEY, ap INT UNIQUE, FOREIGN KEY (ap) REFERENCES a3(id) ON UPDATE CASCADE)");
        ok(ex, "INSERT INTO a3 VALUES (1, NULL), (2, NULL)");
        ok(ex, "INSERT INTO b3 VALUES (10, 1), (20, 2)");
        ok(ex, "ALTER TABLE a3 ADD FOREIGN KEY (bk) REFERENCES b3(ap) ON UPDATE CASCADE");
        ok(ex, "UPDATE a3 SET bk = 1 WHERE id = 2");
        REQUIRE_FALSE(ex.execute_sql("UPDATE a3 SET id = 7 WHERE id = 1").is_ok());
        REQUIRE(dump(ex, "a3") == "1|NULL,2|1");
        REQUIRE(dump(ex, "b3") == "10|1,20|2");
        ok(ex, "UPDATE a3 SET id = 8 WHERE id = 2");
        REQUIRE(dump(ex, "a3") == "1|NULL,8|1");
        REQUIRE(dump(ex, "b3") == "10|1,20|8");
        // ROLLBACK gives the rows of every table back
        ok(ex, "BEGIN");
        ok(ex, "UPDATE a SET id = 9 WHERE id = 7");
        REQUIRE(dump(ex, "c") == "100|9,200|2,300|9");
        ok(ex, "ROLLBACK");
        REQUIRE(dump(ex, "c") == "100|7,200|2,300|7");
    }
    Executor again(dir.path);
    ok(again, "USE d");
    REQUIRE(dump(again, "b") == "10|7,20|2");
    REQUIRE(dump(again, "c") == "100|7,200|2,300|7");
    REQUIRE(dump(again, "b2") == "10|1,20|8");
}

TEST_CASE("R56: a cascade down the chain waits for a row of a grandchild table that another transaction holds", "[r56][chain][concurrency]") {
    TempDataDir dir("r56_chain_claims");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES a(id) ON DELETE CASCADE)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, q INT, v INT, FOREIGN KEY (q) REFERENCES b(id) ON DELETE CASCADE)");
    ok(ex, "INSERT INTO a VALUES (1)");
    ok(ex, "INSERT INTO b VALUES (10, 1)");
    ok(ex, "INSERT INTO c VALUES (100, 10, 0)");
    ok(ex, "BEGIN");
    ok(ex, "UPDATE c SET v = 1 WHERE id = 100"); // (this transaction holds row 100 of the grandchild table)
    std::atomic<bool> finished{false}, failed{false};
    auto shared = ex.get_shared();
    std::thread deleter([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok() || !s.execute_sql("DELETE FROM a WHERE id = 1").is_ok()) failed = true;
        finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE_FALSE(finished);
    ok(ex, "COMMIT");
    deleter.join();
    REQUIRE_FALSE(failed);
    REQUIRE(dump(ex, "c") == "");
}

TEST_CASE("R56: a cascade waits for a session that has just looked for a parent below the row it deletes, and so does a key that changes", "[r56][chain][concurrency]") {
    TempDataDir dir("r56_value_claims");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, p INT UNIQUE, FOREIGN KEY (p) REFERENCES a(id) ON DELETE CASCADE ON UPDATE CASCADE)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b(p) ON DELETE CASCADE ON UPDATE CASCADE)");
    ok(ex, "INSERT INTO a VALUES (1), (2)");
    ok(ex, "INSERT INTO b VALUES (10, 1), (20, 2)");
    ok(ex, "INSERT INTO c VALUES (100, 1), (200, 2)");
    auto shared = ex.get_shared();
    // a child INSERT that has looked for its parent holds the value of the parent's column shared until its statement is over (here: b.p = 1, and b.id = 10 for a table below b.id)
    auto locks = [&]() -> LockManager& { return const_cast<SharedDatabase&>(*shared->read()).lock_mgr; }; // (the lock manager answers to every session at once)
    auto holds = [&](const std::string& claim, const std::string& value, std::uint64_t who) {
        REQUIRE(locks().acquire_shared(claim, value, who).kind == LockResult::Kind::Granted);
    };
    auto waits_for = [&](const std::string& statement, const std::function<void()>& release) {
        std::atomic<bool> finished{false}, failed{false};
        std::thread runner([&] {
            Executor session = Executor::new_session(shared);
            if (!session.execute_sql("USE d").is_ok() || !session.execute_sql(statement).is_ok()) failed = true;
            finished = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const bool waited = !finished;
        release();
        runner.join();
        REQUIRE_FALSE(failed);
        return waited;
    };
    // the DELETE of a row of a goes down to b (whose column p is what c refers to): the claim on b.p = 1 holds it
    holds("d.b#fk:p", "1", 424242);
    REQUIRE(waits_for("DELETE FROM a WHERE id = 1", [&] { locks().release(424242); }));
    REQUIRE(dump(ex, "b") == "20|2");
    REQUIRE(dump(ex, "c") == "200|2");
    // the same for a key that changes: a.id = 2 becomes 7, b.p follows, and c.q below it
    holds("d.b#fk:p", "2", 424243);
    REQUIRE(waits_for("UPDATE a SET id = 7 WHERE id = 2", [&] { locks().release(424243); }));
    REQUIRE(dump(ex, "b") == "20|7");
    REQUIRE(dump(ex, "c") == "200|7");
}

TEST_CASE("R56: parents are deleted down a chain while grandchildren are written", "[r56][chain][concurrency]") {
    TempDataDir dir("r56_chain_threads");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES a(id) ON DELETE CASCADE)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b(id) ON DELETE CASCADE)");
    ok(ex, multi_insert("a", 1, 400, [](int i) { return std::to_string(i); }));
    ok(ex, multi_insert("b", 1, 400, [](int i) { return std::to_string(i) + "," + std::to_string(i); }));
    auto shared = ex.get_shared();
    std::atomic<bool> failed{false};
    std::atomic<int> deleted{0};
    std::thread deleter([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        for (int id = 400; id >= 1; id -= 2) {
            auto r = s.execute_sql("DELETE FROM a WHERE id = " + std::to_string(id));
            if (r.is_ok()) deleted++;
            else failed = true;
        }
    });
    std::thread writer([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        int next = 10000;
        for (int id = 1; id <= 400; id += 2) {
            auto r = s.execute_sql("INSERT INTO c VALUES (" + std::to_string(next++) + ", " + std::to_string(id) + ")");
            if (r.is_err() && r.error().find("Foreign key violation") == std::string::npos) failed = true; // (the b row may be gone already)
        }
    });
    deleter.join();
    writer.join();
    REQUIRE_FALSE(failed);
    REQUIRE(deleted == 200);
    // no grandchild is left without its child, no child without its parent
    REQUIRE(q(ex, "SELECT COUNT(*) FROM c LEFT JOIN b ON b.id = c.q WHERE b.id IS NULL") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM b LEFT JOIN a ON a.id = b.p WHERE a.id IS NULL") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a WHERE id % 2 = 0") == Rows{{"0"}});
}

namespace {
// the names of the columns an answer prints ("id|name"), "" when it prints none
std::string header_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        std::string names;
        for (const std::string& cell : result_text::split_row(line)) names += (names.empty() ? "" : "|") + cell;
        return names;
    }
    return std::string();
}
} // namespace

TEST_CASE("R56: a SELECT that finds no rows still says its columns", "[r56][empty]") {
    TempDataDir dir("r56_empty");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR(10), v INT)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, note VARCHAR(10), FOREIGN KEY (pid) REFERENCES p(id))");
    ok(ex, "INSERT INTO p VALUES (1, 'a', 10), (2, 'b', 20)");
    ok(ex, "INSERT INTO c VALUES (1, 1, 'x'), (2, 2, 'y')");
    ok(ex, "CREATE VIEW vw AS SELECT id, name FROM p");
    // the answer of no rows is the header with nothing under it (and the line every empty answer ends with)
    REQUIRE(answer(ex, "SELECT * FROM p WHERE id < 0") == "+----+------+---+\n| id | name | v |\n+----+------+---+\n+----+------+---+\n0 rows returned.");
    // ... and it names the columns as the answer of the same query that finds rows does, whatever is in the select list and the FROM
    const std::vector<std::string> queries = {
        "SELECT * FROM p",
        "SELECT id, name FROM p",
        "SELECT name, id FROM p",
        "SELECT id AS ident, name AS label FROM p",
        "SELECT p.id, p.name FROM p",
        "SELECT x.id FROM p x",
        "SELECT p.* FROM p",
        "SELECT p.*, c.note FROM p JOIN c ON c.pid = p.id",
        "SELECT * FROM p JOIN c ON c.pid = p.id",
        "SELECT p.id, c.id FROM p LEFT JOIN c ON c.pid = p.id",
        "SELECT name, COUNT(*) FROM p GROUP BY name",
        "SELECT name, SUM(v) AS total FROM p GROUP BY name",
        "SELECT name, MAX(v), MIN(v), AVG(v) FROM p GROUP BY name",
        "SELECT DISTINCT name FROM p",
        "SELECT id, name FROM p ORDER BY name DESC",
        "SELECT d.id, d.name FROM (SELECT id, name FROM p) d",
        "SELECT * FROM (SELECT id, v AS value FROM p) d",
        "SELECT * FROM vw",
        "SELECT id FROM vw",
        "WITH w AS (SELECT id, name FROM p) SELECT * FROM w",
        "WITH w (a, b) AS (SELECT id, name FROM p) SELECT a, b FROM w",
        "WITH RECURSIVE r (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 3) SELECT n FROM r",
        "SELECT id FROM p UNION SELECT pid FROM c",
        "SELECT id AS first_column FROM p UNION ALL SELECT pid FROM c",
        "SELECT id, (SELECT COUNT(*) FROM c WHERE c.pid = p.id) AS kids FROM p",
        "SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM p",
        "SELECT id, CASE WHEN v > 10 THEN 'big' ELSE 'small' END AS size FROM p",
        "SELECT id, UPPER(name) AS up FROM p",
        "SELECT id, v + 1 AS next FROM p",
        "SELECT v + 1 FROM p",
        "SELECT UPPER(name), LENGTH(name) FROM p",
        "SELECT COUNT(*), SUM(v) FROM p GROUP BY name",
        "SELECT `id`, `name` FROM p",
        "SELECT id * 2, v - 1 FROM p",
        "SELECT CONCAT(name, 'x') FROM p",
        "SELECT ROUND(v / 3, 1) FROM p",
        "SELECT COALESCE(name, 'z') FROM p",
        "SELECT CASE WHEN v > 10 THEN 1 END FROM p",
        "SELECT COUNT(DISTINCT name) FROM p GROUP BY v",
        "SELECT 1, 'a' FROM p",
        "SELECT NOW() FROM p",
        "SELECT x.* FROM p x JOIN p y ON y.id = x.id", // (a table joined with itself: x.* is the columns of one of them)
        "SELECT y.* FROM p x JOIN p y ON y.id = x.id",
        "SELECT x.id, y.name FROM p x LEFT JOIN p y ON y.id <> x.id",
    };
    for (const std::string& query : queries) {
        const std::string with_rows = header_of(ex, query);
        REQUIRE_FALSE(with_rows.empty());
        REQUIRE(header_of(ex, query + " LIMIT 0") == with_rows);
    }
    // a USING join names them too (the merged column once, in front; R57)
    REQUIRE(header_of(ex, "SELECT * FROM p JOIN c USING (id) WHERE p.id < 0") == "id|name|v|pid|note");
    // a SELECT that is prepared and executed says them too
    ok(ex, "PREPARE st FROM 'SELECT id, name FROM p WHERE id < ?'");
    ok(ex, "SET @zero = 0");
    REQUIRE(answer(ex, "EXECUTE st USING @zero") == "+----+------+\n| id | name |\n+----+------+\n+----+------+\n0 rows returned.");
    // SHOW TABLES LIKE names its column with the pattern, as MySQL does
    REQUIRE(header_of(ex, "SHOW TABLES LIKE 'nomatch%'").find("Tables_in_d (nomatch%)") != std::string::npos);
    REQUIRE(header_of(ex, "SHOW TABLES LIKE 'p%'").find("Tables_in_d (p%)") != std::string::npos);
    // the headers are read back as the table of no rows they are: a table written from one has its columns, a derived table too
    REQUIRE(q(ex, "SELECT * FROM (SELECT id, name FROM p WHERE id < 0) e").empty());
    ok(ex, "CREATE TABLE copy AS SELECT id, name FROM p WHERE id < 0");
    REQUIRE(q(ex, "SELECT id, name FROM copy").empty());
}

TEST_CASE("R56: a statement that deletes many parents looks the children up by an index of its own, and by the collation of the key", "[r56][chain]") {
    TempDataDir dir("r56_many");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE a (id VARCHAR(8) PRIMARY KEY)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, p VARCHAR(8), FOREIGN KEY (p) REFERENCES a(id) ON DELETE CASCADE)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b(id) ON DELETE CASCADE)");
    // the children name their parent with the other letter case: k1 is K1, K2 is k2 ...
    ok(ex, multi_insert("a", 1, 40, [](int i) { return std::string(i % 2 ? "'k" : "'K") + std::to_string(i) + "'"; }));
    ok(ex, multi_insert("b", 1, 80, [](int i) {
        const int parent = (i - 1) % 40 + 1;
        return std::to_string(i) + "," + (parent % 2 ? "'K" : "'k") + std::to_string(parent) + "'";
    }));
    ok(ex, multi_insert("c", 1, 160, [](int i) { return std::to_string(i) + "," + std::to_string((i - 1) % 80 + 1); }));
    REQUIRE(answer(ex, "DELETE FROM a WHERE id IN ('k1', 'k2', 'k3', 'k4', 'k5', 'k6', 'k7', 'k8', 'k9', 'k10', 'k11', 'k12', 'k13', 'k14', 'k15', 'k16', 'k17', 'k18', 'k19', 'k20')") ==
            "20 row(s) deleted.");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a") == Rows{{"20"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM b") == Rows{{"40"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM c") == Rows{{"80"}});
    // what is left refers to what is left
    REQUIRE(q(ex, "SELECT COUNT(*) FROM b LEFT JOIN a ON a.id = b.p WHERE a.id IS NULL") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM c LEFT JOIN b ON b.id = c.q WHERE b.id IS NULL") == Rows{{"0"}});
    // every row of the whole chain at once
    REQUIRE(answer(ex, "DELETE FROM a") == "20 row(s) deleted.");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM b") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM c") == Rows{{"0"}});
}

TEST_CASE("R56: keys are changed down a chain while grandchildren are written", "[r56][chain][concurrency]") {
    TempDataDir dir("r56_update_threads");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, p INT UNIQUE, FOREIGN KEY (p) REFERENCES a(id) ON UPDATE CASCADE)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b(p) ON UPDATE CASCADE)");
    ok(ex, multi_insert("a", 1, 300, [](int i) { return std::to_string(i); }));
    ok(ex, multi_insert("b", 1, 300, [](int i) { return std::to_string(i) + "," + std::to_string(i); }));
    auto shared = ex.get_shared();
    std::atomic<bool> failed{false};
    std::thread updater([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        for (int id = 1; id <= 300; id += 2) {
            auto r = s.execute_sql("UPDATE a SET id = " + std::to_string(id + 1000) + " WHERE id = " + std::to_string(id));
            if (r.is_err()) failed = true;
        }
    });
    std::thread writer([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failed = true; return; }
        int next = 10000;
        for (int id = 1; id <= 300; id += 2) {
            // (the key may have moved already: the value is then no parent, and the INSERT is refused)
            auto r = s.execute_sql("INSERT INTO c VALUES (" + std::to_string(next++) + ", " + std::to_string(id) + ")");
            if (r.is_err() && r.error().find("Foreign key violation") == std::string::npos) failed = true;
        }
    });
    updater.join();
    writer.join();
    REQUIRE_FALSE(failed);
    // no row of the chain refers to a key that is no more
    REQUIRE(q(ex, "SELECT COUNT(*) FROM b LEFT JOIN a ON a.id = b.p WHERE a.id IS NULL") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM c LEFT JOIN b ON b.p = c.q WHERE b.p IS NULL") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a WHERE id <= 300 AND id % 2 = 1") == Rows{{"0"}});
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// R57: REPLACE below a chain of foreign keys, FLOAT values that are picked and copied, a column in several foreign keys, the columns of an answer of no rows for a USING / NATURAL join and a
// CALL, a string constant that names its column, a select without FROM that has a WHERE
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
// a <- b <- c: b goes with its parent (CASCADE), c refuses it (RESTRICT): a row of a that has a b row that has a c row is protected two tables down
void protected_chain(Executor& ex, const std::string& s) {
    ok(ex, "CREATE TABLE a" + s + " (id INT PRIMARY KEY, v INT)");
    ok(ex, "CREATE TABLE b" + s + " (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES a" + s + "(id) ON DELETE CASCADE)");
    ok(ex, "CREATE TABLE c" + s + " (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES b" + s + "(id) ON DELETE RESTRICT)");
    ok(ex, "INSERT INTO a" + s + " VALUES (1, 10), (2, 20), (3, 30)");
    ok(ex, "INSERT INTO b" + s + " VALUES (11, 1)");
    ok(ex, "INSERT INTO c" + s + " VALUES (111, 11)");
}
} // namespace

TEST_CASE("R57: REPLACE of a row that a RESTRICT protects two tables down changes nothing, not even the rows it replaced before", "[r57][replace]") {
    TempDataDir dir("r57_replace");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        protected_chain(ex, "1");
        // the protected row is the second one: the first used to be deleted and not inserted again (the statement failed with row 2 of a gone)
        REQUIRE(fails(ex, "REPLACE INTO a1 VALUES (2, 21), (1, 11)").find("Foreign key violation") != std::string::npos);
        REQUIRE(dump(ex, "a1") == "1|10,2|20,3|30");
        REQUIRE(dump(ex, "b1") == "11|1");
        REQUIRE(dump(ex, "c1") == "111|11");
        // the same from a SELECT, with the protected row first or last, and with a row between that nothing protects
        REQUIRE_FALSE(ex.execute_sql("REPLACE INTO a1 SELECT id, v + 1 FROM a1 WHERE id IN (2, 1)").is_ok());
        REQUIRE_FALSE(ex.execute_sql("REPLACE INTO a1 VALUES (1, 11), (2, 21)").is_ok());
        REQUIRE_FALSE(ex.execute_sql("REPLACE INTO a1 VALUES (3, 31), (2, 21), (1, 11)").is_ok());
        REQUIRE(dump(ex, "a1") == "1|10,2|20,3|30");
        // rows nothing protects are replaced as before, and a new row is inserted
        ok(ex, "REPLACE INTO a1 VALUES (2, 21), (3, 31), (4, 40)");
        REQUIRE(dump(ex, "a1") == "1|10,2|21,3|31,4|40");
        // the protection gone, the cascade runs and takes the b row with the replaced row
        ok(ex, "DELETE FROM c1");
        ok(ex, "REPLACE INTO a1 VALUES (2, 22), (1, 12)");
        REQUIRE(dump(ex, "a1") == "1|12,2|22,3|31,4|40");
        REQUIRE(dump(ex, "b1") == "");
        // every table of a chain cascades: the grandchildren go with the row that is replaced
        ok(ex, "CREATE TABLE ra (id INT PRIMARY KEY, v INT)");
        ok(ex, "CREATE TABLE rb (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES ra(id) ON DELETE CASCADE)");
        ok(ex, "CREATE TABLE rc (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES rb(id) ON DELETE CASCADE)");
        ok(ex, "INSERT INTO ra VALUES (1, 10), (2, 20)");
        ok(ex, "INSERT INTO rb VALUES (11, 1), (22, 2)");
        ok(ex, "INSERT INTO rc VALUES (111, 11), (222, 22)");
        ok(ex, "REPLACE INTO ra VALUES (1, 11), (3, 30)");
        REQUIRE(dump(ex, "ra") == "1|11,2|20,3|30");
        REQUIRE(dump(ex, "rb") == "22|2");
        REQUIRE(dump(ex, "rc") == "222|22");
        // a protected row in the middle of a chain that nulls: the SET NULL above the RESTRICT is not made either
        ok(ex, "CREATE TABLE sa (id INT PRIMARY KEY, v INT)");
        ok(ex, "CREATE TABLE sb (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES sa(id) ON DELETE SET NULL)");
        ok(ex, "CREATE TABLE sc (id INT PRIMARY KEY, q INT, FOREIGN KEY (q) REFERENCES sb(id) ON DELETE RESTRICT)");
        ok(ex, "INSERT INTO sa VALUES (1, 10), (2, 20)");
        ok(ex, "INSERT INTO sb VALUES (11, 1), (22, 2)");
        ok(ex, "INSERT INTO sc VALUES (111, 11)");
        ok(ex, "REPLACE INTO sa VALUES (2, 21), (1, 11)"); // (SET NULL rewrites sb; it does not delete it, so sc does not refuse)
        REQUIRE(dump(ex, "sa") == "1|11,2|21");
        REQUIRE(dump(ex, "sb") == "11|NULL,22|NULL");
        // a table that is its own child
        ok(ex, "CREATE TABLE rf (id INT PRIMARY KEY, up INT, FOREIGN KEY (up) REFERENCES rf(id) ON DELETE RESTRICT)");
        ok(ex, "INSERT INTO rf VALUES (1, NULL), (2, 1), (3, NULL)");
        REQUIRE_FALSE(ex.execute_sql("REPLACE INTO rf VALUES (3, NULL), (1, NULL)").is_ok());
        REQUIRE(dump(ex, "rf") == "1|NULL,2|1,3|NULL");
    } // (nothing of a refused statement is in the log either: a restart finds the same rows)
    Executor again(dir.path);
    ok(again, "USE d");
    REQUIRE(dump(again, "a1") == "1|12,2|22,3|31,4|40");
    REQUIRE(dump(again, "rf") == "1|NULL,2|1,3|NULL");
}

TEST_CASE("R57: a MERGE whose DELETE a RESTRICT two tables down refuses does not make the UPDATE of the same statement", "[r57][replace]") {
    TempDataDir dir("r57_merge");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    protected_chain(ex, "1");
    ok(ex, "CREATE TABLE src (id INT, act VARCHAR(5), nv INT)");
    ok(ex, "INSERT INTO src VALUES (2, 'upd', 99), (1, 'del', 0)");
    REQUIRE_FALSE(ex.execute_sql("MERGE INTO a1 t USING src s ON t.id = s.id WHEN MATCHED AND s.act = 'del' THEN DELETE WHEN MATCHED THEN UPDATE SET v = s.nv").is_ok());
    REQUIRE(dump(ex, "a1") == "1|10,2|20,3|30");
    REQUIRE(dump(ex, "b1") == "11|1");
    // without the protected row the same statement goes through
    ok(ex, "UPDATE src SET act = 'upd' WHERE id = 1");
    ok(ex, "MERGE INTO a1 t USING src s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.nv");
    REQUIRE(dump(ex, "a1") == "1|0,2|99,3|30");
}

TEST_CASE("R57: a FLOAT is shown with six digits when MIN / MAX / GREATEST / LEAST / COALESCE / IFNULL / NULLIF give it, and is copied as the number it holds", "[r57][float]") {
    TempDataDir dir("r57_float");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fc (id INT PRIMARY KEY, f FLOAT, d DOUBLE, i INT, dc DECIMAL(20,4), t VARCHAR(40))");
    ok(ex, "INSERT INTO fc VALUES (1, 1234567, 1, 1, 1, 'a'), (2, 16777217, 2, 2, 2, 'b'), (3, 0.1, 3, 3, 3, 'c'), (4, NULL, 4, 4, 4, 'd'), (5, 123456789, 5, 5, 5, 'e')");
    // what a FLOAT column shows is what MIN / MAX of it show
    REQUIRE(q(ex, "SELECT f FROM fc ORDER BY id") == (Rows{{"1234570"}, {"16777200"}, {"0.1"}, {"NULL"}, {"123457000"}}));
    REQUIRE(q(ex, "SELECT MIN(f), MAX(f) FROM fc") == (Rows{{"0.1", "123457000"}}));
    REQUIRE(q(ex, "SELECT MAX(f) AS m FROM fc WHERE id = 2") == (Rows{{"16777200"}}));
    REQUIRE(q(ex, "SELECT id, MAX(f) FROM fc GROUP BY id ORDER BY id") == (Rows{{"1", "1234570"}, {"2", "16777200"}, {"3", "0.1"}, {"4", "NULL"}, {"5", "123457000"}}));
    // ... and a function that picks one of its arguments: FLOAT columns, whole-number constants and NULL give a FLOAT
    REQUIRE(q(ex, "SELECT GREATEST(f, 0), LEAST(f, 100000000) FROM fc ORDER BY id") ==
            (Rows{{"1234570", "1234570"}, {"16777200", "16777200"}, {"0.1", "0.1"}, {"NULL", "NULL"}, {"123457000", "100000000"}}));
    REQUIRE(q(ex, "SELECT COALESCE(f, 0), IFNULL(f, 0), NULLIF(f, 0) FROM fc ORDER BY id") ==
            (Rows{{"1234570", "1234570", "1234570"}, {"16777200", "16777200", "16777200"}, {"0.1", "0.1", "0.1"}, {"0", "0", "NULL"}, {"123457000", "123457000", "123457000"}}));
    REQUIRE(q(ex, "SELECT COALESCE(f, NULL) FROM fc WHERE id = 2") == (Rows{{"16777200"}}));
    // the number is still the whole float in arithmetic and in a comparison (the six digits are how it is shown)
    REQUIRE(q(ex, "SELECT GREATEST(f, 0) + 1 FROM fc WHERE id = 1") == (Rows{{"1234568"}}));
    REQUIRE(q(ex, "SELECT id FROM fc WHERE GREATEST(f, 0) = 16777216") == (Rows{{"2"}}));
    REQUIRE(q(ex, "SELECT id FROM fc GROUP BY id HAVING MAX(f) = 16777216") == (Rows{{"2"}}));
    // a column of another type among the arguments gives a DOUBLE, which is not cut to six digits
    REQUIRE(q(ex, "SELECT COALESCE(f, i) FROM fc WHERE id = 2") == (Rows{{"16777216"}}));
    // copied into a column that is not a FLOAT it is the number the float holds, widened: 0.1 is 0.10000000149011612, 16777217 is 16777216
    ok(ex, "CREATE TABLE fd (id INT PRIMARY KEY, x DOUBLE)");
    ok(ex, "CREATE TABLE ff (id INT PRIMARY KEY, x FLOAT)");
    ok(ex, "CREATE TABLE fn (id INT PRIMARY KEY, x DECIMAL(20,4))");
    ok(ex, "CREATE TABLE fi (id INT PRIMARY KEY, x INT)");
    ok(ex, "CREATE TABLE ft (id INT PRIMARY KEY, x VARCHAR(40))");
    ok(ex, "INSERT INTO fd SELECT id, f FROM fc");
    ok(ex, "INSERT INTO ff SELECT id, f FROM fc");
    ok(ex, "INSERT INTO fn SELECT id, f FROM fc");
    ok(ex, "INSERT INTO fi SELECT id, f FROM fc");
    ok(ex, "INSERT INTO ft SELECT id, f FROM fc");
    REQUIRE(dump(ex, "fd") == "1|1234567,2|16777216,3|0.10000000149011612,4|NULL,5|123456792");
    REQUIRE(dump(ex, "ff") == "1|1234570,2|16777200,3|0.1,4|NULL,5|123457000");
    REQUIRE(dump(ex, "fn") == "1|1234567.0000,2|16777216.0000,3|0.1000,4|NULL,5|123456792.0000");
    REQUIRE(dump(ex, "fi") == "1|1234567,2|16777216,3|0,4|NULL,5|123456792");
    REQUIRE(dump(ex, "ft") == "1|1234567,2|16777216,3|0.10000000149011612,4|NULL,5|123456792");
    // ... from an aggregate or a function that is a FLOAT too
    ok(ex, "CREATE TABLE fm (x DOUBLE)");
    ok(ex, "INSERT INTO fm SELECT MAX(f) FROM fc");
    ok(ex, "INSERT INTO fm SELECT MIN(f) FROM fc");
    ok(ex, "INSERT INTO fm SELECT COALESCE(f, 0) FROM fc WHERE id IN (3, 4)");
    REQUIRE(q(ex, "SELECT x FROM fm ORDER BY x") == (Rows{{"0"}, {"0.10000000149011612"}, {"0.10000000149011612"}, {"123456792"}}));
    // UPDATE ... SET d = f copies it the same way
    ok(ex, "UPDATE fc SET d = f");
    REQUIRE(q(ex, "SELECT id, d FROM fc ORDER BY id") == (Rows{{"1", "1234567"}, {"2", "16777216"}, {"3", "0.10000000149011612"}, {"4", "NULL"}, {"5", "123456792"}}));
    ok(ex, "UPDATE fc SET d = 0");
    ok(ex, "UPDATE fc SET d = COALESCE(f, 0)");
    REQUIRE(q(ex, "SELECT id, d FROM fc ORDER BY id") == (Rows{{"1", "1234567"}, {"2", "16777216"}, {"3", "0.10000000149011612"}, {"4", "0"}, {"5", "123456792"}}));
    ok(ex, "UPDATE fc SET t = f");
    REQUIRE(q(ex, "SELECT id, t FROM fc ORDER BY id") == (Rows{{"1", "1234567"}, {"2", "16777216"}, {"3", "0.10000000149011612"}, {"4", "NULL"}, {"5", "123456792"}}));
    // a DOUBLE into a DOUBLE is not touched, a FLOAT into a FLOAT stays the float
    ok(ex, "UPDATE fc SET dc = d");
    ok(ex, "UPDATE fc SET f = f");
    REQUIRE(q(ex, "SELECT f FROM fc WHERE id = 3") == (Rows{{"0.1"}}));
}

TEST_CASE("R57: a column in several foreign keys is checked by each of them, and each acts on its own", "[r57][foreign_key]") {
    TempDataDir dir("r57_multi_fk");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE pa (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE pb (id INT PRIMARY KEY)");
        ok(ex, "INSERT INTO pa VALUES (1), (2), (5)");
        ok(ex, "INSERT INTO pb VALUES (2), (3), (5)");
        ok(ex, "CREATE TABLE ch (id INT PRIMARY KEY, x INT, FOREIGN KEY (x) REFERENCES pa(id), FOREIGN KEY (x) REFERENCES pb(id))");
        // a value has to be in every parent (a value that is in one of them only is refused, naming the parent that lacks it)
        REQUIRE(fails(ex, "INSERT INTO ch VALUES (1, 1)").find("pb'.'id'") != std::string::npos);
        REQUIRE(fails(ex, "INSERT INTO ch VALUES (2, 3)").find("pa'.'id'") != std::string::npos);
        REQUIRE(fails(ex, "INSERT INTO ch VALUES (3, 9)").find("Foreign key violation") != std::string::npos);
        ok(ex, "INSERT INTO ch VALUES (4, 2), (5, NULL), (6, 5)");
        REQUIRE(fails(ex, "UPDATE ch SET x = 1 WHERE id = 4").find("pb'.'id'") != std::string::npos);
        REQUIRE(fails(ex, "UPDATE ch SET x = 3 WHERE id = 4").find("pa'.'id'") != std::string::npos);
        ok(ex, "UPDATE ch SET x = 5 WHERE id = 4");
        REQUIRE(dump(ex, "ch") == "4|5,5|NULL,6|5");
        // a parent row that a child refers to cannot go, whichever of the two tables it is in
        REQUIRE_FALSE(ex.execute_sql("DELETE FROM pa WHERE id = 5").is_ok());
        REQUIRE_FALSE(ex.execute_sql("DELETE FROM pb WHERE id = 5").is_ok());
        ok(ex, "DELETE FROM pa WHERE id = 1");
        // both keys are listed, each under its own name, in the order they were declared
        REQUIRE(q(ex, "SELECT CONSTRAINT_NAME, REFERENCED_TABLE_NAME FROM information_schema.referential_constraints WHERE TABLE_NAME = 'ch' ORDER BY CONSTRAINT_NAME") ==
                (Rows{{"ch_ibfk_1", "pa"}, {"ch_ibfk_2", "pb"}}));
        REQUIRE(q(ex, "SELECT CONSTRAINT_NAME, COLUMN_NAME FROM information_schema.key_column_usage WHERE TABLE_NAME = 'ch' AND REFERENCED_TABLE_NAME IS NOT NULL ORDER BY CONSTRAINT_NAME") ==
                (Rows{{"ch_ibfk_1", "x"}, {"ch_ibfk_2", "x"}}));
        const std::string shown = answer(ex, "SHOW CREATE TABLE ch");
        REQUIRE(shown.find("REFERENCES `pa`") != std::string::npos);
        REQUIRE(shown.find("REFERENCES `pb`") != std::string::npos);
        // the keys are in the catalog after a restart (below), and a table that either refers to cannot be dropped
        REQUIRE(fails(ex, "DROP TABLE pb").find("referenced by a foreign key constraint") != std::string::npos);
        // each key acts on its own: one cascades the delete, the other nulls the column
        ok(ex, "CREATE TABLE qa (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE qb (id INT PRIMARY KEY)");
        ok(ex, "INSERT INTO qa VALUES (1), (2), (3)");
        ok(ex, "INSERT INTO qb VALUES (1), (2), (3)");
        ok(ex, "CREATE TABLE qc (id INT PRIMARY KEY, x INT, FOREIGN KEY (x) REFERENCES qa(id) ON DELETE CASCADE, FOREIGN KEY (x) REFERENCES qb(id) ON DELETE SET NULL)");
        ok(ex, "INSERT INTO qc VALUES (10, 1), (20, 2), (30, 3)");
        ok(ex, "DELETE FROM qb WHERE id = 1");
        REQUIRE(dump(ex, "qc") == "10|NULL,20|2,30|3");
        ok(ex, "DELETE FROM qa WHERE id = 2");
        REQUIRE(dump(ex, "qc") == "10|NULL,30|3");
        // the second key added by ALTER TABLE, with a name of its own; a name is used once; either key is dropped by its name
        ok(ex, "CREATE TABLE ra (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE rb (id INT PRIMARY KEY)");
        ok(ex, "INSERT INTO ra VALUES (1), (2)");
        ok(ex, "INSERT INTO rb VALUES (1), (2)");
        ok(ex, "CREATE TABLE rc (id INT PRIMARY KEY, x INT, CONSTRAINT first_key FOREIGN KEY (x) REFERENCES ra(id))");
        ok(ex, "ALTER TABLE rc ADD CONSTRAINT second_key FOREIGN KEY (x) REFERENCES rb(id)");
        REQUIRE(fails(ex, "ALTER TABLE rc ADD CONSTRAINT second_key FOREIGN KEY (x) REFERENCES rb(id)").find("Duplicate foreign key constraint name") != std::string::npos);
        REQUIRE_FALSE(ex.execute_sql("INSERT INTO rc VALUES (1, 3)").is_ok());
        ok(ex, "INSERT INTO rc VALUES (1, 1)");
        ok(ex, "ALTER TABLE rc DROP FOREIGN KEY first_key");
        REQUIRE(q(ex, "SELECT CONSTRAINT_NAME FROM information_schema.referential_constraints WHERE TABLE_NAME = 'rc'") == (Rows{{"second_key"}}));
        REQUIRE_FALSE(ex.execute_sql("INSERT INTO rc VALUES (2, 3)").is_ok()); // (3 is in neither: the second key is still there)
        ok(ex, "INSERT INTO rc VALUES (2, 2)");
        REQUIRE(fails(ex, "ALTER TABLE rc DROP FOREIGN KEY first_key").find("not found") != std::string::npos);
        // ON UPDATE CASCADE of one parent's key into a column that another key guards too: the statement is refused (MySQL 1452) unless the other parent has the new value as well
        ok(ex, "CREATE TABLE ua (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE ub (id INT PRIMARY KEY)");
        ok(ex, "INSERT INTO ua VALUES (1), (2)");
        ok(ex, "INSERT INTO ub VALUES (1), (7)");
        ok(ex, "CREATE TABLE uc (id INT PRIMARY KEY, x INT, FOREIGN KEY (x) REFERENCES ua(id) ON UPDATE CASCADE, FOREIGN KEY (x) REFERENCES ub(id) ON UPDATE CASCADE)");
        ok(ex, "INSERT INTO uc VALUES (10, 1)");
        REQUIRE(fails(ex, "UPDATE ua SET id = 5 WHERE id = 1").find("ub'.'id'") != std::string::npos); // (5 is not in ub)
        REQUIRE(dump(ex, "ua") == "1,2");
        REQUIRE(dump(ex, "uc") == "10|1");
        ok(ex, "UPDATE ua SET id = 7 WHERE id = 1"); // (7 is in ub: the child follows)
        REQUIRE(dump(ex, "ua") == "2,7");
        REQUIRE(dump(ex, "uc") == "10|7");
        REQUIRE(fails(ex, "UPDATE ub SET id = 9 WHERE id = 7").find("ua'.'id'") != std::string::npos); // (the same from the other parent: 9 is not in ua)
        REQUIRE(dump(ex, "ub") == "1,7");
        REQUIRE(dump(ex, "uc") == "10|7");
    }
    // ... both keys are read back from the catalog of a data directory that is opened again
    Executor again(dir.path);
    ok(again, "USE d");
    REQUIRE(fails(again, "INSERT INTO ch VALUES (7, 3)").find("pa'.'id'") != std::string::npos);
    REQUIRE(fails(again, "INSERT INTO ch VALUES (7, 1)").find("Foreign key violation") != std::string::npos);
    REQUIRE_FALSE(again.execute_sql("DELETE FROM pb WHERE id = 5").is_ok());
    ok(again, "DELETE FROM qb WHERE id = 3");
    REQUIRE(dump(again, "qc") == "10|NULL,30|NULL");
}

TEST_CASE("R57: an answer of no rows names its columns for a USING / NATURAL join and for a CALL, as the answer with rows does", "[r57][empty]") {
    TempDataDir dir("r57_empty");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR(10), v INT)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, note VARCHAR(10))");
    ok(ex, "CREATE TABLE n (id INT PRIMARY KEY, name VARCHAR(10), z INT)");
    ok(ex, "INSERT INTO p VALUES (1, 'a', 10), (2, 'b', 20)");
    ok(ex, "INSERT INTO c VALUES (1, 1, 'x'), (2, 2, 'y')");
    ok(ex, "INSERT INTO n VALUES (1, 'a', 5)");
    // the merged columns come first (once), then what is left of each table in turn
    REQUIRE(header_of(ex, "SELECT * FROM p JOIN c USING (id)") == "id|name|v|pid|note");
    REQUIRE(header_of(ex, "SELECT * FROM p JOIN c USING (id) WHERE id < 0") == "id|name|v|pid|note");
    REQUIRE(header_of(ex, "SELECT * FROM p NATURAL JOIN n WHERE id < 0") == "id|name|v|z");
    REQUIRE(header_of(ex, "SELECT * FROM p JOIN n USING (name, id) WHERE id < 0") == header_of(ex, "SELECT * FROM p JOIN n USING (name, id)"));
    const std::vector<std::string> joins = {
        "SELECT * FROM p JOIN c USING (id)",
        "SELECT * FROM p LEFT JOIN c USING (id)",
        "SELECT * FROM p RIGHT JOIN c USING (id)",
        "SELECT * FROM p NATURAL JOIN c",
        "SELECT * FROM p NATURAL JOIN n",
        "SELECT * FROM p JOIN n USING (id, name)",
        "SELECT * FROM p JOIN c USING (id) JOIN n USING (id)",
        "SELECT id, name FROM p JOIN n USING (id, name)",
        "SELECT p.*, c.note FROM p JOIN c USING (id)",
        "SELECT * FROM p x NATURAL JOIN n y",
    };
    for (const std::string& query : joins) {
        const std::string with_rows = header_of(ex, query);
        REQUIRE_FALSE(with_rows.empty());
        REQUIRE(header_of(ex, query + " LIMIT 0") == with_rows);
    }
    // `a RIGHT JOIN b USING (c)` shows what is left of b before what is left of a (MySQL), with rows or without
    REQUIRE(header_of(ex, "SELECT * FROM p RIGHT JOIN c USING (id)") == "id|pid|note|name|v");
    REQUIRE(header_of(ex, "SELECT * FROM p RIGHT JOIN c USING (id) WHERE id < 0") == "id|pid|note|name|v");
    REQUIRE(q(ex, "SELECT * FROM p RIGHT JOIN c USING (id) ORDER BY id") == (Rows{{"1", "1", "x", "a", "10"}, {"2", "2", "y", "b", "20"}}));
    REQUIRE(q(ex, "SELECT * FROM p LEFT JOIN c USING (id) ORDER BY id") == (Rows{{"1", "a", "10", "1", "x"}, {"2", "b", "20", "2", "y"}}));
    // a UNION whose left side finds no rows is the rows of its right side, under the names of the left (it was a blank row for each of them)
    REQUIRE(q(ex, "SELECT id FROM p WHERE id < 0 UNION SELECT 2") == (Rows{{"2"}}));
    REQUIRE(header_of(ex, "SELECT id FROM p WHERE id < 0 UNION SELECT 2") == "id");
    REQUIRE(q(ex, "SELECT id FROM p WHERE id < 0 UNION ALL SELECT id FROM p ORDER BY id") == (Rows{{"1"}, {"2"}}));
    REQUIRE(q(ex, "SELECT name FROM p WHERE id < 0 UNION SELECT 'z'") == (Rows{{"z"}}));
    REQUIRE(header_of(ex, "SELECT name FROM p WHERE id < 0 UNION SELECT 'z'") == "name");
    REQUIRE(q(ex, "SELECT 1 AS one WHERE 1 = 0 UNION SELECT 2") == (Rows{{"2"}}));
    REQUIRE(header_of(ex, "SELECT 1 AS one WHERE 1 = 0 UNION SELECT 2") == "one");
    REQUIRE(q(ex, "SELECT id FROM p WHERE id < 0 UNION SELECT id FROM p WHERE id < 0").empty());
    REQUIRE(q(ex, "SELECT id FROM p WHERE id < 0 EXCEPT SELECT 2").empty());
    // a CALL that answers with a query that finds no rows says its columns (the last result table it gives; one that has rows keeps its place)
    ok(ex, "CREATE PROCEDURE pe() BEGIN SELECT id, name FROM p WHERE id < 0; END");
    ok(ex, "CREATE PROCEDURE ps() BEGIN SELECT * FROM p WHERE id < 0; END");
    ok(ex, "CREATE PROCEDURE pj() BEGIN SELECT * FROM p JOIN c USING (id) WHERE id < 0; END");
    ok(ex, "CREATE PROCEDURE pt() BEGIN SELECT id FROM p; SELECT name AS label FROM p WHERE id < 0; END");
    REQUIRE(answer(ex, "CALL pe()") == "+----+------+\n| id | name |\n+----+------+\n+----+------+\n0 rows returned.");
    REQUIRE(header_of(ex, "CALL ps()") == "id|name|v");
    REQUIRE(header_of(ex, "CALL pj()") == "id|name|v|pid|note");
    REQUIRE(q(ex, "CALL pt()") == (Rows{{"1"}, {"2"}}));
    // a query that finds no rows is a result table of the CALL like any other, also when statements that write go on after it (the first result set is what a client reads)
    ok(ex, "CREATE PROCEDURE pw() BEGIN SELECT id FROM p WHERE id < 0; UPDATE p SET v = v + 1 WHERE id = 99; END");
    REQUIRE(header_of(ex, "CALL pw()") == "id");
    // a procedure with no query answers its message
    ok(ex, "CREATE PROCEDURE pm() BEGIN UPDATE p SET v = v + 1 WHERE id = 99; END");
    REQUIRE(answer(ex, "CALL pm()").find("+") == std::string::npos);
}

TEST_CASE("R57: a string constant names its column by what it says, and a select without FROM has a WHERE", "[r57][empty]") {
    TempDataDir dir("r57_constant");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR(10))");
    ok(ex, "INSERT INTO p VALUES (1, 'a'), (2, 'b')");
    REQUIRE(header_of(ex, "SELECT 'a'") == "a");
    REQUIRE(header_of(ex, "SELECT 'a b'") == "a b");
    REQUIRE(header_of(ex, "SELECT 'a', 'b'") == "a|b");
    REQUIRE(header_of(ex, "SELECT 'it''s'") == "it's");
    REQUIRE(header_of(ex, "SELECT 'a' AS x") == "x");
    REQUIRE(header_of(ex, "SELECT 'a' FROM p") == "a");
    REQUIRE(header_of(ex, "SELECT 'a' FROM p WHERE id < 0") == "a");
    REQUIRE(header_of(ex, "SELECT id, 'x' FROM p") == "id|x");
    REQUIRE(header_of(ex, "SELECT 'a' + 1") == "'a' + 1"); // (an expression is called by what was written)
    REQUIRE(header_of(ex, "SELECT UPPER('a')") == "UPPER('a')");
    REQUIRE(header_of(ex, "SELECT 1") == "1");
    REQUIRE(q(ex, "SELECT 'a'") == (Rows{{"a"}}));
    REQUIRE(q(ex, "SELECT x FROM (SELECT 'k' AS x) d") == (Rows{{"k"}}));
    ok(ex, "CREATE TABLE made AS SELECT 'a', 'b' FROM p LIMIT 1");
    REQUIRE(q(ex, "SELECT a, b FROM made") == (Rows{{"a", "b"}}));
    // WHERE on a select with no table: the one row is there or the answer has none (and says its columns)
    REQUIRE(q(ex, "SELECT 1 WHERE 1 = 1") == (Rows{{"1"}}));
    REQUIRE(q(ex, "SELECT 1 WHERE 1 = 0").empty());
    REQUIRE(answer(ex, "SELECT 1 WHERE 1 = 0") == "+---+\n| 1 |\n+---+\n+---+\n0 rows returned.");
    REQUIRE(answer(ex, "SELECT 'a', 2 AS b WHERE 1 = 0") == "+---+---+\n| a | b |\n+---+---+\n+---+---+\n0 rows returned.");
    REQUIRE(q(ex, "SELECT 1, 'b' WHERE 2 > 1") == (Rows{{"1", "b"}}));
    REQUIRE(q(ex, "SELECT 1 WHERE NULL").empty());
    REQUIRE(q(ex, "SELECT 1 WHERE EXISTS (SELECT 1 FROM p WHERE id = 1)") == (Rows{{"1"}}));
    REQUIRE(q(ex, "SELECT 1 WHERE EXISTS (SELECT 1 FROM p WHERE id < 0)").empty());
    REQUIRE(q(ex, "SELECT 1 WHERE 1 IN (SELECT id FROM p)") == (Rows{{"1"}}));
    REQUIRE(q(ex, "SELECT 1 WHERE 5 IN (SELECT id FROM p)").empty());
    REQUIRE(q(ex, "SELECT 1 WHERE 1 = 0 UNION ALL SELECT 2") == (Rows{{"2"}}));
    REQUIRE(q(ex, "SELECT 2 UNION ALL SELECT 1 WHERE 1 = 0") == (Rows{{"2"}}));
    ok(ex, "SET @x = 5");
    REQUIRE(q(ex, "SELECT @x WHERE @x > 3") == (Rows{{"5"}}));
    REQUIRE(q(ex, "SELECT @x WHERE @x > 30").empty());
    // INSERT ... SELECT with a WHERE and no FROM
    ok(ex, "INSERT INTO p SELECT 3, 'c' WHERE 1 = 1");
    ok(ex, "INSERT INTO p SELECT 4, 'd' WHERE 1 = 0");
    REQUIRE(q(ex, "SELECT id FROM p ORDER BY id") == (Rows{{"1"}, {"2"}, {"3"}}));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// R58: a FLOAT that meets a column of another type in a function, arithmetic over a function that is a FLOAT, windows over a FLOAT, the columns of * over a chain of USING joins, the names a foreign
// key is given and the rules it reports, a NOT NULL column that an INSERT leaves out
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("R58: a FLOAT met by a DOUBLE, an integer column, a DECIMAL or a fraction in COALESCE / IFNULL / GREATEST / LEAST is the number it holds, widened", "[r58][float]") {
    TempDataDir dir("r58_float_mix");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fm (id INT PRIMARY KEY, f FLOAT, d DOUBLE, i INT, n DECIMAL(10,2))");
    ok(ex, "INSERT INTO fm VALUES (1, 0.1, 0.1, 3, 0.30), (2, 1234567, 2, 1, 1.50), (3, 16777217, 0.1, 0, 2.00), (4, NULL, 5, 4, 2.50), (5, 3.14159265, 3.14159265, 3, 3.14), (6, -2.5, 1, -1, -2.50)");
    // a column of another type makes the value a DOUBLE: the float is 0.10000000149011612, not the 0.1 it is written with
    REQUIRE(q(ex, "SELECT COALESCE(f, d) FROM fm WHERE id = 1") == (Rows{{"0.10000000149011612"}}));
    REQUIRE(q(ex, "SELECT IFNULL(f, d) FROM fm WHERE id = 1") == (Rows{{"0.10000000149011612"}}));
    REQUIRE(q(ex, "SELECT COALESCE(f, i) FROM fm WHERE id = 1") == (Rows{{"0.10000000149011612"}}));
    REQUIRE(q(ex, "SELECT LEAST(f, i) FROM fm WHERE id = 1") == (Rows{{"0.10000000149011612"}}));
    REQUIRE(q(ex, "SELECT GREATEST(f, i) FROM fm WHERE id = 5") == (Rows{{"3.1415927410125732"}}));
    REQUIRE(q(ex, "SELECT GREATEST(f, n) FROM fm WHERE id = 5") == (Rows{{"3.1415927410125732"}}));
    REQUIRE(q(ex, "SELECT GREATEST(f, 0.5) FROM fm WHERE id = 5") == (Rows{{"3.1415927410125732"}}));
    REQUIRE(q(ex, "SELECT COALESCE(f, 0.5) FROM fm WHERE id = 1") == (Rows{{"0.10000000149011612"}}));
    // ... and one inside another function that gives a FLOAT
    REQUIRE(q(ex, "SELECT GREATEST(COALESCE(f, 0), d) FROM fm WHERE id = 1") == (Rows{{"0.10000000149011612"}}));
    // FLOAT columns, whole numbers and NULL alone give a FLOAT: six digits, as before
    REQUIRE(q(ex, "SELECT COALESCE(f, 0), GREATEST(f, 1) FROM fm WHERE id = 5") == (Rows{{"3.14159", "3.14159"}}));
    // NULLIF is of the type of its first argument and compares the float widened: 0.1 (a float) is not 0.1 (a double)
    REQUIRE(q(ex, "SELECT NULLIF(f, d) FROM fm WHERE id = 1") == (Rows{{"0.1"}}));
    REQUIRE(q(ex, "SELECT NULLIF(f, d) FROM fm WHERE id = 5") == (Rows{{"3.14159"}}));
    REQUIRE(q(ex, "SELECT NULLIF(f, 0.5) FROM fm WHERE id = 5") == (Rows{{"3.14159"}}));
    REQUIRE(q(ex, "SELECT id FROM fm WHERE NULLIF(f, 0.1) IS NULL ORDER BY id") == (Rows{{"4"}}));
    // ... what it gives is the first argument as it is (not the widened number), and a float in the second place is compared widened too
    REQUIRE(q(ex, "SELECT CONCAT(NULLIF(f, d), '') FROM fm WHERE id = 1") == (Rows{{"0.1"}}));
    REQUIRE(q(ex, "SELECT NULLIF(d, f) FROM fm WHERE id = 1") == (Rows{{"0.1"}}));
    REQUIRE(q(ex, "SELECT NULLIF(d, f) FROM fm WHERE id = 5") == (Rows{{"3.14159265"}}));
    // a NULL stays NULL
    REQUIRE(q(ex, "SELECT COALESCE(f, d), GREATEST(f, i), NULLIF(f, d) FROM fm WHERE id = 4") == (Rows{{"5", "NULL", "NULL"}}));
}

TEST_CASE("R58: a function that is a FLOAT is the single-precision number in arithmetic and in a comparison, as the column is", "[r58][float]") {
    TempDataDir dir("r58_float_func");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fm (id INT PRIMARY KEY, f FLOAT, d DOUBLE)");
    ok(ex, "INSERT INTO fm VALUES (1, 0.1, 0.1), (2, 1234567, 2), (3, 16777217, 0.1), (4, NULL, 5), (5, 3.14159265, 3.14159265), (6, -2.5, 1), (7, 0.5, 0.5)");
    REQUIRE(q(ex, "SELECT GREATEST(f, 0) + 1 FROM fm WHERE id = 1") == (Rows{{"1.1000000014901161"}}));
    REQUIRE(q(ex, "SELECT COALESCE(f, 0) * 2 FROM fm WHERE id = 1") == (Rows{{"0.20000000298023224"}}));
    REQUIRE(q(ex, "SELECT IFNULL(f, 0) - 0.5 FROM fm WHERE id = 1") == (Rows{{"-0.3999999985098839"}}));
    REQUIRE(q(ex, "SELECT NULLIF(f, 0) + 1 FROM fm WHERE id = 1") == (Rows{{"1.1000000014901161"}}));
    REQUIRE(q(ex, "SELECT LEAST(f, 100) / 3 FROM fm WHERE id = 1") == (Rows{{"0.033333333830038704"}}));
    // a float that holds exactly what is written is not changed by it
    REQUIRE(q(ex, "SELECT GREATEST(f, 0) + 1 FROM fm WHERE id = 2") == (Rows{{"1234568"}}));
    REQUIRE(q(ex, "SELECT COALESCE(f, 0) * 2 FROM fm WHERE id = 6") == (Rows{{"-5"}}));
    // comparisons read the float widened: GREATEST(f, 0) is not 0.1, it is f
    REQUIRE(q(ex, "SELECT id FROM fm WHERE GREATEST(f, 0) = 0.1").empty());
    REQUIRE(q(ex, "SELECT id FROM fm WHERE GREATEST(f, 0) = f ORDER BY id") == (Rows{{"1"}, {"2"}, {"3"}, {"5"}, {"7"}}));
    REQUIRE(q(ex, "SELECT id FROM fm WHERE COALESCE(f, 0) > 0.1 ORDER BY id") == (Rows{{"1"}, {"2"}, {"3"}, {"5"}, {"7"}}));
    REQUIRE(q(ex, "SELECT id FROM fm WHERE 0.1 < COALESCE(f, 0) ORDER BY id") == (Rows{{"1"}, {"2"}, {"3"}, {"5"}, {"7"}}));
    REQUIRE(q(ex, "SELECT id FROM fm WHERE GREATEST(f, 0) BETWEEN 0.1 AND 0.2 ORDER BY id") == (Rows{{"1"}}));
    REQUIRE(q(ex, "SELECT id FROM fm WHERE COALESCE(f, 0) IN (0.1, 1234567) ORDER BY id") == (Rows{{"2"}}));
    // a FLOAT column and a DOUBLE column compare as numbers: 0.1 (a float) is not 0.1 (a double), 0.5 is the same in both
    REQUIRE(q(ex, "SELECT id FROM fm WHERE f = d ORDER BY id") == (Rows{{"7"}}));
}

TEST_CASE("R58: MIN / MAX / LAG / LEAD / FIRST_VALUE / LAST_VALUE / NTH_VALUE of a FLOAT column over a window are FLOATs, shown with six digits", "[r58][float][window]") {
    TempDataDir dir("r58_float_window");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fw (id INT PRIMARY KEY, f FLOAT, g INT, d DOUBLE)");
    ok(ex, "INSERT INTO fw VALUES (1, 0.1, 1, 0.1), (2, 1234567, 1, 1234567), (3, 16777217, 2, 16777217), (4, 3.14159265, 2, 3.14159265)");
    REQUIRE(q(ex, "SELECT MAX(f) OVER () FROM fw WHERE id = 1") == (Rows{{"0.1"}}));
    REQUIRE(q(ex, "SELECT id, MAX(f) OVER (ORDER BY id) FROM fw ORDER BY id") == (Rows{{"1", "0.1"}, {"2", "1234570"}, {"3", "16777200"}, {"4", "16777200"}}));
    REQUIRE(q(ex, "SELECT id, MIN(f) OVER (ORDER BY id) FROM fw ORDER BY id") == (Rows{{"1", "0.1"}, {"2", "0.1"}, {"3", "0.1"}, {"4", "0.1"}}));
    REQUIRE(q(ex, "SELECT id, LAG(f) OVER (ORDER BY id) FROM fw ORDER BY id") == (Rows{{"1", "NULL"}, {"2", "0.1"}, {"3", "1234570"}, {"4", "16777200"}}));
    REQUIRE(q(ex, "SELECT id, LEAD(f) OVER (ORDER BY id) FROM fw ORDER BY id") == (Rows{{"1", "1234570"}, {"2", "16777200"}, {"3", "3.14159"}, {"4", "NULL"}}));
    REQUIRE(q(ex, "SELECT id, FIRST_VALUE(f) OVER (ORDER BY id DESC) FROM fw ORDER BY id") == (Rows{{"1", "3.14159"}, {"2", "3.14159"}, {"3", "3.14159"}, {"4", "3.14159"}}));
    REQUIRE(q(ex, "SELECT id, LAST_VALUE(f) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM fw WHERE id = 1") == (Rows{{"1", "0.1"}}));
    REQUIRE(q(ex, "SELECT id, NTH_VALUE(f, 2) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM fw WHERE id <= 2") == (Rows{{"1", "1234570"}, {"2", "1234570"}}));
    REQUIRE(q(ex, "SELECT id, MAX(f) OVER (PARTITION BY g) FROM fw ORDER BY id") == (Rows{{"1", "1234570"}, {"2", "1234570"}, {"3", "16777200"}, {"4", "16777200"}}));
    // a DOUBLE column is shown whole, and SUM / AVG of a FLOAT are DOUBLEs
    REQUIRE(q(ex, "SELECT id, MAX(d) OVER (ORDER BY id) FROM fw WHERE id = 3") == (Rows{{"3", "16777217"}}));
    REQUIRE(q(ex, "SELECT SUM(f) OVER () FROM fw WHERE id = 1") == (Rows{{"0.10000000149011612"}}));
}

TEST_CASE("R58: `*` over a chain of USING / NATURAL joins puts each join's merged columns in front of what is left of its left side, then of its right side", "[r58][join_names]") {
    TempDataDir dir("r58_using_chain");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ua (x INT, y INT, a INT)");
    ok(ex, "CREATE TABLE ub (x INT, b INT)");
    ok(ex, "CREATE TABLE uc (y INT, c INT)");
    ok(ex, "CREATE TABLE ud (x INT, y INT, z INT)");
    ok(ex, "INSERT INTO ua VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300)");
    ok(ex, "INSERT INTO ub VALUES (1, 11), (2, 22), (4, 44)");
    ok(ex, "INSERT INTO uc VALUES (10, 111), (20, 222), (50, 555)");
    ok(ex, "INSERT INTO ud VALUES (1, 10, 7), (2, 99, 8)");
    // (ua JOIN ub USING (x)) JOIN uc USING (y): y goes in front of the x the first join put there
    REQUIRE(header_of(ex, "SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y)") == "y|x|a|b|c");
    REQUIRE(header_of(ex, "SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) WHERE 1 = 0") == "y|x|a|b|c");
    REQUIRE(q(ex, "SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) ORDER BY y") == (Rows{{"10", "1", "100", "11", "111"}, {"20", "2", "200", "22", "222"}}));
    REQUIRE(header_of(ex, "SELECT * FROM ua LEFT JOIN ub USING (x) LEFT JOIN uc USING (y)") == "y|x|a|b|c");
    // a join with ON does not merge: its tables follow one another, and the next USING puts its column in front
    REQUIRE(header_of(ex, "SELECT * FROM ua JOIN ub ON ua.x = ub.x JOIN uc USING (y)") == "y|x|a|x|b|c");
    // the merged columns of one join are in the order the left side has them, not the order USING lists them
    REQUIRE(header_of(ex, "SELECT * FROM ua JOIN ud USING (y, x)") == "x|y|a|z");
    REQUIRE(header_of(ex, "SELECT * FROM ud JOIN ua USING (y, x)") == "x|y|z|a");
    REQUIRE(q(ex, "SELECT * FROM ua JOIN ud USING (y, x) ORDER BY x") == (Rows{{"1", "10", "100", "7"}}));
    REQUIRE(header_of(ex, "SELECT * FROM ua NATURAL JOIN ud") == "x|y|a|z");
    REQUIRE(header_of(ex, "SELECT * FROM ud NATURAL JOIN ua") == "x|y|z|a");
    REQUIRE(header_of(ex, "SELECT * FROM ua NATURAL JOIN ub NATURAL JOIN uc") == "y|x|a|b|c");
    // RIGHT JOIN ... USING swaps the sides (a RIGHT JOIN b is b LEFT JOIN a); a chain goes on from there
    REQUIRE(header_of(ex, "SELECT * FROM ua RIGHT JOIN ub USING (x)") == "x|b|y|a");
    REQUIRE(header_of(ex, "SELECT * FROM ua RIGHT JOIN ub USING (x) JOIN uc USING (y)") == "y|x|b|a|c");
    REQUIRE(header_of(ex, "SELECT * FROM ua RIGHT JOIN ud USING (y, x)") == "x|y|z|a");
    // a third join over columns the two before it have
    REQUIRE(header_of(ex, "SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) JOIN ud USING (x, y)") == "y|x|a|b|c|z");
    REQUIRE(header_of(ex, "SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) JOIN ud USING (y, x)") == "y|x|a|b|c|z");
    // an answer with no rows says the same
    for (const char* query : {"SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y)", "SELECT * FROM ua RIGHT JOIN ub USING (x) JOIN uc USING (y)",
                              "SELECT * FROM ua JOIN ud USING (y, x)", "SELECT * FROM ua NATURAL JOIN ub NATURAL JOIN uc"}) {
        REQUIRE(header_of(ex, std::string(query) + " LIMIT 0") == header_of(ex, query));
    }
}

TEST_CASE("R58: a foreign key without a name is called <table>_ibfk_<n> in the order the keys were written, and keeps its name when another key goes", "[r58][foreign_key]") {
    TempDataDir dir("r58_fk_names");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p1 (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE p2 (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE p3 (id INT PRIMARY KEY)");
    const auto names = [&](const std::string& table) {
        return q(ex, "SELECT constraint_name, column_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_name = '" + table +
                         "' AND referenced_table_name IS NOT NULL ORDER BY constraint_name");
    };
    // the order they are written, not the order of their columns
    ok(ex, "CREATE TABLE t (a INT, b INT, FOREIGN KEY (b) REFERENCES p1(id), FOREIGN KEY (a) REFERENCES p2(id))");
    REQUIRE(names("t") == (Rows{{"t_ibfk_1", "b", "p1"}, {"t_ibfk_2", "a", "p2"}}));
    // a key that goes leaves the others as they are; a new key is one more than the highest number there is
    ok(ex, "ALTER TABLE t DROP FOREIGN KEY t_ibfk_1");
    REQUIRE(names("t") == (Rows{{"t_ibfk_2", "a", "p2"}}));
    ok(ex, "ALTER TABLE t ADD FOREIGN KEY (b) REFERENCES p3(id)");
    REQUIRE(names("t") == (Rows{{"t_ibfk_2", "a", "p2"}, {"t_ibfk_3", "b", "p3"}}));
    ok(ex, "ALTER TABLE t ADD FOREIGN KEY (a) REFERENCES p3(id)");
    REQUIRE(names("t") == (Rows{{"t_ibfk_2", "a", "p2"}, {"t_ibfk_3", "b", "p3"}, {"t_ibfk_4", "a", "p3"}}));
    // the highest one dropped: its number is taken again
    ok(ex, "ALTER TABLE t DROP FOREIGN KEY t_ibfk_4");
    ok(ex, "ALTER TABLE t ADD FOREIGN KEY (a) REFERENCES p1(id)");
    REQUIRE(names("t") == (Rows{{"t_ibfk_2", "a", "p2"}, {"t_ibfk_3", "b", "p3"}, {"t_ibfk_4", "a", "p1"}}));
    // a table that is renamed renames them (RENAME TABLE and ALTER TABLE ... RENAME TO)
    ok(ex, "RENAME TABLE t TO tt");
    REQUIRE(names("tt") == (Rows{{"tt_ibfk_2", "a", "p2"}, {"tt_ibfk_3", "b", "p3"}, {"tt_ibfk_4", "a", "p1"}}));
    ok(ex, "ALTER TABLE tt RENAME TO t_again");
    REQUIRE(names("t_again") == (Rows{{"t_again_ibfk_2", "a", "p2"}, {"t_again_ibfk_3", "b", "p3"}, {"t_again_ibfk_4", "a", "p1"}}));
    ok(ex, "ALTER TABLE t_again DROP FOREIGN KEY t_again_ibfk_3");
    REQUIRE(names("t_again") == (Rows{{"t_again_ibfk_2", "a", "p2"}, {"t_again_ibfk_4", "a", "p1"}}));
    // only the keys without a name are numbered (a key that was given one is not counted), and a name that looks like a number is one
    ok(ex, "CREATE TABLE n1 (a INT, b INT, CONSTRAINT c1 FOREIGN KEY (a) REFERENCES p1(id), FOREIGN KEY (b) REFERENCES p2(id))");
    REQUIRE(names("n1") == (Rows{{"c1", "a", "p1"}, {"n1_ibfk_1", "b", "p2"}}));
    ok(ex, "CREATE TABLE n2 (a INT, b INT, FOREIGN KEY (a) REFERENCES p1(id), CONSTRAINT c9 FOREIGN KEY (b) REFERENCES p2(id))");
    REQUIRE(names("n2") == (Rows{{"c9", "b", "p2"}, {"n2_ibfk_1", "a", "p1"}}));
    ok(ex, "CREATE TABLE n3 (a INT, b INT, CONSTRAINT n3_ibfk_5 FOREIGN KEY (a) REFERENCES p1(id), FOREIGN KEY (b) REFERENCES p2(id))");
    REQUIRE(names("n3") == (Rows{{"n3_ibfk_1", "b", "p2"}, {"n3_ibfk_5", "a", "p1"}}));
    ok(ex, "ALTER TABLE n3 ADD FOREIGN KEY (a) REFERENCES p3(id)");
    REQUIRE(names("n3") == (Rows{{"n3_ibfk_1", "b", "p2"}, {"n3_ibfk_5", "a", "p1"}, {"n3_ibfk_6", "a", "p3"}}));
    // a name that only starts like a number is none: the next key is the first one
    ok(ex, "CREATE TABLE n5 (a INT, CONSTRAINT n5_ibfk_x FOREIGN KEY (a) REFERENCES p1(id))");
    ok(ex, "ALTER TABLE n5 ADD FOREIGN KEY (a) REFERENCES p2(id)");
    REQUIRE(q(ex, "SELECT constraint_name FROM information_schema.referential_constraints WHERE table_name = 'n5' ORDER BY constraint_name") == (Rows{{"n5_ibfk_1"}, {"n5_ibfk_x"}}));
    // two keys on one column, one dropped and one added in the same ALTER
    ok(ex, "CREATE TABLE n4 (a INT, FOREIGN KEY (a) REFERENCES p1(id), FOREIGN KEY (a) REFERENCES p2(id))");
    ok(ex, "ALTER TABLE n4 DROP FOREIGN KEY n4_ibfk_1, ADD FOREIGN KEY (a) REFERENCES p3(id)");
    REQUIRE(q(ex, "SELECT constraint_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_name = 'n4' AND referenced_table_name IS NOT NULL ORDER BY constraint_name") ==
            (Rows{{"n4_ibfk_2", "p2"}, {"n4_ibfk_3", "p3"}}));
    // a database name in front of the table does not get into the name
    ok(ex, "CREATE TABLE d.q1 (a INT, FOREIGN KEY (a) REFERENCES d.p1(id))");
    REQUIRE(names("q1") == (Rows{{"q1_ibfk_1", "a", "p1"}}));
}

TEST_CASE("R58: the names and the rules of a foreign key are kept over a restart, and a table that is renamed is found by information_schema and by the keys that refer to it", "[r58][foreign_key]") {
    TempDataDir dir("r58_fk_restart");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE p1 (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE r1 (a INT, b INT, FOREIGN KEY (b) REFERENCES p1(id) ON DELETE RESTRICT ON UPDATE NO ACTION, FOREIGN KEY (a) REFERENCES p1(id))");
        ok(ex, "ALTER TABLE r1 DROP FOREIGN KEY r1_ibfk_2");
        ok(ex, "ALTER TABLE r1 ADD FOREIGN KEY (a) REFERENCES p1(id) ON UPDATE RESTRICT");
        ok(ex, "CREATE TABLE pp (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE cc (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES pp(id))");
        ok(ex, "INSERT INTO pp VALUES (1)");
        ok(ex, "INSERT INTO cc VALUES (1, 1)");
        ok(ex, "RENAME TABLE pp TO pq");
    }
    Executor ex(dir.path);
    ok(ex, "USE d");
    REQUIRE(q(ex, "SELECT constraint_name, update_rule, delete_rule FROM information_schema.referential_constraints WHERE table_name = 'r1' ORDER BY constraint_name") ==
            (Rows{{"r1_ibfk_1", "NO ACTION", "RESTRICT"}, {"r1_ibfk_2", "RESTRICT", "NO ACTION"}}));
    // the table that was renamed is called what it is called now, and the key of its child follows it
    REQUIRE(q(ex, "SELECT table_name FROM information_schema.tables WHERE table_schema = 'd' AND table_name IN ('pp', 'pq') ORDER BY table_name") == (Rows{{"pq"}}));
    REQUIRE(q(ex, "SELECT constraint_name, referenced_table_name FROM information_schema.referential_constraints WHERE table_name = 'cc'") == (Rows{{"cc_ibfk_1", "pq"}}));
    REQUIRE(fails(ex, "INSERT INTO cc VALUES (2, 9)").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "DELETE FROM pq WHERE id = 1").find("Foreign key violation") != std::string::npos);
    ok(ex, "INSERT INTO pq VALUES (2)");
    ok(ex, "INSERT INTO cc VALUES (2, 2)");
}

TEST_CASE("R58: a rule left out or written NO ACTION is NO ACTION in information_schema, RESTRICT only when it was written; SHOW CREATE TABLE prints a RESTRICT that was written and no NO ACTION", "[r58][foreign_key]") {
    TempDataDir dir("r58_fk_rules");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE r1 (a INT, FOREIGN KEY (a) REFERENCES p(id))");
    ok(ex, "CREATE TABLE r2 (a INT, FOREIGN KEY (a) REFERENCES p(id) ON DELETE RESTRICT ON UPDATE RESTRICT)");
    ok(ex, "CREATE TABLE r3 (a INT, FOREIGN KEY (a) REFERENCES p(id) ON DELETE NO ACTION ON UPDATE NO ACTION)");
    ok(ex, "CREATE TABLE r4 (a INT, FOREIGN KEY (a) REFERENCES p(id) ON DELETE CASCADE)");
    ok(ex, "CREATE TABLE r5 (a INT, FOREIGN KEY (a) REFERENCES p(id) ON DELETE SET NULL ON UPDATE CASCADE)");
    ok(ex, "CREATE TABLE r6 (a INT, FOREIGN KEY (a) REFERENCES p(id) ON UPDATE RESTRICT)");
    ok(ex, "CREATE TABLE r7 (a INT, FOREIGN KEY (a) REFERENCES p(id) ON DELETE NO ACTION)");
    REQUIRE(q(ex, "SELECT table_name, update_rule, delete_rule FROM information_schema.referential_constraints WHERE constraint_schema = 'd' AND table_name LIKE 'r%' ORDER BY table_name") ==
            (Rows{{"r1", "NO ACTION", "NO ACTION"},
                  {"r2", "RESTRICT", "RESTRICT"},
                  {"r3", "NO ACTION", "NO ACTION"},
                  {"r4", "NO ACTION", "CASCADE"},
                  {"r5", "CASCADE", "SET NULL"},
                  {"r6", "RESTRICT", "NO ACTION"},
                  {"r7", "NO ACTION", "NO ACTION"}}));
    // ALTER TABLE ... ADD FOREIGN KEY says the same
    ok(ex, "ALTER TABLE r1 ADD CONSTRAINT r1_x FOREIGN KEY (a) REFERENCES p(id) ON DELETE RESTRICT");
    ok(ex, "ALTER TABLE r1 ADD CONSTRAINT r1_y FOREIGN KEY (a) REFERENCES p(id) ON UPDATE NO ACTION");
    REQUIRE(q(ex, "SELECT constraint_name, update_rule, delete_rule FROM information_schema.referential_constraints WHERE constraint_schema = 'd' AND table_name = 'r1' ORDER BY constraint_name") ==
            (Rows{{"r1_ibfk_1", "NO ACTION", "NO ACTION"}, {"r1_x", "NO ACTION", "RESTRICT"}, {"r1_y", "NO ACTION", "NO ACTION"}}));
    // both refuse the same way
    ok(ex, "INSERT INTO p VALUES (1)");
    ok(ex, "INSERT INTO r2 VALUES (1)");
    ok(ex, "INSERT INTO r3 VALUES (1)");
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 1").find("Foreign key violation") != std::string::npos);
    ok(ex, "DELETE FROM r2");
    REQUIRE(fails(ex, "DELETE FROM p WHERE id = 1").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE p SET id = 5 WHERE id = 1").find("Foreign key violation") != std::string::npos);
    // SHOW CREATE TABLE as a MySQL client gets it: a RESTRICT that was written, no NO ACTION
    const auto shown = [&](const std::string& table) {
        auto r = ex.show_create_table_mysql("d." + table);
        REQUIRE(r.is_ok());
        return r.value();
    };
    REQUIRE(shown("r1").find("ON DELETE RESTRICT") != std::string::npos); // (r1_x)
    REQUIRE(shown("r2").find("ON DELETE RESTRICT ON UPDATE RESTRICT") != std::string::npos);
    REQUIRE(shown("r3").find("NO ACTION") == std::string::npos);
    REQUIRE(shown("r3").find("ON DELETE") == std::string::npos);
    REQUIRE(shown("r6").find("ON UPDATE RESTRICT") != std::string::npos);
    REQUIRE(shown("r6").find("ON DELETE") == std::string::npos);
    REQUIRE(shown("r4").find("ON DELETE CASCADE") != std::string::npos);
    // ... and the one RuSQL's own SHOW CREATE TABLE writes spells every rule out, so that running it again makes the same table
    REQUIRE(answer(ex, "SHOW CREATE TABLE r3").find("ON DELETE NO ACTION ON UPDATE NO ACTION") != std::string::npos);
    REQUIRE(answer(ex, "SHOW CREATE TABLE r2").find("ON DELETE RESTRICT ON UPDATE RESTRICT") != std::string::npos);
}

TEST_CASE("R58: a NOT NULL column that an INSERT leaves out has no default to take (1364); a NULL that was written is refused as before", "[r58][not_null]") {
    TempDataDir dir("r58_not_null");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE nn (id INT PRIMARY KEY, a INT NOT NULL, b INT)");
    REQUIRE(fails(ex, "INSERT INTO nn (id, b) VALUES (1, 2)") == "Field 'a' doesn't have a default value");
    REQUIRE(fails(ex, "INSERT INTO nn (id, a) VALUES (1, NULL)") == "Column 'a' cannot be NULL");
    REQUIRE(fails(ex, "INSERT INTO nn (a, b) VALUES (1, 2), (3, 4)").find("Field 'id' doesn't have a default value") != std::string::npos);
    ok(ex, "INSERT INTO nn (id, a) VALUES (1, 5)");
    REQUIRE(q(ex, "SELECT id, a, b FROM nn") == (Rows{{"1", "5", "NULL"}}));
}

TEST_CASE("R58: BACKUP writes the rules a foreign key was made with, so that RESTORE makes the same keys (NO ACTION stays NO ACTION, a RESTRICT that was written stays one)", "[r58][foreign_key]") {
    TempDataDir dir("r58_fk_backup");
    const std::string file = "r58_fk_backup.sql";
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE p (id INT PRIMARY KEY)");
        ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, a INT, b INT, e INT, FOREIGN KEY (a) REFERENCES p(id), FOREIGN KEY (b) REFERENCES p(id) ON DELETE RESTRICT ON UPDATE RESTRICT, "
               "CONSTRAINT my_key FOREIGN KEY (e) REFERENCES p(id) ON DELETE CASCADE)");
        ok(ex, "BACKUP DATABASE d INTO '" + file + "'");
    }
    TempDataDir dir2("r58_fk_backup_restore");
    fs::create_directories(dir2.path + "/_backups");
    fs::copy_file(dir.path + "/_backups/" + file, dir2.path + "/_backups/" + file);
    Executor ex(dir2.path);
    ok(ex, "RESTORE FROM '" + file + "'");
    ok(ex, "USE d");
    REQUIRE(q(ex, "SELECT constraint_name, update_rule, delete_rule FROM information_schema.referential_constraints WHERE table_name = 'c' ORDER BY constraint_name") ==
            (Rows{{"c_ibfk_1", "NO ACTION", "NO ACTION"}, {"c_ibfk_2", "RESTRICT", "RESTRICT"}, {"my_key", "NO ACTION", "CASCADE"}}));
}
