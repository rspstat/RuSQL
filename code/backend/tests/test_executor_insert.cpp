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

// Basic INSERT relies on exec_select for RETURNING/INSERT-SELECT tests to be fully
// exercised through execute_sql; where SELECT isn't available yet, this reads rows
// straight out of SharedDatabase instead.
std::vector<Row> table_rows(Executor& ex, const std::string& qualified_table) {
    auto s = ex.get_shared()->read();
    auto it = s->tables.find(qualified_table);
    return it != s->tables.end() ? it->second : std::vector<Row>{};
}
} // namespace

TEST_CASE("Basic INSERT with and without explicit column list", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, code VARCHAR(10), name VARCHAR(50))").is_ok());

    auto r1 = ex.execute_sql("INSERT INTO department (id, code, name) VALUES (1, 'ENG', 'Engineering')");
    REQUIRE(r1.is_ok());
    REQUIRE(r1.value() == "1 row(s) inserted.");

    auto r2 = ex.execute_sql("INSERT INTO department VALUES (2, 'MKT', 'Marketing')");
    REQUIRE(r2.is_ok());

    auto rows = table_rows(ex, "company.department");
    REQUIRE(rows.size() == 2);
    bool found_eng = false, found_mkt = false;
    for (auto& r : rows) {
        if (r.at("code") == "ENG") { found_eng = true; REQUIRE(r.at("name") == "Engineering"); }
        if (r.at("code") == "MKT") { found_mkt = true; REQUIRE(r.at("name") == "Marketing"); }
        REQUIRE(r.at("_xmax") == "0");
    }
    REQUIRE(found_eng);
    REQUIRE(found_mkt);
}

TEST_CASE("Multi-row VALUES and column-count mismatch", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());

    auto multi = ex.execute_sql("INSERT INTO dept VALUES (1, 'A'), (2, 'B'), (3, 'C')");
    REQUIRE(multi.is_ok());
    REQUIRE(multi.value() == "3 row(s) inserted.");
    REQUIRE(table_rows(ex, "company.dept").size() == 3);

    auto bad_count = ex.execute_sql("INSERT INTO dept VALUES (4, 'D', 'extra')");
    REQUIRE(bad_count.is_err());
    REQUIRE(bad_count.error().find("Column count mismatch") != std::string::npos);

    auto bad_col = ex.execute_sql("INSERT INTO dept (id, missing_col) VALUES (5, 'x')");
    REQUIRE(bad_col.is_err());
    REQUIRE(bad_col.error().find("not found") != std::string::npos);
}

TEST_CASE("AUTO_INCREMENT and DEFAULT value handling", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql(
                 "CREATE TABLE emp (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50), status VARCHAR(20) DEFAULT 'active')")
                .is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO emp (name) VALUES ('Alice')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp (name) VALUES ('Bob')").is_ok());

    auto rows = table_rows(ex, "company.emp");
    REQUIRE(rows.size() == 2);
    std::vector<std::string> ids;
    for (auto& r : rows) {
        ids.push_back(r.at("id"));
        REQUIRE(r.at("status") == "active");
    }
    REQUIRE(std::find(ids.begin(), ids.end(), "1") != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "2") != ids.end());
}

TEST_CASE("NOT NULL, duplicate PK, and duplicate UNIQUE violations", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, email VARCHAR(50) UNIQUE, name VARCHAR(50) NOT NULL)").is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'a@x.com', 'Alice')").is_ok());

    auto dup_pk = ex.execute_sql("INSERT INTO t VALUES (1, 'b@x.com', 'Bob')");
    REQUIRE(dup_pk.is_err());
    REQUIRE(dup_pk.error().find("Duplicate value") != std::string::npos);

    auto dup_unique = ex.execute_sql("INSERT INTO t VALUES (2, 'a@x.com', 'Carol')");
    REQUIRE(dup_unique.is_err());

    auto null_violation = ex.execute_sql("INSERT INTO t (id, email) VALUES (3, 'c@x.com')");
    REQUIRE(null_violation.is_err());
    REQUIRE(null_violation.error().find("cannot be NULL") != std::string::npos);

    REQUIRE(table_rows(ex, "company.t").size() == 1);
}

TEST_CASE("INSERT IGNORE and ON DUPLICATE KEY UPDATE", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept (id INT PRIMARY KEY, code VARCHAR(10) UNIQUE, budget INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept VALUES (1, 'ENG', 100)").is_ok());

    auto ignored = ex.execute_sql("INSERT IGNORE INTO dept VALUES (2, 'ENG', 200)");
    REQUIRE(ignored.is_ok());
    REQUIRE(table_rows(ex, "company.dept").size() == 1); // ignored, no new row

    auto on_dup = ex.execute_sql("INSERT INTO dept (id, code, budget) VALUES (1, 'ENG', 999) ON DUPLICATE KEY UPDATE budget=500");
    REQUIRE(on_dup.is_ok());
    auto rows = table_rows(ex, "company.dept");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].at("budget") == "500");
}

TEST_CASE("ENUM and SET column value validation", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, kind ENUM('a','b','c'))").is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'b')").is_ok());
    auto bad_enum = ex.execute_sql("INSERT INTO t VALUES (2, 'zzz')");
    REQUIRE(bad_enum.is_err());
    REQUIRE(bad_enum.error().find("Invalid ENUM value") != std::string::npos);
}

TEST_CASE("Foreign key and CHECK constraint violations", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1)").is_ok());
    REQUIRE(ex.execute_sql(
                 "CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, age INT CHECK (age >= 18), "
                 "CONSTRAINT fk_dept FOREIGN KEY (department_id) REFERENCES department(id))")
                .is_ok());

    auto fk_ok = ex.execute_sql("INSERT INTO employee VALUES (1, 1, 25)");
    REQUIRE(fk_ok.is_ok());

    auto fk_violation = ex.execute_sql("INSERT INTO employee VALUES (2, 999, 25)");
    REQUIRE(fk_violation.is_err());
    REQUIRE(fk_violation.error().find("Foreign key violation") != std::string::npos);

    auto check_violation = ex.execute_sql("INSERT INTO employee VALUES (3, 1, 10)");
    REQUIRE(check_violation.is_err());
    REQUIRE(check_violation.error().find("CHECK constraint") != std::string::npos);
}

TEST_CASE("INSERT RETURNING formats the inserted row(s)", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, code VARCHAR(10), name VARCHAR(50))").is_ok());

    auto ret = ex.execute_sql("INSERT INTO department (id,code,name) VALUES (1,'TMP','Temporary') RETURNING id, code, name");
    REQUIRE(ret.is_ok());
    REQUIRE(ret.value().find("TMP") != std::string::npos);
    REQUIRE(ret.value().find("Temporary") != std::string::npos);
    REQUIRE(ret.value().find("+--") != std::string::npos);
}

TEST_CASE("INSERT into an updatable view redirects to its base table", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_9");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW v_dept AS SELECT id, name FROM department").is_ok());

    auto via_view = ex.execute_sql("INSERT INTO v_dept (id, name) VALUES (1, 'Engineering')");
    REQUIRE(via_view.is_ok());
    REQUIRE(table_rows(ex, "company.department").size() == 1);
}
