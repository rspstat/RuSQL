// R88: the OK packet of ALTER TABLE says how many rows MySQL copied when it copies the table (its ALGORITHM=COPY) and 0 when it does the work in place; ROW_COUNT() says the same.
// The shapes were measured on MySQL 8.4.9; code/test/diff/corpus/alter_copy_rows_r88.sql repeats 83 of them against the server.

#include <filesystem>
#include <sstream>

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

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
}

// the number the OK packet of the statement carries
std::uint64_t reported(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    return ex.statement_affected_rows(r.value());
}

std::string cell(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|' || ++bars < 2) continue;
        const std::size_t end = line.find('|', 1);
        std::string value = line.substr(1, end - 1);
        const std::size_t a = value.find_first_not_of(' '), b = value.find_last_not_of(' ');
        return a == std::string::npos ? std::string() : value.substr(a, b - a + 1);
    }
    return std::string();
}

struct World {
    TempDataDir dir;
    Executor ex;
    explicit World(const std::string& name) : dir(name), ex(dir.path) {
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE apk (id INT PRIMARY KEY)");
        ok(ex, "INSERT INTO apk VALUES (1), (2), (3), (4), (5)");
    }
    // a table of five rows, made again for every statement
    void table() {
        ok(ex, "DROP TABLE IF EXISTS ac");
        ok(ex, "DROP TABLE IF EXISTS ac2");
        ok(ex, "CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, "
               "dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)");
        ok(ex, "INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)");
    }
    std::uint64_t alter(const std::string& what) {
        table();
        return reported(ex, "ALTER TABLE ac " + what);
    }
};
} // namespace

TEST_CASE("R88: an ALTER TABLE that MySQL does in place says 0 rows", "[r88]") {
    World w("r88_data_in_place");
    for (const char* what : {"ADD COLUMN z INT", "ADD COLUMN z INT NOT NULL DEFAULT 5", "DROP COLUMN d", "MODIFY v10 VARCHAR(20)", "MODIFY v10 VARCHAR(63)", "MODIFY v70 VARCHAR(100)",
                              "MODIFY d INT NOT NULL", "MODIFY d INT NULL", "MODIFY i INT NOT NULL DEFAULT 5", "MODIFY i INT COMMENT 'x'", "MODIFY i INT AFTER id", "MODIFY en ENUM('a','b','c')",
                              "MODIFY st SET('a','b','c')", "MODIFY b2 TINYINT(1)", "MODIFY mi MEDIUMINT", "CHANGE d dd INT", "CHANGE v10 w10 VARCHAR(63)", "RENAME COLUMN d TO dd", "ADD INDEX i2 (v10)",
                              "ADD UNIQUE KEY u2 (v10)", "ALTER COLUMN i SET DEFAULT 7", "COMMENT = 'x'", "RENAME TO ac2", "ADD COLUMN z INT, MODIFY v10 VARCHAR(30)",
                              "ADD COLUMN z INT, DROP COLUMN d"}) {
        INFO(what);
        CHECK(w.alter(what) == 0);
    }
}

TEST_CASE("R88: an ALTER TABLE that MySQL copies the table for says how many rows it copied", "[r88]") {
    World w("r88_data_copy");
    for (const char* what : {"MODIFY v10 VARCHAR(64)", "MODIFY v63 VARCHAR(64)", "MODIFY v70 VARCHAR(65)", "MODIFY v10 VARCHAR(5)", "MODIFY ch CHAR(10)", "MODIFY ch CHAR(3)",
                              "MODIFY ch VARCHAR(5)", "MODIFY tx MEDIUMTEXT", "MODIFY v10 TEXT", "MODIFY ti INT", "MODIFY i TINYINT", "MODIFY i BIGINT", "MODIFY i INT UNSIGNED",
                              "MODIFY ui INT", "MODIFY i DECIMAL(10,2)", "MODIFY i VARCHAR(20)", "MODIFY dt DATETIME", "MODIFY dtt DATE", "MODIFY dtt DATETIME(3)",
                              "MODIFY dec1 DECIMAL(12,2)", "MODIFY dec1 DECIMAL(10,3)", "MODIFY fl DOUBLE", "MODIFY en ENUM('b','a')", "MODIFY en ENUM('a')", "MODIFY st SET('b','a')",
                              "MODIFY b2 INT", "MODIFY bl MEDIUMBLOB", "MODIFY ts TIMESTAMP(3)", "MODIFY tm TIME(3)", "MODIFY mi INT", "MODIFY id INT AUTO_INCREMENT", "CHANGE i j BIGINT", "CHANGE v10 w10 VARCHAR(8)", "MODIFY i BIGINT, MODIFY v10 VARCHAR(63)",
                              "ADD COLUMN z INT, MODIFY i BIGINT", "DROP COLUMN ti, MODIFY ui INT", "ADD CHECK (i > 0)", "ADD CONSTRAINT ck CHECK (i > 0)",
                              "ADD COLUMN z INT, ADD CHECK (i > 0)", "ADD FOREIGN KEY (d) REFERENCES apk (id)"}) {
        INFO(what);
        CHECK(w.alter(what) == 5);
    }
}

TEST_CASE("R88: ROW_COUNT() says the same, an empty table copies no rows, and foreign keys that are not checked copy nothing", "[r88]") {
    World w("r88_data_row_count");
    Executor& ex = w.ex;
    CHECK(w.alter("MODIFY i BIGINT") == 5);
    CHECK(cell(ex, "SELECT ROW_COUNT()") == "5");
    CHECK(reported(ex, "ALTER TABLE ac ADD COLUMN z INT") == 0);
    CHECK(cell(ex, "SELECT ROW_COUNT()") == "0");
    ok(ex, "DELETE FROM ac WHERE id > 2"); // (the rows that are left are the ones copied)
    CHECK(reported(ex, "ALTER TABLE ac MODIFY bi INT") == 2);
    ok(ex, "CREATE TABLE empty (a INT)");
    CHECK(reported(ex, "ALTER TABLE empty MODIFY a BIGINT") == 0);
    // while foreign keys are not checked MySQL adds one in place
    w.table();
    ok(ex, "SET FOREIGN_KEY_CHECKS = 0");
    CHECK(reported(ex, "ALTER TABLE ac ADD FOREIGN KEY (d) REFERENCES apk (id)") == 0);
    ok(ex, "SET FOREIGN_KEY_CHECKS = 1");
    // a statement that fails changes nothing
    w.table();
    CHECK(ex.execute_sql("ALTER TABLE ac MODIFY nosuch BIGINT").is_err());
    CHECK(cell(ex, "SELECT COUNT(*) FROM ac") == "5");
}
