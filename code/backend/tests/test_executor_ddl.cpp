#include <algorithm>
#include <filesystem>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
// Each test gets its own throwaway data directory so tests don't interfere.
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};
} // namespace

TEST_CASE("SHOW TABLES doesn't crash when every table name is shorter than the 'Tables' header", "[executor][ddl]") {
    // Regression test: exec_show_tables's column width floors at 5 (matching Rust's
    // `.max(5)`, itself shorter than "Tables".len()==6 -- a pre-existing quirk in the
    // Rust original, faithfully preserved). The original C++ port's padding used an
    // unguarded `max_len - text.size()` (both unsigned), which underflowed to a huge
    // value and crashed (std::string allocation failure) whenever every real table
    // name was shorter than "Tables" itself, e.g. a single-character table name "t".
    TempDataDir dir("exec_ddl_data_short_table_name");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d1").is_ok());
    REQUIRE(ex.execute_sql("USE d1").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());

    auto r = ex.execute_sql("SHOW TABLES");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("Tables") != std::string::npos);
    REQUIRE(r.value().find("t") != std::string::npos);
}

TEST_CASE("new_session creates an independent Executor sharing the same SharedDatabase", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_new_session");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d1").is_ok());
    REQUIRE(ex.execute_sql("USE d1").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());

    Executor sess = Executor::new_session(ex.get_shared());
    REQUIRE(sess.execute_sql("USE d1").is_ok());

    auto r = sess.execute_sql("SHOW TABLES");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("t") != std::string::npos);

    auto r2 = sess.execute_sql("SELECT * FROM t");
    REQUIRE(r2.is_ok());
    REQUIRE(r2.value().find("1 row(s) returned.") != std::string::npos);
}

TEST_CASE("CREATE DATABASE / USE / SHOW TABLES lifecycle", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_1");
    Executor ex(dir.path);

    auto r1 = ex.execute_sql("CREATE DATABASE company");
    REQUIRE(r1.is_ok());
    REQUIRE(r1.value() == "Database 'company' created.");

    // exec_create_database also switches current_db, matching Rust.
    auto r2 = ex.execute_sql("SHOW TABLES");
    REQUIRE(r2.is_ok());
    REQUIRE(r2.value() == "No tables found in database 'company'.");

    auto r3 = ex.execute_sql("USE company");
    REQUIRE(r3.is_ok());
    REQUIRE(r3.value() == "Database changed to 'company'.");

    auto r4 = ex.execute_sql("USE no_such_db");
    REQUIRE(r4.is_err());
}

TEST_CASE("CREATE TABLE / IF NOT EXISTS / SHOW TABLES / DROP TABLE / TRUNCATE", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());

    auto create1 = ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(100))");
    REQUIRE(create1.is_ok());
    REQUIRE(create1.value() == "Table 'company.department' created.");

    auto create2 = ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)");
    REQUIRE(create2.is_err()); // no IF NOT EXISTS -> catalog rejects duplicate

    auto create3 = ex.execute_sql("CREATE TABLE IF NOT EXISTS department (id INT PRIMARY KEY)");
    REQUIRE(create3.is_ok());
    REQUIRE(create3.value().find("already exists, skipped") != std::string::npos);

    auto shown = ex.execute_sql("SHOW TABLES");
    REQUIRE(shown.is_ok());
    REQUIRE(shown.value().find("department") != std::string::npos);

    auto trunc = ex.execute_sql("TRUNCATE TABLE department");
    REQUIRE(trunc.is_ok());
    REQUIRE(trunc.value() == "Table 'company.department' truncated.");

    auto drop1 = ex.execute_sql("DROP TABLE department");
    REQUIRE(drop1.is_ok());
    REQUIRE(drop1.value() == "Table 'company.department' dropped.");

    auto drop2 = ex.execute_sql("DROP TABLE department");
    REQUIRE(drop2.is_err());

    auto drop3 = ex.execute_sql("DROP TABLE IF EXISTS department");
    REQUIRE(drop3.is_ok());
    REQUIRE(drop3.value().find("does not exist, skipped") != std::string::npos);
}

TEST_CASE("ALTER TABLE add/drop/rename/modify column", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, job_title VARCHAR(100))").is_ok());

    auto add_col = ex.execute_sql("ALTER TABLE employee ADD COLUMN profile_url VARCHAR(200)");
    REQUIRE(add_col.is_ok());
    REQUIRE(add_col.value().find("added") != std::string::npos);

    auto rename_col = ex.execute_sql("ALTER TABLE employee RENAME COLUMN profile_url TO linkedin_url");
    REQUIRE(rename_col.is_ok());
    REQUIRE(rename_col.value().find("renamed") != std::string::npos);

    auto drop_col = ex.execute_sql("ALTER TABLE employee DROP COLUMN linkedin_url");
    REQUIRE(drop_col.is_ok());
    REQUIRE(drop_col.value().find("dropped") != std::string::npos);

    auto modify_col = ex.execute_sql("ALTER TABLE employee MODIFY COLUMN job_title VARCHAR(150) DEFAULT 'Staff'");
    REQUIRE(modify_col.is_ok());
    REQUIRE(modify_col.value().find("modified") != std::string::npos);

    auto modify_missing = ex.execute_sql("ALTER TABLE employee MODIFY COLUMN nonexistent VARCHAR(10)");
    REQUIRE(modify_missing.is_err());
}

TEST_CASE("ALTER TABLE MODIFY COLUMN rejects incompatible existing data", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR(20))").is_ok());

    // Insert a row directly (bypassing the not-yet-ported INSERT statement) so the
    // MODIFY COLUMN validation loop has a non-numeric value to reject.
    {
        auto s = ex.get_shared()->write();
        Row row;
        row["id"] = "1";
        row["val"] = "not_a_number";
        s->tables["company.t"].push_back(row);
    }

    auto modify = ex.execute_sql("ALTER TABLE t MODIFY COLUMN val INT");
    REQUIRE(modify.is_err());
    REQUIRE(modify.error().find("Cannot convert value") != std::string::npos);
}

TEST_CASE("ALTER TABLE RENAME TABLE moves schema, rows, and indexes to the new key", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE old_name (id INT PRIMARY KEY, val VARCHAR(20))").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_val ON old_name (val)").is_ok());

    Statement rename = Statement::AlterTable{"old_name", AlterAction::RenameTable{"new_name"}};
    auto result = ex.execute(rename);
    REQUIRE(result.is_ok());

    auto s = ex.get_shared()->read();
    REQUIRE(s->catalog.tables.count("company.new_name") == 1);
    REQUIRE(s->catalog.tables.count("company.old_name") == 0);
    REQUIRE(s->tables.count("company.new_name") == 1);
    REQUIRE(s->indexes.count("company.old_name_idx_val") == 0);
    REQUIRE(s->indexes.count("company.new_name_idx_val") == 1);
}

TEST_CASE("ALTER TABLE foreign key / unique / check constraints", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE project (id INT PRIMARY KEY, department_id INT, name VARCHAR(50), team_size INT)").is_ok());

    Statement add_fk = Statement::AlterTable{
        "project", AlterAction::AddForeignKey{std::optional<std::string>("fk_proj_dept"), "department_id", "department", "id",
                                                FkAction::Cascade, FkAction::Restrict}};
    auto fk_result = ex.execute(add_fk);
    REQUIRE(fk_result.is_ok());
    {
        auto s = ex.get_shared()->read();
        auto* schema = s->catalog.get_table("company.project");
        REQUIRE(schema != nullptr);
        auto col = std::find_if(schema->columns.begin(), schema->columns.end(), [](auto& c) { return c.name == "department_id"; });
        REQUIRE(col != schema->columns.end());
        REQUIRE(col->foreign_key.has_value());
        REQUIRE(col->foreign_key->ref_table == "department");
    }

    // DropForeignKey matches by column name or by the auto-derived "fk_{column}" label —
    // the custom constraint name given to ADD is only ever used for the success message,
    // never stored (ForeignKey has no `name` field), so it can't be used to target DROP.
    // This is a faithfully-ported quirk of the Rust original, not a C++ port bug.
    Statement drop_fk = Statement::AlterTable{"project", AlterAction::DropForeignKey{"department_id"}};
    REQUIRE(ex.execute(drop_fk).is_ok());

    auto add_unique = ex.execute_sql("ALTER TABLE project ADD CONSTRAINT uq_proj_name UNIQUE (name)");
    REQUIRE(add_unique.is_ok());
    auto drop_unique = ex.execute_sql("ALTER TABLE project DROP CONSTRAINT uq_proj_name");
    REQUIRE(drop_unique.is_ok());

    auto add_check = ex.execute_sql("ALTER TABLE project ADD CONSTRAINT chk_team_size CHECK (team_size >= 1)");
    REQUIRE(add_check.is_ok());
    auto drop_check = ex.execute_sql("ALTER TABLE project DROP CONSTRAINT chk_team_size");
    REQUIRE(drop_check.is_ok());

    auto drop_missing = ex.execute_sql("ALTER TABLE project DROP CONSTRAINT no_such_constraint");
    REQUIRE(drop_missing.is_err());
}

TEST_CASE("CREATE INDEX btree/hash/composite and DROP INDEX", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, department_id INT, salary INT, email VARCHAR(100))").is_ok());

    auto btree_idx = ex.execute_sql("CREATE INDEX idx_emp_dept ON employee (department_id)");
    REQUIRE(btree_idx.is_ok());
    REQUIRE(btree_idx.value().find("Index") != std::string::npos);

    auto hash_idx = ex.execute_sql("CREATE INDEX idx_emp_email ON employee (email) USING HASH");
    REQUIRE(hash_idx.is_ok());
    REQUIRE(hash_idx.value().find("Hash index") != std::string::npos);

    auto composite_idx = ex.execute_sql("CREATE INDEX idx_emp_dept_sal ON employee (department_id, salary)");
    REQUIRE(composite_idx.is_ok());
    REQUIRE(composite_idx.value().find("Composite index") != std::string::npos);

    {
        auto s = ex.get_shared()->read();
        REQUIRE(s->indexes.count("company.employee_idx_emp_dept") == 1);
        REQUIRE(s->hash_indexes.count("company.employee_idx_emp_email") == 1);
        REQUIRE(s->composite_indexes.count("idx_emp_dept_sal") == 1);
    }

    REQUIRE(ex.execute_sql("DROP INDEX IF EXISTS idx_emp_dept").is_ok());
    REQUIRE(ex.execute_sql("DROP INDEX IF EXISTS idx_emp_email").is_ok());
    REQUIRE(ex.execute_sql("DROP INDEX IF EXISTS idx_emp_dept_sal").is_ok());
    // Dropping again is idempotent (no if_exists flag needed — always "skipped" if absent).
    auto redundant = ex.execute_sql("DROP INDEX IF EXISTS idx_emp_dept");
    REQUIRE(redundant.is_ok());
    REQUIRE(redundant.value().find("does not exist, skipped") != std::string::npos);

    {
        auto s = ex.get_shared()->read();
        REQUIRE(s->indexes.count("company.employee_idx_emp_dept") == 0);
        REQUIRE(s->hash_indexes.count("company.employee_idx_emp_email") == 0);
        REQUIRE(s->composite_indexes.count("idx_emp_dept_sal") == 0);
    }
}

TEST_CASE("CREATE VIEW rejects unknown base table and DROP VIEW is idempotent", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());

    auto bad_view = ex.execute_sql("CREATE VIEW v_missing AS SELECT id FROM no_such_table");
    REQUIRE(bad_view.is_err());

    auto good_view = ex.execute_sql("CREATE VIEW v_dept AS SELECT id, name FROM department");
    REQUIRE(good_view.is_ok());
    // qualify_stmt qualifies CreateView's name with the current db before exec_create_view runs.
    REQUIRE(good_view.value() == "View 'company.v_dept' created.");

    {
        auto s = ex.get_shared()->read();
        REQUIRE(s->views.count("company.v_dept") == 1);
    }

    auto drop_view = ex.execute_sql("DROP VIEW v_dept");
    REQUIRE(drop_view.is_ok());
    REQUIRE(drop_view.value() == "View 'company.v_dept' dropped.");

    auto drop_again = ex.execute_sql("DROP VIEW v_dept");
    REQUIRE(drop_again.is_ok());
    REQUIRE(drop_again.value().find("does not exist, skipped") != std::string::npos);
}

TEST_CASE("SELECT * FROM a view preserves an empty cell's column position", "[executor][ddl]") {
    // Regression test: parse_table_output (used to reconstruct Row objects from a
    // view's own box-drawn table output when the outer query wraps it) used to drop
    // whitespace-only cells entirely instead of keeping them as "", shifting every
    // later column in that row one position left. This only manifests when a row has
    // an EMPTY value in a column that ISN'T the last one, so a value-then-empty
    // column pair is needed to catch it (a trailing empty cell wouldn't expose the
    // bug, since there'd be nothing after it to shift). Verified precisely (rather
    // than by string-position heuristics) by filtering on the column that would have
    // received the shifted value if the bug were present.
    TempDataDir dir("exec_ddl_data_view_empty_cell");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(20), b VARCHAR(20))").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW v AS SELECT id, a, b FROM t").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t (id, b) VALUES (1, 'present')").is_ok()); // a left NULL/empty

    // If the bug were present, "present" would have shifted into column a's slot when
    // the view's own output got re-parsed, so this filter on b would find nothing.
    auto r = ex.execute_sql("SELECT id FROM v WHERE b = 'present'");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);

    // And column a should NOT have received the shifted value either.
    auto r2 = ex.execute_sql("SELECT id FROM v WHERE a = 'present'");
    REQUIRE(r2.is_ok());
    REQUIRE(r2.value().find("0 rows returned.") != std::string::npos);
}

TEST_CASE("DROP DATABASE removes tables/indexes/views scoped to that db only", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_9");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_dept ON department (id)").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW v_dept AS SELECT id FROM department").is_ok());

    REQUIRE(ex.execute_sql("CREATE DATABASE other").is_ok());
    REQUIRE(ex.execute_sql("USE other").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE keep_me (id INT PRIMARY KEY)").is_ok());

    auto drop_db = ex.execute_sql("DROP DATABASE company");
    REQUIRE(drop_db.is_ok());

    auto s = ex.get_shared()->read();
    REQUIRE(s->databases.count("company") == 0);
    REQUIRE(s->tables.count("company.department") == 0);
    REQUIRE(s->indexes.count("company.department_idx_dept") == 0);
    REQUIRE(s->views.count("company.v_dept") == 0);
    // The other database's data must survive untouched.
    REQUIRE(s->databases.count("other") == 1);
    REQUIRE(s->tables.count("other.keep_me") == 1);
}

TEST_CASE("A query that references a nonexistent table errors rather than misbehaving", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_10");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());

    // As of Phase 8f, every Statement variant executor.rs dispatches has a matching
    // exec_with_s arm (verified by cross-checking every variant in the AST's Statement
    // variant against executor_core.cpp), so there's no longer a genuinely
    // not-yet-implemented statement to use as an example here. MERGE against a missing
    // source table is used instead as a stand-in for "this must error, not crash."
    auto result = ex.execute_sql("MERGE INTO t USING s ON t.id = s.id WHEN MATCHED THEN UPDATE SET t.v = s.v");
    REQUIRE(result.is_err());
}

TEST_CASE("A fresh Executor pointed at the same directory reloads persisted schema/tables/indexes", "[executor][ddl]") {
    TempDataDir dir("exec_ddl_data_11");
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
        REQUIRE(ex.execute_sql("USE company").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE department (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX idx_dept_name ON department (name)").is_ok());
    }
    {
        Executor ex2(dir.path);
        auto s = ex2.get_shared()->read();
        REQUIRE(s->databases.count("company") == 1);
        REQUIRE(s->catalog.tables.count("company.department") == 1);
        REQUIRE(s->index_meta.count("idx_dept_name") == 1);
    }
}
