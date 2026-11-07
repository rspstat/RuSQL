#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// R72: a REPEATABLE READ (or SERIALIZABLE) transaction reads through the snapshot it took. When it changes or deletes a row that another session changed after that snapshot, the
// snapshot still showed an older version of the row next to the one the transaction wrote, so the transaction saw the row twice. These are the two-session sequences
// code/test/diff/two_sessions.py runs against a real MySQL (the expected rows are MySQL's).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : r.value().substr(0, 80)));
    REQUIRE(r.is_ok());
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    Rows rows;
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        rows.push_back(result_text::split_row(line));
    }
    return rows;
}

// two sessions A (at the level asked for) and B (autocommit) over the table t (id, v, g) with rows (1,10,1) (2,20,1) (3,30,2) and c (a, b, v) with a key of two columns
struct Sessions {
    TempDataDir dir;
    Executor a;
    Executor b;
    explicit Sessions(const std::string& name, const std::string& level = "REPEATABLE READ")
        : dir("r72_" + name), a(dir.path), b(Executor::new_session(a.get_shared())) {
        ok(a, "CREATE DATABASE d");
        ok(a, "USE d");
        ok(b, "USE d");
        ok(a, "CREATE TABLE t (id INT PRIMARY KEY, v INT, g INT, KEY (g))");
        ok(a, "INSERT INTO t VALUES (1, 10, 1), (2, 20, 1), (3, 30, 2)");
        ok(a, "CREATE TABLE c (a INT, b INT, v INT, PRIMARY KEY (a, b))");
        ok(a, "INSERT INTO c VALUES (1, 1, 10), (1, 2, 20)");
        ok(a, "CREATE TABLE p (id INT PRIMARY KEY)");
        ok(a, "INSERT INTO p VALUES (1), (2)");
        ok(a, "CREATE TABLE ch (id INT PRIMARY KEY, p INT, v INT, FOREIGN KEY (p) REFERENCES p(id) ON DELETE CASCADE ON UPDATE CASCADE)");
        ok(a, "INSERT INTO ch VALUES (1, 1, 10), (2, 2, 20)");
        ok(a, "SET SESSION TRANSACTION ISOLATION LEVEL " + level);
    }
};
const char* kAll = "SELECT * FROM t ORDER BY id";
} // namespace

TEST_CASE("R72: a row this transaction updated after another session changed it is seen once", "[r72][rr]") {
    Sessions s("update");
    ok(s.a, "BEGIN");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}}); // (the snapshot)
    ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1");                                              // (it adds to what is committed now: 11)
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "12", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
    REQUIRE(rows_of(s.a, "SELECT * FROM t WHERE id = 1") == Rows{{"1", "12", "1"}});
    REQUIRE(rows_of(s.a, "SELECT * FROM t WHERE g = 1 ORDER BY id") == Rows{{"1", "12", "1"}, {"2", "20", "1"}});
    ok(s.a, "UPDATE t SET v = v * 2 WHERE id = 1"); // (a second write to the same row)
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "24", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    ok(s.a, "COMMIT");
    REQUIRE(rows_of(s.b, kAll) == Rows{{"1", "24", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
}

TEST_CASE("R72: SERIALIZABLE sees the row once too", "[r72][rr]") {
    Sessions s("serializable", "SERIALIZABLE");
    ok(s.a, "BEGIN");
    (void)rows_of(s.a, kAll);
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "12", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
    ok(s.a, "ROLLBACK");
}

TEST_CASE("R72: a row this transaction deleted after another session changed it is gone, and may come back as a new row", "[r72][rr]") {
    Sessions s("delete");
    ok(s.a, "BEGIN");
    (void)rows_of(s.a, kAll);
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    ok(s.a, "DELETE FROM t WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"2", "20", "1"}, {"3", "30", "2"}});
    REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM t") == Rows{{"2"}});
    ok(s.a, "INSERT INTO t VALUES (1, 99, 1)");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "99", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    ok(s.a, "COMMIT");
    REQUIRE(rows_of(s.b, kAll) == Rows{{"1", "99", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
}

TEST_CASE("R72: several rows, a key that changes, a key of two columns", "[r72][rr]") {
    {
        Sessions s("several");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, kAll);
        ok(s.b, "UPDATE t SET v = v + 1 WHERE g = 1");
        ok(s.a, "UPDATE t SET v = v + 10 WHERE g = 1");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "21", "1"}, {"2", "31", "1"}, {"3", "30", "2"}});
        REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
        ok(s.a, "COMMIT");
    }
    {
        Sessions s("key_changes");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, kAll);
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
        ok(s.a, "UPDATE t SET id = 10 WHERE id = 1");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"2", "20", "1"}, {"3", "30", "2"}, {"10", "11", "1"}});
        ok(s.a, "COMMIT");
    }
    {
        Sessions s("composite");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, "SELECT * FROM c ORDER BY a, b");
        ok(s.b, "UPDATE c SET v = v + 1 WHERE a = 1 AND b = 1");
        ok(s.a, "UPDATE c SET v = v + 5 WHERE a = 1 AND b = 1");
        REQUIRE(rows_of(s.a, "SELECT * FROM c ORDER BY a, b") == Rows{{"1", "1", "16"}, {"1", "2", "20"}});
        REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM c") == Rows{{"2"}});
        ok(s.a, "COMMIT");
    }
}

TEST_CASE("R72: rows another session inserted or deleted after the snapshot", "[r72][rr]") {
    {
        Sessions s("inserted");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, kAll);
        ok(s.b, "INSERT INTO t VALUES (4, 40, 2)");
        REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
        ok(s.a, "UPDATE t SET v = 41 WHERE id = 4"); // (the update reads what is committed: the row it changes is seen from then on)
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}, {"4", "41", "2"}});
        ok(s.a, "COMMIT");
    }
    {
        Sessions s("deleted");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, kAll);
        ok(s.b, "DELETE FROM t WHERE id = 2");
        ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 2"); // (nothing is there to update)
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}}); // (the snapshot still has it)
        ok(s.a, "COMMIT");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"3", "30", "2"}});
    }
}

TEST_CASE("R72: ROLLBACK TO SAVEPOINT and ROLLBACK give the older version back", "[r72][rr]") {
    {
        Sessions s("savepoint");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, kAll);
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
        ok(s.a, "SAVEPOINT s");
        ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "12", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
        ok(s.a, "ROLLBACK TO SAVEPOINT s");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}}); // (the snapshot again: once)
        ok(s.a, "COMMIT");
    }
    {
        // ... and what an earlier statement hid stays hidden
        Sessions s("savepoint_after");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, kAll);
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
        ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1");
        ok(s.a, "SAVEPOINT s");
        ok(s.b, "UPDATE t SET v = v + 100 WHERE id = 2");
        ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 2");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "12", "1"}, {"2", "121", "1"}, {"3", "30", "2"}});
        ok(s.a, "ROLLBACK TO SAVEPOINT s");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "12", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
        ok(s.a, "COMMIT");
    }
    {
        Sessions s("rollback");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, kAll);
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
        ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1");
        ok(s.a, "ROLLBACK");
        ok(s.a, "BEGIN");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "20", "1"}, {"3", "30", "2"}}); // (a new snapshot sees what B committed, once)
        ok(s.a, "COMMIT");
    }
}

TEST_CASE("R72: a foreign key action that ends a child row another session changed leaves one version of it", "[r72][rr]") {
    {
        Sessions s("cascade_delete");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, "SELECT * FROM ch ORDER BY id");
        ok(s.b, "UPDATE ch SET v = v + 1 WHERE id = 1");
        ok(s.a, "DELETE FROM p WHERE id = 1"); // (ON DELETE CASCADE takes the child with it)
        REQUIRE(rows_of(s.a, "SELECT * FROM ch ORDER BY id") == Rows{{"2", "2", "20"}});
        REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM ch") == Rows{{"1"}});
        ok(s.a, "COMMIT");
        REQUIRE(rows_of(s.b, "SELECT * FROM ch ORDER BY id") == Rows{{"2", "2", "20"}});
    }
    {
        Sessions s("cascade_update");
        ok(s.a, "BEGIN");
        (void)rows_of(s.a, "SELECT * FROM ch ORDER BY id");
        ok(s.b, "UPDATE ch SET v = v + 1 WHERE id = 1");
        ok(s.a, "UPDATE p SET id = 10 WHERE id = 1"); // (ON UPDATE CASCADE rewrites the child)
        REQUIRE(rows_of(s.a, "SELECT * FROM ch ORDER BY id") == Rows{{"1", "10", "11"}, {"2", "2", "20"}});
        REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM ch") == Rows{{"2"}});
        ok(s.a, "COMMIT");
    }
}

TEST_CASE("R72: READ COMMITTED never had the problem and still does not", "[r72][rr]") {
    Sessions s("rc", "READ COMMITTED");
    ok(s.a, "BEGIN");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "12", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    ok(s.a, "COMMIT");
}

TEST_CASE("R72: a locking read reads what is committed now, not the snapshot", "[r72][rr]") {
    Sessions s("locking");
    ok(s.a, "BEGIN");
    (void)rows_of(s.a, kAll);
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, "SELECT * FROM t WHERE id = 1") == Rows{{"1", "10", "1"}});                  // (the snapshot)
    REQUIRE(rows_of(s.a, "SELECT * FROM t WHERE id = 1 FOR UPDATE") == Rows{{"1", "11", "1"}});       // (the latest)
    REQUIRE(rows_of(s.a, "SELECT * FROM t WHERE id = 1 LOCK IN SHARE MODE") == Rows{{"1", "11", "1"}});
    ok(s.a, "COMMIT");
}
