// R98: (1) the rows of INSERT / REPLACE / CREATE TABLE ... [AS] may come from a WITH ... SELECT (MySQL 8: `INSERT INTO t WITH c AS (...) SELECT ...`), which the parser
// refused before; (2) TRACE of a SELECT that was just run shows the whole path (an answer taken from the cache had no steps); (3) the pk -> position cache is filled when
// a table is loaded, so an aggregate that starts from an index does so right after a restart (it read the whole table until an UPDATE / DELETE had gone by).

#include <filesystem>
#include <limits>

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

std::string text(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    return r.value();
}

std::string error_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql);
    REQUIRE(r.is_err());
    return r.error();
}
} // namespace

TEST_CASE("INSERT / REPLACE / CREATE TABLE take their rows from WITH ... SELECT", "[r98][with]") {
    TempDataDir dir("r98_with_rows");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE src (id INT PRIMARY KEY, name VARCHAR(20), v INT)");
    ok(ex, "INSERT INTO src VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30)");
    ok(ex, "CREATE TABLE dst (id INT PRIMARY KEY, name VARCHAR(20), v INT)");

    // INSERT ... WITH ... SELECT, with and without a column list
    ok(ex, "INSERT INTO dst WITH big AS (SELECT id, name, v FROM src WHERE v >= 20) SELECT id, name, v FROM big");
    REQUIRE(text(ex, "SELECT id, name, v FROM dst ORDER BY id").find("| 3  | c    | 30 |") != std::string::npos);
    REQUIRE(text(ex, "SELECT COUNT(*) FROM dst").find("| 2 ") != std::string::npos);
    ok(ex, "INSERT INTO dst (id, v) WITH one AS (SELECT 9 AS i, 90 AS w) SELECT i, w FROM one");
    REQUIRE(text(ex, "SELECT v FROM dst WHERE id = 9").find("| 90 ") != std::string::npos);

    // a recursive list, INSERT IGNORE, REPLACE
    ok(ex, "INSERT INTO dst (id, v) WITH RECURSIVE n (k) AS (SELECT 100 UNION ALL SELECT k + 1 FROM n WHERE k < 104) SELECT k, k * 2 FROM n");
    REQUIRE(text(ex, "SELECT COUNT(*) FROM dst").find("| 8 ") != std::string::npos);
    ok(ex, "INSERT IGNORE INTO dst WITH s AS (SELECT id, name, v FROM src) SELECT id, name, v FROM s");
    REQUIRE(text(ex, "SELECT COUNT(*) FROM dst").find("| 9 ") != std::string::npos); // only id 1 was new
    ok(ex, "REPLACE INTO dst WITH s AS (SELECT id, 'z' AS name, v + 1 AS v FROM src WHERE id = 2) SELECT id, name, v + 1 FROM s");
    REQUIRE(text(ex, "SELECT name, v FROM dst WHERE id = 2").find("| z    | 22 |") != std::string::npos);

    // CREATE TABLE ... [AS] WITH ... SELECT: the rows, and the types of a plain column as the same query without WITH gives them
    ok(ex, "CREATE TABLE plain AS SELECT id, name FROM src");
    ok(ex, "CREATE TABLE with_as AS WITH x AS (SELECT 1 AS one) SELECT id, name FROM src");
    ok(ex, "CREATE TABLE with_bare WITH x AS (SELECT 1 AS one) SELECT id, name FROM src");
    REQUIRE(text(ex, "DESCRIBE with_as") == text(ex, "DESCRIBE plain"));
    REQUIRE(text(ex, "DESCRIBE with_bare") == text(ex, "DESCRIBE plain"));
    REQUIRE(text(ex, "SELECT COUNT(*) FROM with_as").find("| 3 ") != std::string::npos);
    ok(ex, "CREATE TABLE from_cte (a INT, b VARCHAR(20)) AS WITH x AS (SELECT id, name FROM src WHERE id < 3) SELECT id, name FROM x");
    REQUIRE(text(ex, "SELECT COUNT(*) FROM from_cte").find("| 2 ") != std::string::npos);

    // a view over a WITH (what mysqldump writes for one), and it follows its table
    ok(ex, "CREATE VIEW big_src AS WITH c AS (SELECT id, v FROM src) SELECT id, v FROM c WHERE v > 10");
    REQUIRE(text(ex, "SELECT COUNT(*) FROM big_src").find("| 2 ") != std::string::npos);
    ok(ex, "INSERT INTO src VALUES (4, 'd', 40)");
    REQUIRE(text(ex, "SELECT COUNT(*) FROM big_src").find("| 3 ") != std::string::npos);
    REQUIRE(text(ex, "SHOW CREATE VIEW big_src").find("WITH c AS") != std::string::npos);

    // INSERT ... SELECT ... ON DUPLICATE KEY UPDATE, with and without WITH: a key that is there is updated, a new one is inserted
    ok(ex, "CREATE TABLE up (id INT PRIMARY KEY, v INT, n INT DEFAULT 0)");
    ok(ex, "INSERT INTO up (id, v) VALUES (1, 1), (9, 9)");
    ok(ex, "INSERT INTO up (id, v) SELECT id, v FROM src ON DUPLICATE KEY UPDATE v = VALUES(v) + 100, n = n + 1"); // (id 1 is updated, 2 to 4 are new; the affected-row counts are compared with MySQL in the corpus insert_select_dup_r98)
    REQUIRE(text(ex, "SELECT v FROM up WHERE id = 1").find("| 110 ") != std::string::npos);
    REQUIRE(text(ex, "SELECT n FROM up WHERE id = 1").find("| 1 ") != std::string::npos);
    REQUIRE(text(ex, "SELECT v FROM up WHERE id = 9").find("| 9 ") != std::string::npos); // (not in the SELECT: untouched)
    ok(ex, "INSERT INTO up (id, v) WITH s AS (SELECT id, v FROM src WHERE id <= 2) SELECT id, v FROM s ON DUPLICATE KEY UPDATE n = n + 10");
    REQUIRE(text(ex, "SELECT n FROM up WHERE id = 1").find("| 11 ") != std::string::npos);
    REQUIRE(text(ex, "SELECT n FROM up WHERE id = 2").find("| 10 ") != std::string::npos);
    REQUIRE(error_of(ex, "INSERT INTO up (id, v) SELECT id, v FROM src ON DUPLICATE KEY").find("Expected UPDATE") != std::string::npos);

    // the main part of the WITH must be a query, not a write
    REQUIRE(error_of(ex, "INSERT INTO dst WITH s AS (SELECT 1) DELETE FROM src").find("Expected SELECT after the WITH list") != std::string::npos);
    // the old form (WITH first, then INSERT ... SELECT) still works
    ok(ex, "WITH s AS (SELECT 7 AS i) INSERT INTO dst (id, v) SELECT i, i FROM s");
    REQUIRE(text(ex, "SELECT v FROM dst WHERE id = 7").find("| 7 ") != std::string::npos);
}

TEST_CASE("TRACE of a SELECT that was just run lists the whole path, not the cache hit", "[r98][trace]") {
    TempDataDir dir("r98_trace_cache");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO t VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30)");
    const std::string sql = "SELECT g, SUM(v) FROM t GROUP BY g ORDER BY g";
    ok(ex, sql);
    REQUIRE(text(ex, sql).find("2 row(s) returned") != std::string::npos); // (the second one is the cached answer)
    const std::string path = text(ex, "TRACE " + sql);
    for (const char* step : {"Parse", "Prepare (cache, locks)", "Scan d.t", "Group / aggregate", "Return the answer", "Total"}) {
        INFO(step);
        REQUIRE(path.find(step) != std::string::npos);
    }
}

TEST_CASE("the position cache is filled when the tables are loaded: an aggregate starts from the index right after a restart", "[r98][dml_index]") {
    TempDataDir dir("r98_warm_cache");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, a INT, d INT)");
        ok(ex, "CREATE INDEX ia ON t (a)");
        for (int i = 1; i <= 300; i++) ok(ex, "INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 30) + ", 1)");
    } // (not flushed at shutdown: the next start loads from the disk and the redo log)
    struct IndexRows { // (the smallest table an index start is tried for: every table here, then none, then what it was)
        std::size_t prev = Executor::dml_index_min_rows.load();
        void set(std::size_t n) { Executor::dml_index_min_rows = n; }
        ~IndexRows() { Executor::dml_index_min_rows = prev; }
    } rows;
    rows.set(0);
    Executor ex(dir.path);
    ok(ex, "USE d");
    const std::string agg = "SELECT COUNT(*), SUM(id) FROM t WHERE a = 3";
    const auto before = Executor::dml_index_hits.load();
    const std::string answer = text(ex, agg);
    REQUIRE(answer.find("| 10 ") != std::string::npos);
    REQUIRE(Executor::dml_index_hits.load() == before + 1); // through the index, with no UPDATE / DELETE before it
    // the same answer as a table scan gives
    rows.set(std::numeric_limits<std::size_t>::max());
    REQUIRE(text(ex, "SELECT COUNT(*), SUM(id) FROM t WHERE a = 3 AND id > 0") == answer);
}
