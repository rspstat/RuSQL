#include <algorithm>
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

// MVCC: filters to live rows only (_xmax == "0") -- UPDATE now appends a new version and
// marks the old one dead instead of mutating in place, so a raw unfiltered read would
// return both the current row and every superseded version still awaiting VACUUM.
std::vector<Row> table_rows(Executor& ex, const std::string& qualified_table) {
    auto s = ex.get_shared()->read();
    auto it = s->tables.find(qualified_table);
    if (it == s->tables.end()) return {};
    std::vector<Row> live;
    for (auto& r : it->second) {
        auto xit = r.find("_xmax");
        if (xit == r.end() || xit->second == "0") live.push_back(r);
    }
    return live;
}
} // namespace

TEST_CASE("Basic UPDATE modifies matching rows only", "[executor][update]") {
    TempDataDir dir("exec_upd_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1, 1000), (2, 2000), (3, 3000)").is_ok());

    auto upd = ex.execute_sql("UPDATE emp SET salary = 5000 WHERE id = 2");
    REQUIRE(upd.is_ok());
    REQUIRE(upd.value() == "1 row(s) updated.");

    auto rows = table_rows(ex, "company.emp");
    for (auto& r : rows) {
        if (r.at("id") == "2") REQUIRE(r.at("salary") == "5000");
        else REQUIRE(r.at("salary") != "5000");
    }
}

TEST_CASE("UPDATE on a composite-PK table only touches the row matching every PK column", "[executor][update][regression]") {
    // Regression: exec_update_inner used to reduce a row's identity to just the first
    // PK-marked column (matching_pks keyed on that alone). On a composite PK, distinct
    // rows sharing the same leading column's value got conflated, so `WHERE a=1 AND b=1`
    // incorrectly updated every row with a=1 regardless of b. Faithfully-preserved from
    // the original Rust (code/legacy/rusql-core/src/engine/executor.rs), fixed here by using
    // a \x00-joined composite key (mirrors the composite index convention) for WHERE-
    // condition row matching, while locking/undo/PK-index upkeep keep using the single
    // leading column, unchanged, matching the rest of the engine's row-identity model.
    TempDataDir dir("exec_upd_data_composite_pk");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (a INT, b INT, val VARCHAR(50), PRIMARY KEY(a, b))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 1, 'row-a1-b1')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 2, 'row-a1-b2')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 1, 'row-a2-b1')").is_ok());

    auto upd = ex.execute_sql("UPDATE t SET val = 'ONLY_B1' WHERE a = 1 AND b = 1");
    REQUIRE(upd.is_ok());
    REQUIRE(upd.value() == "1 row(s) updated.");

    auto rows = table_rows(ex, "company.t");
    for (auto& r : rows) {
        if (r.at("a") == "1" && r.at("b") == "1") REQUIRE(r.at("val") == "ONLY_B1");
        else REQUIRE(r.at("val") != "ONLY_B1");
    }

    // RETURNING has the identical bug surface (same matching_pks membership check) --
    // only the a=1,b=2 row should come back, not a=1,b=1 (also a=1) or a=2,b=1.
    auto ret = ex.execute_sql("UPDATE t SET val = 'RET_B2' WHERE a = 1 AND b = 2 RETURNING a, b, val");
    REQUIRE(ret.is_ok());
    std::size_t hit_count = 0;
    std::size_t pos = 0;
    while ((pos = ret.value().find("RET_B2", pos)) != std::string::npos) { hit_count++; pos += 1; }
    REQUIRE(hit_count == 1);
}

TEST_CASE("UPDATE with arithmetic self-reference evaluates RHS before writing", "[executor][update]") {
    TempDataDir dir("exec_upd_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1, 1000)").is_ok());

    auto upd = ex.execute_sql("UPDATE emp SET salary = salary + 500 WHERE id = 1");
    REQUIRE(upd.is_ok());
    REQUIRE(table_rows(ex, "company.emp")[0].at("salary") == "1500");
}

TEST_CASE("UPDATE RETURNING formats the modified row(s)", "[executor][update]") {
    TempDataDir dir("exec_upd_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, first_name VARCHAR(50), salary INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1, 'Alice', 1000)").is_ok());

    auto ret = ex.execute_sql("UPDATE emp SET salary=salary+1000 WHERE id=1 RETURNING id, first_name, salary");
    REQUIRE(ret.is_ok());
    REQUIRE(ret.value().find("2000") != std::string::npos);
    REQUIRE(ret.value().find("Alice") != std::string::npos);
}

TEST_CASE("UPDATE rejects invalid ENUM value", "[executor][update]") {
    TempDataDir dir("exec_upd_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, kind ENUM('a','b'))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'a')").is_ok());

    auto bad = ex.execute_sql("UPDATE t SET kind = 'zzz' WHERE id = 1");
    REQUIRE(bad.is_err());
    REQUIRE(bad.error().find("Invalid ENUM value") != std::string::npos);
    // Row must be unchanged after the rejected update.
    REQUIRE(table_rows(ex, "company.t")[0].at("kind") == "a");
}

TEST_CASE("UPDATE ON UPDATE CASCADE propagates the new key to referencing rows", "[executor][update]") {
    TempDataDir dir("exec_upd_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, "
                            "CONSTRAINT fk_dept FOREIGN KEY (department_id) REFERENCES department(id) ON UPDATE CASCADE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 1)").is_ok());

    auto upd = ex.execute_sql("UPDATE department SET id = 99 WHERE id = 1");
    REQUIRE(upd.is_ok());
    REQUIRE(table_rows(ex, "company.employee")[0].at("department_id") == "99");
}

TEST_CASE("Basic DELETE removes only matching rows", "[executor][delete]") {
    TempDataDir dir("exec_del_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol')").is_ok());

    auto del = ex.execute_sql("DELETE FROM emp WHERE id = 2");
    REQUIRE(del.is_ok());
    REQUIRE(del.value() == "1 row(s) deleted.");

    auto rows = table_rows(ex, "company.emp");
    REQUIRE(rows.size() == 2);
    for (auto& r : rows) REQUIRE(r.at("id") != "2");
}

TEST_CASE("DELETE with BETWEEN predicate uses the range-delete fast path", "[executor][delete]") {
    TempDataDir dir("exec_del_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1),(2),(3),(4),(5)").is_ok());

    auto del = ex.execute_sql("DELETE FROM t WHERE id BETWEEN 2 AND 4");
    REQUIRE(del.is_ok());
    REQUIRE(del.value() == "3 row(s) deleted.");
    auto rows = table_rows(ex, "company.t");
    REQUIRE(rows.size() == 2);
    std::vector<std::string> ids;
    for (auto& r : rows) ids.push_back(r.at("id"));
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids == std::vector<std::string>{"1", "5"});
}

TEST_CASE("DELETE RETURNING formats the deleted row(s)", "[executor][delete]") {
    TempDataDir dir("exec_del_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, code VARCHAR(10), name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1, 'TMP', 'Temporary')").is_ok());

    auto ret = ex.execute_sql("DELETE FROM department WHERE code='TMP' RETURNING id, name");
    REQUIRE(ret.is_ok());
    REQUIRE(ret.value().find("Temporary") != std::string::npos);
    REQUIRE(table_rows(ex, "company.department").empty());
}

TEST_CASE("DELETE with FK RESTRICT blocks deletion of a referenced row", "[executor][delete]") {
    TempDataDir dir("exec_del_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, "
                            "CONSTRAINT fk_dept FOREIGN KEY (department_id) REFERENCES department(id))")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 1)").is_ok());

    auto del = ex.execute_sql("DELETE FROM department WHERE id = 1");
    REQUIRE(del.is_err());
    REQUIRE(del.error().find("Foreign key violation") != std::string::npos);
    REQUIRE(table_rows(ex, "company.department").size() == 1);
}

TEST_CASE("DELETE with FK CASCADE removes dependent rows", "[executor][delete]") {
    TempDataDir dir("exec_del_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1), (2)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, "
                            "CONSTRAINT fk_dept FOREIGN KEY (department_id) REFERENCES department(id) ON DELETE CASCADE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 1), (2, 2)").is_ok());

    auto del = ex.execute_sql("DELETE FROM department WHERE id = 1");
    REQUIRE(del.is_ok());

    auto emp_rows = table_rows(ex, "company.employee");
    REQUIRE(emp_rows.size() == 1);
    REQUIRE(emp_rows[0].at("department_id") == "2");
}

TEST_CASE("DELETE with FK SET NULL clears the referencing column", "[executor][delete]") {
    TempDataDir dir("exec_del_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, "
                            "CONSTRAINT fk_dept FOREIGN KEY (department_id) REFERENCES department(id) ON DELETE SET NULL)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 1)").is_ok());

    auto del = ex.execute_sql("DELETE FROM department WHERE id = 1");
    REQUIRE(del.is_ok());
    REQUIRE(table_rows(ex, "company.employee")[0].at("department_id") == "NULL");
}

TEST_CASE("UPDATE and DELETE against an updatable view redirect to the base table", "[executor][update][delete]") {
    TempDataDir dir("exec_upd_del_data_view");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1, 'Eng')").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW v_dept AS SELECT id, name FROM department").is_ok());

    auto upd = ex.execute_sql("UPDATE v_dept SET name = 'Engineering' WHERE id = 1");
    REQUIRE(upd.is_ok());
    REQUIRE(table_rows(ex, "company.department")[0].at("name") == "Engineering");

    auto del = ex.execute_sql("DELETE FROM v_dept WHERE id = 1");
    REQUIRE(del.is_ok());
    REQUIRE(table_rows(ex, "company.department").empty());
}
