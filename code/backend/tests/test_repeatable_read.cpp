#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/ast_json.hpp"
#include "engine/parser/parser.hpp"
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

// R73: the snapshot is taken by the first consistent read of the transaction, as MySQL's is -- not by BEGIN. A change another session commits between BEGIN and the first read
// is seen; one committed after it is not. A write, a locking read and a SELECT without a table take no snapshot. START TRANSACTION WITH CONSISTENT SNAPSHOT takes it at once.
TEST_CASE("R73: the snapshot is taken at the first read, not at BEGIN", "[r73][rr]") {
    for (const char* level : {"REPEATABLE READ", "SERIALIZABLE"}) {
        Sessions s("first_read", level);
        ok(s.a, "BEGIN");
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "20", "1"}, {"3", "30", "2"}}); // (committed before the first read: seen)
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 2");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "20", "1"}, {"3", "30", "2"}}); // (committed after it: not)
        ok(s.a, "ROLLBACK"); // (a SERIALIZABLE transaction that read row 2 could not commit: the other session changed it since)
        REQUIRE(rows_of(s.b, kAll) == Rows{{"1", "11", "1"}, {"2", "21", "1"}, {"3", "30", "2"}});
    }
}

TEST_CASE("R73: a write, a locking read and a SELECT without a table take no snapshot", "[r73][rr]") {
    {
        Sessions s("write");
        ok(s.a, "BEGIN");
        ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1");
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 2");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "21", "1"}, {"3", "30", "2"}}); // (its own write and the other session's, which came before the first read)
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 3");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "21", "1"}, {"3", "30", "2"}});
        ok(s.a, "COMMIT");
    }
    {
        Sessions s("locking");
        ok(s.a, "BEGIN");
        REQUIRE(rows_of(s.a, "SELECT * FROM t WHERE id = 3 FOR UPDATE") == Rows{{"3", "30", "2"}});
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 2");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
        ok(s.a, "COMMIT");
    }
    {
        Sessions s("no_table");
        ok(s.a, "BEGIN");
        REQUIRE(rows_of(s.a, "SELECT 1") == Rows{{"1"}});
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
        ok(s.a, "COMMIT");
    }
    {
        Sessions s("trigger");
        ok(s.a, "CREATE TABLE tl (n INT)");
        ok(s.a, "CREATE TRIGGER t_au AFTER UPDATE ON t FOR EACH ROW INSERT INTO tl VALUES (NEW.id)");
        ok(s.a, "BEGIN");
        ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1"); // (finds the rows it fires the trigger for: not a consistent read)
        ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 2");
        REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "21", "1"}, {"3", "30", "2"}});
        ok(s.a, "COMMIT");
    }
}

TEST_CASE("R73: a read of another table takes the snapshot for all of them", "[r73][rr]") {
    Sessions s("other_table");
    ok(s.a, "BEGIN");
    REQUIRE(rows_of(s.a, "SELECT * FROM p ORDER BY id") == Rows{{"1"}, {"2"}});
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    ok(s.a, "COMMIT");
}

TEST_CASE("R73: START TRANSACTION WITH CONSISTENT SNAPSHOT takes it at once", "[r73][rr]") {
    {
        Parser parser("START TRANSACTION WITH CONSISTENT SNAPSHOT");
        auto parsed = parser.parse();
        REQUIRE(parsed.is_ok());
        REQUIRE(std::get<Statement::Begin>(parsed.value().data).consistent_snapshot);
        nlohmann::json j = parsed.value();
        REQUIRE(std::get<Statement::Begin>(j.get<Statement>().data).consistent_snapshot);
        Parser both("START TRANSACTION READ ONLY, WITH CONSISTENT SNAPSHOT");
        auto b = both.parse();
        REQUIRE(b.is_ok());
        REQUIRE(std::get<Statement::Begin>(b.value().data).consistent_snapshot);
        REQUIRE(std::get<Statement::Begin>(b.value().data).read_only == std::optional<bool>(true));
        Parser plain("START TRANSACTION");
        REQUIRE(!std::get<Statement::Begin>(plain.parse().value().data).consistent_snapshot);
    }
    Sessions s("consistent");
    ok(s.a, "START TRANSACTION WITH CONSISTENT SNAPSHOT");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    ok(s.a, "COMMIT");
    ok(s.a, "START TRANSACTION"); // (plain: the snapshot waits for the first read)
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 2");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "21", "1"}, {"3", "30", "2"}});
    ok(s.a, "COMMIT");
}

TEST_CASE("R73: the next transaction and COMMIT AND CHAIN take their own snapshots", "[r73][rr]") {
    Sessions s("next");
    ok(s.a, "BEGIN");
    (void)rows_of(s.a, kAll);
    ok(s.a, "COMMIT");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    ok(s.a, "BEGIN");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 2");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "21", "1"}, {"3", "30", "2"}});
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 3");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "11", "1"}, {"2", "21", "1"}, {"3", "30", "2"}});
    ok(s.a, "COMMIT AND CHAIN");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "12", "1"}, {"2", "21", "1"}, {"3", "31", "2"}});
    ok(s.a, "COMMIT");
}

TEST_CASE("R73: the duplicate-version fix still holds when the snapshot comes late", "[r73][rr]") {
    Sessions s("late");
    ok(s.a, "BEGIN");
    (void)rows_of(s.a, "SELECT 1");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    (void)rows_of(s.a, kAll);                       // (the snapshot: v = 11)
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1"); // (12, after the snapshot)
    ok(s.a, "UPDATE t SET v = v + 1 WHERE id = 1"); // (adds to what is committed now)
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "13", "1"}, {"2", "20", "1"}, {"3", "30", "2"}});
    REQUIRE(rows_of(s.a, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
    ok(s.a, "COMMIT");
}

TEST_CASE("R73: the source of an INSERT ... SELECT is read as a locking read: what is committed now, and no snapshot taken", "[r73][rr]") {
    Sessions s("insert_select");
    ok(s.a, "CREATE TABLE d (id INT PRIMARY KEY, v INT)");
    ok(s.a, "INSERT INTO d VALUES (1, 10)");
    ok(s.a, "BEGIN");
    (void)rows_of(s.a, kAll); // (the snapshot)
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    ok(s.a, "INSERT INTO d SELECT id + 10, v FROM t"); // (copies the 11)
    REQUIRE(rows_of(s.a, "SELECT * FROM d ORDER BY id") == Rows{{"1", "10"}, {"11", "11"}, {"12", "20"}, {"13", "30"}});
    REQUIRE(rows_of(s.a, kAll) == Rows{{"1", "10", "1"}, {"2", "20", "1"}, {"3", "30", "2"}}); // (the snapshot is still the old one)
    ok(s.a, "COMMIT");
    ok(s.a, "BEGIN");
    ok(s.a, "INSERT INTO d SELECT id + 20, v FROM t"); // (no snapshot yet)
    ok(s.b, "UPDATE d SET v = v + 5 WHERE id = 1");
    REQUIRE(rows_of(s.a, "SELECT v FROM d WHERE id = 1") == Rows{{"15"}}); // (the first read takes it now)
    ok(s.b, "UPDATE d SET v = v + 5 WHERE id = 1");
    REQUIRE(rows_of(s.a, "SELECT v FROM d WHERE id = 1") == Rows{{"15"}});
    ok(s.a, "COMMIT");
}

// R74: what MySQL does with the tables a statement reads. In a REPEATABLE READ transaction every statement that is not a plain SELECT reads them as a locking read does -- what is
// committed now, not the snapshot (the expected values are what a real MySQL 8.4 answered to the same sequences).
namespace {
struct ReadSessions : Sessions {
    explicit ReadSessions(const std::string& name, const std::string& level = "REPEATABLE READ") : Sessions(name, level) {
        ok(a, "CREATE TABLE d (id INT PRIMARY KEY, v INT)");
        ok(a, "CREATE TABLE u (id INT PRIMARY KEY, v INT)");
        ok(a, "CREATE TABLE lg (n INT AUTO_INCREMENT PRIMARY KEY, v INT)");
        ok(a, "CREATE TRIGGER trg_ins AFTER UPDATE ON u FOR EACH ROW INSERT INTO lg (v) VALUES ((SELECT v FROM t WHERE id = 1))");
        ok(a, "CREATE TRIGGER trg_sel AFTER UPDATE ON d FOR EACH ROW BEGIN DECLARE x INT; SELECT v INTO x FROM t WHERE id = 1; INSERT INTO lg (v) VALUES (x); END");
        ok(a, "CREATE PROCEDURE p_into() BEGIN DECLARE x INT; SELECT v INTO x FROM t WHERE id = 1; SET @y = x; END");
        ok(a, "CREATE PROCEDURE p_ins() BEGIN INSERT INTO d VALUES (5, (SELECT v FROM t WHERE id = 1)); END");
        ok(a, "CREATE PROCEDURE p_set() BEGIN DECLARE x INT; SET x = (SELECT v FROM t WHERE id = 1); SET @z = x; END");
    }
    // t.v of row 1 is 10 in A's snapshot and 11 once B changed it
    void start() {
        ok(b, "UPDATE t SET v = 10 WHERE id = 1");
        ok(a, "DELETE FROM d");
        ok(a, "INSERT INTO d VALUES (1, 10)");
        ok(a, "DELETE FROM u");
        ok(a, "INSERT INTO u VALUES (1, 1)");
        ok(a, "DELETE FROM lg");
        ok(a, "BEGIN");
        (void)rows_of(a, kAll);
        ok(b, "UPDATE t SET v = 11 WHERE id = 1");
    }
    void finish() {
        REQUIRE(rows_of(a, "SELECT v FROM t WHERE id = 1") == Rows{{"10"}}); // (A's own reads still use the snapshot)
        ok(a, "ROLLBACK");
    }
};
} // namespace

TEST_CASE("R74: INSERT, REPLACE, UPDATE, DELETE and SET read their subqueries as what is committed now", "[r74][rr]") {
    ReadSessions s("reads");
    struct Case {
        const char* statement;
        const char* show;
        Rows expect;
    };
    const Case cases[] = {
        {"INSERT INTO d VALUES (5, (SELECT v FROM t WHERE id = 1))", "SELECT v FROM d WHERE id = 5", {{"11"}}},
        {"INSERT INTO d VALUES (1, 0) ON DUPLICATE KEY UPDATE v = (SELECT v FROM t WHERE id = 1)", "SELECT v FROM d WHERE id = 1", {{"11"}}},
        {"REPLACE INTO d VALUES (1, (SELECT v FROM t WHERE id = 1))", "SELECT v FROM d WHERE id = 1", {{"11"}}},
        {"UPDATE d SET v = (SELECT v FROM t WHERE id = 1) WHERE id = 1", "SELECT v FROM d WHERE id = 1", {{"11"}}},
        {"UPDATE d SET v = 7 WHERE v < (SELECT v FROM t WHERE id = 1)", "SELECT v FROM d WHERE id = 1", {{"7"}}},
        {"DELETE FROM d WHERE v < (SELECT v FROM t WHERE id = 1)", "SELECT COUNT(*) FROM d", {{"0"}}},
        {"UPDATE d JOIN t ON t.id = d.id SET d.v = t.v", "SELECT v FROM d WHERE id = 1", {{"11"}}},
        {"DELETE d FROM d JOIN t ON t.id = d.id WHERE t.v = 11", "SELECT COUNT(*) FROM d", {{"0"}}},
        {"UPDATE d JOIN u ON u.id = d.id SET d.v = 7 WHERE d.v < (SELECT v FROM t WHERE id = 1)", "SELECT v FROM d WHERE id = 1", {{"7"}}},
        {"DELETE d FROM d JOIN u ON u.id = d.id WHERE d.v < (SELECT v FROM t WHERE id = 1)", "SELECT COUNT(*) FROM d", {{"0"}}},
        {"INSERT INTO d SELECT id + 10, (SELECT v FROM t WHERE id = 1) FROM u", "SELECT v FROM d WHERE id = 11", {{"11"}}},
        {"SET @x = (SELECT v FROM t WHERE id = 1)", "SELECT @x", {{"11"}}},
        // a plain SELECT, SELECT ... INTO included, reads the snapshot
        {"SELECT v INTO @x FROM t WHERE id = 1", "SELECT @x", {{"10"}}},
        // the statements of a trigger and of a procedure count each by its own kind
        {"UPDATE u SET v = v + 1 WHERE id = 1", "SELECT v FROM lg", {{"11"}}},
        {"UPDATE d SET v = v + 1 WHERE id = 1", "SELECT v FROM lg", {{"10"}}}, // (a SELECT ... INTO of the body: the snapshot)
        {"CALL p_into()", "SELECT @y", {{"10"}}},
        {"CALL p_ins()", "SELECT v FROM d WHERE id = 5", {{"11"}}},
        {"CALL p_set()", "SELECT @z", {{"11"}}},
    };
    for (const Case& c : cases) {
        INFO(c.statement);
        s.start();
        ok(s.a, c.statement);
        REQUIRE(rows_of(s.a, c.show) == c.expect);
        s.finish();
    }
    // the SELECT of a WHERE reads the snapshot: 10 < 10 is false
    s.start();
    REQUIRE(rows_of(s.a, "SELECT id FROM d WHERE v < (SELECT v FROM t WHERE id = 1)").empty());
    s.finish();
}

TEST_CASE("R74: in READ COMMITTED every statement reads what is committed", "[r74][rr]") {
    ReadSessions s("reads_rc", "READ COMMITTED");
    s.start();
    REQUIRE(rows_of(s.a, "SELECT id FROM d WHERE v < (SELECT v FROM t WHERE id = 1)") == Rows{{"1"}});
    ok(s.a, "UPDATE d SET v = (SELECT v FROM t WHERE id = 1) WHERE id = 1");
    REQUIRE(rows_of(s.a, "SELECT v FROM d WHERE id = 1") == Rows{{"11"}});
    ok(s.a, "ROLLBACK");
}

namespace {
bool blocked(Executor& ex, const std::string& sql) { // (the statement waits for a lock until the timeout of the session ends)
    auto r = ex.execute_sql(sql);
    return r.is_err() && r.error().find("1205") != std::string::npos;
}
} // namespace

TEST_CASE("R74: the rows an INSERT ... SELECT reads are locked for sharing until the transaction ends (REPEATABLE READ)", "[r74][rr][locks]") {
    ReadSessions s("source_locks");
    ok(s.b, "SET @lock_wait_timeout = 250");
    ok(s.a, "INSERT INTO d VALUES (1, 10)");
    ok(s.a, "BEGIN");
    ok(s.a, "INSERT INTO d SELECT id + 10, v FROM t WHERE id = 1");
    REQUIRE(blocked(s.b, "UPDATE t SET v = v + 1 WHERE id = 1")); // the row it read
    REQUIRE(blocked(s.b, "DELETE FROM t WHERE id = 1"));
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 2");                // another row
    ok(s.b, "INSERT INTO t VALUES (4, 40, 2)");                    // outside the range it read
    ok(s.b, "UPDATE d SET v = v + 1 WHERE id = 1");                // the table it wrote is not the one it read
    ok(s.a, "COMMIT");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1"); // its locks are gone
    // the whole table: the rows and the gaps between and after them
    ok(s.a, "BEGIN");
    ok(s.a, "INSERT INTO d SELECT id + 20, v FROM t");
    REQUIRE(blocked(s.b, "UPDATE t SET v = v + 1 WHERE id = 3"));
    REQUIRE(blocked(s.b, "INSERT INTO t VALUES (9, 90, 9)"));
    ok(s.a, "ROLLBACK");
    ok(s.b, "INSERT INTO t VALUES (9, 90, 9)");
}

TEST_CASE("R74: an INSERT ... SELECT locks nothing in READ COMMITTED or outside a transaction", "[r74][rr][locks]") {
    ReadSessions s("source_no_locks", "READ COMMITTED");
    ok(s.b, "SET @lock_wait_timeout = 250");
    ok(s.a, "BEGIN");
    ok(s.a, "INSERT INTO d SELECT id + 10, v FROM t");
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
    ok(s.a, "COMMIT");
    ok(s.a, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    ok(s.a, "INSERT INTO d SELECT id + 20, v FROM t"); // (autocommit: the statement is the transaction)
    ok(s.b, "UPDATE t SET v = v + 1 WHERE id = 1");
}

namespace {
// A waits (in its own thread) for the row B wrote and has not committed; B commits (or rolls back) after a while. What A's statement read is what is committed by then.
struct WaitingRead {
    std::string answer;
    bool err = false;
    long long waited_ms = 0;
};
WaitingRead read_after_wait(ReadSessions& s, const std::string& statement, bool b_commits) {
    ok(s.b, "BEGIN");
    ok(s.b, "UPDATE t SET v = 99 WHERE id = 1");
    ok(s.a, "BEGIN");
    WaitingRead out;
    std::thread reader([&] {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = s.a.execute_sql(statement);
        out.waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        out.err = r.is_err();
        out.answer = r.is_err() ? r.error() : r.value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ok(s.b, b_commits ? "COMMIT" : "ROLLBACK");
    reader.join();
    return out;
}
} // namespace

TEST_CASE("R74: a locking read that waited for a lock reads what the other transaction committed meanwhile", "[r74][rr][locks]") {
    for (const char* statement : {"SELECT v FROM t WHERE id = 1 FOR UPDATE", "SELECT v FROM t WHERE id = 1 FOR SHARE"}) {
        INFO(statement);
        ReadSessions s("wait_read");
        WaitingRead committed = read_after_wait(s, statement, true);
        REQUIRE_FALSE(committed.err);
        REQUIRE(committed.waited_ms >= 300);
        REQUIRE(committed.answer.find("| 99 ") != std::string::npos);
        ok(s.a, "ROLLBACK");
    }
    // and when the other transaction rolled back, the old value
    ReadSessions s("wait_read_rollback");
    WaitingRead rolled_back = read_after_wait(s, "SELECT v FROM t WHERE id = 1 FOR UPDATE", false);
    REQUIRE_FALSE(rolled_back.err);
    REQUIRE(rolled_back.answer.find("| 10 ") != std::string::npos);
    ok(s.a, "ROLLBACK");
}

TEST_CASE("R74: an INSERT ... SELECT that waited for a lock copies what the other transaction committed meanwhile", "[r74][rr][locks]") {
    ReadSessions s("wait_insert_select");
    WaitingRead copied = read_after_wait(s, "INSERT INTO d SELECT id + 10, v FROM t WHERE id = 1", true);
    REQUIRE_FALSE(copied.err);
    REQUIRE(copied.waited_ms >= 300);
    REQUIRE(rows_of(s.a, "SELECT * FROM d ORDER BY id") == Rows{{"11", "99"}});
    ok(s.a, "COMMIT");
}
