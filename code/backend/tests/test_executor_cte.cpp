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
