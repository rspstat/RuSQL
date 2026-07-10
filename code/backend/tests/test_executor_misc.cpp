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

TEST_CASE("EXPLAIN describes a SELECT's access plan without executing it", "[executor][misc]") {
    TempDataDir dir("exec_misc_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    auto r = ex.execute_sql("EXPLAIN SELECT * FROM t WHERE id = 1");
    REQUIRE(r.is_ok());
    REQUIRE_FALSE(r.value().empty());

    auto not_select = ex.execute_sql("EXPLAIN INSERT INTO t VALUES (2, 200)");
    REQUIRE(not_select.is_ok());
    REQUIRE(not_select.value().find("not a SELECT") != std::string::npos);

    // EXPLAIN must not have actually inserted a row.
    auto s = ex.get_shared()->read();
    REQUIRE(s->tables.at("company.t").size() == 1);
}

TEST_CASE("EXPLAIN ANALYZE executes the query and reports actual timing/row counts", "[executor][misc]") {
    TempDataDir dir("exec_misc_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2)").is_ok());

    auto r = ex.execute_sql("EXPLAIN ANALYZE SELECT * FROM t");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("QUERY PLAN (ANALYZE)") != std::string::npos);
    REQUIRE(r.value().find("Actual rows") != std::string::npos);
    REQUIRE(r.value().find("Execution time") != std::string::npos);
}

TEST_CASE("BACKUP produces SQL text and RESTORE replays it into a fresh database", "[executor][misc]") {
    TempDataDir dir("exec_misc_data_3");
    std::string backup_file = dir.path + "_backup.sql";
    fs::remove(backup_file);

    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
        REQUIRE(ex.execute_sql("USE company").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'Alice')").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 'Bob')").is_ok());

        auto backup = ex.execute_sql("BACKUP DATABASE company INTO '" + backup_file + "'");
        REQUIRE(backup.is_ok());
        REQUIRE(backup.value().find("Backup of 'company' written") != std::string::npos);
        REQUIRE(fs::exists(backup_file));
    }

    TempDataDir dir2("exec_misc_data_3_restore");
    Executor ex2(dir2.path);
    auto restore = ex2.execute_sql("RESTORE FROM '" + backup_file + "'");
    REQUIRE(restore.is_ok());
    REQUIRE(restore.value().find("statement(s) OK") != std::string::npos);

    auto s = ex2.get_shared()->read();
    REQUIRE(s->tables.count("company.t") == 1);
    REQUIRE(s->tables.at("company.t").size() == 2);

    fs::remove(backup_file);
}

TEST_CASE("Multi-table UPDATE via JOIN updates matching rows in the target table", "[executor][misc]") {
    TempDataDir dir("exec_misc_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept (id INT PRIMARY KEY, budget INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept_upd (dept_id INT PRIMARY KEY, new_budget INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept VALUES (1, 100)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept VALUES (2, 200)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES (1, 999)").is_ok());

    auto r = ex.execute_sql("UPDATE dept JOIN dept_upd ON dept.id = dept_upd.dept_id SET dept.budget = dept_upd.new_budget");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "1 row(s) updated.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.dept");
    auto it = std::find_if(rows.begin(), rows.end(), [](const Row& row) { return row.at("id") == "1"; });
    REQUIRE(it != rows.end());
    REQUIRE(it->at("budget") == "999");
    auto it2 = std::find_if(rows.begin(), rows.end(), [](const Row& row) { return row.at("id") == "2"; });
    REQUIRE(it2 != rows.end());
    REQUIRE(it2->at("budget") == "200"); // untouched
}

TEST_CASE("Multi-table DELETE via JOIN deletes only matching rows", "[executor][misc]") {
    TempDataDir dir("exec_misc_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, dept_id INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE to_remove (dept_id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1, 10)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (2, 20)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO to_remove VALUES (10)").is_ok());

    auto r = ex.execute_sql("DELETE emp FROM emp JOIN to_remove ON emp.dept_id = to_remove.dept_id");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "1 row(s) deleted.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.emp");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].at("id") == "2");
}

TEST_CASE("Multi-table UPDATE on a composite-PK target only touches the row matching every PK column",
          "[executor][misc][regression]") {
    // Regression, same class as plain UPDATE (test_executor_update_delete.cpp): the
    // target-row identity used to be just the first PK-marked column, so a composite-PK
    // target table's rows sharing that column's value got conflated -- both would be
    // updated (with whichever merged row's assignments happened to land in the shared
    // pk_updates[] entry last), not just the one the JOIN+WHERE actually matched.
    TempDataDir dir("exec_misc_data_multi_upd_composite");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t1 (a INT, b INT, val VARCHAR(50), PRIMARY KEY(a, b))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t2 (id INT PRIMARY KEY, a_ref INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t1 VALUES (1, 1, 'x')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t1 VALUES (1, 2, 'y')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t2 VALUES (1, 1)").is_ok());

    auto r = ex.execute_sql("UPDATE t1, t2 SET t1.val = 'JOINED' WHERE t1.a = t2.a_ref AND t2.id = 1 AND t1.b = 1");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "1 row(s) updated.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.t1");
    for (auto& row : rows) {
        if (row.at("a") == "1" && row.at("b") == "1") REQUIRE(row.at("val") == "JOINED");
        else REQUIRE(row.at("val") != "JOINED");
    }
}

TEST_CASE("Multi-table DELETE on a composite-PK target only deletes the row matching every PK column",
          "[executor][misc][regression]") {
    TempDataDir dir("exec_misc_data_multi_del_composite");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t3 (a INT, b INT, val VARCHAR(50), PRIMARY KEY(a, b))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t2 (id INT PRIMARY KEY, a_ref INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t3 VALUES (1, 1, 'x')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t3 VALUES (1, 2, 'y')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t2 VALUES (1, 1)").is_ok());

    auto r = ex.execute_sql("DELETE t3 FROM t3 JOIN t2 ON t3.a = t2.a_ref WHERE t2.id = 1 AND t3.b = 1");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "1 row(s) deleted.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.t3");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].at("a") == "1");
    REQUIRE(rows[0].at("b") == "2");
}

TEST_CASE("MERGE on a composite-PK target only updates the row matching every PK column", "[executor][misc][regression]") {
    TempDataDir dir("exec_misc_data_merge_composite");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t4 (a INT, b INT, val VARCHAR(50), PRIMARY KEY(a, b))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t4_src (a INT, b INT, newval VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t4 VALUES (1, 1, 'x')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t4 VALUES (1, 2, 'y')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t4_src VALUES (1, 1, 'MERGED')").is_ok());

    auto r = ex.execute_sql("MERGE INTO t4 USING t4_src ON t4.a = t4_src.a AND t4.b = t4_src.b "
                             "WHEN MATCHED THEN UPDATE SET val = t4_src.newval");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "MERGE: 1 updated, 0 deleted, 0 inserted.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.t4");
    for (auto& row : rows) {
        if (row.at("a") == "1" && row.at("b") == "1") REQUIRE(row.at("val") == "MERGED");
        else REQUIRE(row.at("val") != "MERGED");
    }
}

TEST_CASE("MERGE applies matched-update, matched-delete, and not-matched-insert branches", "[executor][misc]") {
    TempDataDir dir("exec_misc_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, code VARCHAR(10), name VARCHAR(50), budget INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept_upd (code VARCHAR(10) PRIMARY KEY, name VARCHAR(50), budget INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1, 'ENG', 'Engineering', 1000)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (2, 'TMP', 'Temp Dept', 0)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES ('ENG', 'Engineering Pro', 6000)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES ('TMP', 'Temp Closed', 0)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES ('NEW', 'New Division', 500)").is_ok());

    auto r = ex.execute_sql("MERGE INTO department USING dept_upd ON department.code = dept_upd.code "
                             "WHEN MATCHED AND dept_upd.budget = 0 THEN DELETE "
                             "WHEN MATCHED THEN UPDATE SET budget = dept_upd.budget "
                             "WHEN NOT MATCHED THEN INSERT (code, name, budget) VALUES (dept_upd.code, dept_upd.name, dept_upd.budget)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "MERGE: 1 updated, 1 deleted, 1 inserted.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.department");
    REQUIRE(rows.size() == 2); // ENG (updated), NEW (inserted); TMP deleted

    auto eng = std::find_if(rows.begin(), rows.end(), [](const Row& r2) { return r2.at("code") == "ENG"; });
    REQUIRE(eng != rows.end());
    REQUIRE(eng->at("budget") == "6000");

    auto tmp = std::find_if(rows.begin(), rows.end(), [](const Row& r2) { return r2.at("code") == "TMP"; });
    REQUIRE(tmp == rows.end());

    auto new_dept = std::find_if(rows.begin(), rows.end(), [](const Row& r2) { return r2.at("code") == "NEW"; });
    REQUIRE(new_dept != rows.end());
    REQUIRE(new_dept->at("name") == "New Division");
}

TEST_CASE("INFORMATION_SCHEMA.TABLES / .COLUMNS reflect created tables", "[executor][misc]") {
    TempDataDir dir("exec_misc_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL)").is_ok());

    auto tables = ex.execute_sql("SELECT table_name FROM information_schema.tables WHERE table_schema = 'company'");
    REQUIRE(tables.is_ok());
    REQUIRE(tables.value().find("employee") != std::string::npos);

    auto cols = ex.execute_sql("SELECT column_name, data_type FROM information_schema.columns "
                                "WHERE table_schema = 'company' AND table_name = 'employee'");
    REQUIRE(cols.is_ok());
    REQUIRE(cols.value().find("name") != std::string::npos);
    REQUIRE(cols.value().find("varchar") != std::string::npos);
}
