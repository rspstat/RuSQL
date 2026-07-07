#include <filesystem>

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

// Runs `SELECT <expr> AS v FROM t` (t has exactly one row) and returns the "v" cell.
std::string eval_expr(Executor& ex, const std::string& expr) {
    auto r = ex.execute_sql("SELECT " + expr + " AS v FROM t");
    REQUIRE(r.is_ok());
    std::string out = r.value();
    auto header_end = out.find('\n');
    auto data_start = out.find('\n', header_end + 1) + 1;
    // Row line looks like "| <value> |\n" for a single-column result.
    auto bar1 = out.find('|', data_start);
    auto bar2 = out.find('|', bar1 + 1);
    std::string cell = out.substr(bar1 + 1, bar2 - bar1 - 1);
    auto a = cell.find_first_not_of(' ');
    auto b = cell.find_last_not_of(' ');
    return a == std::string::npos ? "" : cell.substr(a, b - a + 1);
}
} // namespace

TEST_CASE("Scalar string functions", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    REQUIRE(eval_expr(ex, "UPPER('hello')") == "HELLO");
    REQUIRE(eval_expr(ex, "LOWER('HELLO')") == "hello");
    REQUIRE(eval_expr(ex, "LENGTH('hello')") == "5");
    REQUIRE(eval_expr(ex, "TRIM('  hi  ')") == "hi");
    REQUIRE(eval_expr(ex, "CONCAT('a','b','c')") == "abc");
    REQUIRE(eval_expr(ex, "CONCAT_WS(',','a','b')") == "a,b");
    REQUIRE(eval_expr(ex, "SUBSTR('hello',2,3)") == "ell");
    REQUIRE(eval_expr(ex, "REPLACE('foobar','foo','baz')") == "bazbar");
    REQUIRE(eval_expr(ex, "REVERSE('abc')") == "cba");
    REQUIRE(eval_expr(ex, "REPEAT('ab',3)") == "ababab");
    REQUIRE(eval_expr(ex, "LEFT('hello',3)") == "hel");
    REQUIRE(eval_expr(ex, "RIGHT('hello',3)") == "llo");
    REQUIRE(eval_expr(ex, "LTRIM('  hi')") == "hi");
    REQUIRE(eval_expr(ex, "RTRIM('hi  ')") == "hi");
    REQUIRE(eval_expr(ex, "LPAD('5',3,'0')") == "005");
    REQUIRE(eval_expr(ex, "RPAD('5',3,'0')") == "500");
    REQUIRE(eval_expr(ex, "INSTR('hello world','world')") == "7");
    REQUIRE(eval_expr(ex, "CHAR_LENGTH('hello')") == "5");
}

TEST_CASE("Scalar conditional/null-handling functions", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 200000)").is_ok());

    REQUIRE(eval_expr(ex, "COALESCE(NULL, NULL, 'x')") == "x");
    REQUIRE(eval_expr(ex, "IFNULL(NULL, 'y')") == "y");
    // IF's first argument must be a comparison expression in this SQL dialect's grammar
    // (a pre-existing parser constraint from Phase 4, not something this phase changes).
    REQUIRE(eval_expr(ex, "IF(salary>100000, 'yes', 'no')") == "yes");
    REQUIRE(eval_expr(ex, "IF(salary<100000, 'yes', 'no')") == "no");
    REQUIRE(eval_expr(ex, "NULLIF('a','a')") == "NULL");
    REQUIRE(eval_expr(ex, "ISNULL(NULL)") == "1");
    REQUIRE(eval_expr(ex, "GREATEST(3,7,2)") == "7");
    REQUIRE(eval_expr(ex, "LEAST(3,7,2)") == "2");
}

TEST_CASE("Scalar math functions", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    REQUIRE(eval_expr(ex, "ABS(-5)") == "5");
    REQUIRE(eval_expr(ex, "CEIL(4.1)") == "5");
    REQUIRE(eval_expr(ex, "FLOOR(4.9)") == "4");
    REQUIRE(eval_expr(ex, "ROUND(3.14159, 2)") == "3.14");
    REQUIRE(eval_expr(ex, "MOD(10, 3)") == "1");
    REQUIRE(eval_expr(ex, "SIGN(-3)") == "-1");
    REQUIRE(eval_expr(ex, "POWER(2, 10)") == "1024");
}

TEST_CASE("MD5 matches a known reference hash", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    // MD5("") is a well-known reference value.
    REQUIRE(eval_expr(ex, "MD5('')") == "d41d8cd98f00b204e9800998ecf8427e");
    // MD5("hello") is likewise well-known.
    REQUIRE(eval_expr(ex, "MD5('hello')") == "5d41402abc4b2a76b9719d911017c592");
}

TEST_CASE("Date functions", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    REQUIRE(eval_expr(ex, "YEAR('2024-03-15')") == "2024");
    REQUIRE(eval_expr(ex, "MONTH('2024-03-15')") == "03");
    REQUIRE(eval_expr(ex, "DAY('2024-03-15')") == "15");
    REQUIRE(eval_expr(ex, "DATEDIFF('2024-03-15','2024-03-01')") == "14");
    // DATE_ADD/DATE_SUB use MySQL's DATE_ADD(date, INTERVAL n unit) grammar (a special
    // parse path, not a plain function-call arg list).
    REQUIRE(eval_expr(ex, "DATE_ADD('2024-01-31', INTERVAL 1 MONTH)") == "2024-01-31"); // invalid Feb 31 -> falls back to original
    REQUIRE(eval_expr(ex, "DATE_ADD('2024-01-15', INTERVAL 1 MONTH)") == "2024-02-15");
    REQUIRE(eval_expr(ex, "DATE_ADD('2024-03-10', INTERVAL 5 DAY)") == "2024-03-15");
    REQUIRE(eval_expr(ex, "DATE_SUB('2024-03-15', INTERVAL 5 DAY)") == "2024-03-10");
    REQUIRE(eval_expr(ex, "LAST_DAY('2024-02-10')") == "2024-02-29"); // 2024 is a leap year
}

TEST_CASE("CAST converts between types", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    REQUIRE(eval_expr(ex, "CAST('42' AS INT)") == "42");
    REQUIRE(eval_expr(ex, "CAST(3.99 AS INT)") == "3");
    REQUIRE(eval_expr(ex, "CAST('3.5' AS FLOAT)") == "3.5");
    REQUIRE(eval_expr(ex, "CAST(1 AS BOOLEAN)") == "1");
}

TEST_CASE("JSON_EXTRACT reads nested paths and array indices", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, data VARCHAR(200))").is_ok());
    REQUIRE(ex.execute_sql(R"(INSERT INTO t VALUES (1, '{"name":"Alice","tags":["a","b","c"],"addr":{"city":"Seoul"}}'))").is_ok());

    auto r1 = ex.execute_sql("SELECT JSON_EXTRACT(data, '$.name') AS v FROM t");
    REQUIRE(r1.is_ok());
    REQUIRE(r1.value().find("\"Alice\"") != std::string::npos);

    auto r2 = ex.execute_sql("SELECT JSON_EXTRACT(data, '$.addr.city') AS v FROM t");
    REQUIRE(r2.is_ok());
    REQUIRE(r2.value().find("\"Seoul\"") != std::string::npos);

    auto r3 = ex.execute_sql("SELECT JSON_EXTRACT(data, '$.tags[1]') AS v FROM t");
    REQUIRE(r3.is_ok());
    REQUIRE(r3.value().find("\"b\"") != std::string::npos);

    auto r4 = ex.execute_sql("SELECT JSON_UNQUOTE(JSON_EXTRACT(data, '$.name')) AS v FROM t");
    REQUIRE(r4.is_ok());
    REQUIRE(r4.value().find("Alice") != std::string::npos);
    REQUIRE(r4.value().find("\"Alice\"") == std::string::npos);
}

TEST_CASE("REGEXP scalar functions", "[executor][scalar_func]") {
    TempDataDir dir("exec_sf_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    REQUIRE(eval_expr(ex, "REGEXP_LIKE('hello123', '[0-9]+')") == "1");
    REQUIRE(eval_expr(ex, "REGEXP_LIKE('hello', '[0-9]+')") == "0");
    REQUIRE(eval_expr(ex, "REGEXP_REPLACE('hello123world', '[0-9]+', '-')") == "hello-world");
}
