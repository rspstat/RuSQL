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

TEST_CASE("CREATE USER / DROP USER / GRANT / REVOKE / SHOW GRANTS", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_1");
    Executor ex(dir.path);

    auto create = ex.execute_sql("CREATE USER 'alice'@'localhost' IDENTIFIED BY 'pw123'");
    REQUIRE(create.is_ok());
    REQUIRE(create.value() == "User 'alice@localhost' created.");

    auto dup = ex.execute_sql("CREATE USER 'alice'@'localhost' IDENTIFIED BY 'pw123'");
    REQUIRE(dup.is_err());

    auto dup_ine = ex.execute_sql("CREATE USER IF NOT EXISTS 'alice'@'localhost' IDENTIFIED BY 'pw123'");
    REQUIRE(dup_ine.is_ok());
    REQUIRE(dup_ine.value().find("already exists") != std::string::npos);

    auto grant = ex.execute_sql("GRANT SELECT, INSERT ON *.* TO 'alice'@'localhost'");
    REQUIRE(grant.is_ok());
    REQUIRE(grant.value().find("Granted") != std::string::npos);

    auto show = ex.execute_sql("SHOW GRANTS FOR 'alice'@'localhost'");
    REQUIRE(show.is_ok());
    REQUIRE(show.value().find("SELECT") != std::string::npos);
    REQUIRE(show.value().find("INSERT") != std::string::npos);

    auto revoke = ex.execute_sql("REVOKE INSERT ON *.* FROM 'alice'@'localhost'");
    REQUIRE(revoke.is_ok());
    REQUIRE(revoke.value().find("Revoked") != std::string::npos);

    auto show2 = ex.execute_sql("SHOW GRANTS FOR 'alice'@'localhost'");
    REQUIRE(show2.is_ok());
    REQUIRE(show2.value().find("SELECT") != std::string::npos);
    REQUIRE(show2.value().find("INSERT") == std::string::npos);

    auto drop = ex.execute_sql("DROP USER 'alice'@'localhost'");
    REQUIRE(drop.is_ok());
    REQUIRE(drop.value() == "User 'alice@localhost' dropped.");

    auto drop_again = ex.execute_sql("DROP USER 'alice'@'localhost'");
    REQUIRE(drop_again.is_err());
}

TEST_CASE("SHOW DATABASES lists created databases", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE zeta").is_ok());
    REQUIRE(ex.execute_sql("CREATE DATABASE alpha").is_ok());

    auto r = ex.execute_sql("SHOW DATABASES");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("alpha") != std::string::npos);
    REQUIRE(r.value().find("zeta") != std::string::npos);
    // sorted alphabetically: alpha should appear before zeta
    REQUIRE(r.value().find("alpha") < r.value().find("zeta"));
}

TEST_CASE("CREATE ROLE / GRANT ROLE / REVOKE ROLE / DROP ROLE", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_3");
    Executor ex(dir.path);

    auto create = ex.execute_sql("CREATE ROLE analyst");
    REQUIRE(create.is_ok());

    auto dup = ex.execute_sql("CREATE ROLE analyst");
    REQUIRE(dup.is_err());

    auto grant_role = ex.execute_sql("GRANT ROLE analyst TO 'bob'@'localhost'");
    REQUIRE(grant_role.is_ok());

    auto show_roles = ex.execute_sql("SHOW ROLES");
    REQUIRE(show_roles.is_ok());
    REQUIRE(show_roles.value().find("analyst") != std::string::npos);

    auto revoke_role = ex.execute_sql("REVOKE ROLE analyst FROM 'bob'@'localhost'");
    REQUIRE(revoke_role.is_ok());

    auto revoke_again = ex.execute_sql("REVOKE ROLE analyst FROM 'bob'@'localhost'");
    REQUIRE(revoke_again.is_err());

    auto drop = ex.execute_sql("DROP ROLE analyst");
    REQUIRE(drop.is_ok());
    auto drop_again = ex.execute_sql("DROP ROLE analyst");
    REQUIRE(drop_again.is_err());
    auto drop_if_exists = ex.execute_sql("DROP ROLE IF EXISTS analyst");
    REQUIRE(drop_if_exists.is_ok());
}

TEST_CASE("CREATE SYNONYM / SHOW SYNONYMS / DROP SYNONYM", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY)").is_ok());

    auto create = ex.execute_sql("CREATE SYNONYM emp FOR employee");
    REQUIRE(create.is_ok());

    auto dup = ex.execute_sql("CREATE SYNONYM emp FOR employee");
    REQUIRE(dup.is_err());

    auto show = ex.execute_sql("SHOW SYNONYMS");
    REQUIRE(show.is_ok());
    REQUIRE(show.value().find("emp") != std::string::npos);

    auto drop = ex.execute_sql("DROP SYNONYM emp");
    REQUIRE(drop.is_ok());
    auto drop_again = ex.execute_sql("DROP SYNONYM emp");
    REQUIRE(drop_again.is_err());
}

TEST_CASE("DESCRIBE shows column metadata", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL)").is_ok());

    auto r = ex.execute_sql("DESCRIBE employee");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("id") != std::string::npos);
    REQUIRE(r.value().find("VARCHAR(50)") != std::string::npos);
    REQUIRE(r.value().find("YES") != std::string::npos);
}

TEST_CASE("SHOW CREATE TABLE reproduces DDL including PK/FK/DEFAULT", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_6");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE dept (id INT PRIMARY KEY, name VARCHAR(50) DEFAULT 'unknown')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, dept_id INT, "
                            "FOREIGN KEY (dept_id) REFERENCES dept(id))")
                .is_ok());

    auto r = ex.execute_sql("SHOW CREATE TABLE employee");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("FOREIGN KEY") != std::string::npos);
    REQUIRE(r.value().find("REFERENCES `dept`(`id`)") != std::string::npos);

    auto r2 = ex.execute_sql("SHOW CREATE TABLE dept");
    REQUIRE(r2.is_ok());
    REQUIRE(r2.value().find("DEFAULT 'unknown'") != std::string::npos);
    REQUIRE(r2.value().find("PRIMARY KEY") != std::string::npos);
}

TEST_CASE("SHOW CREATE VIEW and SHOW INDEX", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE employee (id INT PRIMARY KEY, name VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW v_emp AS SELECT id, name FROM employee").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX idx_name ON employee (name)").is_ok());

    auto view_ddl = ex.execute_sql("SHOW CREATE VIEW v_emp");
    REQUIRE(view_ddl.is_ok());
    REQUIRE(view_ddl.value().find("CREATE VIEW") != std::string::npos);

    auto idx = ex.execute_sql("SHOW INDEX FROM employee");
    REQUIRE(idx.is_ok());
    REQUIRE(idx.value().find("idx_name") != std::string::npos);
    REQUIRE(idx.value().find("name") != std::string::npos);
}

TEST_CASE("VACUUM removes dead (deleted) rows and reclaims them", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_8");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2)").is_ok());

    // Mark row 1 dead (_xmax != "0") the same way MVCC delete does, without going
    // through DELETE (which already physically removes non-transactional rows).
    {
        auto sw = ex.get_shared()->write();
        sw->tables.at("company.t")[0]["_xmax"] = "999";
    }

    auto r = ex.execute_sql("VACUUM t");
    REQUIRE(r.is_ok());
    REQUIRE(r.value() == "VACUUM complete. 1 dead row(s) removed.");

    auto s = ex.get_shared()->read();
    REQUIRE(s->tables.at("company.t").size() == 1);
}

TEST_CASE("ANALYZE TABLE collects column statistics", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_9");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 200)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (3, 300)").is_ok());

    auto r = ex.execute_sql("ANALYZE TABLE t");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("ANALYZE") != std::string::npos);
    REQUIRE(r.value().find("3 rows") != std::string::npos);

    auto s = ex.get_shared()->read();
    REQUIRE(s->table_stats.count("company.t") == 1);
    auto& stats = s->table_stats.at("company.t");
    REQUIRE(stats.total_rows == 3);
    REQUIRE(stats.columns.at("val").distinct_count == 3);
    REQUIRE(stats.columns.at("val").min_val == "100");
    REQUIRE(stats.columns.at("val").max_val == "300");
}

TEST_CASE("SHOW PROCESSLIST includes the current session", "[executor][dcl]") {
    TempDataDir dir("exec_dcl_data_10");
    Executor ex(dir.path);
    ex.register_process("root", "localhost");

    auto r = ex.execute_sql("SHOW PROCESSLIST");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("root") != std::string::npos);
}
