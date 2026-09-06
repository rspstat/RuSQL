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

TEST_CASE("UNION deduplicates rows across both queries", "[executor][setops]") {
    TempDataDir dir("exec_setops_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1),(2)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (2),(3)").is_ok());

    auto r = ex.execute_sql("SELECT id FROM a UNION SELECT id FROM b");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("3 row(s) returned.") != std::string::npos); // 1,2,3 deduplicated
}

TEST_CASE("UNION correctly round-trips a value containing a literal '|' or newline", "[executor][setops]") {
    // Regression test: UNION executes each side via execute_with_s() and reconstructs
    // its Rows by re-parsing the ASCII table string via parse_table_output(). Before
    // Executor::escape_cell() was introduced, a value containing a literal '|' was
    // indistinguishable from a real cell boundary during that re-parse, silently
    // truncating the value (everything after the embedded '|' was dropped) instead of
    // corrupting the row count outright -- the more dangerous kind of bug since nothing
    // about the result *looked* wrong.
    TempDataDir dir("exec_setops_data_pipe");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY, note VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1, 'a|b')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY, note VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (2, 'c')").is_ok());

    auto r = ex.execute_sql("SELECT id, note FROM a UNION SELECT id, note FROM b ORDER BY id");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("2 row(s) returned.") != std::string::npos);
    // The embedded '|' survives the internal round-trip only if it was escaped before
    // parse_table_output() ever saw it -- shown here (correctly) as "a\|b", not
    // truncated to just "a".
    REQUIRE(r.value().find("a\\|b") != std::string::npos);
}

TEST_CASE("UNION ALL keeps duplicates", "[executor][setops]") {
    TempDataDir dir("exec_setops_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1),(2)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (2),(3)").is_ok());

    auto r = ex.execute_sql("SELECT id FROM a UNION ALL SELECT id FROM b");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("4 row(s) returned.") != std::string::npos);
}

TEST_CASE("INTERSECT returns only rows present in both queries", "[executor][setops]") {
    TempDataDir dir("exec_setops_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1),(2),(3)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (2),(3),(4)").is_ok());

    auto r = ex.execute_sql("SELECT id FROM a INTERSECT SELECT id FROM b");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("2 row(s) returned.") != std::string::npos);
    REQUIRE(r.value().find("1") == std::string::npos);
    REQUIRE(r.value().find("4") == std::string::npos);
}

TEST_CASE("EXCEPT returns rows in the left query but not the right", "[executor][setops]") {
    TempDataDir dir("exec_setops_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1),(2),(3)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (2),(3)").is_ok());

    auto r = ex.execute_sql("SELECT id FROM a EXCEPT SELECT id FROM b");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("1 row(s) returned.") != std::string::npos);
    auto lines_pos = r.value().find("| 1");
    REQUIRE(lines_pos != std::string::npos);
}

TEST_CASE("UNION with ORDER BY and LIMIT", "[executor][setops]") {
    TempDataDir dir("exec_setops_data_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (5),(1)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (3)").is_ok());

    auto r = ex.execute_sql("SELECT id FROM a UNION ALL SELECT id FROM b ORDER BY id ASC LIMIT 2");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("2 row(s) returned.") != std::string::npos);
    auto pos1 = r.value().find("| 1");
    auto pos3 = r.value().find("| 3");
    REQUIRE(pos1 != std::string::npos);
    REQUIRE(pos3 != std::string::npos);
    REQUIRE(pos1 < pos3);
    REQUIRE(r.value().find("| 5") == std::string::npos);
}
