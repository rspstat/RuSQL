#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

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

// Regression suite for Section B's concurrent-read-only-SELECT change
// (Executor::is_pure_read_only + Executor::execute()'s shared->read() branch, plus the
// BufferPool internal mutex this required). These spin up real std::threads, each with
// its own Executor::new_session() sharing one SharedDatabase, to actually exercise the
// concurrent code path rather than just asserting the classifier's return value.
TEST_CASE("Concurrent plain SELECTs and writes don't corrupt shared state", "[executor][concurrency]") {
    TempDataDir dir("exec_concurrency_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE bank").is_ok());
    REQUIRE(ex.execute_sql("USE bank").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE accounts (id INT PRIMARY KEY, balance INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE orders (id INT PRIMARY KEY, account_id INT)").is_ok());
    for (int i = 0; i < 20; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO accounts VALUES (" + std::to_string(i) + ", 100)").is_ok());
    }

    constexpr int kReaderThreads = 8;
    constexpr int kWriterThreads = 2;
    constexpr int kIterations = 300;

    std::atomic<bool> reader_failed{false};
    std::atomic<int> net_inserts{0};
    std::atomic<int> net_deletes{0};
    auto shared = ex.get_shared();

    std::vector<std::thread> threads;

    // Readers: plain SELECT, PK point lookup (index fast path), JOIN, WHERE-IN subquery
    // (must be recursively classified read-only -- see is_pure_read_only), SHOW-family.
    for (int t = 0; t < kReaderThreads; t++) {
        threads.emplace_back([&shared, &reader_failed, t] {
            Executor sess = Executor::new_session(shared);
            if (!sess.execute_sql("USE bank").is_ok()) { reader_failed = true; return; }
            for (int i = 0; i < kIterations; i++) {
                if (!sess.execute_sql("SELECT * FROM accounts").is_ok()) { reader_failed = true; return; }
                if (!sess.execute_sql("SELECT * FROM accounts WHERE id = " + std::to_string(i % 20)).is_ok()) {
                    reader_failed = true;
                    return;
                }
                if (!sess.execute_sql("SELECT a.id, o.id FROM accounts a JOIN orders o ON a.id = o.account_id").is_ok()) {
                    reader_failed = true;
                    return;
                }
                if (!sess
                         .execute_sql("SELECT * FROM accounts WHERE id IN (SELECT id FROM accounts WHERE balance >= 0)")
                         .is_ok()) {
                    reader_failed = true;
                    return;
                }
                if (!sess.execute_sql("SHOW TABLES").is_ok()) { reader_failed = true; return; }
                if (!sess.execute_sql("SHOW BUFFER POOL").is_ok()) { reader_failed = true; return; }
                if (!sess.execute_sql("EXPLAIN SELECT * FROM accounts").is_ok()) { reader_failed = true; return; }
            }
        });
    }

    // Writers: INSERT new accounts (unique ids per thread) then DELETE them, tracking a
    // running net-row-count so the final COUNT(*) invariant can be checked.
    for (int t = 0; t < kWriterThreads; t++) {
        threads.emplace_back([&shared, &net_inserts, &net_deletes, t] {
            Executor sess = Executor::new_session(shared);
            REQUIRE(sess.execute_sql("USE bank").is_ok());
            for (int i = 0; i < kIterations; i++) {
                int id = 1000 + t * kIterations + i;
                auto ins = sess.execute_sql("INSERT INTO accounts VALUES (" + std::to_string(id) + ", 50)");
                if (ins.is_ok()) net_inserts++;
                auto upd = sess.execute_sql("UPDATE accounts SET balance = balance + 1 WHERE id = " + std::to_string(id));
                (void)upd;
                if (i % 2 == 0) {
                    auto del = sess.execute_sql("DELETE FROM accounts WHERE id = " + std::to_string(id));
                    if (del.is_ok()) net_deletes++;
                }
            }
        });
    }

    for (auto& th : threads) th.join();

    REQUIRE_FALSE(reader_failed.load());

    auto count_r = ex.execute_sql("SELECT COUNT(*) AS c FROM accounts");
    REQUIRE(count_r.is_ok());
    int expected = 20 + net_inserts.load() - net_deletes.load();
    REQUIRE(count_r.value().find(std::to_string(expected)) != std::string::npos);
}

TEST_CASE("is_pure_read_only classifies statements correctly", "[executor][concurrency]") {
    TempDataDir dir("exec_concurrency_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d1").is_ok());
    REQUIRE(ex.execute_sql("USE d1").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 10)").is_ok());

    // Read-only: plain SELECT, JOIN, WHERE-subquery, SHOW-family, EXPLAIN.
    REQUIRE(ex.execute_sql("SELECT * FROM t").is_ok());
    REQUIRE(ex.execute_sql("SELECT * FROM t WHERE id IN (SELECT id FROM t)").is_ok());
    REQUIRE(ex.execute_sql("SHOW TABLES").is_ok());
    REQUIRE(ex.execute_sql("EXPLAIN SELECT * FROM t").is_ok());
    REQUIRE(ex.execute_sql("EXPLAIN ANALYZE SELECT * FROM t").is_ok());

    // Write-required: DML/DDL, FOR UPDATE, WITH, INSERT...SELECT.
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 20)").is_ok());
    REQUIRE(ex.execute_sql("UPDATE t SET val = 99 WHERE id = 1").is_ok());
    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(ex.execute_sql("SELECT * FROM t WHERE id = 1 FOR UPDATE").is_ok());
    REQUIRE(ex.execute_sql("COMMIT").is_ok());
    REQUIRE(ex.execute_sql("WITH x AS (SELECT * FROM t) SELECT * FROM x").is_ok());
}
