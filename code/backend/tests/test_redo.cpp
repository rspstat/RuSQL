#include <filesystem>
#include <fstream>
#include <memory>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Crash-durability tests for the redo log. A "crash" is simulated by destroying the Executor
// without any shutdown/checkpoint step (the destructor flushes nothing) and booting a new
// Executor on the same data directory -- exactly what a restart after kill -9 sees on disk.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

std::string rows_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_ok());
    return r.value();
}

// Number of data rows a SELECT returned, parsed from the "N row(s) returned." footer.
int count_rows(Executor& ex, const std::string& table) {
    auto r = ex.execute_sql("SELECT COUNT(*) FROM " + table);
    REQUIRE(r.is_ok());
    const std::string& v = r.value();
    // table output: | COUNT(*) | ... | <n> |
    auto last_bar = v.find_last_of('|');
    auto prev_bar = v.find_last_of('|', last_bar - 1);
    auto line_end = v.rfind('\n', prev_bar);
    // find the numeric row: the line after the header separator
    std::size_t p = v.find("+\n|", 0);
    REQUIRE(p != std::string::npos);
    std::size_t row_start = v.find("+\n|", p + 1);
    REQUIRE(row_start != std::string::npos);
    std::string line = v.substr(row_start + 2, v.find('\n', row_start + 2) - row_start - 2);
    int n = 0;
    for (char c : line) if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
    (void)line_end;
    return n;
}

void setup(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, v INT)").is_ok());
}
} // namespace

TEST_CASE("Redo: autocommit INSERT/UPDATE/DELETE survive a crash", "[redo][durability]") {
    TempDataDir dir("redo_t1");
    {
        Executor ex(dir.path);
        setup(ex);
        for (int i = 1; i <= 20; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i * 10) + ")").is_ok());
        REQUIRE(ex.execute_sql("UPDATE t SET v = 999 WHERE id = 5").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 7").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM t WHERE id BETWEEN 15 AND 17").is_ok());
    } // crash: nothing flushed the table files
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "t") == 16); // 20 - 1 - 3
    REQUIRE(rows_of(ex, "SELECT v FROM t WHERE id = 5").find("999") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 7").find("0 rows") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 16").find("0 rows") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 18").find("1 row(s)") != std::string::npos);
}

TEST_CASE("Redo: committed explicit transaction survives, uncommitted one does not", "[redo][durability]") {
    TempDataDir dir("redo_t2");
    {
        Executor ex(dir.path);
        setup(ex);
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,10)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2,20)").is_ok());
        REQUIRE(ex.execute_sql("UPDATE t SET v = 11 WHERE id = 1").is_ok()); // update of a row this txn created
        REQUIRE(ex.execute_sql("COMMIT").is_ok());

        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (3,30)").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 2").is_ok());
        // crash with this transaction still open
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "t") == 2);
    REQUIRE(rows_of(ex, "SELECT v FROM t WHERE id = 1").find("11") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 2").find("1 row(s)") != std::string::npos); // uncommitted DELETE undone
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 3").find("0 rows") != std::string::npos);    // uncommitted INSERT gone
}

TEST_CASE("Redo: ROLLBACK and ROLLBACK TO SAVEPOINT leave nothing durable", "[redo][durability]") {
    TempDataDir dir("redo_t3");
    {
        Executor ex(dir.path);
        setup(ex);
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,1)").is_ok());
        REQUIRE(ex.execute_sql("SAVEPOINT sp").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2,2)").is_ok());
        REQUIRE(ex.execute_sql("UPDATE t SET v = 100 WHERE id = 1").is_ok());
        REQUIRE(ex.execute_sql("ROLLBACK TO sp").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (3,3)").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());

        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (9,9)").is_ok());
        REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "t") == 2); // ids 1 and 3 only
    REQUIRE(rows_of(ex, "SELECT v FROM t WHERE id = 1").find("100") == std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 2").find("0 rows") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 9").find("0 rows") != std::string::npos);
}

TEST_CASE("Redo: statements outside the redo-covered set are still durable (legacy flush)", "[redo][durability]") {
    TempDataDir dir("redo_t4");
    int p_before = 0, c_before = 0;
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE p (id INT PRIMARY KEY, n INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1,0),(2,0)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (10,1),(11,1),(12,2)").is_ok()); // FK child: lock set > 1 table
        REQUIRE(ex.execute_sql("DELETE FROM p WHERE id = 1").is_ok());                // cascades into c
        REQUIRE(ex.execute_sql("INSERT INTO p VALUES (3,5) ON DUPLICATE KEY UPDATE n = n + 1").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO p VALUES (3,5) ON DUPLICATE KEY UPDATE n = n + 1").is_ok());
        REQUIRE(ex.execute_sql("REPLACE INTO p VALUES (2, 77)").is_ok()); // physically replaces parent 2 -> cascades child 12 away
        p_before = count_rows(ex, "p");
        c_before = count_rows(ex, "c");
        REQUIRE(p_before == 2); // ids 2, 3
        REQUIRE(c_before == 0); // 10, 11 via DELETE cascade, 12 via REPLACE's delete of parent 2
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "p") == p_before);
    REQUIRE(count_rows(ex, "c") == c_before);
    REQUIRE(rows_of(ex, "SELECT n FROM p WHERE id = 3").find("6") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT n FROM p WHERE id = 2").find("77") != std::string::npos);
}

TEST_CASE("Redo: DDL after DML is not undone by replaying the older redo log", "[redo][durability]") {
    TempDataDir dir("redo_t5");
    {
        Executor ex(dir.path);
        setup(ex);
        for (int i = 1; i <= 10; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ",1)").is_ok());
        REQUIRE(ex.execute_sql("TRUNCATE TABLE t").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (100,1)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE gone (id INT PRIMARY KEY)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO gone VALUES (1)").is_ok());
        REQUIRE(ex.execute_sql("DROP TABLE gone").is_ok());
        REQUIRE(ex.execute_sql("ALTER TABLE t ADD COLUMN extra INT").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (101,1,5)").is_ok());
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "t") == 2); // 100 and 101 only -- the 10 pre-TRUNCATE rows must not resurrect
    REQUIRE(ex.execute_sql("SELECT * FROM gone").is_err());
}

TEST_CASE("Redo: replay is idempotent and tolerates a torn tail", "[redo][durability]") {
    TempDataDir dir("redo_t6");
    {
        Executor ex(dir.path);
        setup(ex);
        for (int i = 1; i <= 5; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ",1)").is_ok());
        REQUIRE(ex.execute_sql("UPDATE t SET v = 2 WHERE id = 1").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 2").is_ok());
    }
    std::string redo = dir.path + "/rusql.redo";
    REQUIRE(fs::exists(redo));
    fs::copy_file(redo, dir.path + "/saved.redo");
    { // first recovery replays and clears the log
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(count_rows(ex, "t") == 4);
    }
    // restore the already-applied log (crash between "table files flushed" and "log cleared"),
    // plus a torn half-written batch at the end
    fs::copy_file(dir.path + "/saved.redo", redo, fs::copy_options::overwrite_existing);
    { std::ofstream f(redo, std::ios::binary | std::ios::app); f << "\x20\x00\x00\x00garbage-torn"; }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "t") == 4); // no duplicates, nothing resurrected
    REQUIRE(rows_of(ex, "SELECT v FROM t WHERE id = 1").find("2") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 2").find("0 rows") != std::string::npos);
}

TEST_CASE("Redo: crossing the checkpoint threshold keeps every row", "[redo][durability]") {
    TempDataDir dir("redo_t7");
    const int N = 600;
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE big (id INT PRIMARY KEY, pad TEXT)").is_ok());
        std::string pad(8192, 'x'); // ~8KB/row => the 4MB redo threshold is crossed at ~500 rows
        for (int i = 1; i <= N; i++) REQUIRE(ex.execute_sql("INSERT INTO big VALUES (" + std::to_string(i) + ",'" + pad + "')").is_ok());
        REQUIRE(ex.execute_sql("UPDATE big SET pad = 'short' WHERE id = 3").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM big WHERE id = 4").is_ok());
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "big") == N - 1);
    REQUIRE(rows_of(ex, "SELECT pad FROM big WHERE id = 3").find("short") != std::string::npos);
}

TEST_CASE("Redo: recovered state is fully usable (indexes, further DML, second crash)", "[redo][durability]") {
    TempDataDir dir("redo_t8");
    {
        Executor ex(dir.path);
        setup(ex);
        REQUIRE(ex.execute_sql("CREATE INDEX iv ON t (v)").is_ok());
        for (int i = 1; i <= 30; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i % 3) + ")").is_ok());
        REQUIRE(ex.execute_sql("UPDATE t SET v = 7 WHERE id = 10").is_ok());
    }
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(rows_of(ex, "SELECT id FROM t WHERE v = 7").find("1 row(s)") != std::string::npos); // secondary index repaired
        REQUIRE(rows_of(ex, "SELECT id FROM t WHERE id = 10").find("1 row(s)") != std::string::npos); // PK path
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (31, 7)").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 1").is_ok());
    } // second crash, on top of a recovered database
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "t") == 30);
    REQUIRE(rows_of(ex, "SELECT id FROM t WHERE v = 7").find("2 row(s)") != std::string::npos);
    // duplicate PK must still be rejected after recovery
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (10, 1)").is_err());
}

TEST_CASE("Redo: a COMMIT no longer rewrites the table file", "[redo][perf]") {
    TempDataDir dir("redo_t9");
    Executor ex(dir.path);
    setup(ex);
    auto count_rdb = [&] {
        int n = 0;
        for (auto& e : fs::recursive_directory_iterator(dir.path)) if (e.path().extension() == ".rdb") n++;
        return n;
    };
    // Statement-level durability and COMMIT must not rewrite table files at all any more.
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,1)").is_ok());
    REQUIRE(count_rdb() == 0);
    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    for (int i = 2; i <= 50; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ",1)").is_ok());
    REQUIRE(ex.execute_sql("COMMIT").is_ok());
    REQUIRE(ex.execute_sql("UPDATE t SET v = 2 WHERE id = 1").is_ok());
    REQUIRE(count_rdb() == 0); // all of it lives in memory + rusql.redo until a checkpoint
    REQUIRE(fs::exists(dir.path + "/rusql.redo"));
    REQUIRE(fs::file_size(dir.path + "/rusql.redo") > 0);
}

// Every statement family the engine has, run in autocommit, then a crash: the recovered
// tables must be byte-for-byte what the live session saw (this is what catches a mutation
// path that neither logs redo ops nor flushes).
TEST_CASE("Redo: every statement family is durable across a crash", "[redo][durability]") {
    TempDataDir dir("redo_t10");
    std::string before_a, before_b, before_audit, before_s;
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY, v INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY, v INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE s (id INT PRIMARY KEY, v INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE audit (id INT PRIMARY KEY, note VARCHAR(40))").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1,10),(2,20),(3,30),(4,40)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO b VALUES (1,1),(2,2)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO s VALUES (2,200),(3,300),(9,900)").is_ok());
        // trigger-fired inserts (structural dispatch path)
        REQUIRE(ex.execute_sql("CREATE TRIGGER trg AFTER INSERT ON b FOR EACH ROW INSERT INTO audit VALUES (7, 'ins')").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO b VALUES (3,3)").is_ok());
        REQUIRE(ex.execute_sql("DROP TRIGGER trg").is_ok()); // PK of audit is fixed in the trigger body -- fire it exactly once
        REQUIRE(ex.execute_sql("INSERT INTO b VALUES (4,4)").is_ok());
        // INSERT ... SELECT, UPDATE with subquery, multi-table UPDATE, MERGE, DELETE with subquery, REPLACE
        REQUIRE(ex.execute_sql("INSERT INTO b SELECT id, v FROM s WHERE id = 9").is_ok());
        REQUIRE(ex.execute_sql("UPDATE a SET v = v + 1 WHERE id IN (SELECT id FROM s)").is_ok());
        REQUIRE(ex.execute_sql("UPDATE a JOIN s ON a.id = s.id SET a.v = s.v").is_ok());
        REQUIRE(ex.execute_sql("MERGE INTO a USING s ON a.id = s.id WHEN MATCHED THEN UPDATE SET v = s.v + 1 WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.v)").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM b WHERE id IN (SELECT id FROM s)").is_ok());
        REQUIRE(ex.execute_sql("REPLACE INTO a VALUES (1, 111)").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM a WHERE id = 4").is_ok());
        REQUIRE(ex.execute_sql("UPDATE a SET v = 5 WHERE id = 3").is_ok());
        before_a = rows_of(ex, "SELECT * FROM a ORDER BY id");
        before_b = rows_of(ex, "SELECT * FROM b ORDER BY id");
        before_audit = rows_of(ex, "SELECT * FROM audit ORDER BY id");
        before_s = rows_of(ex, "SELECT * FROM s ORDER BY id");
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    auto strip_time = [](std::string t) {
        auto p = t.rfind("row(s)");
        return p == std::string::npos ? t : t.substr(0, p);
    };
    REQUIRE(strip_time(rows_of(ex, "SELECT * FROM a ORDER BY id")) == strip_time(before_a));
    REQUIRE(strip_time(rows_of(ex, "SELECT * FROM b ORDER BY id")) == strip_time(before_b));
    REQUIRE(strip_time(rows_of(ex, "SELECT * FROM audit ORDER BY id")) == strip_time(before_audit));
    REQUIRE(strip_time(rows_of(ex, "SELECT * FROM s ORDER BY id")) == strip_time(before_s));
}

// Regression for a bug the crash fuzzer found: recovery's undo of an open transaction used to
// re-insert the old image of rows it had deleted/updated, assuming those changes had reached
// the table files. With redo-log commits they usually have not, so recovery manufactured a
// row version (here: the intermediate v=118) that never existed on disk.
TEST_CASE("Redo: crash with an open transaction that updated then deleted a committed row", "[redo][durability]") {
    TempDataDir dir("redo_t11");
    {
        Executor ex(dir.path);
        setup(ex);
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (14, 518)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (15, 1)").is_ok());
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("UPDATE t SET v = 118 WHERE id = 14").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 14").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (16, 9)").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 16").is_ok());
        REQUIRE(ex.execute_sql("UPDATE t SET v = 2 WHERE id = 15").is_ok());
    } // crash with the transaction open
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "t") == 2);
    REQUIRE(rows_of(ex, "SELECT v FROM t WHERE id = 14").find("518") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT v FROM t WHERE id = 15").find("| 1 ") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM t WHERE id = 16").find("0 rows") != std::string::npos);
}
