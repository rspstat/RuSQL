#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

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

// A table without a primary key can hold rows with identical images (same values, same _xmin when one transaction
// inserted them). The replay used to treat "identical image" as "already present": `BEGIN; INSERT (1,'x'),(1,'x');
// COMMIT` came back with one row after a crash, and killing two identical rows stamped only one of them.
namespace {
std::string count_where(Executor& ex, const std::string& table, const std::string& where) {
    return rows_of(ex, "SELECT COUNT(*) FROM " + table + " WHERE " + where);
}
bool has_count(const std::string& out, int n) { return out.find("| " + std::to_string(n) + " ") != std::string::npos; }
} // namespace

TEST_CASE("Redo: identical rows of a table without a primary key all survive recovery", "[redo][durability]") {
    TempDataDir dir("redo_dup_rows");
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE np (a INT, b VARCHAR(5))").is_ok());
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO np VALUES (1, 'x'), (1, 'x'), (1, 'x'), (2, 'y'), (2, 'y')").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO np VALUES (3, 'z'), (3, 'z')").is_ok()); // autocommit
    } // crash
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(count_rows(ex, "np") == 7);
        REQUIRE(has_count(count_where(ex, "np", "a = 1"), 3));
        REQUIRE(has_count(count_where(ex, "np", "a = 2"), 2));
        // the same shapes again on top of the recovered state: kill two identical rows, rewrite three
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM np WHERE a = 2").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("UPDATE np SET b = 'w' WHERE a = 1").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());
    } // crash again
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "np") == 5);
    REQUIRE(has_count(count_where(ex, "np", "a = 2"), 0));
    REQUIRE(has_count(count_where(ex, "np", "b = 'w'"), 3));
    REQUIRE(has_count(count_where(ex, "np", "b = 'x'"), 0));
    REQUIRE(has_count(count_where(ex, "np", "a = 3"), 2));
}

TEST_CASE("Redo: replaying a log whose effects are already in the table files changes nothing", "[redo][durability]") {
    TempDataDir dir("redo_replay_twice");
    const std::string redo = dir.path + "/rusql.redo";
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE np (a INT, b VARCHAR(5))").is_ok());
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO np VALUES (1, 'x'), (1, 'x'), (1, 'x'), (2, 'y'), (2, 'y')").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM np WHERE a = 2").is_ok());
        REQUIRE(ex.execute_sql("UPDATE np SET b = 'w' WHERE a = 1").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());
    } // crash
    REQUIRE(fs::exists(redo));
    fs::copy_file(redo, dir.path + "/redo.saved", fs::copy_options::overwrite_existing);
    {
        Executor ex(dir.path); // first recovery: replays, flushes the table files, clears the log
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(count_rows(ex, "np") == 3);
    }
    // a crash between "table files written" and "log cleared": the old log is back next to the already-updated files
    fs::copy_file(dir.path + "/redo.saved", redo, fs::copy_options::overwrite_existing);
    for (int boot = 0; boot < 2; boot++) {
        {
            Executor ex(dir.path);
            REQUIRE(ex.execute_sql("USE d").is_ok());
            REQUIRE(count_rows(ex, "np") == 3);
            REQUIRE(has_count(count_where(ex, "np", "b = 'w'"), 3));
            REQUIRE(has_count(count_where(ex, "np", "a = 2"), 0));
        }
        fs::copy_file(dir.path + "/redo.saved", redo, fs::copy_options::overwrite_existing);
    }
}

// An INSERT into a table with a foreign key reads the parent table (its lock set holds two tables) but writes its own table only, which the redo ops describe. It
// used to take the legacy path for that reason -- both table files rewritten and a marker fsynced after every statement (~7 ms against 0.7 ms for a table without a
// foreign key). Now it is logged like any INSERT, and a COMMIT of such rows appends to the log too. What the parent holds is read at insert time and not touched.
TEST_CASE("Redo: an INSERT into a child table is logged, not flushed, and survives a crash", "[redo][durability]") {
    TempDataDir dir("redo_child_insert");
    auto count_rdb = [&] {
        int n = 0;
        for (auto& e : fs::recursive_directory_iterator(dir.path)) if (e.path().extension() == ".rdb") n++;
        return n;
    };
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE p (id INT PRIMARY KEY, n INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1,0),(2,0),(3,0)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (10,1)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (11,2), (12,2)").is_ok());
        REQUIRE(count_rdb() == 0); // no table file was written for them
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (13,99)").is_err()); // the parent is still looked for
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (20,3)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (21,3)").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());
        REQUIRE(count_rdb() == 0);
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (30,1)").is_ok());
        REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO c VALUES (31,1)").is_ok()); // never committed
        REQUIRE(count_rdb() == 0);
    } // crash
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(count_rows(ex, "p") == 3);
    REQUIRE(count_rows(ex, "c") == 5); // 10, 11, 12, 20, 21
    REQUIRE(rows_of(ex, "SELECT * FROM c WHERE id = 30").find("0 rows") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM c WHERE id = 31").find("0 rows") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT * FROM c WHERE id = 21").find("1 row(s)") != std::string::npos);
    // the recovered child still has its parent's key: the cascade and the check work
    REQUIRE(ex.execute_sql("INSERT INTO c VALUES (40,99)").is_err());
    REQUIRE(ex.execute_sql("DELETE FROM p WHERE id = 2").is_ok());
    REQUIRE(count_rows(ex, "c") == 3);
}

// ... but only an INSERT is logged although its lock set holds two tables. An UPDATE or DELETE of a parent cascades into the children, and a cascade is not in the redo
// ops: it must still be made durable the old way, whatever else is in the lock set. And so must an UPDATE of a child (a foreign key of it is read, its parent is locked).
// Each case ends on the statement under test, so that no later flush can hide it.
TEST_CASE("Redo: a cascade and an UPDATE of a child survive a crash right after logged child INSERTs", "[redo][durability]") {
    const std::vector<std::string> last = {"UPDATE p SET id = 20 WHERE id = 1",  // ON UPDATE CASCADE: c 10 follows
                                           "DELETE FROM p WHERE id = 2",        // ON DELETE CASCADE: c 11 goes
                                           "UPDATE c SET pid = 4 WHERE id = 12"}; // a child moves to another parent
    for (std::size_t which = 0; which < last.size(); which++) {
        INFO(last[which]);
        TempDataDir dir("redo_child_cascade");
        {
            Executor ex(dir.path);
            REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
            REQUIRE(ex.execute_sql("USE d").is_ok());
            REQUIRE(ex.execute_sql("CREATE TABLE p (id INT PRIMARY KEY, n INT)").is_ok());
            REQUIRE(ex.execute_sql("CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON UPDATE CASCADE ON DELETE CASCADE)").is_ok());
            REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1,0),(2,0),(3,0),(4,0)").is_ok());
            REQUIRE(ex.execute_sql("INSERT INTO c VALUES (10,1),(11,2),(12,3),(13,4)").is_ok()); // logged
            REQUIRE(ex.execute_sql("INSERT INTO c VALUES (14,3)").is_ok());                      // logged
            REQUIRE(ex.execute_sql(last[which]).is_ok());
        } // crash
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("USE d").is_ok());
        if (which == 0) {
            REQUIRE(count_rows(ex, "p") == 4);
            REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = 20").find("1 row(s)") != std::string::npos);
            REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = 1").find("0 rows") != std::string::npos);
            REQUIRE(count_rows(ex, "c") == 5);
        } else if (which == 1) {
            REQUIRE(count_rows(ex, "p") == 3);
            REQUIRE(rows_of(ex, "SELECT * FROM c WHERE id = 11").find("0 rows") != std::string::npos);
            REQUIRE(count_rows(ex, "c") == 4);
        } else {
            REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = 4").find("2 row(s)") != std::string::npos); // 12 and 13
            REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = 3").find("1 row(s)") != std::string::npos); // 14
            REQUIRE(count_rows(ex, "c") == 5);
        }
    }
}

// REPLACE, INSERT IGNORE, ON DUPLICATE KEY UPDATE, INSERT ... SELECT, and an UPDATE / DELETE with a foreign key on either side are all logged now (their cascades included):
// none of them writes a table file, and every one of them survives a crash -- also when it is the last statement before the crash.
TEST_CASE("Redo: REPLACE, IGNORE, ON DUPLICATE KEY UPDATE, INSERT ... SELECT and statements with foreign keys are logged, not flushed", "[redo][durability]") {
    TempDataDir dir("redo_more_kinds");
    auto count_rdb = [&] {
        int n = 0;
        for (auto& e : fs::recursive_directory_iterator(dir.path)) if (e.path().extension() == ".rdb") n++;
        return n;
    };
    const std::vector<std::string> statements = {
        "REPLACE INTO c VALUES (10, 2, 'r')",
        "INSERT IGNORE INTO c VALUES (11, 3, 'i'), (13, 3, 'j')",
        "INSERT INTO c VALUES (11, 3, 'o') ON DUPLICATE KEY UPDATE tag = 'updated'",
        "INSERT INTO c SELECT id + 100, pid, tag FROM c WHERE id < 13",
        "UPDATE c SET pid = 3 WHERE id = 12",
        "DELETE FROM c WHERE id = 12",
        "UPDATE p SET n = 7 WHERE id = 1",
        "UPDATE p SET id = 21 WHERE id = 2",   // ON UPDATE CASCADE
        "DELETE FROM p WHERE id = 3",          // ON DELETE CASCADE
    };
    for (std::size_t last = 0; last < statements.size(); last++) {
        INFO("last statement: " << statements[last]);
        fs::remove_all(dir.path);
        std::string before_p, before_c;
        {
            Executor ex(dir.path);
            REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
            REQUIRE(ex.execute_sql("USE d").is_ok());
            REQUIRE(ex.execute_sql("CREATE TABLE p (id INT PRIMARY KEY, n INT)").is_ok());
            REQUIRE(ex.execute_sql("CREATE TABLE c (id INT PRIMARY KEY, pid INT, tag VARCHAR(8), FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE ON UPDATE CASCADE)").is_ok());
            REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1,0),(2,0),(3,0)").is_ok());
            REQUIRE(ex.execute_sql("INSERT INTO c VALUES (10,1,'a'),(11,2,'b'),(12,3,'c')").is_ok());
            for (std::size_t i = 0; i <= last; i++) REQUIRE(ex.execute_sql(statements[i]).is_ok());
            REQUIRE(count_rdb() == 0); // nothing was written to a table file
            before_p = rows_of(ex, "SELECT * FROM p ORDER BY id");
            before_c = rows_of(ex, "SELECT * FROM c ORDER BY id");
        } // crash
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("USE d").is_ok());
        auto strip_time = [](std::string t) {
            auto p = t.rfind("row(s)");
            return p == std::string::npos ? t : t.substr(0, p);
        };
        REQUIRE(strip_time(rows_of(ex, "SELECT * FROM p ORDER BY id")) == strip_time(before_p));
        REQUIRE(strip_time(rows_of(ex, "SELECT * FROM c ORDER BY id")) == strip_time(before_c));
    }
}

// A table file is rewritten in the middle of an open transaction by a checkpoint, by another session's statement that flushes the tables, or by ROLLBACK TO
// SAVEPOINT of the transaction itself. The file must hold committed work only: a ROLLBACK changes the rows in memory and nothing else, so the versions the
// transaction had made (rows it inserted, rows it updated or deleted) would stay in the file and come back after a crash.
TEST_CASE("Redo: a table flushed in the middle of an open transaction does not keep its rows after ROLLBACK", "[redo][durability]") {
    // the table as "id=v;" in id order
    auto dump = [](Executor& ex) {
        auto r = ex.execute_sql("SELECT id, v FROM t ORDER BY id");
        REQUIRE(r.is_ok());
        std::string out, line;
        std::istringstream in(r.value());
        while (std::getline(in, line)) {
            if (line.empty() || line[0] != '|') continue;
            std::vector<std::string> cells;
            std::istringstream cs(line);
            std::string cell;
            while (std::getline(cs, cell, '|')) {
                cell.erase(0, cell.find_first_not_of(" \t\r"));
                cell.erase(cell.find_last_not_of(" \t\r") + 1);
                if (!cell.empty()) cells.push_back(cell);
            }
            if (cells.size() == 2 && std::isdigit(static_cast<unsigned char>(cells[0][0]))) out += cells[0] + "=" + cells[1] + ";";
        }
        return out;
    };
    enum Flush { OwnRollbackTo, OtherSessionDdl };
    enum End { Rollback, Commit, StayOpen };
    const char* flush_name[] = {"ROLLBACK TO SAVEPOINT of the transaction", "DDL of another session"};
    const char* end_name[] = {"ROLLBACK", "COMMIT", "crash with the transaction open"};
    const char* work_name[] = {"INSERT + UPDATE + DELETE", "DELETE of a committed row only"};
    for (int f = 0; f < 2; f++) {
        for (int e = 0; e < 3; e++) {
            for (int w = 0; w < 2; w++) {
                INFO("flushed by " << flush_name[f] << ", ended by " << end_name[e] << ", work: " << work_name[w]);
                TempDataDir dir("redo_mid_txn_flush");
                // (the INSERT of the other session, id 4, is committed in every case)
                const std::string want = e != Commit ? "1=10;2=20;3=30;4=40;" : w == 0 ? "1=100;3=30;4=40;10=10;" : "1=10;3=30;4=40;";
                {
                    Executor a(dir.path);
                    setup(a);
                    for (int i = 1; i <= 3; i++) REQUIRE(a.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i * 10) + ")").is_ok());
                    Executor b = Executor::new_session(a.get_shared());
                    REQUIRE(b.execute_sql("USE d").is_ok());
                    REQUIRE(a.execute_sql("BEGIN").is_ok());
                    if (w == 0) {
                        REQUIRE(a.execute_sql("INSERT INTO t VALUES (10,10)").is_ok());
                        REQUIRE(a.execute_sql("UPDATE t SET v = 100 WHERE id = 1").is_ok());
                    }
                    REQUIRE(a.execute_sql("DELETE FROM t WHERE id = 2").is_ok());
                    // (the other session's INSERT leaves a table with changes only the redo log holds, which is what makes the next statement that
                    // flushes the tables write the file -- and nothing may follow the end of the transaction, or a later flush would hide the fault)
                    REQUIRE(b.execute_sql("INSERT INTO t VALUES (4,40)").is_ok());
                    if (f == OwnRollbackTo) {
                        REQUIRE(a.execute_sql("SAVEPOINT s").is_ok());
                        REQUIRE(a.execute_sql("ROLLBACK TO s").is_ok());
                    } else {
                        REQUIRE(b.execute_sql("CREATE TABLE u (id INT PRIMARY KEY)").is_ok());
                    }
                    if (e == Rollback) REQUIRE(a.execute_sql("ROLLBACK").is_ok());
                    if (e == Commit) REQUIRE(a.execute_sql("COMMIT").is_ok());
                } // crash
                Executor ex(dir.path);
                REQUIRE(ex.execute_sql("USE d").is_ok());
                REQUIRE(dump(ex) == want);
            }
        }
    }
}

// ALTER TABLE rewrites the table file itself (the columns changed in every row). Another session's transaction that is still open must not get its rows into that file:
// after its ROLLBACK and a crash the file would bring them back.
TEST_CASE("Redo: an ALTER TABLE in the middle of another session's transaction does not put its rows in the file", "[redo][durability]") {
    struct Alter {
        const char* name;
        std::vector<std::string> statements;
        std::string table, column;
    };
    const std::vector<Alter> alters = {
        {"ADD COLUMN", {"ALTER TABLE t ADD COLUMN c INT"}, "t", "v"},
        {"DROP COLUMN", {"ALTER TABLE t ADD COLUMN c INT", "ALTER TABLE t DROP COLUMN c"}, "t", "v"},
        {"RENAME COLUMN", {"ALTER TABLE t RENAME COLUMN v TO w"}, "t", "w"},
        {"RENAME TO", {"ALTER TABLE t RENAME TO t2"}, "t2", "v"},
    };
    for (auto& alter : alters) {
        for (int open_at_crash = 0; open_at_crash < 2; open_at_crash++) {
            INFO(alter.name << (open_at_crash ? ", transaction open at the crash" : ", transaction rolled back"));
            TempDataDir dir("redo_alter_mid_txn");
            auto dump = [&](Executor& ex) {
                auto r = ex.execute_sql("SELECT id, " + alter.column + " FROM " + alter.table + " ORDER BY id");
                REQUIRE(r.is_ok());
                std::string out, line;
                std::istringstream in(r.value());
                while (std::getline(in, line)) {
                    if (line.empty() || line[0] != '|') continue;
                    std::istringstream cs(line);
                    std::string cell, joined;
                    int cells = 0;
                    while (std::getline(cs, cell, '|')) {
                        cell.erase(0, cell.find_first_not_of(" \t\r"));
                        cell.erase(cell.find_last_not_of(" \t\r") + 1);
                        if (cell.empty()) continue;
                        joined += (cells++ ? "=" : "") + cell;
                    }
                    if (cells == 2 && std::isdigit(static_cast<unsigned char>(joined[0]))) out += joined + ";";
                }
                return out;
            };
            {
                Executor a(dir.path);
                setup(a);
                for (int i = 1; i <= 3; i++) REQUIRE(a.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i * 10) + ")").is_ok());
                Executor b = Executor::new_session(a.get_shared());
                REQUIRE(b.execute_sql("USE d").is_ok());
                REQUIRE(a.execute_sql("BEGIN").is_ok());
                REQUIRE(a.execute_sql("INSERT INTO t VALUES (10,10)").is_ok());
                REQUIRE(a.execute_sql("UPDATE t SET v = 100 WHERE id = 1").is_ok());
                REQUIRE(a.execute_sql("DELETE FROM t WHERE id = 2").is_ok());
                for (auto& sql : alter.statements) REQUIRE(b.execute_sql(sql).is_ok());
                if (!open_at_crash) REQUIRE(a.execute_sql("ROLLBACK").is_ok()); // (nothing may follow: a later flush would hide the fault)
            } // crash
            Executor ex(dir.path);
            REQUIRE(ex.execute_sql("USE d").is_ok());
            REQUIRE(dump(ex) == "1=10;2=20;3=30;");
        }
    }
}

// R51: information_schema.recovery -- one row about the last start of the engine: how long it took to open the data and what the crash recovery replayed.
TEST_CASE("R51: information_schema.recovery reports what the last start did", "[r51][redo][durability]") {
    TempDataDir dir("r51_recovery");
    auto crash_detected = [](Executor& ex) {
        auto v = rows_of(ex, "SELECT CRASH_DETECTED FROM information_schema.recovery");
        return v.find("YES") != std::string::npos ? std::string("YES") : v.find("NO") != std::string::npos ? std::string("NO") : v;
    };
    {
        Executor ex(dir.path);
        REQUIRE(crash_detected(ex) == "NO"); // a fresh directory has nothing to recover
        setup(ex);
        for (int i = 1; i <= 5; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i * 10) + ")").is_ok());
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (6,60)").is_ok()); // still open at the crash
    } // crash
    {
        Executor ex(dir.path);
        REQUIRE(crash_detected(ex) == "YES");
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(count_rows(ex, "t") == 5);
        // the numbers are there: the redo log replayed the five inserts, the WAL undid the transaction that was open, and the start took some time
        REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM information_schema.recovery WHERE REDO_OPS >= 5 AND REDO_TABLES >= 1 AND WAL_TXNS_UNDONE >= 1 AND OPEN_MS > 0").find("| 1 ") != std::string::npos);
    } // (no crash this time: the recovery cleaned the log)
    Executor ex(dir.path);
    REQUIRE(crash_detected(ex) == "NO");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM information_schema.recovery WHERE REDO_OPS = 0 AND WAL_TXNS_REPLAYED = 0 AND WAL_TXNS_UNDONE = 0").find("| 1 ") != std::string::npos);
}
