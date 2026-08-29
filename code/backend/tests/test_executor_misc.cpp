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
    std::string backup_filename = "exec_misc_data_3_backup.sql";

    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
        REQUIRE(ex.execute_sql("USE company").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'Alice')").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 'Bob')").is_ok());

        auto backup = ex.execute_sql("BACKUP DATABASE company INTO '" + backup_filename + "'");
        REQUIRE(backup.is_ok());
        REQUIRE(backup.value().find("Backup of 'company' written") != std::string::npos);
        // BACKUP/RESTORE are sandboxed to <data_dir>/_backups/ (PLAN.md section D fix),
        // not the raw path the caller supplied.
        REQUIRE(fs::exists(dir.path + "/_backups/" + backup_filename));
    }

    // Simulate "the backup file was carried over to a different data dir" by copying it
    // into the restoring Executor's own sandboxed directory.
    TempDataDir dir2("exec_misc_data_3_restore");
    fs::create_directories(dir2.path + "/_backups");
    fs::copy_file(dir.path + "/_backups/" + backup_filename, dir2.path + "/_backups/" + backup_filename);

    Executor ex2(dir2.path);
    auto restore = ex2.execute_sql("RESTORE FROM '" + backup_filename + "'");
    REQUIRE(restore.is_ok());
    REQUIRE(restore.value().find("statement(s) OK") != std::string::npos);

    auto s = ex2.get_shared()->read();
    REQUIRE(s->tables.count("company.t") == 1);
    REQUIRE(s->tables.at("company.t").size() == 2);
}

TEST_CASE("BACKUP/RESTORE reject path traversal and absolute paths", "[executor][misc][regression]") {
    // Regression, faithfully preserved from Rust (code/legacy/rusql-core/src/engine/executor.rs
    // exec_backup/exec_restore): the output/source filename used to be passed straight to
    // std::ofstream/ifstream with no validation -- BACKUP could overwrite an arbitrary
    // file the server process can reach, and RESTORE would read *and execute as SQL*
    // whatever text is at an arbitrary path. Both are now confined to
    // <data_dir>/_backups/, rejecting anything but a bare alnum/'_'/'-'/'.' filename.
    TempDataDir dir("exec_misc_data_backup_sandbox");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());

    auto traversal = ex.execute_sql("BACKUP DATABASE company INTO '../escape.sql'");
    REQUIRE(traversal.is_err());

    auto absolute_unix = ex.execute_sql("BACKUP DATABASE company INTO '/etc/passwd'");
    REQUIRE(absolute_unix.is_err());

    auto restore_traversal = ex.execute_sql("RESTORE FROM '../../some_other_file.sql'");
    REQUIRE(restore_traversal.is_err());

    // A plain filename must still work (the sandboxed, non-malicious path).
    auto ok = ex.execute_sql("BACKUP DATABASE company INTO 'plain_name.sql'");
    REQUIRE(ok.is_ok());
    REQUIRE(fs::exists(dir.path + "/_backups/plain_name.sql"));
}

TEST_CASE("The _backups sandbox directory never surfaces as a real database", "[executor][misc][regression]") {
    // Regression introduced by the BACKUP/RESTORE sandboxing fix above: a fresh Executor
    // picks its default current_db as *std::min_element(databases) (executor_core.cpp),
    // and DiskManager::list_databases() originally only excluded "_system" -- so once a
    // data dir had ever taken a backup, the new top-level "_backups" directory sorted
    // before any real db name and got silently adopted as the default database for every
    // later session with no explicit USE, breaking unqualified statements like
    // "DROP TABLE IF EXISTS t" (resolved against "_backups.t" instead of the real db).
    TempDataDir dir("exec_misc_data_backup_no_fake_db");
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
        REQUIRE(ex.execute_sql("USE company").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
        REQUIRE(ex.execute_sql("BACKUP DATABASE company INTO 'b.sql'").is_ok());
        REQUIRE(fs::exists(dir.path + "/_backups/b.sql"));
    }

    Executor ex2(dir.path);
    REQUIRE(ex2.current_db == "company");
    auto dbs = ex2.execute_sql("SHOW DATABASES");
    REQUIRE(dbs.is_ok());
    REQUIRE(dbs.value().find("_backups") == std::string::npos);
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

// Regression for switching multi-table UPDATE's index maintenance from "clone the whole
// target table and rebuild every index kind from scratch" to per-row incremental
// updates (PK B+Tree, secondary/hash via index_remove_row/index_insert_row, composite).
TEST_CASE("Multi-table UPDATE keeps the target table's composite secondary index consistent",
          "[executor][misc][regression]") {
    TempDataDir dir("exec_misc_data_multi_upd_idx");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept (id INT PRIMARY KEY, region INT, budget INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept_upd (dept_id INT PRIMARY KEY, new_budget INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept VALUES (1, 5, 100), (2, 6, 200)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES (1, 999)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_region_budget ON dept (region, budget)").is_ok());

    auto r = ex.execute_sql("UPDATE dept JOIN dept_upd ON dept.id = dept_upd.dept_id SET dept.budget = dept_upd.new_budget");
    REQUIRE(r.is_ok());

    auto s = ex.get_shared()->read();
    auto& ci = s->composite_indexes.at("company.dept_idx_region_budget");
    REQUIRE_FALSE(ci.search_exact({"5", "100"}).has_value());
    REQUIRE(ci.search_exact({"5", "999"}).has_value());
    REQUIRE(ci.search_exact({"6", "200"}).has_value()); // untouched row still present
}

// Same "full rebuild -> incremental" regression, for multi-table DELETE.
TEST_CASE("Multi-table DELETE keeps the target table's composite secondary index consistent",
          "[executor][misc][regression]") {
    TempDataDir dir("exec_misc_data_multi_del_idx");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, dept_id INT, region INT, salary INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE to_remove (dept_id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1, 10, 5, 1000), (2, 20, 6, 2000)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO to_remove VALUES (10)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_region_sal ON emp (region, salary)").is_ok());

    auto r = ex.execute_sql("DELETE emp FROM emp JOIN to_remove ON emp.dept_id = to_remove.dept_id");
    REQUIRE(r.is_ok());

    auto s = ex.get_shared()->read();
    auto& ci = s->composite_indexes.at("company.emp_idx_region_sal");
    REQUIRE_FALSE(ci.search_exact({"5", "1000"}).has_value());
    REQUIRE(ci.search_exact({"6", "2000"}).has_value());
}

// Regression for PLAN.md P2: multi-table DELETE previously only maintained the PK B+Tree
// and composite indexes (test above) -- secondary B+Tree and hash indexes were never
// touched at all, left stale after the deleted rows' entries should have been removed.
TEST_CASE("Multi-table DELETE keeps secondary and hash indexes consistent, not stale",
          "[executor][misc][regression]") {
    TempDataDir dir("exec_misc_data_multi_del_sec_hash_idx");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY, dept_id INT, region INT, email VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE to_remove (dept_id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES (1, 10, 5, 'a@x.com'), (2, 20, 6, 'b@x.com')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO to_remove VALUES (10)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_region ON emp (region)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_email ON emp (email) USING HASH").is_ok());

    auto r = ex.execute_sql("DELETE emp FROM emp JOIN to_remove ON emp.dept_id = to_remove.dept_id");
    REQUIRE(r.is_ok());

    auto s = ex.get_shared()->read();
    auto& idx = s->indexes.at("company.emp_idx_region");
    auto bucket = idx.search("5");
    REQUIRE(bucket.has_value());
    REQUIRE(bucket.value() == "[]"); // deleted row's entry removed, not left stale
    REQUIRE(idx.search("6").has_value());

    auto& hi = s->hash_indexes.at("company.emp_idx_email");
    REQUIRE(hi.get("a@x.com").empty());
    REQUIRE(hi.get("b@x.com").size() == 1);
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

// Regression for PLAN.md P2 (the most severe of the three): MERGE previously touched ZERO
// index kinds at all -- not PK B+Tree, not secondary, not hash, not composite -- across its
// UPDATE/DELETE/INSERT branches. This exercises all three branches in one MERGE and checks
// every index kind stays consistent, not stale.
TEST_CASE("MERGE keeps secondary, hash, and composite indexes fresh across update/delete/insert branches",
          "[executor][misc][regression]") {
    TempDataDir dir("exec_misc_data_merge_indexes");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY AUTO_INCREMENT, code VARCHAR(10), "
                            "region INT, contact VARCHAR(50), budget INT)")
                .is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept_upd (code VARCHAR(10) PRIMARY KEY, region INT, contact VARCHAR(50), budget INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department (id, code, region, contact, budget) VALUES (1, 'ENG', 5, 'eng@x.com', 1000)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department (id, code, region, contact, budget) VALUES (2, 'TMP', 9, 'tmp@x.com', 0)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_region ON department (region)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_contact ON department (contact) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_region_budget ON department (region, budget)").is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES ('ENG', 7, 'newcontact@x.com', 6000)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES ('TMP', 9, 'tmp@x.com', 0)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept_upd VALUES ('NEW', 3, 'new@x.com', 500)").is_ok());

    auto r = ex.execute_sql(
        "MERGE INTO department USING dept_upd ON department.code = dept_upd.code "
        "WHEN MATCHED AND dept_upd.budget = 0 THEN DELETE "
        "WHEN MATCHED THEN UPDATE SET region = dept_upd.region, contact = dept_upd.contact, budget = dept_upd.budget "
        "WHEN NOT MATCHED THEN INSERT (code, region, contact, budget) VALUES (dept_upd.code, dept_upd.region, dept_upd.contact, dept_upd.budget)");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "MERGE: 1 updated, 1 deleted, 1 inserted.");

    auto s = ex.get_shared()->read();
    auto& idx = s->indexes.at("company.department_idx_region");
    auto& hi = s->hash_indexes.at("company.department_idx_contact");
    auto& ci = s->composite_indexes.at("company.department_idx_region_budget");

    // UPDATE branch (ENG): old region/contact/budget gone, new values present.
    auto old_region_bucket = idx.search("5");
    REQUIRE(old_region_bucket.has_value());
    REQUIRE(old_region_bucket.value() == "[]");
    REQUIRE(idx.search("7").has_value());
    REQUIRE(hi.get("eng@x.com").empty());
    REQUIRE(hi.get("newcontact@x.com").size() == 1);
    REQUIRE_FALSE(ci.search_exact({"5", "1000"}).has_value());
    REQUIRE(ci.search_exact({"7", "6000"}).has_value());

    // DELETE branch (TMP): its region/contact/budget entries gone.
    auto deleted_region_bucket = idx.search("9");
    REQUIRE(deleted_region_bucket.has_value());
    REQUIRE(deleted_region_bucket.value() == "[]");
    REQUIRE(hi.get("tmp@x.com").empty());
    REQUIRE_FALSE(ci.search_exact({"9", "0"}).has_value());

    // INSERT branch (NEW): its region/contact/budget now present.
    REQUIRE(idx.search("3").has_value());
    REQUIRE(hi.get("new@x.com").size() == 1);
    REQUIRE(ci.search_exact({"3", "500"}).has_value());
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
