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

TEST_CASE("SUM/COUNT over a comparison expression", "[executor][select]") {
    // Regression: the aggregate-argument parser only accepted '*' or a bare column
    // identifier, so SUM(col > x) (a common "conditional sum" idiom, e.g. counting rows
    // matching a predicate) failed to parse with "Expected ')' after aggregate". Fixed by
    // reusing the same predicate-tail parser WHERE uses, converting it into the existing
    // SumCase/CountCase machinery (the same one already used for SUM(col IS NULL)).
    TempDataDir dir("exec_sel_data_7b");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, age INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,30),(2,25),(3,40)").is_ok());

    auto r1 = ex.execute_sql("SELECT SUM(age > 26) AS cnt FROM emp");
    REQUIRE(r1.is_ok());
    REQUIRE(r1.value().find("2") != std::string::npos); // ages 30 and 40 qualify

    auto r2 = ex.execute_sql("SELECT COUNT(age >= 30) AS cnt FROM emp");
    REQUIRE(r2.is_ok());
    REQUIRE(r2.value().find("2") != std::string::npos);
}

TEST_CASE("SELECT with BIT_AND/BIT_OR aggregate functions", "[executor][select]") {
    TempDataDir dir("exec_sel_data_bitagg");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, flags INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,6),(2,3),(3,7)").is_ok()); // 110, 011, 111

    auto and_r = ex.execute_sql("SELECT BIT_AND(flags) AS a FROM t");
    REQUIRE(and_r.is_ok());
    REQUIRE(and_r.value().find("| 2 ") != std::string::npos); // 110 & 011 & 111 = 010 = 2

    auto or_r = ex.execute_sql("SELECT BIT_OR(flags) AS o FROM t");
    REQUIRE(or_r.is_ok());
    REQUIRE(or_r.value().find("| 7 ") != std::string::npos); // 110 | 011 | 111 = 111 = 7

    // Per-group correctness (compute_aggregates is also called once per GROUP BY group).
    REQUIRE(ex.execute_sql("CREATE TABLE g (id INT PRIMARY KEY, grp INT, flags INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO g VALUES (1,1,6),(2,1,2),(3,2,5),(4,2,1)").is_ok());
    auto grouped = ex.execute_sql("SELECT grp, BIT_AND(flags) AS a FROM g GROUP BY grp ORDER BY grp");
    REQUIRE(grouped.is_ok());
    REQUIRE(grouped.value().find("2") != std::string::npos); // group 1: 6 & 2 = 2
    REQUIRE(grouped.value().find("1") != std::string::npos); // group 2: 5 & 1 = 1
}

TEST_CASE("SELECT with FILTER (WHERE ...) narrows only that one aggregate", "[executor][select]") {
    TempDataDir dir("exec_sel_data_aggfilter");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, status VARCHAR(10), val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,'active',10),(2,'active',20),(3,'inactive',30)").is_ok());

    // A filtered and an unfiltered aggregate in the SAME SELECT -- the filter must not
    // leak into the unfiltered one (this is exactly what a shared, unfiltered `grp`
    // reused across every SelectColumn in one compute_aggregates() call would get wrong).
    auto r = ex.execute_sql("SELECT COUNT(*) AS total, COUNT(*) FILTER (WHERE status = 'active') AS active_count FROM t");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("| 3 ") != std::string::npos); // total unaffected by the filter
    REQUIRE(r.value().find("| 2 ") != std::string::npos); // active_count only counts the 2 active rows

    auto sum_r = ex.execute_sql("SELECT SUM(val) FILTER (WHERE status = 'active') AS active_sum FROM t");
    REQUIRE(sum_r.is_ok());
    REQUIRE(sum_r.value().find("30") != std::string::npos); // 10 + 20, row 3 (val=30) excluded by the filter

    // GROUP BY interaction: the filter applies within each group independently -- group 1
    // has 3 active rows (a distinctive count), group 2 has zero (the filter should be able
    // to legitimately produce 0, not silently fall back to the group's full unfiltered set).
    REQUIRE(ex.execute_sql("CREATE TABLE g (id INT PRIMARY KEY, grp INT, status VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO g VALUES "
                            "(1,1,'active'),(2,1,'active'),(3,1,'active'),(4,1,'inactive'),"
                            "(5,2,'inactive'),(6,2,'inactive')")
                .is_ok());
    auto grouped = ex.execute_sql("SELECT grp, COUNT(*) FILTER (WHERE status = 'active') AS c FROM g GROUP BY grp ORDER BY grp");
    REQUIRE(grouped.is_ok());
    REQUIRE(grouped.value().find("2 row(s) returned.") != std::string::npos);
    REQUIRE(grouped.value().find("| 1   | 3 |") != std::string::npos); // group 1: 3 active
    REQUIRE(grouped.value().find("| 2   | 0 |") != std::string::npos); // group 2: 0 active
}

TEST_CASE("SELECT with JSON_AGG aggregate function", "[executor][select]") {
    TempDataDir dir("exec_sel_data_jsonagg");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT, label VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,10,'a'),(2,20,'b'),(3,30,NULL)").is_ok());

    auto num_r = ex.execute_sql("SELECT JSON_AGG(val) AS j FROM t");
    REQUIRE(num_r.is_ok());
    REQUIRE(num_r.value().find("[10,20,30]") != std::string::npos); // clean integers, no ".0" suffix

    auto str_r = ex.execute_sql("SELECT JSON_AGG(label) AS j FROM t");
    REQUIRE(str_r.is_ok());
    REQUIRE(str_r.value().find("\"a\"") != std::string::npos);
    REQUIRE(str_r.value().find("\"b\"") != std::string::npos);
    REQUIRE(str_r.value().find("null") != std::string::npos); // NULL label becomes JSON null
}

TEST_CASE("SELECT with JOIN LATERAL correlated subquery (INNER)", "[executor][select]") {
    TempDataDir dir("exec_sel_data_lateral_inner");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO customers VALUES (1,'Alice'),(2,'Bob'),(3,'Carol')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO orders VALUES (1,1,100),(2,1,300),(3,1,200),(4,2,50)").is_ok());

    // Carol has no orders -- INNER JOIN LATERAL must drop her row entirely.
    auto r = ex.execute_sql(
        "SELECT c.name, o.total FROM customers c JOIN LATERAL "
        "(SELECT total FROM orders WHERE customer_id = c.id ORDER BY total DESC LIMIT 1) o ON 1=1");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Alice") != std::string::npos);
    REQUIRE(r.value().find("300") != std::string::npos); // Alice's top order, per-row correlated re-eval
    REQUIRE(r.value().find("Bob") != std::string::npos);
    REQUIRE(r.value().find("50") != std::string::npos);
    REQUIRE(r.value().find("Carol") == std::string::npos);
}

TEST_CASE("SELECT with LEFT JOIN LATERAL fills NULL when subquery returns no rows", "[executor][select]") {
    TempDataDir dir("exec_sel_data_lateral_left");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO customers VALUES (1,'Alice'),(2,'Bob'),(3,'Carol')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO orders VALUES (1,1,100),(2,1,300),(3,1,200),(4,2,50)").is_ok());

    auto r = ex.execute_sql(
        "SELECT c.name, o.total FROM customers c LEFT JOIN LATERAL "
        "(SELECT total FROM orders WHERE customer_id = c.id ORDER BY total DESC LIMIT 1) o ON 1=1");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("3 row(s) returned") != std::string::npos); // Carol kept, unlike INNER
    REQUIRE(r.value().find("Carol") != std::string::npos);
    REQUIRE(r.value().find("NULL") != std::string::npos);
}

TEST_CASE("SELECT with CROSS JOIN LATERAL re-evaluates subquery per left row", "[executor][select]") {
    TempDataDir dir("exec_sel_data_lateral_cross");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO customers VALUES (1,'Alice'),(2,'Bob'),(3,'Carol')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO orders VALUES (1,1,100),(2,1,300),(3,1,200),(4,2,50)").is_ok());

    // CROSS JOIN LATERAL needs no ON clause; each left row gets its own subquery execution
    // (Alice: 3 orders, Bob: 1 order, Carol: 0 orders -> 4 rows total).
    auto r = ex.execute_sql(
        "SELECT c.name, o.total FROM customers c CROSS JOIN LATERAL "
        "(SELECT total FROM orders WHERE customer_id = c.id) o");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("4 row(s) returned") != std::string::npos);
    REQUIRE(r.value().find("Carol") == std::string::npos);
}

TEST_CASE("JOIN LATERAL rejects RIGHT/NATURAL/FULL join types", "[executor][select]") {
    TempDataDir dir("exec_sel_data_lateral_reject");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total INT)").is_ok());

    REQUIRE(ex.execute_sql("SELECT c.name FROM customers c RIGHT JOIN LATERAL "
                            "(SELECT total FROM orders WHERE customer_id = c.id) o ON 1=1")
                .is_err());
    REQUIRE(ex.execute_sql("SELECT c.name FROM customers c NATURAL JOIN LATERAL "
                            "(SELECT total FROM orders WHERE customer_id = c.id) o")
                .is_err());
    REQUIRE(ex.execute_sql("SELECT c.name FROM customers c FULL JOIN LATERAL "
                            "(SELECT total FROM orders WHERE customer_id = c.id) o ON 1=1")
                .is_err());
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
