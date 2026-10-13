#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A transaction that has deleted a row, or moved its UNIQUE key, has not given the value away yet: it comes back if the transaction rolls back. The engine used to
// let another session INSERT the value in the meantime, and the ROLLBACK then left two rows holding it. Now the DELETE / UPDATE holds the value (a claim in the lock
// manager, the same one an INSERT takes) to the end of its transaction: the INSERT waits for it, and looks again when it is over -- a ROLLBACK brings the row back and the
// INSERT is a duplicate, a COMMIT frees the value and the INSERT goes in. (InnoDB waits for the delete in the same way.)

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

Rows table_cells(const std::string& output) {
    Rows rows;
    std::istringstream in(output);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        std::vector<std::string> row;
        std::size_t pos = 1;
        while (pos < line.size()) {
            std::size_t bar = line.find('|', pos);
            if (bar == std::string::npos) break;
            std::string v = line.substr(pos, bar - pos);
            v.erase(0, v.find_first_not_of(' '));
            v.erase(v.find_last_not_of(' ') + 1);
            row.push_back(v);
            pos = bar + 1;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return table_cells(r.value());
}

// Runs `sql` in `session` on a thread, waits until it is waiting for a lock (it must be: the test fails if it comes back by itself), calls `then`, and returns the answer.
std::optional<StringResult> run_blocked(Executor& session, const std::shared_ptr<RwLock<SharedDatabase>>& shared, const std::string& sql, const std::function<void()>& then) {
    std::optional<StringResult> answer;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        answer.emplace(session.execute_sql(sql));
        done = true;
    });
    for (int i = 0; i < 800 && !done && shared->read()->lock_mgr.wait_for_rows().empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool was_waiting = !done;
    then();
    worker.join();
    INFO("the statement '" << sql << "' should have waited");
    REQUIRE(was_waiting);
    return answer;
}
} // namespace

TEST_CASE("an INSERT waits for a transaction that has deleted or moved the UNIQUE value", "[uncommitted_unique]") {
    TempDataDir dir("uncommitted_unique");
    Executor s1(dir.path);
    ok(s1, "CREATE DATABASE d");
    ok(s1, "USE d");
    auto shared = s1.get_shared();
    Executor s2 = Executor::new_session(shared);
    ok(s2, "USE d");
    const char* create[] = {"CREATE TABLE t (id INT PRIMARY KEY, k VARCHAR(8) UNIQUE, v INT)",
                            "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(8), b INT, v INT, UNIQUE (a, b))",
                            "CREATE TABLE t (a VARCHAR(8), b INT, v INT, PRIMARY KEY (a, b))"};
    // the rows, the statement that frees the value of row 1, the INSERT of that value (a free id for tables that have one), and the rows the key leaves behind
    const char* rows[] = {"(1, 'one', 0), (2, 'two', 0)", "(1, 'x', 1, 0), (2, 'x', 2, 0)", "('x', 1, 0), ('x', 2, 0)"};
    const char* del[] = {"DELETE FROM t WHERE k = 'one'", "DELETE FROM t WHERE a = 'x' AND b = 1", "DELETE FROM t WHERE a = 'x' AND b = 1"};
    const char* move[] = {"UPDATE t SET k = 'moved' WHERE id = 1", "UPDATE t SET b = 11 WHERE id = 1", "UPDATE t SET b = 11 WHERE a = 'x' AND b = 1"};
    const char* again[] = {"INSERT INTO t VALUES (3, 'ONE', 0)", "INSERT INTO t VALUES (3, 'X', 1, 0)", "INSERT INTO t VALUES ('X', 1, 0)"};
    const char* again_ignore[] = {"INSERT IGNORE INTO t VALUES (3, 'ONE', 0)", "INSERT IGNORE INTO t VALUES (3, 'X', 1, 0)", "INSERT IGNORE INTO t VALUES ('X', 1, 0)"};
    const char* count_of_key[] = {"SELECT COUNT(*) FROM t WHERE k = 'one'", "SELECT COUNT(*) FROM t WHERE a = 'x' AND b = 1", "SELECT COUNT(*) FROM t WHERE a = 'x' AND b = 1"};
    for (int kind = 0; kind < 3; kind++) {
        for (int freeing = 0; freeing < 2; freeing++) {         // 0: DELETE, 1: UPDATE that moves the key
            for (int outcome = 0; outcome < 2; outcome++) {    // 0: the transaction rolls back, 1: it commits
                for (int how = 0; how < 2; how++) {            // 0: plain INSERT, 1: INSERT IGNORE
                    INFO("key " << kind << (freeing ? " moved" : " deleted") << (outcome ? " and committed" : " and rolled back") << (how ? ", INSERT IGNORE" : ", INSERT"));
                    ok(s1, "DROP TABLE IF EXISTS t");
                    ok(s1, create[kind]);
                    ok(s1, std::string("INSERT INTO t VALUES ") + rows[kind]);
                    ok(s1, "BEGIN");
                    ok(s1, freeing ? move[kind] : del[kind]);
                    auto answer = run_blocked(s2, shared, how ? again_ignore[kind] : again[kind], [&] { ok(s1, outcome ? "COMMIT" : "ROLLBACK"); });
                    REQUIRE(answer.has_value());
                    if (outcome == 0) {
                        // the row is back: the value is a duplicate (IGNORE leaves it alone)
                        if (how == 0) {
                            REQUIRE(answer->is_err());
                            REQUIRE(answer->error().find("Duplicate") != std::string::npos);
                        } else {
                            REQUIRE(answer->is_ok());
                        }
                        REQUIRE(rows_of(s1, count_of_key[kind]) == Rows{{"1"}});
                    } else {
                        // the value is free now (deleted: gone; moved: the key is the new one): the INSERT went in
                        REQUIRE(answer->is_ok());
                        REQUIRE(rows_of(s1, count_of_key[kind]) == Rows{{"1"}}); // one row holds the value: the new one
                    }
                }
            }
        }
    }
}

// What the transaction keeps is only what it frees: a value it did not touch is still held by a live row, so another session's INSERT of it is a duplicate at once --
// it does not wait for the transaction.
TEST_CASE("an INSERT of a UNIQUE value that is still held does not wait for the transaction that updated the row", "[uncommitted_unique]") {
    TempDataDir dir("uncommitted_unique_unchanged");
    Executor s1(dir.path);
    ok(s1, "CREATE DATABASE d");
    ok(s1, "USE d");
    auto shared = s1.get_shared();
    Executor s2 = Executor::new_session(shared);
    ok(s2, "USE d");
    ok(s1, "CREATE TABLE t (id INT PRIMARY KEY, k VARCHAR(8) UNIQUE, a INT, b INT, v INT, UNIQUE (a, b))");
    ok(s1, "INSERT INTO t VALUES (1, 'one', 1, 1, 0), (2, 'two', 2, 2, 0)");
    ok(s1, "BEGIN");
    ok(s1, "UPDATE t SET v = 5 WHERE id = 1"); // no key changes
    for (const char* dup : {"INSERT INTO t VALUES (3, 'one', 9, 9, 0)", "INSERT INTO t VALUES (3, 'x', 1, 1, 0)", "UPDATE t SET k = 'one' WHERE id = 2",
                            "UPDATE t SET a = 1, b = 1 WHERE id = 2"}) {
        INFO(dup);
        auto t0 = std::chrono::steady_clock::now();
        auto r = s2.execute_sql(dup);
        REQUIRE(r.is_err());
        REQUIRE(r.error().find("Duplicate") != std::string::npos);
        REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    }
    ok(s1, "ROLLBACK");
}

TEST_CASE("sessions that wait for each other's UNIQUE values are told, not frozen", "[uncommitted_unique]") {
    TempDataDir dir("uncommitted_unique_deadlock");
    Executor s1(dir.path);
    ok(s1, "CREATE DATABASE d");
    ok(s1, "USE d");
    auto shared = s1.get_shared();
    Executor s2 = Executor::new_session(shared);
    ok(s2, "USE d");
    ok(s1, "CREATE TABLE t (id INT PRIMARY KEY, k VARCHAR(8) UNIQUE)");
    ok(s1, "INSERT INTO t VALUES (1, 'a'), (2, 'b')");
    ok(s1, "BEGIN");
    ok(s2, "BEGIN");
    ok(s1, "DELETE FROM t WHERE k = 'a'");
    ok(s2, "DELETE FROM t WHERE k = 'b'");
    // s2 wants 'a' (s1 holds it) and then s1 wants 'b' (s2 holds it): the second request closes the circle and is refused as the deadlock; s1 then rolls back, the row 'a' is
    // back, and the INSERT s2 was waiting with is a duplicate -- but it does not hang
    std::optional<StringResult> refused;
    auto waiting = run_blocked(s2, shared, "INSERT INTO t VALUES (3, 'a')", [&] {
        refused.emplace(s1.execute_sql("INSERT INTO t VALUES (4, 'b')"));
        ok(s1, "ROLLBACK");
    });
    REQUIRE(refused.has_value());
    REQUIRE(refused->is_err());
    REQUIRE(refused->error().find("Deadlock") != std::string::npos);
    REQUIRE(waiting.has_value());
    REQUIRE(waiting->is_err());
    REQUIRE(waiting->error().find("Duplicate") != std::string::npos);
    ok(s2, "ROLLBACK");
    REQUIRE(rows_of(s1, "SELECT id, k FROM t ORDER BY id") == Rows{{"1", "a"}, {"2", "b"}});
}

TEST_CASE("a cascaded DELETE in a transaction holds the UNIQUE values of the child rows it removes", "[uncommitted_unique]") {
    TempDataDir dir("uncommitted_unique_cascade");
    Executor s1(dir.path);
    ok(s1, "CREATE DATABASE d");
    ok(s1, "USE d");
    auto shared = s1.get_shared();
    Executor s2 = Executor::new_session(shared);
    ok(s2, "USE d");
    for (int outcome = 0; outcome < 2; outcome++) {
        INFO("outcome " << outcome << " (0: rollback, 1: commit)");
        ok(s1, "DROP TABLE IF EXISTS c");
        ok(s1, "DROP TABLE IF EXISTS p");
        ok(s1, "CREATE TABLE p (id INT PRIMARY KEY)");
        ok(s1, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, u VARCHAR(8) UNIQUE, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE)");
        ok(s1, "INSERT INTO p VALUES (1), (2)");
        ok(s1, "INSERT INTO c VALUES (10, 1, 'x'), (11, 2, 'y')");
        ok(s1, "BEGIN");
        ok(s1, "DELETE FROM p WHERE id = 1"); // c 10 goes with it, and 'x' with it
        auto answer = run_blocked(s2, shared, "INSERT INTO c VALUES (12, 2, 'X')", [&] { ok(s1, outcome ? "COMMIT" : "ROLLBACK"); });
        REQUIRE(answer.has_value());
        if (outcome == 0) {
            REQUIRE(answer->is_err());
            REQUIRE(answer->error().find("Duplicate") != std::string::npos);
            REQUIRE(rows_of(s1, "SELECT COUNT(*) FROM c WHERE u = 'x'") == Rows{{"1"}});
        } else {
            REQUIRE(answer->is_ok());
            REQUIRE(rows_of(s1, "SELECT id FROM c WHERE u = 'x'") == Rows{{"12"}});
        }
    }
}
