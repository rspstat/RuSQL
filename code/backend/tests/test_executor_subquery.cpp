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

TEST_CASE("WHERE IN (uncorrelated subquery)", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50), is_active VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'Eng','true'),(2,'Old','false')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1,'Alice'),(2,2,'Bob')").is_ok());

    auto r = ex.execute_sql(
        "SELECT name FROM employee WHERE department_id IN (SELECT id FROM department WHERE is_active = 'true')");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Alice") != std::string::npos);
    REQUIRE(r.value().find("Bob") == std::string::npos);
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
}

TEST_CASE("WHERE NOT IN (uncorrelated subquery)", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, is_active VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'true'),(2,'false')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1,'Alice'),(2,2,'Bob')").is_ok());

    auto r = ex.execute_sql(
        "SELECT name FROM employee WHERE department_id NOT IN (SELECT id FROM department WHERE is_active = 'true')");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Bob") != std::string::npos);
    REQUIRE(r.value().find("Alice") == std::string::npos);
}

TEST_CASE("WHERE EXISTS (correlated subquery)", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'Eng'),(2,'Empty')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1)").is_ok()); // only dept 1 has an employee

    auto r = ex.execute_sql(
        "SELECT name FROM department WHERE EXISTS (SELECT 1 FROM employee WHERE employee.department_id = department.id)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Eng") != std::string::npos);
    REQUIRE(r.value().find("Empty") == std::string::npos);
}

TEST_CASE("WHERE NOT EXISTS (correlated subquery)", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'Eng'),(2,'Empty')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1)").is_ok());

    auto r = ex.execute_sql(
        "SELECT name FROM department WHERE NOT EXISTS (SELECT 1 FROM employee WHERE employee.department_id = department.id)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Empty") != std::string::npos);
    REQUIRE(r.value().find("Eng") == std::string::npos);
}

TEST_CASE("WHERE = (scalar subquery comparison)", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50), salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,'Alice',5000),(2,'Bob',3000)").is_ok());

    auto r = ex.execute_sql("SELECT name FROM employee WHERE salary = (SELECT MAX(salary) FROM employee)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Alice") != std::string::npos);
    REQUIRE(r.value().find("Bob") == std::string::npos);
}

TEST_CASE("WHERE > (scalar subquery comparison) with GROUP BY aggregate", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50), salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,'Alice',5000),(2,'Bob',1000),(3,'Carol',2000)").is_ok());

    auto r = ex.execute_sql("SELECT name FROM employee WHERE salary > (SELECT AVG(salary) FROM employee)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Alice") != std::string::npos);
    REQUIRE(r.value().find("Bob") == std::string::npos);
    REQUIRE(r.value().find("Carol") == std::string::npos);
}

TEST_CASE("SELECT-list scalar subquery (uncorrelated) evaluates once and repeats per row", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_5b");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50), salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,'Alice',100000),(2,'Eve',140000),(3,'Karen',110000)").is_ok());

    auto r = ex.execute_sql("SELECT name, (SELECT MAX(salary) FROM employee) AS max_salary FROM employee ORDER BY salary DESC");
    REQUIRE(r.is_ok());
    // Every row should carry the same max (140000), not an empty/stubbed value.
    INFO(r.value());
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = r.value().find("140000", pos)) != std::string::npos) {
        count++;
        pos += 6;
    }
    REQUIRE(count == 3); // header value doesn't contain "140000", only the 3 data rows do
}

TEST_CASE("SELECT-list scalar subquery (correlated) re-evaluates per outer row", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_5c");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50), department_id INT, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'Engineering'),(2,'Sales')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,'Alice',1,100000),(2,'Bob',1,120000),(3,'Carol',2,90000)").is_ok());

    auto r = ex.execute_sql("SELECT d.name, (SELECT MAX(e2.salary) FROM employee e2 WHERE e2.department_id = d.id) AS dept_max "
                             "FROM department d ORDER BY d.id");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("120000") != std::string::npos); // Engineering's max
    REQUIRE(r.value().find("90000") != std::string::npos);  // Sales' max
}

TEST_CASE("UPDATE WHERE with a subquery condition", "[executor][subquery]") {
    TempDataDir dir("exec_subq_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, is_active VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'true'),(2,'false')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, bonus INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1,0),(2,2,0)").is_ok());

    auto r = ex.execute_sql(
        "UPDATE employee SET bonus = 100 WHERE department_id IN (SELECT id FROM department WHERE is_active = 'true')");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "1 row(s) updated.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.employee");
    for (auto& row : rows) {
        // MVCC: UPDATE appends a new version and marks the old one dead instead of
        // mutating in place -- skip superseded (_xmax != "0") rows, only the live one
        // reflects this UPDATE's result.
        auto xit = row.find("_xmax");
        if (xit != row.end() && xit->second != "0") continue;
        if (row.at("department_id") == "1") REQUIRE(row.at("bonus") == "100");
        else REQUIRE(row.at("bonus") == "0");
    }
}

TEST_CASE("DELETE WHERE with a subquery condition deletes nothing (faithfully-preserved Rust quirk)", "[executor][subquery]") {
    // The Rust original's exec_delete_inner slow path computes rows_to_delete (used for
    // RETURNING/FK-cascade side effects) via the subquery-aware matcher, but the actual
    // deletion pass — both the transactional MVCC-mark loop and the non-transaction
    // `rows.retain(...)` — always re-checks with the plain, non-subquery-aware
    // matches_condexpr. Since that matcher's ConditionValue::Subquery arm is always
    // false, DELETE with a subquery WHERE clause matches zero rows for the actual
    // deletion, no matter what rows_to_delete found. This is a genuine pre-existing bug
    // in the Rust original (see migration plan: bugs are preserved, not fixed, during
    // porting), not something introduced by this port.
    TempDataDir dir("exec_subq_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, is_active VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1,'true'),(2,'false')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1,1),(2,2)").is_ok());

    auto r = ex.execute_sql(
        "DELETE FROM employee WHERE department_id IN (SELECT id FROM department WHERE is_active = 'false')");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "0 row(s) deleted.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.employee");
    REQUIRE(rows.size() == 2);
}
