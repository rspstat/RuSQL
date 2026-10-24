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
    REQUIRE(joined.find("Step 1 Scan d.o + JOIN d.c : 4 rows") != std::string::npos);
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
            Rows{{"fk_c_p", "RESTRICT", "CASCADE", "p"}});
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
