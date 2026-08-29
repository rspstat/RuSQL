#include <algorithm>
#include <filesystem>
#include <vector>

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
// straight out of SharedDatabase instead. MVCC: filters to live rows only (_xmax ==
// "0") -- a soft-deleted or UPDATE-superseded dead version can now coexist physically
// alongside the current one, which raw unfiltered access would otherwise surface.
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

// Row-level-concurrency prep: AUTO_INCREMENT allocation switched from "copy the counter,
// mutate the copy across the whole batch, write it back once at the end" to "allocate
// directly against the real Catalog entry, immediately, per row." A single multi-row
// batch INSERT is exactly the case where the old copy-based approach's within-batch
// bookkeeping mattered even with no concurrency at all -- confirms the new approach still
// gives every row in one statement a distinct, sequential value.
TEST_CASE("AUTO_INCREMENT gives every row in a multi-row batch INSERT a distinct sequential value",
          "[executor][insert][regression]") {
    TempDataDir dir("exec_insert_data_ai_batch");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE emp (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50))").is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO emp (name) VALUES ('a'), ('b'), ('c'), ('d'), ('e')").is_ok());

    auto rows = table_rows(ex, "company.emp");
    REQUIRE(rows.size() == 5);
    std::vector<int> ids;
    for (auto& r : rows) ids.push_back(std::stoi(r.at("id")));
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids == std::vector<int>{1, 2, 3, 4, 5});

    // A following statement must continue from where the batch left off, not restart or
    // collide -- and the counter must have actually persisted to disk (save_schema is
    // now only called once per statement, driven by any_auto_increment_allocated).
    REQUIRE(ex.execute_sql("INSERT INTO emp (name) VALUES ('f')").is_ok());
    auto rows2 = table_rows(ex, "company.emp");
    REQUIRE(rows2.size() == 6);
    bool found_six = false;
    for (auto& r : rows2) {
        if (r.at("id") == "6") found_six = true;
    }
    REQUIRE(found_six);
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

// Regression for PLAN.md P2: ON DUPLICATE KEY UPDATE's in-place mutation previously only
// refreshed the PK B+Tree, leaving secondary/hash/composite indexes on the changed columns
// stale (still pointing at the pre-conflict values).
TEST_CASE("ON DUPLICATE KEY UPDATE keeps secondary, hash, and composite indexes fresh, not stale",
          "[executor][insert][regression]") {
    TempDataDir dir("exec_insert_data_on_dup_indexes");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept (id INT PRIMARY KEY, dept_code INT, region VARCHAR(20), budget INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO dept VALUES (1, 10, 'east', 100)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_dept_code ON dept (dept_code)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_region ON dept (region) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_dept_budget ON dept (dept_code, budget)").is_ok());

    auto r = ex.execute_sql("INSERT INTO dept (id, dept_code, region, budget) VALUES (1, 20, 'west', 999) "
                             "ON DUPLICATE KEY UPDATE dept_code = 20, region = 'west', budget = 999");
    REQUIRE(r.is_ok());

    auto s = ex.get_shared()->read();

    auto& idx = s->indexes.at("company.dept_idx_dept_code");
    auto old_bucket = idx.search("10");
    REQUIRE(old_bucket.has_value());
    REQUIRE(old_bucket.value() == "[]"); // old value's bucket emptied, not left stale
    auto new_bucket = idx.search("20");
    REQUIRE(new_bucket.has_value());
    REQUIRE(new_bucket->find("\"id\":\"1\"") != std::string::npos);

    auto& hi = s->hash_indexes.at("company.dept_idx_region");
    REQUIRE(hi.get("east").empty());
    REQUIRE(hi.get("west").size() == 1);

    auto& ci = s->composite_indexes.at("company.dept_idx_dept_budget");
    REQUIRE_FALSE(ci.search_exact({"10", "100"}).has_value());
    REQUIRE(ci.search_exact({"20", "999"}).has_value());
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

TEST_CASE("Foreign key validation into a non-PK but hash-indexed column still works", "[executor][insert]") {
    TempDataDir dir("exec_insert_data_7b");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, code VARCHAR(10) UNIQUE)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_dept_code ON department(code) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO department VALUES (1, 'ENG')").is_ok());
    REQUIRE(ex.execute_sql(
                 "CREATE TABLE employee (id INT PRIMARY KEY, dept_code VARCHAR(10), "
                 "CONSTRAINT fk_dept FOREIGN KEY (dept_code) REFERENCES department(code))")
                .is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO employee VALUES (1, 'ENG')").is_ok());
    auto fk_violation = ex.execute_sql("INSERT INTO employee VALUES (2, 'NOPE')");
    REQUIRE(fk_violation.is_err());
    REQUIRE(fk_violation.error().find("Foreign key violation") != std::string::npos);
}

TEST_CASE("Executor::index_or_scan_exists finds matches via PK index, hash index, and plain linear scan alike",
          "[executor][insert][index_or_scan_exists]") {
    TempDataDir dir("exec_insert_data_7c");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, tag VARCHAR(10), plain VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_t_tag ON t(tag) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'alpha', 'x')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 'beta', 'y')").is_ok());

    auto s = ex.get_shared()->read();
    const auto& rows = s->tables.at("company.t");
    auto always_ok = [](const Row&) { return true; };
    auto never_ok = [](const Row&) { return false; };

    // Via the PK B+Tree index.
    REQUIRE(Executor::index_or_scan_exists(*s, "company.t", rows, "id", "1", always_ok));
    REQUIRE_FALSE(Executor::index_or_scan_exists(*s, "company.t", rows, "id", "999", always_ok));
    REQUIRE_FALSE(Executor::index_or_scan_exists(*s, "company.t", rows, "id", "1", never_ok)); // accept() still applied

    // Via the hash index.
    REQUIRE(Executor::index_or_scan_exists(*s, "company.t", rows, "tag", "alpha", always_ok));
    REQUIRE_FALSE(Executor::index_or_scan_exists(*s, "company.t", rows, "tag", "gamma", always_ok));
    REQUIRE_FALSE(Executor::index_or_scan_exists(*s, "company.t", rows, "tag", "alpha", never_ok));

    // No index on `plain` at all -- falls back to a linear scan, still correct.
    REQUIRE(Executor::index_or_scan_exists(*s, "company.t", rows, "plain", "x", always_ok));
    REQUIRE_FALSE(Executor::index_or_scan_exists(*s, "company.t", rows, "plain", "z", always_ok));
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
