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
} // namespace

TEST_CASE("SELECT * with WHERE returns matching rows", "[executor][select]") {
    TempDataDir dir("exec_sel_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(50), salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,'Alice',1000),(2,'Bob',2000),(3,'Carol',3000)").is_ok());

    auto r = ex.execute_sql("SELECT * FROM emp WHERE salary > 1500");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Bob") != std::string::npos);
    REQUIRE(r.value().find("Carol") != std::string::npos);
    REQUIRE(r.value().find("Alice") == std::string::npos);
    REQUIRE(r.value().find("2 row(s) returned.") != std::string::npos);
}

TEST_CASE("WHERE supports an arithmetic expression on the right-hand side", "[executor][select]") {
    // PLAN.md P0 regression test: the RHS of a comparison used to parse a single
    // token, so `WHERE v > id + 100` silently dropped `+ 100` and evaluated as
    // `v > id`. id=1,v=50: 50 > 101 is false. id=2,v=150: 150 > 102 is true.
    TempDataDir dir("exec_sel_data_where_arith");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, v INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,50),(2,150)").is_ok());

    auto r = ex.execute_sql("SELECT id, v FROM t WHERE v > id + 100");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
    REQUIRE(r.value().find("150") != std::string::npos);
}

TEST_CASE("Double unary minus evaluates correctly instead of producing the literal string \"--N\"",
          "[executor][select]") {
    // PLAN.md P0 regression test: the lexer folds a `-` immediately followed by a
    // digit into a single negative NumberLit token whenever the preceding token
    // isn't itself a value (see lexer.cpp) -- which is exactly what happens to the
    // second `-` in `- -5` (the first bare Minus operator isn't a "value", so "-5"
    // lexes as one already-negative token). Several parser call sites then blindly
    // prepended another '-' onto that already-negative text, turning `- -5` into
    // the literal string "--5" instead of the correctly re-negated "5".
    TempDataDir dir("exec_sel_data_double_unary_minus");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, v INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,5),(2,10),(3,-5)").is_ok());

    auto r_scalar = ex.execute_sql("SELECT - -5 AS a");
    REQUIRE(r_scalar.is_ok());
    REQUIRE(r_scalar.value().find("| 5 |") != std::string::npos);
    REQUIRE(r_scalar.value().find("--5") == std::string::npos);

    auto r_eq = ex.execute_sql("SELECT id FROM t WHERE v = - -5");
    REQUIRE(r_eq.is_ok());
    REQUIRE(r_eq.value().find("1 row(s) returned.") != std::string::npos);

    auto r_between = ex.execute_sql("SELECT id FROM t WHERE v BETWEEN - -5 AND 10");
    REQUIRE(r_between.is_ok());
    REQUIRE(r_between.value().find("2 row(s) returned.") != std::string::npos);

    auto r_in = ex.execute_sql("SELECT id FROM t WHERE v IN (- -5, 10)");
    REQUIRE(r_in.is_ok());
    REQUIRE(r_in.value().find("2 row(s) returned.") != std::string::npos);

    auto r_interval = ex.execute_sql("SELECT DATE_ADD('2024-01-10', INTERVAL - -3 DAY) AS d");
    REQUIRE(r_interval.is_ok());
    REQUIRE(r_interval.value().find("2024-01-13") != std::string::npos);
}

TEST_CASE("WHERE with a simple RHS still resolves to a PK index access path", "[executor][select]") {
    // Regression guard for the arithmetic-RHS fix above: simple comparisons
    // (`WHERE id = 2`) must still reduce to ConditionValue::Literal so the
    // Planner's PK/secondary-index access-path selection keeps working, instead
    // of every comparison going through the new, more general Arith path.
    TempDataDir dir("exec_sel_data_where_simple_still_indexed");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, v INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,50),(2,150)").is_ok());

    auto ex_plan = ex.execute_sql("EXPLAIN SELECT * FROM t WHERE id = 2");
    REQUIRE(ex_plan.is_ok());
    REQUIRE(ex_plan.value().find("Index Scan") != std::string::npos);
}

TEST_CASE("Correlated subquery outer reference works inside an arithmetic RHS expression", "[executor][subquery]") {
    // PLAN.md P0 fix follow-up: substitute_correlated_condexpr must substitute an
    // outer-row column reference wherever it appears inside a ConditionValue::Arith
    // tree (e.g. `d.id = e.dept_id + 0`), not just when the whole RHS is a bare
    // outer-column reference.
    TempDataDir dir("exec_sel_data_correlated_arith");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, dept_id INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept VALUES (1),(2)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,1),(2,2),(3,1)").is_ok());

    auto r = ex.execute_sql("SELECT id FROM emp e WHERE EXISTS (SELECT 1 FROM dept d WHERE d.id = e.dept_id + 0)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("3 row(s) returned.") != std::string::npos);
}

TEST_CASE("SELECT with explicit column list and alias", "[executor][select]") {
    TempDataDir dir("exec_sel_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, first_name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,'Alice')").is_ok());

    auto r = ex.execute_sql("SELECT first_name AS name FROM emp");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("name") != std::string::npos);
    REQUIRE(r.value().find("Alice") != std::string::npos);
}

TEST_CASE("SELECT with no matching rows returns 0 rows returned", "[executor][select]") {
    TempDataDir dir("exec_sel_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1)").is_ok());

    auto r = ex.execute_sql("SELECT * FROM emp WHERE id = 999");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "0 rows returned.");
}

TEST_CASE("SELECT ORDER BY / LIMIT / OFFSET", "[executor][select]") {
    TempDataDir dir("exec_sel_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,50),(2,10),(3,30),(4,20),(5,40)").is_ok());

    auto r = ex.execute_sql("SELECT id FROM t ORDER BY val ASC LIMIT 2 OFFSET 1");
    REQUIRE(r.is_ok());
    // Sorted by val ascending: id=2(10), id=4(20), id=3(30), id=5(40), id=1(50)
    // offset 1, limit 2 -> id=4, id=3
    auto v = r.value();
    auto pos4 = v.find("| 4"); // first column value cell for id=4
    auto pos3 = v.find("| 3");
    REQUIRE(pos4 != std::string::npos);
    REQUIRE(pos3 != std::string::npos);
    REQUIRE(pos4 < pos3);
    REQUIRE(v.find("| 2") == std::string::npos);
    REQUIRE(v.find("| 5") == std::string::npos);
    REQUIRE(v.find("| 1") == std::string::npos);
}

TEST_CASE("SELECT DISTINCT removes duplicate rows", "[executor][select]") {
    TempDataDir dir("exec_sel_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, kind VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,'a'),(2,'a'),(3,'b')").is_ok());

    auto r = ex.execute_sql("SELECT DISTINCT kind FROM t");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("2 row(s) returned.") != std::string::npos);
}

TEST_CASE("SELECT with aggregate functions and no GROUP BY", "[executor][select]") {
    TempDataDir dir("exec_sel_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,1000),(2,2000),(3,3000)").is_ok());

    auto r = ex.execute_sql("SELECT COUNT(*) AS cnt, SUM(salary) AS total, AVG(salary) AS avg_sal FROM emp");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("cnt") != std::string::npos);
    REQUIRE(r.value().find("3") != std::string::npos);
    REQUIRE(r.value().find("6000") != std::string::npos);
    REQUIRE(r.value().find("2000.0000") != std::string::npos);
}

TEST_CASE("SELECT with GROUP BY and HAVING", "[executor][select]") {
    TempDataDir dir("exec_sel_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, department_id INT, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,1,1000),(2,1,2000),(3,2,5000),(4,2,6000)").is_ok());

    auto r = ex.execute_sql("SELECT department_id, SUM(salary) AS total FROM emp GROUP BY department_id HAVING SUM(salary) > 5000");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("11000") != std::string::npos); // dept 2: 5000+6000
    REQUIRE(r.value().find("3000") == std::string::npos);  // dept 1 (1000+2000) filtered out by HAVING
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
}

TEST_CASE("SELECT with INNER JOIN", "[executor][select]") {
    TempDataDir dir("exec_sel_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'Eng'),(2,'Sales')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1,'Alice'),(2,2,'Bob')").is_ok());

    auto r = ex.execute_sql(
        "SELECT employee.name, department.name FROM employee JOIN department ON employee.department_id = department.id "
        "WHERE department.name = 'Eng'");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Alice") != std::string::npos);
    REQUIRE(r.value().find("Bob") == std::string::npos);
}

TEST_CASE("SELECT LEFT JOIN fills NULL for unmatched right rows", "[executor][select]") {
    TempDataDir dir("exec_sel_data_9");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'Eng')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,99)").is_ok()); // no matching department

    auto r = ex.execute_sql("SELECT * FROM employee LEFT JOIN department ON employee.department_id = department.id");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
}

TEST_CASE("SELECT against a view executes the view's stored query", "[executor][select]") {
    TempDataDir dir("exec_sel_data_10");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50), is_active VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'Eng','true'),(2,'Old','false')").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW v_active AS SELECT id, name FROM department WHERE is_active = 'true'").is_ok());

    auto r = ex.execute_sql("SELECT * FROM v_active");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Eng") != std::string::npos);
    REQUIRE(r.value().find("Old") == std::string::npos);
}

TEST_CASE("SELECT FOR UPDATE requires an active transaction", "[executor][select]") {
    TempDataDir dir("exec_sel_data_11");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    auto r = ex.execute_sql("SELECT * FROM t FOR UPDATE");
    REQUIRE(r.is_err());
    REQUIRE(r.error().find("active transaction") != std::string::npos);
}

TEST_CASE("Query cache serves repeated identical SELECTs and is invalidated by DML", "[executor][select]") {
    TempDataDir dir("exec_sel_data_12");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    auto r1 = ex.execute_sql("SELECT * FROM t");
    REQUIRE(r1.is_ok());
    REQUIRE(r1.value().find("100") != std::string::npos);

    REQUIRE(ex.execute_sql("UPDATE t SET val = 999 WHERE id = 1").is_ok());

    auto r2 = ex.execute_sql("SELECT * FROM t");
    REQUIRE(r2.is_ok());
    REQUIRE(r2.value().find("999") != std::string::npos);
    REQUIRE(r2.value().find("100") == std::string::npos);
}
