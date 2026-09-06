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

TEST_CASE("Basic non-recursive CTE materializes and is queryable", "[executor][cte]") {
    TempDataDir dir("exec_cte_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50), salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,'Alice',9000),(2,'Bob',3000)").is_ok());

    auto r = ex.execute_sql("WITH high_earner AS (SELECT id, name, salary FROM employee WHERE salary > 5000) SELECT * FROM high_earner");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Alice") != std::string::npos);
    REQUIRE(r.value().find("Bob") == std::string::npos);
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
}

TEST_CASE("CTE correctly round-trips a value containing an embedded newline", "[executor][cte]") {
    // Regression test: a non-recursive CTE materializes its base term via
    // execute_with_s() + parse_table_output(), which splits the ASCII table string into
    // lines on literal '\n'. Before Executor::escape_cell() was introduced, a value
    // containing an embedded newline split its own row across two "lines" mid-parse,
    // silently truncating the value at the newline instead of visibly corrupting the
    // result.
    TempDataDir dir("exec_cte_data_newline");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE note (id INT PRIMARY KEY, content VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO note VALUES (1, 'line1" "\n" "line2')").is_ok());

    auto r = ex.execute_sql("WITH n AS (SELECT id, content FROM note) SELECT * FROM n");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
    // The embedded newline survives the internal round-trip only if it was escaped
    // before parse_table_output() ever saw it -- shown here (correctly) as the 2-char
    // sequence "\n", not truncated to just "line1".
    REQUIRE(r.value().find("line1\\nline2") != std::string::npos);
}

TEST_CASE("CTE table is torn down afterward and does not leak into the catalog", "[executor][cte]") {
    TempDataDir dir("exec_cte_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,9000)").is_ok());

    REQUIRE(ex.execute_sql("WITH t AS (SELECT id FROM employee) SELECT * FROM t").is_ok());

    auto s = ex.get_shared()->read();
    REQUIRE(s->tables.count("company.t") == 0);
    REQUIRE(s->catalog.tables.count("company.t") == 0);
}

TEST_CASE("Multiple CTEs in one WITH clause", "[executor][cte]") {
    TempDataDir dir("exec_cte_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1,5000),(2,1,7000),(3,2,4000)").is_ok());

    auto r = ex.execute_sql(
        "WITH dept_stats AS (SELECT department_id, COUNT(*) AS headcount FROM employee GROUP BY department_id), "
        "big_depts AS (SELECT department_id FROM dept_stats WHERE headcount >= 2) "
        "SELECT department_id FROM big_depts");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
}

TEST_CASE("CTE name conflicting with an existing table is rejected", "[executor][cte]") {
    TempDataDir dir("exec_cte_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1)").is_ok());

    auto r = ex.execute_sql("WITH employee AS (SELECT id FROM employee) SELECT * FROM employee");
    REQUIRE(r.is_err());
    REQUIRE(r.error().find("conflicts") != std::string::npos);
}

TEST_CASE("Recursive CTE walks a management tree to completion", "[executor][cte]") {
    TempDataDir dir("exec_cte_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50), manager_id INT)").is_ok());
    // 1 is the root (no manager), 2 and 3 report to 1, 4 reports to 2.
    REQUIRE(ex.execute_sql("INSERT INTO employee (id, name) VALUES (1, 'Boss')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (2, 'Mid', 1)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (3, 'Mid2', 1)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (4, 'Leaf', 2)").is_ok());

    auto r = ex.execute_sql(
        "WITH RECURSIVE mgmt_tree AS ("
        "SELECT id, name, manager_id, 0 AS depth FROM employee WHERE manager_id IS NULL "
        "UNION ALL "
        "SELECT e.id, e.name, e.manager_id, t.depth + 1 FROM employee e JOIN mgmt_tree t ON e.manager_id = t.id"
        ") SELECT id, name, depth FROM mgmt_tree ORDER BY depth, id");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("4 row(s) returned.") != std::string::npos);
    REQUIRE(r.value().find("Boss") != std::string::npos);
    REQUIRE(r.value().find("Leaf") != std::string::npos);
    // Boss (depth 0) must appear before Leaf (depth 2) since ORDER BY depth ASC.
    REQUIRE(r.value().find("Boss") < r.value().find("Leaf"));
}

// Perf (semi-naive evaluation): a diamond DAG where two different paths converge on the
// same node (4) in the SAME iteration -- this is exactly the shape that distinguishes
// "join against the whole accumulated table" from "join against just the previous
// iteration's delta" if the two were NOT provably equivalent. Also exercises the
// preserved quirk (within-one-iteration duplicates aren't deduped against each other):
// node 4 is derived twice in one iteration (via 2->4 and 3->4), so the CTE's own raw rows
// contain a literal duplicate -- the outer SELECT DISTINCT is what a real query would use
// to get a clean answer, and that's what this test asserts on.
TEST_CASE("Recursive CTE over a diamond DAG finds all reachable nodes despite converging paths",
          "[executor][cte]") {
    TempDataDir dir("exec_cte_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE edges (src INT, dst INT)").is_ok());
    // 1->2, 1->3, 2->4, 3->4 (both 2 and 3 reach 4)
    REQUIRE(ex.execute_sql("INSERT INTO edges VALUES (1,2),(1,3),(2,4),(3,4)").is_ok());

    auto r = ex.execute_sql(
        "WITH RECURSIVE reach AS ("
        "SELECT 1 AS node "
        "UNION ALL "
        "SELECT e.dst FROM edges e JOIN reach r ON e.src = r.node"
        ") SELECT DISTINCT node FROM reach ORDER BY node");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("4 row(s) returned.") != std::string::npos); // {1,2,3,4}, deduped
}

// Perf (semi-naive evaluation): a WHERE-clause subquery in the recursive term must force
// the safety check to fall back to the original always-correct full-accumulation
// evaluation (not attempt delta-only substitution) -- this test exists to confirm the
// fallback path itself still produces the correct answer, not just that it compiles.
TEST_CASE("Recursive CTE with a subquery in its recursive term's WHERE clause still works (forces the "
          "non-semi-naive fallback path)",
          "[executor][cte]") {
    TempDataDir dir("exec_cte_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE edges (src INT, dst INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO edges VALUES (1,2),(2,3),(3,4)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE excluded (node INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO excluded VALUES (99)").is_ok()); // excludes nothing real

    auto r = ex.execute_sql(
        "WITH RECURSIVE reach AS ("
        "SELECT 1 AS node "
        "UNION ALL "
        "SELECT e.dst FROM edges e JOIN reach r ON e.src = r.node "
        "WHERE e.dst NOT IN (SELECT node FROM excluded)"
        ") SELECT node FROM reach ORDER BY node");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("4 row(s) returned.") != std::string::npos); // {1,2,3,4}
}

TEST_CASE("Non-terminating recursive CTE errors instead of silently returning a truncated result", "[executor][cte]") {
    // Regression: exec_with's 1000-iteration cap used to have no way to distinguish
    // "stopped because a genuine fixed point was reached" from "stopped because the
    // iteration cap was hit while still producing new rows" -- both fell through to the
    // same code, silently returning whatever partial rows had accumulated with no
    // indication the result might be incomplete. A recursive CTE with no natural
    // termination (each iteration produces a strictly new value, so the row set never
    // stops growing) must now fail with a clear error instead of quietly truncating.
    TempDataDir dir("exec_cte_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());

    auto r = ex.execute_sql(
        "WITH RECURSIVE counter AS ("
        "SELECT 1 AS n "
        "UNION ALL "
        "SELECT n + 1 FROM counter WHERE n < 100000"
        ") SELECT * FROM counter");
    REQUIRE(r.is_err());
    REQUIRE(r.error().find("did not reach a fixed point") != std::string::npos);
    REQUIRE(r.error().find("1000 iterations") != std::string::npos);
}
