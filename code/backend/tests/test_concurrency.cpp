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

// Stage 4 (table-level concurrency): two sessions writing to genuinely DIFFERENT tables
// should both make real progress without either blocking the other on the whole
// database -- unlike the old single global exclusive lock, where every write (regardless
// of which table) fully serialized against every other statement. This doesn't assert on
// wall-clock timing (too flaky for CI) -- it just verifies correctness under real
// concurrent access to two separate tables, which the old model made structurally
// impossible to exercise (a writer to table X always fully excluded a writer to table Y).
TEST_CASE("Stage 4: concurrent writes to different tables both complete correctly", "[executor][concurrency][stage4]") {
    TempDataDir dir("exec_concurrency_data_stage4_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE tx (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE ty (id INT PRIMARY KEY, val INT)").is_ok());

    constexpr int kN = 2000;
    auto shared = ex.get_shared();
    std::atomic<bool> x_failed{false}, y_failed{false};

    std::thread tx_thread([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { x_failed = true; return; }
        for (int i = 0; i < kN; i++) {
            if (!sess.execute_sql("INSERT INTO tx VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")").is_ok()) {
                x_failed = true;
                return;
            }
        }
    });
    std::thread ty_thread([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { y_failed = true; return; }
        for (int i = 0; i < kN; i++) {
            if (!sess.execute_sql("INSERT INTO ty VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")").is_ok()) {
                y_failed = true;
                return;
            }
        }
    });
    tx_thread.join();
    ty_thread.join();

    REQUIRE_FALSE(x_failed.load());
    REQUIRE_FALSE(y_failed.load());
    auto cx = ex.execute_sql("SELECT COUNT(*) AS c FROM tx");
    auto cy = ex.execute_sql("SELECT COUNT(*) AS c FROM ty");
    REQUIRE(cx.is_ok());
    REQUIRE(cy.is_ok());
    REQUIRE(cx.value().find(std::to_string(kN)) != std::string::npos);
    REQUIRE(cy.value().find(std::to_string(kN)) != std::string::npos);
}

// Stage 4: a table with a trigger, an updatable view, and a plain CTE query all fall
// back to the full structural exclusive lock (table_lock_set_for's conservative default
// for anything it can't safely pre-lock a fixed table set for -- see its doc comment) --
// verify each still behaves correctly while a concurrent, unrelated table is being
// hammered by another session, since the fallback path must still serialize against
// per-table-locked statements correctly (both share the same underlying
// RwLock<SharedDatabase>, just used in different modes).
TEST_CASE("Stage 4: trigger/view/CTE fallback paths stay correct under concurrent unrelated-table writes",
          "[executor][concurrency][stage4]") {
    TempDataDir dir("exec_concurrency_data_stage4_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE audited (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE audit_log (id INT PRIMARY KEY AUTO_INCREMENT, msg VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql(
                "CREATE TRIGGER trg_audit AFTER INSERT ON audited FOR EACH ROW INSERT INTO audit_log (msg) VALUES ('inserted')")
                .is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE base (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO base VALUES (1, 10), (2, 20)").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW v_base AS SELECT id, val FROM base").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE unrelated (id INT PRIMARY KEY, val INT)").is_ok());

    constexpr int kN = 500;
    auto shared = ex.get_shared();
    std::atomic<bool> unrelated_failed{false};

    std::thread unrelated_thread([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { unrelated_failed = true; return; }
        for (int i = 0; i < kN; i++) {
            if (!sess.execute_sql("INSERT INTO unrelated VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")").is_ok()) {
                unrelated_failed = true;
                return;
            }
            if (!sess.execute_sql("UPDATE unrelated SET val = val + 1 WHERE id = " + std::to_string(i)).is_ok()) {
                unrelated_failed = true;
                return;
            }
        }
    });

    Executor fallback_sess = Executor::new_session(shared);
    REQUIRE(fallback_sess.execute_sql("USE d").is_ok());
    for (int i = 0; i < kN / 5; i++) {
        REQUIRE(fallback_sess.execute_sql("INSERT INTO audited VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")").is_ok());
        REQUIRE(fallback_sess.execute_sql("UPDATE v_base SET val = val + 1 WHERE id = 1").is_ok());
        REQUIRE(fallback_sess.execute_sql("WITH c AS (SELECT * FROM base) SELECT * FROM c").is_ok());
    }

    unrelated_thread.join();
    REQUIRE_FALSE(unrelated_failed.load());

    auto audited_count = fallback_sess.execute_sql("SELECT COUNT(*) AS c FROM audited");
    auto audit_log_count = fallback_sess.execute_sql("SELECT COUNT(*) AS c FROM audit_log");
    REQUIRE(audited_count.is_ok());
    REQUIRE(audit_log_count.is_ok());
    REQUIRE(audited_count.value().find(std::to_string(kN / 5)) != std::string::npos);
    REQUIRE(audit_log_count.value().find(std::to_string(kN / 5)) != std::string::npos);

    auto base_val = fallback_sess.execute_sql("SELECT val FROM base WHERE id = 1");
    REQUIRE(base_val.is_ok());
    REQUIRE(base_val.value().find(std::to_string(10 + kN / 5)) != std::string::npos);

    auto unrelated_count = fallback_sess.execute_sql("SELECT COUNT(*) AS c FROM unrelated");
    REQUIRE(unrelated_count.is_ok());
    REQUIRE(unrelated_count.value().find(std::to_string(kN)) != std::string::npos);
}

// MVCC Stage 1 regression: index-based fast-path SELECTs (AccessPath::PkPoint etc.,
// executor_select.cpp) used to read s.indexes with no session/transaction isolation at
// all -- session_tables only ever swaps s.tables, never s.indexes, and INSERT mutates
// indexes live against whichever table view is currently swapped in. Since the exclusive
// per-statement write lock is released between a transaction's own statements (not held
// for the whole transaction), another session's read-only SELECT taking shared->read()
// in that gap could see the still-open transaction's uncommitted INSERT through the
// index path -- a real dirty read, independent of the isolation level in effect.
TEST_CASE("Index-based PK point lookup does not leak another session's uncommitted INSERT", "[executor][concurrency][mvcc]") {
    TempDataDir dir("exec_concurrency_mvcc_1");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    // Default isolation is ReadCommitted -- b must not see a's still-uncommitted row,
    // even via the PkPoint index fast path (this exact query planned to it).
    auto seen = b.execute_sql("SELECT * FROM t WHERE id = 1");
    REQUIRE(seen.is_ok());
    REQUIRE(seen.value().find("0 rows returned.") != std::string::npos);

    REQUIRE(a.execute_sql("COMMIT").is_ok());

    auto seen_after = b.execute_sql("SELECT * FROM t WHERE id = 1");
    REQUIRE(seen_after.is_ok());
    REQUIRE(seen_after.value().find("1 row(s) returned.") != std::string::npos);
}

// MVCC Stage 1: this is the first point where ReadUncommitted and ReadCommitted actually
// diverge in code (SnapshotCtx construction, Executor::current_read_ctx) -- previously
// both isolation levels were coded identically (a documented gap). RU's ctx treats every
// transaction id as already committed, so it must see a's uncommitted insert; RC must not.
TEST_CASE("READ UNCOMMITTED sees another session's in-progress INSERT via the index path; READ COMMITTED does not",
          "[executor][concurrency][mvcc]") {
    TempDataDir dir("exec_concurrency_mvcc_2");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    auto shared = a.get_shared();
    Executor ru = Executor::new_session(shared);
    REQUIRE(ru.execute_sql("USE d").is_ok());
    REQUIRE(ru.execute_sql("SET ISOLATION LEVEL READ UNCOMMITTED").is_ok());
    Executor rc = Executor::new_session(shared);
    REQUIRE(rc.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    auto ru_seen = ru.execute_sql("SELECT * FROM t WHERE id = 1");
    REQUIRE(ru_seen.is_ok());
    REQUIRE(ru_seen.value().find("1 row(s) returned.") != std::string::npos);

    auto rc_seen = rc.execute_sql("SELECT * FROM t WHERE id = 1");
    REQUIRE(rc_seen.is_ok());
    REQUIRE(rc_seen.value().find("0 rows returned.") != std::string::npos);

    REQUIRE(a.execute_sql("ROLLBACK").is_ok());
}
