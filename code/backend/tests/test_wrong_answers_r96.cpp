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
