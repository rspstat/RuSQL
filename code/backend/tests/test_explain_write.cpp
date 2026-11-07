#include <filesystem>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/server_status.hpp"

using namespace engine;
namespace fs = std::filesystem;

// R70: EXPLAIN of an UPDATE and a DELETE (the plan of the rows they look for, nothing is run), and the server's limit on connections (@@max_connections).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : r.value().substr(0, 80)));
    REQUIRE(r.is_ok());
}

std::string text_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    return r.is_err() ? "ERROR: " + r.error() : r.value();
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void fill(Executor& ex) {
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, cat INT, flag INT)");
    for (int start = 1; start <= 2000; start += 500) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = start; i < start + 500; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 200) + ",0)";
        ok(ex, sql);
    }
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, t_id INT, qty INT)");
    ok(ex, "INSERT INTO o VALUES (1, 1, 5), (2, 2, 6), (3, 3, 7)");
}
} // namespace

TEST_CASE("R70: EXPLAIN UPDATE and EXPLAIN DELETE show the plan of the rows they look for, and run nothing", "[r70][explain]") {
    TempDataDir dir("r70_explain_write");
    Executor ex(dir.path);
    fill(ex);
    // the plan of an UPDATE / DELETE names its kind, the table and how the rows are found
    std::string plan = text_of(ex, "EXPLAIN UPDATE t SET flag = 1 WHERE cat = 5");
    REQUIRE(has(plan, "QUERY PLAN"));
    REQUIRE(has(plan, "Statement"));
    REQUIRE(has(plan, "UPDATE"));
    REQUIRE(has(plan, "Seq Scan"));
    plan = text_of(ex, "EXPLAIN DELETE FROM t WHERE id = 7");
    REQUIRE(has(plan, "DELETE"));
    REQUIRE_FALSE(has(plan, "Seq Scan")); // (the primary key finds the row)
    // an index changes the plan of the write as it changes that of a SELECT
    ok(ex, "CREATE INDEX idx_t_cat ON t (cat)");
    plan = text_of(ex, "EXPLAIN UPDATE t SET flag = 1 WHERE cat = 5");
    REQUIRE_FALSE(has(plan, "Seq Scan"));
    REQUIRE(has(plan, "idx_t_cat"));
    // nothing was run: no row changed
    REQUIRE(has(text_of(ex, "SELECT COUNT(*) FROM t WHERE flag = 1"), "| 0"));
    REQUIRE(has(text_of(ex, "SELECT COUNT(*) FROM t"), "2000"));
    // a multi-table UPDATE / DELETE
    plan = text_of(ex, "EXPLAIN UPDATE t JOIN o ON o.t_id = t.id SET t.flag = 9 WHERE o.qty = 6");
    REQUIRE(has(plan, "UPDATE (multi-table)"));
    REQUIRE(has(plan, "Join"));
    plan = text_of(ex, "EXPLAIN DELETE t FROM t JOIN o ON o.t_id = t.id WHERE o.qty = 6");
    REQUIRE(has(plan, "DELETE (multi-table)"));
    REQUIRE(has(text_of(ex, "SELECT COUNT(*) FROM t WHERE flag = 9"), "| 0"));
    // an INSERT is still not planned
    REQUIRE(has(text_of(ex, "EXPLAIN INSERT INTO t VALUES (9999, 1, 1)"), "not a SELECT"));
}

TEST_CASE("R70: EXPLAIN ANALYZE of an UPDATE tells the plan and runs the statement", "[r70][explain]") {
    TempDataDir dir("r70_explain_analyze_write");
    Executor ex(dir.path);
    fill(ex);
    std::string answer = text_of(ex, "EXPLAIN ANALYZE UPDATE t SET flag = 1 WHERE cat = 5");
    REQUIRE(has(answer, "QUERY PLAN (ANALYZE)"));
    REQUIRE(has(answer, "UPDATE"));
    REQUIRE(has(answer, "Execution time"));
    REQUIRE(has(text_of(ex, "SELECT COUNT(*) FROM t WHERE flag = 1"), "10")); // (2000 rows over 200 values of cat: the 10 rows were changed)
}

TEST_CASE("R70: max_connections is a server variable, and a place among the connections is refused past it", "[r70][connections]") {
    TempDataDir dir("r70_max_connections");
    Executor ex(dir.path);
    // the default is MySQL's, it is readable, and SHOW STATUS counts the refusals
    REQUIRE(has(text_of(ex, "SELECT @@max_connections"), "151"));
    REQUIRE(has(text_of(ex, "SHOW GLOBAL STATUS LIKE 'Connection_errors_max_connections'"), "| 0"));
    // SET GLOBAL changes it for the whole server: another session reads the new value on its next statement
    Executor other = Executor::new_session(ex.get_shared());
    REQUIRE(has(text_of(other, "SELECT @@max_connections"), "151"));
    ok(ex, "SET GLOBAL max_connections = 3");
    REQUIRE(has(text_of(ex, "SELECT @@max_connections"), "| 3"));
    REQUIRE(has(text_of(other, "SELECT @@max_connections"), "| 3"));
    // a number out of range is brought into it, and what is not a number is an error
    ok(ex, "SET GLOBAL max_connections = 0");
    REQUIRE(has(text_of(ex, "SELECT @@max_connections"), "| 1"));
    ok(ex, "SET GLOBAL max_connections = 5000000");
    REQUIRE(has(text_of(ex, "SELECT @@max_connections"), "100000"));
    REQUIRE(has(text_of(ex, "SET GLOBAL max_connections = 'many'"), "ERROR"));
    // the places themselves: a connection past the limit is refused, and gives its place back when it ends
    auto counters = ex.get_shared()->read()->counters;
    ok(ex, "SET GLOBAL max_connections = 2");
    {
        ConnectionSlot first(*counters), second(*counters);
        REQUIRE(first.ok());
        REQUIRE(second.ok());
        ConnectionSlot third(*counters);
        REQUIRE_FALSE(third.ok());
        REQUIRE(counters->open_connections.load() == 2); // (a refused one holds no place)
        REQUIRE(counters->refused_connections.load() == 1);
    }
    REQUIRE(counters->open_connections.load() == 0);
    ConnectionSlot again(*counters);
    REQUIRE(again.ok());
    REQUIRE(has(text_of(ex, "SHOW GLOBAL STATUS LIKE 'Connection_errors_max_connections'"), "| 1"));
}
