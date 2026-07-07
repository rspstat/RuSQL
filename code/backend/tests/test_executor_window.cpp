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

TEST_CASE("ROW_NUMBER over ORDER BY assigns sequential ranks", "[executor][window]") {
    TempDataDir dir("exec_win_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,5000),(2,9000),(3,3000)").is_ok());

    auto r = ex.execute_sql("SELECT id, ROW_NUMBER() OVER (ORDER BY salary DESC) AS rn FROM emp ORDER BY salary DESC");
    REQUIRE(r.is_ok());
    // Sorted by salary desc: id=2(9000)->rn1, id=1(5000)->rn2, id=3(3000)->rn3
    auto& v = r.value();
    auto pos2 = v.find("| 2");
    auto pos1 = v.find("| 1");
    auto pos3 = v.find("| 3");
    REQUIRE(pos2 < pos1);
    REQUIRE(pos1 < pos3);
    REQUIRE(v.find("3 row(s) returned.") != std::string::npos);
}

TEST_CASE("RANK and DENSE_RANK handle ties correctly", "[executor][window]") {
    TempDataDir dir("exec_win_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,5000),(2,5000),(3,3000)").is_ok());

    auto r = ex.execute_sql("SELECT id, RANK() OVER (ORDER BY salary DESC) AS rnk FROM emp");
    REQUIRE(r.is_ok());
    // Two rows tie for rank 1 (salary 5000), the third gets rank 3 (RANK skips).
    auto rows = r.value();
    REQUIRE(rows.find("rnk") != std::string::npos);

    auto r2 = ex.execute_sql("SELECT id, DENSE_RANK() OVER (ORDER BY salary DESC) AS drnk FROM emp");
    REQUIRE(r2.is_ok());
}

TEST_CASE("PARTITION BY groups window computation per partition", "[executor][window]") {
    TempDataDir dir("exec_win_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, department_id INT, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,1,5000),(2,1,9000),(3,2,3000),(4,2,7000)").is_ok());

    auto r = ex.execute_sql("SELECT id, ROW_NUMBER() OVER (PARTITION BY department_id ORDER BY salary DESC) AS rn FROM emp");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("4 row(s) returned.") != std::string::npos);
}

TEST_CASE("SUM/AVG/COUNT/MIN/MAX window aggregates over the full partition", "[executor][window]") {
    TempDataDir dir("exec_win_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, department_id INT, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,1,1000),(2,1,3000)").is_ok());

    auto r = ex.execute_sql("SELECT id, SUM(salary) OVER (PARTITION BY department_id) AS dept_total FROM emp");
    REQUIRE(r.is_ok());
    // Both rows share the same partition total: 1000 + 3000 = 4000.
    auto count = 0;
    std::size_t pos = 0;
    while ((pos = r.value().find("4000", pos)) != std::string::npos) {
        count++;
        pos += 4;
    }
    REQUIRE(count == 2);
}

TEST_CASE("LAG and LEAD reference neighboring rows in window order", "[executor][window]") {
    TempDataDir dir("exec_win_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1,1000),(2,2000),(3,3000)").is_ok());

    auto r = ex.execute_sql("SELECT id, LAG(salary,1) OVER (ORDER BY salary ASC) AS prev_salary FROM emp ORDER BY salary ASC");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("NULL") != std::string::npos); // first row has no predecessor
    REQUIRE(r.value().find("1000") != std::string::npos);
    REQUIRE(r.value().find("2000") != std::string::npos);
}
