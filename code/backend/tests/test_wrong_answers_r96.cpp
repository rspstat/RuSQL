#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// R96 -- the five "wrong answers" RECOMMAND.md kept (a statement that runs and gives MySQL's answer in other digits, or one MySQL refuses), each widened to the family it belongs to. The expected
// values are the ones a real MySQL 8.4.9 gives (checked while writing; corpus/wrong_answers_r96.sql compares the same statements with it):
//   1. a column that a USING join made one and a later table has again is ambiguous (1052);
//   2. a FLOAT read as text is the six digits MySQL writes it with;
//   3. PI() has six fixed places, and so has arithmetic on it;
//   4. COALESCE / IFNULL / GREATEST / LEAST / UNION write exact numbers with the places of their widest side;
//   5. ROUND / TRUNCATE stop at the places the number shows.

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

Rows rows_of(Executor& ex, const std::string& sql) {
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

std::string text_of(Executor& ex, const std::string& sql) {
    const Rows rows = rows_of(ex, sql);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 1);
    return rows[0][0];
}

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

Rows column(const std::vector<std::string>& cells) {
    Rows out;
    for (const auto& c : cells) out.push_back({c});
    return out;
}
} // namespace

TEST_CASE("R96: a column a USING join made one is ambiguous again when a later table has it too", "[r96][join]") {
    TempDataDir dir("r96_using_ambiguous");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ua (x INT, a INT)");
    ok(ex, "CREATE TABLE ub (x INT, y INT, b INT)");
    ok(ex, "CREATE TABLE uc (y INT, c INT)");
    ok(ex, "CREATE TABLE ud (x INT, y INT, d INT)");
    ok(ex, "INSERT INTO ua VALUES (1, 10), (2, 20), (3, 30)");
    ok(ex, "INSERT INTO ub VALUES (1, 5, 100), (2, 6, 200), (3, 5, 300)");
    ok(ex, "INSERT INTO uc VALUES (5, 1000), (6, 2000)");
    ok(ex, "INSERT INTO ud VALUES (1, 5, 7), (2, 6, 8), (3, 5, 9)");
    const std::string chain = " FROM ua JOIN ub USING (x) JOIN uc USING (y) JOIN ud USING (x)";
    // y is merged by the second join, ud has a y of its own that no join merged: ambiguous wherever a bare y is read (MySQL 1052)
    CHECK(error_of(ex, "SELECT *" + chain + " ORDER BY y").find("Column 'y' in order clause is ambiguous") != std::string::npos);
    CHECK(error_of(ex, "SELECT a, y" + chain).find("Column 'y' in field list is ambiguous") != std::string::npos);
    CHECK(error_of(ex, "SELECT a, c" + chain + " WHERE y = 5").find("Column 'y' in where clause is ambiguous") != std::string::npos);
    CHECK(error_of(ex, "SELECT COUNT(*)" + chain + " GROUP BY y").find("Column 'y' in group statement is ambiguous") != std::string::npos);
    // what is not ambiguous still runs: x joined by two USING joins is one column, y of the first two tables is one column, the qualified names say which table
    CHECK(rows_of(ex, "SELECT a, b, c, d" + chain + " ORDER BY x, a") == Rows{{"10", "100", "1000", "7"}, {"20", "200", "2000", "8"}, {"30", "300", "1000", "9"}});
    CHECK(rows_of(ex, "SELECT *" + chain + " ORDER BY x").size() == 3);
    CHECK(rows_of(ex, "SELECT *  FROM ua JOIN ub USING (x) JOIN uc USING (y) ORDER BY y, x").size() == 3);
    CHECK(rows_of(ex, "SELECT y FROM ua JOIN ub USING (x) JOIN uc USING (y) ORDER BY y, a") == column({"5", "5", "6"}));
    CHECK(rows_of(ex, "SELECT ub.y, ud.y" + chain + " ORDER BY ub.y, a") == Rows{{"5", "5"}, {"5", "5"}, {"6", "6"}});
    // `t.*` in the select list: ORDER BY y reads the one y of the list
    CHECK(rows_of(ex, "SELECT ua.*, uc.*" + chain + " ORDER BY y, a").size() == 3);
    CHECK(error_of(ex, "SELECT ua.*, uc.*, ud.*" + chain + " ORDER BY y").find("Column 'y' in order clause is ambiguous") != std::string::npos);
    // without a USING that merges it a name two tables have was ambiguous already
    CHECK(error_of(ex, "SELECT * FROM ua JOIN ub USING (x) JOIN ud USING (x) ORDER BY y").find("ambiguous") != std::string::npos);
}

TEST_CASE("R96: a FLOAT that goes into a string function is the six digits MySQL writes it with", "[r96][float]") {
    TempDataDir dir("r96_float_text");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fl (id INT PRIMARY KEY, f FLOAT, d DOUBLE)");
    ok(ex, "INSERT INTO fl VALUES (1, 3.1415927, 3.1415927), (2, 0.1, 0.1), (3, 16777217, 1), (4, NULL, NULL), (5, 0.00001, 1), (6, 123456.789, 2)");
    CHECK(rows_of(ex, "SELECT CONCAT(f, '') FROM fl ORDER BY id") == column({"3.14159", "0.1", "16777200", "NULL", "0.00001", "123457"}));
    CHECK(rows_of(ex, "SELECT LENGTH(f) FROM fl WHERE id IN (1, 6) ORDER BY id") == column({"7", "6"}));
    CHECK(text_of(ex, "SELECT CONCAT_WS('-', f, d) FROM fl WHERE id = 1") == "3.14159-3.1415927"); // (the DOUBLE column keeps its digits)
    CHECK(text_of(ex, "SELECT RIGHT(f, 3) FROM fl WHERE id = 1") == "159");
    CHECK(text_of(ex, "SELECT REPLACE(f, '.', ',') FROM fl WHERE id = 1") == "3,14159");
    CHECK(text_of(ex, "SELECT LPAD(f, 12, '0') FROM fl WHERE id = 1") == "000003.14159");
    CHECK(text_of(ex, "SELECT CAST(f AS CHAR) FROM fl WHERE id = 1") == "3.14159");
    CHECK(text_of(ex, "SELECT UPPER(f) FROM fl WHERE id = 1") == "3.14159");
    CHECK(text_of(ex, "SELECT REVERSE(f) FROM fl WHERE id = 1") == "95141.3");
    CHECK(text_of(ex, "SELECT INSTR(f, '.') FROM fl WHERE id = 6") == "0"); // (123457: no point)
    CHECK(text_of(ex, "SELECT FORMAT(f, 3) FROM fl WHERE id = 6") == "123,456.789"); // (the single-precision number is 123456.7890625)
    CHECK(text_of(ex, "SELECT GROUP_CONCAT(f ORDER BY id) FROM fl") == "3.14159,0.1,16777200,0.00001,123457");
    CHECK(text_of(ex, "SELECT GROUP_CONCAT(f ORDER BY id SEPARATOR '|') FROM fl WHERE id < 3") == "3.14159|0.1");
    // a FLOAT the functions COALESCE / IFNULL / GREATEST give is the double it widens to; a CASE over FLOAT columns is a column
    CHECK(text_of(ex, "SELECT CONCAT(COALESCE(f, 0), '') FROM fl WHERE id = 1") == "3.1415927410125732");
    CHECK(text_of(ex, "SELECT CONCAT(IFNULL(f, 0), '') FROM fl WHERE id = 2") == "0.10000000149011612");
    CHECK(text_of(ex, "SELECT CONCAT(CASE WHEN id > 0 THEN f END, '') FROM fl WHERE id = 1") == "3.14159");
    CHECK(text_of(ex, "SELECT CONCAT(IF(id > 0, f, 0), '') FROM fl WHERE id = 1") == "3.14159");
    // a text comparison reads the same text
    CHECK(rows_of(ex, "SELECT id FROM fl WHERE CONCAT(f, '') = '3.14159'") == column({"1"}));
    CHECK(rows_of(ex, "SELECT id FROM fl WHERE f LIKE '%14159'") == column({"1"}));
    CHECK(rows_of(ex, "SELECT id FROM fl WHERE f LIKE '%27'").empty());
    // the number itself is unchanged
    CHECK(text_of(ex, "SELECT f FROM fl WHERE id = 1") == "3.14159");
    CHECK(text_of(ex, "SELECT f + 0 FROM fl WHERE id = 2") == "0.10000000149011612");
}

TEST_CASE("R96: PI() has six fixed places, and arithmetic on it keeps them", "[r96][pi]") {
    TempDataDir dir("r96_pi");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, d DOUBLE)");
    ok(ex, "INSERT INTO t VALUES (1, 0.1), (2, 2)");
    CHECK(text_of(ex, "SELECT PI()") == "3.141593");
    CHECK(text_of(ex, "SELECT PI() + 0") == "3.141593");
    CHECK(text_of(ex, "SELECT PI() * 2") == "6.283185");
    CHECK(text_of(ex, "SELECT PI() - 1") == "2.141593");
    CHECK(text_of(ex, "SELECT 2 * PI()") == "6.283185");
    CHECK(text_of(ex, "SELECT -PI()") == "-3.141593");
    CHECK(text_of(ex, "SELECT PI() * PI()") == "9.869604");
    CHECK(text_of(ex, "SELECT PI() + 1.5") == "4.641593");
    CHECK(text_of(ex, "SELECT PI() / 2") == "1.5707963268"); // (the places of the widest side plus 4)
    CHECK(text_of(ex, "SELECT 1 / PI()") == "0.3183098862");
    CHECK(text_of(ex, "SELECT PI() * 1.123456789") == "3.529443595"); // (a side with more places widens them)
    CHECK(text_of(ex, "SELECT ABS(PI())") == "3.141593");
    CHECK(text_of(ex, "SELECT MOD(PI(), 2)") == "1.141593");
    CHECK(text_of(ex, "SELECT LEAST(PI(), 4)") == "3.141593");
    CHECK(text_of(ex, "SELECT COALESCE(PI(), 1)") == "3.141593");
    CHECK(text_of(ex, "SELECT CONCAT(PI() * 2, '')") == "6.283185");
    CHECK(text_of(ex, "SELECT CONCAT('a', PI())") == "a3.141593");
    CHECK(text_of(ex, "SELECT LENGTH(PI())") == "8");
    // the value inside is the whole double: functions that take it work on all of it
    CHECK(text_of(ex, "SELECT ROUND(PI() * 2, 10)") == "6.2831853072");
    CHECK(text_of(ex, "SELECT SQRT(PI() * PI())") == "3.141592653589793");
    CHECK(text_of(ex, "SELECT DEGREES(PI())") == "180");
    // a DOUBLE column has no fixed places and takes them away
    CHECK(text_of(ex, "SELECT PI() + d FROM t WHERE id = 2") == "5.141592653589793");
    // two numbers with fixed places are equal when they differ by less than 5 in the place after the places they both have
    CHECK(text_of(ex, "SELECT PI() = 3.141593") == "1");
    CHECK(text_of(ex, "SELECT PI() > 3.14159265") == "0");
    CHECK(text_of(ex, "SELECT PI() >= 3.1415926") == "1");
    CHECK(text_of(ex, "SELECT PI() < 3.14159266") == "1");
    CHECK(text_of(ex, "SELECT PI() > 3.141592") == "1");
    CHECK(text_of(ex, "SELECT 3.14159265 < PI()") == "0");
    CHECK(rows_of(ex, "SELECT id FROM t WHERE PI() = 3.141593").size() == 2);
    CHECK(rows_of(ex, "SELECT id FROM t WHERE PI() > 3.141592").size() == 2);
    CHECK(rows_of(ex, "SELECT id FROM t WHERE PI() > 3.14159265").empty());
}

TEST_CASE("R96: COALESCE, IFNULL, LEAST, GREATEST and UNION write exact numbers with the places of the widest side", "[r96][decimal]") {
    TempDataDir dir("r96_places");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE dg (id INT PRIMARY KEY, n DECIMAL(10,2), e DECIMAL(10,4), d DOUBLE, i INT)");
    ok(ex, "INSERT INTO dg VALUES (1, 1.50, 1.5000, 1.5, 1), (2, NULL, 2.2500, 2.25, 2), (3, 3.00, NULL, NULL, NULL)");
    CHECK(text_of(ex, "SELECT COALESCE(1.5, 2.25)") == "1.50");
    CHECK(rows_of(ex, "SELECT IFNULL(n, 0) FROM dg ORDER BY id") == column({"1.50", "0.00", "3.00"}));
    CHECK(rows_of(ex, "SELECT IFNULL(n, 0.0) FROM dg ORDER BY id") == column({"1.50", "0.00", "3.00"}));
    CHECK(rows_of(ex, "SELECT IFNULL(n, 1.234) FROM dg ORDER BY id") == column({"1.500", "1.234", "3.000"}));
    CHECK(rows_of(ex, "SELECT COALESCE(n, e) FROM dg ORDER BY id") == column({"1.5000", "2.2500", "3.0000"}));
    CHECK(rows_of(ex, "SELECT COALESCE(NULL, n) FROM dg ORDER BY id") == column({"1.50", "NULL", "3.00"}));
    CHECK(rows_of(ex, "SELECT COALESCE(n, i) FROM dg ORDER BY id") == column({"1.50", "2.00", "3.00"}));
    CHECK(text_of(ex, "SELECT LEAST(1.5, 2.25)") == "1.50");
    CHECK(rows_of(ex, "SELECT GREATEST(n, e) FROM dg WHERE id = 1") == column({"1.5000"}));
    CHECK(rows_of(ex, "SELECT COALESCE(n, 0) * 3 FROM dg ORDER BY id") == column({"4.50", "0.00", "9.00"})); // (the places go on into arithmetic)
    // a DOUBLE among them makes the value a DOUBLE: no places are added
    CHECK(rows_of(ex, "SELECT COALESCE(n, d) FROM dg WHERE id = 2") == column({"2.25"}));
    // UNION / INTERSECT / EXCEPT: the column has the places of its widest side, and 1.50 and 1.5000 are one value
    CHECK(rows_of(ex, "SELECT 1.5 UNION ALL SELECT 2.25") == column({"1.50", "2.25"}));
    CHECK(rows_of(ex, "SELECT 1 UNION ALL SELECT 2.5") == column({"1.0", "2.5"}));
    CHECK(rows_of(ex, "SELECT n FROM dg UNION ALL SELECT e FROM dg").size() == 6);
    CHECK(rows_of(ex, "SELECT n FROM dg UNION ALL SELECT e FROM dg")[0][0] == "1.5000");
    CHECK(rows_of(ex, "SELECT n FROM dg UNION SELECT e FROM dg ORDER BY 1").size() == 4); // (1.5, 2.25, 3, NULL)
    CHECK(rows_of(ex, "SELECT 1.5 INTERSECT SELECT 1.50") == column({"1.50"}));
    CHECK(rows_of(ex, "SELECT 1.50 EXCEPT SELECT 1.5").empty());
    CHECK(rows_of(ex, "SELECT 'a' UNION ALL SELECT 1.5")[1][0] == "1.5"); // (a text with it: no places)
}

TEST_CASE("R96: ROUND and TRUNCATE stop at the places the number shows", "[r96][round]") {
    TempDataDir dir("r96_round");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE dg (id INT PRIMARY KEY, n DECIMAL(10,2), i INT, d DOUBLE)");
    ok(ex, "INSERT INTO dg VALUES (1, 1.50, 1, 1.5), (2, NULL, 2, 2.25)");
    CHECK(text_of(ex, "SELECT ROUND(10 / 3, 6)") == "3.3333"); // (a quotient shows 4 places, though it holds 9)
    CHECK(text_of(ex, "SELECT ROUND(10 / 3 * 3, 6)") == "10.0000");
    CHECK(text_of(ex, "SELECT TRUNCATE(10 / 3, 6)") == "3.3333");
    CHECK(text_of(ex, "SELECT ROUND(10 / 3, 9)") == "3.3333");
    CHECK(text_of(ex, "SELECT ROUND(1 / 3, 8)") == "0.3333");
    CHECK(text_of(ex, "SELECT ROUND(10 / 3 + 0.5, 7)") == "3.8333");
    CHECK(text_of(ex, "SELECT ROUND(1.0 * 10 / 3, 6)") == "3.33333");
    CHECK(text_of(ex, "SELECT ROUND(1.5, 6)") == "1.5");
    CHECK(text_of(ex, "SELECT ROUND(10 / 3, 6) = 3.3333") == "1"); // (the value is cut, not only its text)
    CHECK(text_of(ex, "SELECT ROUND(10 / 3, 6) + 1") == "4.3333");
    CHECK(text_of(ex, "SELECT ROUND(AVG(i) * 3, 6) FROM dg") == "4.5000"); // (an average of integers shows 4 places)
    // places the number does show, a column and a DOUBLE are not cut
    CHECK(text_of(ex, "SELECT ROUND(10 / 3, 2)") == "3.33");
    CHECK(text_of(ex, "SELECT ROUND(10 / 3, 4)") == "3.3333");
    CHECK(rows_of(ex, "SELECT ROUND(n, 6) FROM dg ORDER BY id") == column({"1.50", "NULL"}));
    CHECK(rows_of(ex, "SELECT ROUND(d, 6) FROM dg ORDER BY id") == column({"1.5", "2.25"}));
    CHECK(text_of(ex, "SELECT ROUND(1234.5678, -2)") == "1200");
    CHECK(text_of(ex, "SELECT FORMAT(10 / 3, 6)") == "3.333333"); // (FORMAT keeps all the digits: MySQL does)
}

// R96b -- the three rows RECOMMAND.md still kept after R96 (HEX of a FLOAT, an aggregate of PI() with BETWEEN / IN, a number written with an exponent), widened to their families.
// The expected values are the ones a real MySQL 8.4.9 gives (corpus/wrong_answers_r96.sql compares the same statements with it).
namespace {
void make_fl(Executor& ex) {
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fl (id INT PRIMARY KEY, f FLOAT, d DOUBLE, n DECIMAL(10,2), i INT)");
    ok(ex, "INSERT INTO fl VALUES (1, 3.1415927, 3.1415927, 3.14, 3), (2, 0.1, 0.1, 0.10, 0), (3, 16777217, 1, 1.00, 255), (4, NULL, NULL, NULL, NULL),"
           " (5, 123456.789, 2, 255.70, -1), (6, -2.5, 1.5, -1.50, 16), (7, 1e10, 3, 7.77, 65)");
}
} // namespace

TEST_CASE("R96b: HEX of a number is the hex of the integer it rounds to; BIN, OCT and CONV read a text", "[r96][hex]") {
    TempDataDir dir("r96b_hex");
    Executor ex(dir.path);
    make_fl(ex);
    // an integer column used to be HEX of its digits ('3' gave 33), a FLOAT column too
    CHECK(rows_of(ex, "SELECT HEX(i) FROM fl ORDER BY id") == column({"3", "0", "FF", "NULL", "FFFFFFFFFFFFFFFF", "10", "41"}));
    CHECK(rows_of(ex, "SELECT HEX(f) FROM fl ORDER BY id") == column({"3", "0", "1000000", "NULL", "1E241", "FFFFFFFFFFFFFFFE", "2540BE400"})); // (-2.5 rounds half to even: -2)
    CHECK(rows_of(ex, "SELECT HEX(d) FROM fl ORDER BY id") == column({"3", "0", "1", "NULL", "2", "2", "3"}));
    CHECK(rows_of(ex, "SELECT HEX(n) FROM fl ORDER BY id") == column({"3", "0", "1", "NULL", "100", "FFFFFFFFFFFFFFFE", "8"})); // (a DECIMAL rounds half away from zero: 255.70 is 256, -1.50 is -2)
    CHECK(rows_of(ex, "SELECT HEX(f * 2) FROM fl ORDER BY id") == column({"6", "0", "2000000", "NULL", "3C482", "FFFFFFFFFFFFFFFB", "4A817C800"}));
    CHECK(rows_of(ex, "SELECT HEX(-f) FROM fl ORDER BY id") == column({"FFFFFFFFFFFFFFFD", "0", "FFFFFFFFFF000000", "NULL", "FFFFFFFFFFFE1DBF", "2", "FFFFFFFDABF41C00"}));
    CHECK(rows_of(ex, "SELECT HEX(COALESCE(f, 0)) FROM fl ORDER BY id") == column({"3", "0", "1000000", "0", "1E241", "FFFFFFFFFFFFFFFE", "2540BE400"}));
    CHECK(rows_of(ex, "SELECT HEX(i + 1) FROM fl WHERE id < 4 ORDER BY id") == column({"4", "1", "100"}));
    // a DOUBLE rounds half to even, a DECIMAL half away from zero
    CHECK(rows_of(ex, "SELECT HEX(2.5e0), HEX(3.5e0), HEX(2.5), HEX(-2.5), HEX(-2.5e0), HEX(255.5e0), HEX(1e3), HEX(PI()), HEX(PI() * 100), HEX(255.7)")[0] ==
          std::vector<std::string>{"2", "4", "3", "FFFFFFFFFFFFFFFD", "FFFFFFFFFFFFFFFE", "100", "3E8", "3", "13A", "100"});
    // a text is still the hex of its bytes
    CHECK(rows_of(ex, "SELECT HEX('255'), HEX(CONCAT('a', 'b')), HEX(UPPER('abc')), HEX(LENGTH('abc'))")[0] == std::vector<std::string>{"323535", "6162", "414243", "3"});
    // BIN(x), OCT(x) and CONV(x, 10, b) read x as the text it is written with: the digits after a point are cut off, not rounded
    CHECK(rows_of(ex, "SELECT CONV(255.7, 10, 16), BIN(255.7), OCT(255.7), CONV(-2.5, 10, 16), BIN(PI() * 100), CONV(PI(), 10, 16)")[0] ==
          std::vector<std::string>{"FF", "11111111", "377", "FFFFFFFFFFFFFFFE", "100111010", "3"});
    CHECK(text_of(ex, "SELECT BIN(-2.5)") == "1111111111111111111111111111111111111111111111111111111111111110");
    CHECK(rows_of(ex, "SELECT BIN(n) FROM fl WHERE id IN (1, 3, 5, 7) ORDER BY id") == column({"11", "1", "11111111", "111"}));
    CHECK(rows_of(ex, "SELECT OCT(n) FROM fl WHERE id IN (1, 3, 5, 7) ORDER BY id") == column({"3", "1", "377", "7"}));
    CHECK(rows_of(ex, "SELECT CONV(f, 10, 16) FROM fl ORDER BY id") == column({"3", "0", "FFFFF0", "NULL", "1E241", "FFFFFFFFFFFFFFFE", "2540BE400"})); // (a FLOAT is the text of six digits: 16777217 is 16777200)
    CHECK(rows_of(ex, "SELECT CONV(d, 10, 2) FROM fl ORDER BY id") == column({"11", "0", "1", "NULL", "10", "1", "11"}));
}

TEST_CASE("R96b: SUM, AVG, MIN and MAX of a DOUBLE with fixed places are shown with them; BETWEEN and IN meet it as = does", "[r96][pi][aggregate]") {
    TempDataDir dir("r96b_pi_aggregate");
    Executor ex(dir.path);
    make_fl(ex); // (seven rows)
    CHECK(rows_of(ex, "SELECT SUM(PI()), AVG(PI()), MAX(PI()), MIN(PI()) FROM fl")[0] == std::vector<std::string>{"21.991149", "3.1415926536", "3.141593", "3.141593"});
    CHECK(rows_of(ex, "SELECT SUM(PI() * 2), AVG(PI() * 2), SUM(PI() + i), SUM(PI() + n), MIN(PI() + n) FROM fl")[0] ==
          std::vector<std::string>{"43.982297", "6.2831853072", "356.849556", "285.059556", "1.641593"});
    CHECK(rows_of(ex, "SELECT i, SUM(PI() * i) FROM fl WHERE i IS NOT NULL GROUP BY i ORDER BY i")[2] == std::vector<std::string>{"3", "9.424778"});
    CHECK(text_of(ex, "SELECT SUM(PI()) FROM fl WHERE id > 100") == "NULL");
    // the value an expression or a string function goes on with is the whole double; only a result column is written with the places
    CHECK(text_of(ex, "SELECT CONCAT(SUM(PI()), '|', AVG(PI())) FROM fl") == "21.991148575128552|3.141592653589793");
    CHECK(text_of(ex, "SELECT SUM(PI()) FROM fl HAVING SUM(PI()) > 21.9911485") == "21.991149");
    // named in an expression, an aggregate hands on the places of its argument (SUM(PI()) + 0 is 21.991149); MIN and MAX do so for a string function too: CONCAT(MAX(PI()), '') is 3.141593
    CHECK(text_of(ex, "SELECT CONCAT(SUM(PI()) + 0, '|', AVG(PI()) + 0, '|', SUM(PI()) * 2) FROM fl") == "21.991149|3.1415926536|43.982297");
    CHECK(text_of(ex, "SELECT CONCAT(MIN(PI()), '|', MAX(PI()) + 0, '|', MAX(PI()) * 2, '|', MAX(PI() * 2), '|', MIN(PI() + n), '|', UPPER(MAX(PI())), '|', LENGTH(MAX(PI()))) FROM fl") ==
          "3.141593|3.141593|6.283185|6.283185|1.641593|3.141593|8");
    CHECK(rows_of(ex, "SELECT MAX(PI()) > 3.1415926, MAX(PI()) = 3.141593, MAX(PI()) BETWEEN 3.1415926 AND 3.14159265 FROM fl")[0] == std::vector<std::string>{"1", "1", "0"});
    CHECK(text_of(ex, "SELECT CONCAT(MAX(PI() + d), '|', MIN(PI() + 1e0)) FROM fl") == "6.2831853535897935|4.141592653589793");
    // a DOUBLE with no fixed places (a DOUBLE column, 1e0) takes them away
    CHECK(text_of(ex, "SELECT SUM(PI() + d) FROM fl") == "29.591148621538757");
    CHECK(text_of(ex, "SELECT SUM(PI() + 1e0) FROM fl") == "28.991148575128552");
    // BETWEEN and IN take PI() and a written number as equal when they differ by less than 5 in the place after the places they both have
    CHECK(rows_of(ex, "SELECT PI() IN (3.141593), PI() NOT IN (3.141593), PI() IN (3.14159265), PI() IN (3.1415927), PI() IN (3.14159), PI() * 2 IN (6.283185)")[0] ==
          std::vector<std::string>{"1", "0", "1", "1", "0", "1"});
    CHECK(rows_of(ex, "SELECT PI() * 2 BETWEEN 6.283185 AND 6.283185, PI() BETWEEN 3.1415926 AND 3.1415927, PI() BETWEEN 3.14 AND 3.141592, PI() NOT BETWEEN 3.141593 AND 4")[0] ==
          std::vector<std::string>{"1", "1", "0", "1"});
    // a list of one value is `=`, and so is BETWEEN a AND a; a longer list, bounds that differ and NOT BETWEEN compare plain doubles
    CHECK(rows_of(ex, "SELECT PI() IN (3.141593, 1), PI() BETWEEN 3.141593 AND 4, PI() BETWEEN 3.141593 AND 3.141593, PI() NOT BETWEEN 3.141593 AND 3.141593, PI() IN (3.141593, 3.141593)")[0] ==
          std::vector<std::string>{"0", "0", "1", "1", "0"});
    CHECK(text_of(ex, "SELECT COUNT(*) FROM fl WHERE PI() IN (3.141593)") == "7");
    CHECK(text_of(ex, "SELECT COUNT(*) FROM fl WHERE PI() * i BETWEEN 9.424778 AND 9.424778") == "1");
}

TEST_CASE("R96b: a number written with an exponent is a DOUBLE", "[r96][double]") {
    TempDataDir dir("r96b_double_literal");
    Executor ex(dir.path);
    make_fl(ex);
    // PI() + 1e0 has no fixed places (1e0 takes them away): the whole double
    CHECK(text_of(ex, "SELECT CONCAT(PI() + 1e0, '|', PI() * 1e0, '|', PI() / 1e0, '|', 1e0 + PI(), '|', PI() - 1e0, '|', PI() + 1.5e0, '|', -1e0 + PI(), '|', PI() + -1e0)") ==
          "4.141592653589793|3.141592653589793|3.141592653589793|4.141592653589793|2.141592653589793|4.641592653589793|2.141592653589793|2.141592653589793");
    CHECK(text_of(ex, "SELECT PI() + 1e0") == "4.141592653589793");
    // arithmetic with it is floating point: 2e0 / 3 holds 16 digits, 0.1e0 + 0.2 is the double sum, 3.14 + 1e0 is not 4.14
    CHECK(text_of(ex, "SELECT CONCAT(2e0 / 3, '|', 1 / 3e0, '|', 10 / 4e0, '|', 0.1e0 + 0.2, '|', 1e0, '|', 1e3, '|', 1.5e3, '|', -1e0, '|', 1e0 + 0, '|', 1e3 + 1)") ==
          "0.6666666666666666|0.3333333333333333|2.5|0.30000000000000004|1|1000|1500|-1|1|1001");
    CHECK(text_of(ex, "SELECT CONCAT(n + 1e0, '|', n * 1e0, '|', i + 1e0, '|', COALESCE(n, 1e0)) FROM fl WHERE id = 1") == "4.140000000000001|3.14|4|3.14");
    CHECK(text_of(ex, "SELECT CONCAT(COALESCE(n, 1e0), '') FROM fl WHERE id = 4") == "1"); // (a DOUBLE: not 1.00)
    // ROUND of a DOUBLE rounds half to even, of a DECIMAL half away from zero
    CHECK(rows_of(ex, "SELECT ROUND(2.5e0), ROUND(3.5e0), ROUND(4.5e0), ROUND(-2.5e0), ROUND(2.5), TRUNCATE(2.55e0, 1), FORMAT(2.5e0, 0), FORMAT(2.5, 0)")[0] ==
          std::vector<std::string>{"2", "4", "4", "-2", "3", "2.5", "2", "3"});
    CHECK(rows_of(ex, "SELECT 1e0 = 1, 1e0 IN (1), 1e0 BETWEEN 0 AND 1, -(1e0), MOD(10e0, 3), 10e0 DIV 3")[0] == std::vector<std::string>{"1", "1", "1", "-1", "1", "3"});
}

// R96c -- the names HAVING and WHERE read (the row of RECOMMAND.md "a name HAVING cannot see is 1054, not 1052"), widened to the family: HAVING with no GROUP BY and no aggregate, the select list's
// names, an aggregate in HAVING, a select-list alias in WHERE / ON / the select list. The answers are the ones a real MySQL 8.4.9 gives (corpus/having_names_r96c.sql compares them).
TEST_CASE("R96c: HAVING reads the names of the select list and the columns grouped by, WHERE does not read an alias", "[r96][having]") {
    TempDataDir dir("r96c_having_names");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ua (x INT, a INT)");
    ok(ex, "CREATE TABLE ub (x INT, y INT, b INT)");
    ok(ex, "INSERT INTO ua VALUES (1, 10), (2, 20), (3, 30)");
    ok(ex, "INSERT INTO ub VALUES (1, 5, 100), (2, 6, 200), (3, 5, 300)");
    const std::string join = " FROM ua JOIN ub ON ua.x = ub.x";
    // a HAVING with no aggregate and no GROUP BY filters rows; an alias of the select list is read before a column
    CHECK(rows_of(ex, "SELECT a FROM ua HAVING a > 10 ORDER BY a") == column({"20", "30"}));
    CHECK(rows_of(ex, "SELECT x AS k FROM ua HAVING k > 1 ORDER BY k") == column({"2", "3"}));
    CHECK(rows_of(ex, "SELECT x AS k FROM ua HAVING x > 1 ORDER BY k") == column({"2", "3"})); // (the source of an alias is a name of the select list too)
    CHECK(rows_of(ex, "SELECT a + 1 AS k FROM ua HAVING k > 11 ORDER BY k") == column({"21", "31"}));
    CHECK(rows_of(ex, "SELECT * FROM ua HAVING x > 1 ORDER BY x")[0] == std::vector<std::string>{"2", "20"});
    CHECK(rows_of(ex, "SELECT ua.* FROM ua JOIN ub ON ua.x = ub.x HAVING x > 2")[0] == std::vector<std::string>{"3", "30"}); // (`t.*` gives its one x)
    CHECK(error_of(ex, "SELECT a FROM ua HAVING SUM(x) > 0").find("In aggregated query without GROUP BY") == 0); // (an aggregate makes it an aggregated query)
    // any other name is unknown (1054), even one two tables have; a column grouped by is a name of HAVING
    CHECK(error_of(ex, "SELECT a FROM ua HAVING x > 1").find("Unknown column 'x' in 'having clause'") == 0);
    CHECK(error_of(ex, "SELECT a + 1 FROM ua HAVING a > 11").find("Unknown column 'a' in 'having clause'") == 0);
    CHECK(error_of(ex, "SELECT a" + join + " HAVING x > 0").find("Unknown column 'x' in 'having clause'") == 0);
    CHECK(error_of(ex, "SELECT a" + join + " HAVING ua.x > 0").find("Unknown column 'ua.x' in 'having clause'") == 0);
    CHECK(error_of(ex, "SELECT COUNT(*) FROM ua GROUP BY x HAVING a > 1").find("Unknown column 'a' in 'having clause'") == 0);
    CHECK(rows_of(ex, "SELECT COUNT(*) FROM ua GROUP BY x HAVING x > 1 ORDER BY 1").size() == 2);
    // two select items of one name are ambiguous (1052); an aggregate in HAVING reads the tables
    CHECK(error_of(ex, "SELECT ua.x, ub.x" + join + " HAVING x > 0").find("Column 'x' in having clause is ambiguous") == 0);
    CHECK(error_of(ex, "SELECT a, COUNT(*)" + join + " GROUP BY a HAVING COUNT(x) > 0").find("Column 'x' in having clause is ambiguous") == 0);
    CHECK(rows_of(ex, "SELECT a AS x" + join + " HAVING x > 0").size() == 3); // (an alias wins over the two columns)
    // a select-list alias is no name of WHERE, ON or the select list itself
    CHECK(error_of(ex, "SELECT x AS k FROM ua WHERE k > 1").find("Unknown column 'k' in 'where clause'") == 0);
    CHECK(error_of(ex, "SELECT a + 1 AS k FROM ua WHERE k > 11").find("Unknown column 'k' in 'where clause'") == 0);
    CHECK(error_of(ex, "SELECT x AS k FROM ua WHERE EXISTS (SELECT 1 FROM ua u2 WHERE u2.x = k)").find("Unknown column 'k' in 'where clause'") == 0);
    CHECK(error_of(ex, "SELECT x AS k, k + 1 FROM ua").find("Unknown column 'k' in 'field list'") == 0);
    CHECK(rows_of(ex, "SELECT x AS k FROM ua ORDER BY k DESC") == column({"3", "2", "1"})); // (ORDER BY and GROUP BY read aliases)
    CHECK(rows_of(ex, "SELECT * FROM (SELECT x AS k FROM ua) d WHERE k > 1 ORDER BY k") == column({"2", "3"})); // (a derived table's column is a column)
    CHECK(rows_of(ex, "SELECT a AS x FROM ua WHERE x > 1 ORDER BY a") == column({"20", "30"})); // (WHERE reads the column x, not the alias)
}

// R97 -- a sweep of statement grids (functions x edge values, compared with a real MySQL 8.4.9) found these; the answers below are the ones MySQL writes (corpus/grid_r97.sql compares more).
TEST_CASE("R97: typed literals, <=>, BETWEEN of constants, LOG / RADIANS / POW, CAST of an exponent, REGEXP '', bit operators of texts", "[r97]") {
    TempDataDir dir("r97_grid_fixes");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // DATE '..', TIME '..' and TIMESTAMP '..' are literals of that type
    CHECK(text_of(ex, "SELECT DATE '2024-01-31'") == "2024-01-31");
    CHECK(text_of(ex, "SELECT TIME '10:20:30'") == "10:20:30");
    CHECK(text_of(ex, "SELECT TIMESTAMP '2024-01-31 10:20:30'") == "2024-01-31 10:20:30");
    CHECK(text_of(ex, "SELECT TIMESTAMP '2024-01-31 10:20:30.5'") == "2024-01-31 10:20:30.5");
    CHECK(text_of(ex, "SELECT DATE '2024-01-31' + INTERVAL 1 DAY") == "2024-02-01");
    CHECK(text_of(ex, "SELECT DATEDIFF(DATE '2024-02-01', DATE '2024-01-01')") == "31");
    CHECK(text_of(ex, "SELECT DATE '2024-01-31' IN (DATE '2024-01-31')") == "1");
    CHECK(text_of(ex, "SELECT GREATEST(DATE '2024-01-01', DATE '2024-02-01')") == "2024-02-01");
    CHECK(text_of(ex, "SELECT COALESCE(NULL, DATE '2024-01-01')") == "2024-01-01");
    CHECK(text_of(ex, "SELECT IF(1, DATE '2024-01-01', DATE '2024-02-01')") == "2024-01-01");
    CHECK(text_of(ex, "SELECT CASE DATE '2024-01-31' WHEN DATE '2024-01-31' THEN 'm' ELSE 'n' END") == "m");
    CHECK(text_of(ex, "SELECT DATE '2024-01-01' + 0") == "20240101");
    CHECK(text_of(ex, "SELECT TIME '10:20:30' + 0") == "102030");
    CHECK(text_of(ex, "SELECT TIMESTAMP '2024-01-01 10:20:30' + 0") == "20240101102030");
    CHECK(text_of(ex, "SELECT 1 < DATE '2024-01-01'") == "1");
    CHECK(text_of(ex, "SELECT 0 = DATE '2024-01-01'") == "0");
    CHECK(text_of(ex, "SELECT 20240101 = DATE '2024-01-01'") == "1");
    CHECK(text_of(ex, "SELECT DATE '2024-01-31' <=> '2024-01-31'") == "1");
    CHECK(error_of(ex, "SELECT DATE '2024-13-01'").find("Incorrect DATE value: '2024-13-01'") == 0);
    CHECK(error_of(ex, "SELECT DATE 'x'").find("Incorrect DATE value: 'x'") == 0);
    CHECK(error_of(ex, "SELECT TIME 'x'").find("Incorrect TIME value: 'x'") == 0);
    // <=> reads a text and a number as numbers, as = does
    CHECK(rows_of(ex, "SELECT 0 <=> 'a', 1 <=> '1abc', '1' <=> 1, '1.0' <=> '1', NULL <=> NULL, 1 <=> NULL, NULL <=> 'a'")[0] == std::vector<std::string>{"1", "1", "1", "0", "1", "0", "0"});
    CHECK(rows_of(ex, "SELECT 0 = 'a', 0 <=> '', 1.5 <=> '1.5', '1.5' <=> 1.5")[0] == std::vector<std::string>{"1", "1", "1", "1"});
    // three constants of different types are compared as numbers
    CHECK(rows_of(ex, "SELECT '1' BETWEEN '1.0' AND 5, '1' BETWEEN 'a' AND 5, 10 BETWEEN '9' AND '11', '10' BETWEEN '9' AND '11', '10' BETWEEN '9' AND 11")[0] == std::vector<std::string>{"1", "1", "1", "0", "1"});
    // GREATEST / LEAST of values that are equal
    CHECK(rows_of(ex, "SELECT GREATEST('a', 'A'), GREATEST('A', 'a'), LEAST('a', 'A'), LEAST('A', 'a')")[0] == std::vector<std::string>{"A", "a", "a", "A"});
    // LOG with a base below 1, RADIANS and DEGREES as MySQL multiplies, POW out of range
    CHECK(rows_of(ex, "SELECT LOG(0.1, 100), LOG(0.5, 8), LOG(2, 8), LOG(1, 8), LOG(0, 8), LOG(-2, 8)")[0] == std::vector<std::string>{"-2.0000000000000004", "-3", "3", "NULL", "NULL", "NULL"});
    CHECK(rows_of(ex, "SELECT RADIANS(1.5), RADIANS(1e10), DEGREES(0.1), DEGREES(5.5)")[0] == std::vector<std::string>{"0.026179938779914945", "174532925.19943297", "5.729577951308233", "315.12678732195275"});
    CHECK(error_of(ex, "SELECT POW(-1, 1.5)").find("DOUBLE value is out of range in '") == 0);
    CHECK(error_of(ex, "SELECT POW(0, -1)").find("DOUBLE value is out of range in '") == 0);
    CHECK(error_of(ex, "SELECT EXP(1e10)").find("DOUBLE value is out of range in '") == 0);
    CHECK(error_of(ex, "SELECT COT(0)").find("DOUBLE value is out of range in '") == 0);
    // an exponent in a text read as a DECIMAL, not as an integer
    CHECK(rows_of(ex, "SELECT CAST('4e2' AS DECIMAL), CAST('4e2' AS DECIMAL(10,2)), CAST('4e2' AS SIGNED), CAST('1.5e1' AS DECIMAL(10,1)), CAST('1e-2' AS DECIMAL(10,3))")[0] == std::vector<std::string>{"400", "400.00", "4", "15.0", "0.010"});
    // REGEXP with an empty pattern, FIND_IN_SET of an empty list
    CHECK(error_of(ex, "SELECT 'a' REGEXP ''").find("Illegal argument to a regular expression") == 0);
    CHECK(rows_of(ex, "SELECT FIND_IN_SET('', ''), FIND_IN_SET('a', ''), FIND_IN_SET('', 'a,,b'), FIND_IN_SET('b', 'a,b')")[0] == std::vector<std::string>{"0", "0", "2", "2"});
    // a text is read as an integer up to its point by the bit operators
    CHECK(rows_of(ex, "SELECT '5.5abc' | 0, 0 ^ '5.5abc', 1 << '5.5abc', 5.5 | 0, -1 >> '5.5abc', ~'5.5abc'")[0] == std::vector<std::string>{"5", "5", "32", "6", "576460752303423487", "18446744073709551610"});
    // DIV and MOD of NULL
    CHECK(rows_of(ex, "SELECT 5 DIV NULL, NULL DIV 5, 5 MOD NULL, 7 DIV 2, 7 DIV -2")[0] == std::vector<std::string>{"NULL", "NULL", "NULL", "3", "-3"});
}

// R97 -- BINARY(n), VARBINARY(n), BIT(n), ZEROFILL and CHAR(n) BINARY were no column types (CREATE TABLE said 1064). They are read now as the CHAR(n) / VARCHAR(n) that holds bytes and the unsigned
// integer (no binary collation, no 0x00 padding, no BIT width; a byte that is no UTF-8 cannot be stored by any column yet).
TEST_CASE("R97: BINARY, VARBINARY, BIT, ZEROFILL and CHAR BINARY columns are accepted", "[r97]") {
    TempDataDir dir("r97_binary_types");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE bt (id BINARY(16) PRIMARY KEY, name VARCHAR(20), h VARBINARY(8), flag BIT(1), n INT(5) ZEROFILL, c CHAR(3) BINARY, w BIT(8))");
    ok(ex, "INSERT INTO bt VALUES (UNHEX('00112233445566770102030405060708'), 'a', UNHEX('0A0B'), 1, 7, 'abc', 5)");
    ok(ex, "INSERT INTO bt VALUES (UNHEX('7F7E7D7C7B7A79780102030405060708'), 'b', UNHEX('0A0C'), 0, 8, 'xyz', 255)");
    CHECK(error_of(ex, "INSERT INTO bt VALUES (UNHEX('00112233445566770102030405060708'), 'dup', NULL, 1, 9, 'q', 1)").find("Duplicate") == 0);
    CHECK(rows_of(ex, "SELECT HEX(id), name, HEX(h), flag + 0, n, w + 0 FROM bt ORDER BY name")[1] == std::vector<std::string>{"7F7E7D7C7B7A79780102030405060708", "b", "0A0C", "0", "8", "255"});
    CHECK(text_of(ex, "SELECT name FROM bt WHERE id = UNHEX('7F7E7D7C7B7A79780102030405060708')") == "b");
    CHECK(text_of(ex, "SELECT name FROM bt WHERE h = UNHEX('0A0B')") == "a");
    CHECK(text_of(ex, "SELECT COUNT(*) FROM bt WHERE flag") == "1");
    CHECK(text_of(ex, "SELECT HEX(MAX(id)) FROM bt") == "7F7E7D7C7B7A79780102030405060708");
    ok(ex, "CREATE TABLE ty (a TINYINT ZEROFILL, b INT UNSIGNED ZEROFILL, c VARCHAR(5) BINARY, d BIT)");
}
