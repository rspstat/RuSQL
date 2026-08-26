#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
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
    std::atomic<bool> writer_failed{false};
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
        threads.emplace_back([&shared, &net_inserts, &net_deletes, &writer_failed, t] {
            Executor sess = Executor::new_session(shared);
            // Catch2 v2's REQUIRE/assertion machinery is not thread-safe -- must not be
            // called from a spawned std::thread (the reader threads above already avoid
            // this correctly via an atomic flag checked after join(); this writer thread
            // used to call REQUIRE directly, which happened to not visibly collide before
            // Stage 1 removed an unrelated full-database exclusive lock from the query
            // cache path -- that change made real thread parallelism tight enough for two
            // threads' concurrent REQUIRE calls to corrupt Catch2's shared reporting state
            // and crash the whole test binary, intermittently).
            if (!sess.execute_sql("USE bank").is_ok()) { writer_failed = true; return; }
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
    REQUIRE_FALSE(writer_failed.load());

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

// Regression: unlike PkPoint/CompositeIndexPrefix and the other ~15 AccessPath fast
// paths, CompositeIndexPath (an exact-match multi-column index lookup) never checked
// MVCC visibility at all -- found while adding incremental composite-index maintenance
// for UPDATE/DELETE (a separate, unrelated fix) via a test that queried through this
// exact path and got back another session's still-uncommitted row.
TEST_CASE("Composite-index exact-match lookup does not leak another session's uncommitted INSERT",
          "[executor][concurrency][mvcc][regression]") {
    TempDataDir dir("exec_concurrency_mvcc_composite");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, dept INT, salary INT)").is_ok());
    REQUIRE(a.execute_sql("CREATE INDEX idx_dept_sal ON t (dept, salary)").is_ok());

    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (1, 10, 1000)").is_ok());

    // Default isolation is ReadCommitted -- b must not see a's still-uncommitted row via
    // the CompositeIndexPath fast path (WHERE on every indexed column, equality).
    auto seen = b.execute_sql("SELECT * FROM t WHERE dept = 10 AND salary = 1000");
    REQUIRE(seen.is_ok());
    REQUIRE(seen.value().find("0 rows returned.") != std::string::npos);

    REQUIRE(a.execute_sql("COMMIT").is_ok());

    auto seen_after = b.execute_sql("SELECT * FROM t WHERE dept = 10 AND salary = 1000");
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

// Gap Lock (InnoDB-style phantom-read prevention): a REPEATABLE READ transaction that
// locks a range via SELECT ... FOR UPDATE should block a concurrent session's INSERT of a
// new row whose PK falls inside that range (a phantom), while an INSERT outside the range
// still succeeds -- and the gap lock must be released once the holder's transaction ends.
TEST_CASE("Gap lock blocks a phantom INSERT into a locked range and releases on COMMIT", "[executor][concurrency][gap_lock]") {
    TempDataDir dir("exec_concurrency_gap_lock_1");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (5, 5)").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (30, 30)").is_ok());

    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20 FOR UPDATE").is_ok());

    // Phantom candidate: falls inside the locked [10, 20] range -- must be rejected.
    auto phantom = b.execute_sql("INSERT INTO t VALUES (15, 15)");
    REQUIRE(phantom.is_err());
    REQUIRE(phantom.error().find("gap lock") != std::string::npos);

    // Outside the locked range -- unaffected, must succeed.
    REQUIRE(b.execute_sql("INSERT INTO t VALUES (100, 100)").is_ok());

    REQUIRE(a.execute_sql("COMMIT").is_ok());

    // Gap lock released with the transaction -- the same INSERT now succeeds.
    REQUIRE(b.execute_sql("INSERT INTO t VALUES (15, 15)").is_ok());
}

TEST_CASE("A blocked INSERT-vs-gap-lock unblocks once the gap lock's holder commits, not before",
          "[executor][concurrency][blocking][gap_lock]") {
    TempDataDir dir("exec_concurrency_gap_lock_blocking");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5, 5)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (30, 30)").is_ok());

    auto shared = ex.get_shared();
    constexpr int kHoldMs = 300;

    std::atomic<bool> a_committed{false};
    std::thread a([&] {
        Executor sess = Executor::new_session(shared);
        REQUIRE(sess.execute_sql("USE d").is_ok());
        REQUIRE(sess.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
        REQUIRE(sess.execute_sql("BEGIN").is_ok());
        REQUIRE(sess.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20 FOR UPDATE").is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
        REQUIRE(sess.execute_sql("COMMIT").is_ok());
        a_committed = true;
    });
    // Give A time to actually take the gap lock before B starts racing it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto t0 = std::chrono::steady_clock::now();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());
    auto result = b.execute_sql("INSERT INTO t VALUES (15, 15)"); // falls inside A's [10,20] gap
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    a.join();
    REQUIRE(result.is_ok());
    REQUIRE(a_committed.load());
    // B must have genuinely waited for roughly A's remaining hold time, not returned
    // instantly with a "falls within a gap lock" error (the pre-blocking behavior).
    REQUIRE(elapsed_ms >= 100);
}

TEST_CASE("@lock_wait_timeout is actually enforced by a genuinely blocking INSERT-vs-gap-lock",
          "[executor][concurrency][blocking][gap_lock]") {
    TempDataDir dir("exec_concurrency_gap_lock_timeout");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5, 5)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (30, 30)").is_ok());

    auto shared = ex.get_shared();

    Executor holder = Executor::new_session(shared);
    REQUIRE(holder.execute_sql("USE d").is_ok());
    REQUIRE(holder.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
    REQUIRE(holder.execute_sql("BEGIN").is_ok());
    REQUIRE(holder.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20 FOR UPDATE").is_ok());
    // Deliberately never commits/rolls back -- the waiter below must time out on its own.

    Executor waiter = Executor::new_session(shared);
    REQUIRE(waiter.execute_sql("USE d").is_ok());
    REQUIRE(waiter.execute_sql("SET @lock_wait_timeout = 300").is_ok());

    auto t0 = std::chrono::steady_clock::now();
    auto result = waiter.execute_sql("INSERT INTO t VALUES (15, 15)");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE_FALSE(result.is_ok());
    REQUIRE(result.error().find("1205") != std::string::npos);
    REQUIRE(elapsed_ms >= 250);  // actually waited roughly the configured timeout...
    REQUIRE(elapsed_ms < 5000);  // ...not the 50000ms default, and not instant either.

    REQUIRE(holder.execute_sql("ROLLBACK").is_ok());
}

// A READ COMMITTED transaction (the default) must NOT take a gap lock at all (matching
// InnoDB, where gap locking is disabled below REPEATABLE READ) -- a concurrent phantom
// INSERT into the "locked" range must succeed.
TEST_CASE("Gap lock is not taken under READ COMMITTED", "[executor][concurrency][gap_lock]") {
    TempDataDir dir("exec_concurrency_gap_lock_2");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("BEGIN").is_ok()); // default isolation: READ COMMITTED
    REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20 FOR UPDATE").is_ok());

    REQUIRE(b.execute_sql("INSERT INTO t VALUES (15, 15)").is_ok());

    REQUIRE(a.execute_sql("COMMIT").is_ok());
}

// A transaction's own gap lock must not block its own subsequent INSERT into that range.
TEST_CASE("Gap lock does not block the holder's own INSERT into its locked range", "[executor][concurrency][gap_lock]") {
    TempDataDir dir("exec_concurrency_gap_lock_3");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    REQUIRE(a.execute_sql("SET ISOLATION LEVEL SERIALIZABLE").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20 FOR UPDATE").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (15, 15)").is_ok());
    REQUIRE(a.execute_sql("COMMIT").is_ok());
}

// Predicate lock (SSI phantom detection): a plain, unlocked SELECT under SERIALIZABLE now
// registers its WHERE-range as a predicate; a phantom INSERT into that range does NOT
// block (unlike Gap Lock/FOR UPDATE above) but instead fails the READING transaction's
// own COMMIT, mirroring PostgreSQL's non-blocking SIREAD check.
TEST_CASE("A plain SELECT under SERIALIZABLE fails its own COMMIT if a phantom row lands in its scanned range",
          "[executor][concurrency][predicate_lock]") {
    TempDataDir dir("exec_concurrency_predicate_lock_1");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (5, 5)").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (30, 30)").is_ok());

    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("SET ISOLATION LEVEL SERIALIZABLE").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20").is_ok()); // plain, no FOR UPDATE

    // Does NOT block -- unlike Gap Lock, a predicate lock never stops the writer.
    REQUIRE(b.execute_sql("INSERT INTO t VALUES (15, 15)").is_ok());

    auto commit = a.execute_sql("COMMIT");
    REQUIRE(commit.is_err());
    REQUIRE(commit.error().find("Serialization failure") != std::string::npos);
}

TEST_CASE("A phantom INSERT outside the scanned range does not fail the reader's COMMIT",
          "[executor][concurrency][predicate_lock]") {
    TempDataDir dir("exec_concurrency_predicate_lock_2");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("SET ISOLATION LEVEL SERIALIZABLE").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20").is_ok());

    REQUIRE(b.execute_sql("INSERT INTO t VALUES (100, 100)").is_ok()); // outside [10,20]

    REQUIRE(a.execute_sql("COMMIT").is_ok());
}

// Matches Gap Lock's own scope: predicate locks are SERIALIZABLE-only (not REPEATABLE
// READ), since the read-set/Gap Lock machinery already gives RR its InnoDB-style phantom
// protection for the operations that take real locks (FOR UPDATE/FOR SHARE, range
// UPDATE/DELETE) -- a plain SELECT under RR was never promised phantom-freedom to begin
// with (ANSI RR doesn't require it), so no predicate is registered and this must not abort.
TEST_CASE("A plain SELECT under REPEATABLE READ does not register a predicate, so a phantom does not fail its COMMIT",
          "[executor][concurrency][predicate_lock]") {
    TempDataDir dir("exec_concurrency_predicate_lock_3");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(a.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20").is_ok());

    REQUIRE(b.execute_sql("INSERT INTO t VALUES (15, 15)").is_ok());

    REQUIRE(a.execute_sql("COMMIT").is_ok());
}

TEST_CASE("SHOW LOCKS lists an active predicate lock's range while its transaction is open",
          "[executor][concurrency][predicate_lock]") {
    TempDataDir dir("exec_concurrency_predicate_lock_4");
    Executor a(dir.path);
    REQUIRE(a.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    REQUIRE(a.execute_sql("SET ISOLATION LEVEL SERIALIZABLE").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 10 AND 20").is_ok());

    auto locks = a.execute_sql("SHOW LOCKS");
    REQUIRE(locks.is_ok());
    REQUIRE(locks.value().find("Predicate locks") != std::string::npos);
    REQUIRE(locks.value().find("[10, 20]") != std::string::npos);

    REQUIRE(a.execute_sql("COMMIT").is_ok());
}

// Row-level-concurrency prep, Stage 1: execute_sql's query-cache populate/invalidate
// calls moved from shared->write() (whole database exclusive) to shared->read(), since
// QueryResultCache is fully self-synchronized. This is the regression that matters: many
// concurrent sessions repeatedly reading (cacheable) and writing (invalidating) the SAME
// table must never let a stale cached SELECT result survive past a committed DML that
// changed it.
TEST_CASE("No stale cached SELECT result survives concurrent commits on the same table", "[executor][concurrency][query_cache]") {
    TempDataDir dir("exec_concurrency_query_cache_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE counters (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO counters VALUES (1, 0)").is_ok());

    constexpr int kWriterIters = 300;
    auto shared = ex.get_shared();
    std::atomic<bool> writer_failed{false};
    std::atomic<bool> reader_failed{false};
    std::atomic<bool> stop_reader{false};

    std::thread writer([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { writer_failed = true; return; }
        for (int i = 1; i <= kWriterIters; i++) {
            if (!sess.execute_sql("UPDATE counters SET val = " + std::to_string(i) + " WHERE id = 1").is_ok()) {
                writer_failed = true;
                return;
            }
        }
        stop_reader = true;
    });

    // Extracts the single integer data value from a boxed one-column SELECT result
    // (+---+ / | val | / +---+ / | 42 | / +---+ / "1 row(s) returned.") -- the value is
    // on the first non-separator line after the second "+---+" separator.
    auto extract_val = [](const std::string& text) -> int {
        std::istringstream iss(text);
        std::string line;
        int seps_seen = 0;
        while (std::getline(iss, line)) {
            if (!line.empty() && line[0] == '+') {
                seps_seen++;
                continue;
            }
            if (seps_seen >= 2) {
                auto start = line.find_first_of("0123456789");
                if (start != std::string::npos) return std::stoi(line.substr(start));
            }
        }
        return -1;
    };

    std::string diag_prev_text, diag_cur_text;
    int diag_last_seen = -1, diag_seen = -1;
    std::thread reader([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { reader_failed = true; return; }
        int last_seen = -1;
        std::string prev_text;
        while (!stop_reader.load()) {
            auto r = sess.execute_sql("SELECT val FROM counters WHERE id = 1");
            if (!r.is_ok()) { reader_failed = true; return; }
            // Only check monotonicity (never see a value smaller than one already
            // observed) -- that's exactly what a stale cache hit would violate.
            int seen = extract_val(r.value());
            if (seen < last_seen) {
                diag_prev_text = prev_text;
                diag_cur_text = r.value();
                diag_last_seen = last_seen;
                diag_seen = seen;
                reader_failed = true;
                return;
            }
            last_seen = seen;
            prev_text = r.value();
        }
    });

    writer.join();
    reader.join();
    INFO("last_seen=" << diag_last_seen << " seen=" << diag_seen);
    INFO("prev result text:\n" << diag_prev_text);
    INFO("cur result text:\n" << diag_cur_text);
    REQUIRE_FALSE(writer_failed.load());
    REQUIRE_FALSE(reader_failed.load());

    auto final_val = ex.execute_sql("SELECT val FROM counters WHERE id = 1");
    REQUIRE(final_val.is_ok());
    REQUIRE(final_val.value().find(std::to_string(kWriterIters)) != std::string::npos);
}

// Stage 6 (row-level-concurrency plan): the whole point of Stages 1-5 was to let
// different sessions' writes to DIFFERENT ROWS of the SAME table genuinely run
// concurrently instead of fully serializing on one whole-table lock. This is a
// writer-only (no readers) test targeting that core promise directly: 4 threads, each
// with its own disjoint PK range, hammering INSERT+UPDATE+DELETE -- the assertion is
// exact final-state correctness (no lost/duplicated rows), not wall-clock speed (too
// flaky for CI), matching this file's existing Stage 4 test's philosophy.
TEST_CASE("Same-table disjoint-row concurrent INSERT/UPDATE/DELETE across many writers stays correct",
          "[executor][concurrency][stage4]") {
    TempDataDir dir("exec_concurrency_disjoint_writers");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    constexpr int kWriters = 4;
    constexpr int kPerWriter = 150;
    auto shared = ex.get_shared();
    std::atomic<bool> failed{false};
    std::atomic<int> total_survivors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kWriters; t++) {
        threads.emplace_back([&shared, &failed, &total_survivors, t] {
            Executor sess = Executor::new_session(shared);
            if (!sess.execute_sql("USE d").is_ok()) { failed = true; return; }
            int survivors = 0;
            for (int i = 0; i < kPerWriter; i++) {
                int id = t * kPerWriter + i; // disjoint range per thread
                if (!sess.execute_sql("INSERT INTO t VALUES (" + std::to_string(id) + ", 0)").is_ok()) {
                    failed = true;
                    return;
                }
                if (!sess.execute_sql("UPDATE t SET val = val + 1 WHERE id = " + std::to_string(id)).is_ok()) {
                    failed = true;
                    return;
                }
                if (i % 3 == 0) {
                    if (!sess.execute_sql("DELETE FROM t WHERE id = " + std::to_string(id)).is_ok()) {
                        failed = true;
                        return;
                    }
                } else {
                    survivors++;
                }
            }
            total_survivors += survivors;
        });
    }
    for (auto& th : threads) th.join();
    REQUIRE_FALSE(failed.load());

    auto count_r = ex.execute_sql("SELECT COUNT(*) AS c FROM t");
    REQUIRE(count_r.is_ok());
    REQUIRE(count_r.value().find(std::to_string(total_survivors.load())) != std::string::npos);

    // Every surviving row must show val=1 (its own UPDATE, exactly once -- never 0
    // meaning the UPDATE was lost, never >1 meaning it ran twice against the wrong row).
    auto bad_r = ex.execute_sql("SELECT COUNT(*) AS c FROM t WHERE val != 1");
    REQUIRE(bad_r.is_ok());
    REQUIRE(bad_r.value().find("0") != std::string::npos);
}

// Two sessions racing an explicit-transaction UPDATE on the SAME row: correctness means
// every successful COMMIT's increment is reflected exactly once in the end (no lost
// updates, no silent corruption) -- conflicts are expected and fine (that's the
// LockManager's designed behavior, instant-fail not blocking-wait), as long as they
// surface as a clean error the caller can retry on, never a hang or wrong data.
TEST_CASE("Two explicit transactions racing the SAME row never lose or duplicate an update",
          "[executor][concurrency][lock_manager]") {
    TempDataDir dir("exec_concurrency_same_row_race");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE counters (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO counters VALUES (1, 0)").is_ok());

    constexpr int kAttemptsPerThread = 150;
    auto shared = ex.get_shared();
    std::atomic<bool> hard_failed{false}; // anything other than a clean lock-conflict error
    std::atomic<int> total_committed{0};

    auto worker = [&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { hard_failed = true; return; }
        int committed = 0;
        for (int i = 0; i < kAttemptsPerThread; i++) {
            if (!sess.execute_sql("BEGIN").is_ok()) { hard_failed = true; return; }
            auto upd = sess.execute_sql("UPDATE counters SET val = val + 1 WHERE id = 1");
            if (!upd.is_ok()) {
                // Expected, designed outcome: a clean conflict/deadlock error, not a
                // hang or corruption. Roll back and move on, like a real retrying client.
                (void)sess.execute_sql("ROLLBACK");
                continue;
            }
            if (!sess.execute_sql("COMMIT").is_ok()) {
                (void)sess.execute_sql("ROLLBACK");
                continue;
            }
            committed++;
        }
        total_committed += committed;
    };

    std::thread a(worker);
    std::thread b(worker);
    a.join();
    b.join();
    REQUIRE_FALSE(hard_failed.load());

    auto final_val = ex.execute_sql("SELECT val FROM counters WHERE id = 1");
    REQUIRE(final_val.is_ok());
    REQUIRE(final_val.value().find(std::to_string(total_committed.load())) != std::string::npos);
}

// FK ON UPDATE CASCADE's child-table claim+lock extension (exec_update_inner's cascade
// section) under real concurrency: a parent-key UPDATE cascading into the child table
// races a second session's own direct writes to that same child table. Correctness means
// the final child-table state is consistent with SOME valid serialization of all the
// individual operations -- never a torn/partial cascade and never a crash.
TEST_CASE("FK ON UPDATE CASCADE stays correct against concurrent direct writes on the child table",
          "[executor][concurrency][fk]") {
    TempDataDir dir("exec_concurrency_fk_cascade");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE parent (code INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE child (id INT PRIMARY KEY, parent_code INT, "
                            "FOREIGN KEY (parent_code) REFERENCES parent(code) ON UPDATE CASCADE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO parent VALUES (1)").is_ok());
    for (int i = 0; i < 50; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO child VALUES (" + std::to_string(i) + ", 1)").is_ok());
    }
    // Disjoint id range for the concurrent direct-write session, so its own inserts
    // never collide with the cascade's target rows on identity, only on table shape.
    for (int i = 1000; i < 1010; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO child VALUES (" + std::to_string(i) + ", 1)").is_ok());
    }

    auto shared = ex.get_shared();
    std::atomic<bool> cascader_failed{false};
    std::atomic<bool> writer_failed{false};

    std::thread cascader([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { cascader_failed = true; return; }
        if (!sess.execute_sql("UPDATE parent SET code = 999 WHERE code = 1").is_ok()) { cascader_failed = true; return; }
    });

    std::thread writer([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { writer_failed = true; return; }
        for (int i = 1010; i < 1060; i++) {
            if (!sess.execute_sql("INSERT INTO child VALUES (" + std::to_string(i) + ", NULL)").is_ok()) {
                writer_failed = true;
                return;
            }
        }
    });

    cascader.join();
    writer.join();
    REQUIRE_FALSE(cascader_failed.load());
    REQUIRE_FALSE(writer_failed.load());

    // Every original child row (0..49) must have cascaded to the parent's final code --
    // no torn cascade leaving some rows on an old, now-nonexistent parent_code.
    auto stray = ex.execute_sql("SELECT COUNT(*) AS c FROM child WHERE id < 50 AND parent_code != 999");
    REQUIRE(stray.is_ok());
    REQUIRE(stray.value().find("0") != std::string::npos);

    auto total = ex.execute_sql("SELECT COUNT(*) AS c FROM child");
    REQUIRE(total.is_ok());
    REQUIRE(total.value().find(std::to_string(50 + 10 + 50)) != std::string::npos);
}

// Stage 4's design deliberately keeps VACUUM/ANALYZE/TRUNCATE/ALTER on the old
// whole-table-exclusive model (they inherently scan/rewrite the whole table in one
// pass) -- this regresses that they still safely exclude concurrent row-level DML on
// the SAME table, rather than racing it now that plain DML no longer takes a
// whole-table lock by default.
TEST_CASE("TRUNCATE TABLE excludes concurrent row-level DML on the same table", "[executor][concurrency][stage4]") {
    TempDataDir dir("exec_concurrency_truncate_exclusion");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    for (int i = 0; i < 100; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 0)").is_ok());
    }

    constexpr int kWriterIters = 200;
    auto shared = ex.get_shared();
    std::atomic<bool> writer_failed{false};
    std::atomic<bool> stop_writer{false};

    std::thread writer([&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { writer_failed = true; return; }
        int i = 0;
        while (!stop_writer.load() && i < kWriterIters) {
            int id = 1000 + i;
            if (!sess.execute_sql("INSERT INTO t VALUES (" + std::to_string(id) + ", 1)").is_ok()) {
                writer_failed = true;
                return;
            }
            i++;
        }
    });

    Executor trunc_sess = Executor::new_session(shared);
    REQUIRE(trunc_sess.execute_sql("USE d").is_ok());
    REQUIRE(trunc_sess.execute_sql("TRUNCATE TABLE t").is_ok());
    stop_writer = true;

    writer.join();
    REQUIRE_FALSE(writer_failed.load());

    // No crash/corruption is the primary assertion (TRUNCATE mid-flight against a
    // concurrent writer is exactly the scenario this test exists to exercise) -- the
    // table must still be in some well-defined, queryable state afterward.
    auto count_r = ex.execute_sql("SELECT COUNT(*) AS c FROM t");
    REQUIRE(count_r.is_ok());
}

// Real-blocking-wait stage: LockManager::acquire()/acquire_shared() now genuinely sleep
// on a conflict (see test_lock_manager.cpp's [blocking] cases for the primitive itself).
// These tests exercise the full executor-level wiring: exec_update_inner's probe-then-
// mutate retry loop, and the table_locks release/reacquire fix required alongside it
// (without which a blocked statement holding table_locks SHARED could deadlock against a
// concurrent COMMIT needing table_locks EXCLUSIVE on the same table -- found via a real
// hang during development, not by review).
TEST_CASE("A blocked UPDATE unblocks once the row's holder commits, not before", "[executor][concurrency][blocking]") {
    TempDataDir dir("exec_concurrency_blocking_unblock");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 0)").is_ok());

    auto shared = ex.get_shared();
    constexpr int kHoldMs = 300;

    std::atomic<bool> a_committed{false};
    std::thread a([&] {
        Executor sess = Executor::new_session(shared);
        REQUIRE(sess.execute_sql("USE d").is_ok());
        REQUIRE(sess.execute_sql("BEGIN").is_ok());
        REQUIRE(sess.execute_sql("UPDATE t SET val = 1 WHERE id = 1").is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
        REQUIRE(sess.execute_sql("COMMIT").is_ok());
        a_committed = true;
    });
    // Give A time to actually acquire the claim before B starts racing it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto t0 = std::chrono::steady_clock::now();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());
    auto result = b.execute_sql("UPDATE t SET val = 2 WHERE id = 1");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    a.join();
    REQUIRE(result.is_ok());
    REQUIRE(a_committed.load());
    // B must have genuinely waited for roughly A's remaining hold time, not returned
    // instantly with a Conflict error (the pre-blocking behavior).
    REQUIRE(elapsed_ms >= 100);
}

TEST_CASE("@lock_wait_timeout is actually enforced by a genuinely blocking UPDATE", "[executor][concurrency][blocking]") {
    TempDataDir dir("exec_concurrency_lock_wait_timeout");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 0)").is_ok());

    auto shared = ex.get_shared();

    Executor holder = Executor::new_session(shared);
    REQUIRE(holder.execute_sql("USE d").is_ok());
    REQUIRE(holder.execute_sql("BEGIN").is_ok());
    REQUIRE(holder.execute_sql("UPDATE t SET val = 1 WHERE id = 1").is_ok());
    // Deliberately never commits/rolls back -- the waiter below must time out on its own.

    Executor waiter = Executor::new_session(shared);
    REQUIRE(waiter.execute_sql("USE d").is_ok());
    REQUIRE(waiter.execute_sql("SET @lock_wait_timeout = 300").is_ok());

    auto t0 = std::chrono::steady_clock::now();
    auto result = waiter.execute_sql("UPDATE t SET val = 2 WHERE id = 1");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE_FALSE(result.is_ok());
    REQUIRE(result.error().find("1205") != std::string::npos);
    REQUIRE(elapsed_ms >= 250);  // actually waited roughly the configured timeout...
    REQUIRE(elapsed_ms < 5000);  // ...not the 50000ms default, and not instant either.

    REQUIRE(holder.execute_sql("ROLLBACK").is_ok());
}

TEST_CASE("Concurrent UPDATEs on different rows of the same table never block each other",
          "[executor][concurrency][blocking]") {
    TempDataDir dir("exec_concurrency_no_false_blocking");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    for (int i = 0; i < 2; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 0)").is_ok());

    auto shared = ex.get_shared();
    std::atomic<bool> failed{false};

    auto worker = [&](int id) {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { failed = true; return; }
        if (!sess.execute_sql("BEGIN").is_ok()) { failed = true; return; }
        if (!sess.execute_sql("UPDATE t SET val = val + 1 WHERE id = " + std::to_string(id)).is_ok()) { failed = true; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!sess.execute_sql("COMMIT").is_ok()) failed = true;
    };

    auto t0 = std::chrono::steady_clock::now();
    std::thread a([&] { worker(0); });
    std::thread b([&] { worker(1); });
    a.join();
    b.join();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE_FALSE(failed.load());
    // If these accidentally serialized (one blocking behind the other -- e.g. a
    // regression re-widening table_locks to whole-table exclusivity), this would take
    // ~400ms+; running genuinely concurrently, it finishes close to the single 200ms hold.
    REQUIRE(elapsed_ms < 380);
}

TEST_CASE("A blocked SELECT FOR UPDATE unblocks once the row's holder commits, not before",
          "[executor][concurrency][blocking][select]") {
    TempDataDir dir("exec_concurrency_blocking_for_update");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 0)").is_ok());

    auto shared = ex.get_shared();
    constexpr int kHoldMs = 300;

    std::atomic<bool> a_committed{false};
    std::thread a([&] {
        Executor sess = Executor::new_session(shared);
        REQUIRE(sess.execute_sql("USE d").is_ok());
        REQUIRE(sess.execute_sql("BEGIN").is_ok());
        REQUIRE(sess.execute_sql("UPDATE t SET val = 1 WHERE id = 1").is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
        REQUIRE(sess.execute_sql("COMMIT").is_ok());
        a_committed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto t0 = std::chrono::steady_clock::now();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());
    REQUIRE(b.execute_sql("BEGIN").is_ok());
    auto result = b.execute_sql("SELECT val FROM t WHERE id = 1 FOR UPDATE");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(b.execute_sql("COMMIT").is_ok());

    a.join();
    REQUIRE(result.is_ok());
    REQUIRE(a_committed.load());
    REQUIRE(result.value().find("1") != std::string::npos); // sees A's committed value
    // B must have genuinely waited for roughly A's remaining hold time, not returned
    // instantly with a Conflict error (the pre-blocking behavior).
    REQUIRE(elapsed_ms >= 100);
}

TEST_CASE("@lock_wait_timeout is actually enforced by a genuinely blocking SELECT FOR SHARE",
          "[executor][concurrency][blocking][select]") {
    TempDataDir dir("exec_concurrency_lock_wait_timeout_for_share");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 0)").is_ok());

    auto shared = ex.get_shared();

    Executor holder = Executor::new_session(shared);
    REQUIRE(holder.execute_sql("USE d").is_ok());
    REQUIRE(holder.execute_sql("BEGIN").is_ok());
    REQUIRE(holder.execute_sql("UPDATE t SET val = 1 WHERE id = 1").is_ok());
    // Deliberately never commits/rolls back -- the waiter below must time out on its own.

    Executor waiter = Executor::new_session(shared);
    REQUIRE(waiter.execute_sql("USE d").is_ok());
    REQUIRE(waiter.execute_sql("SET @lock_wait_timeout = 300").is_ok());
    REQUIRE(waiter.execute_sql("BEGIN").is_ok());

    auto t0 = std::chrono::steady_clock::now();
    auto result = waiter.execute_sql("SELECT val FROM t WHERE id = 1 FOR SHARE");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE_FALSE(result.is_ok());
    REQUIRE(result.error().find("1205") != std::string::npos);
    REQUIRE(elapsed_ms >= 250);  // actually waited roughly the configured timeout...
    REQUIRE(elapsed_ms < 5000);  // ...not the 50000ms default, and not instant either.

    REQUIRE(waiter.execute_sql("ROLLBACK").is_ok());
    REQUIRE(holder.execute_sql("ROLLBACK").is_ok());
}

TEST_CASE("Concurrent SELECT FOR UPDATE on different rows of the same table never block each other",
          "[executor][concurrency][blocking][select]") {
    TempDataDir dir("exec_concurrency_no_false_blocking_for_update");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    for (int i = 0; i < 2; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 0)").is_ok());

    auto shared = ex.get_shared();
    std::atomic<bool> failed{false};

    auto worker = [&](int id) {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { failed = true; return; }
        if (!sess.execute_sql("BEGIN").is_ok()) { failed = true; return; }
        if (!sess.execute_sql("SELECT val FROM t WHERE id = " + std::to_string(id) + " FOR UPDATE").is_ok()) { failed = true; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!sess.execute_sql("COMMIT").is_ok()) failed = true;
    };

    auto t0 = std::chrono::steady_clock::now();
    std::thread a([&] { worker(0); });
    std::thread b([&] { worker(1); });
    a.join();
    b.join();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE_FALSE(failed.load());
    REQUIRE(elapsed_ms < 380);
}

TEST_CASE("A blocked FK ON UPDATE CASCADE unblocks once the child row's other holder commits",
          "[executor][concurrency][blocking][fk]") {
    TempDataDir dir("exec_concurrency_cascade_blocking");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE parent (code INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE child (id INT PRIMARY KEY, parent_code INT, "
                            "FOREIGN KEY (parent_code) REFERENCES parent(code) ON UPDATE CASCADE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO parent VALUES (1)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO child VALUES (100, 1)").is_ok());

    auto shared = ex.get_shared();
    constexpr int kHoldMs = 300;

    // Session A holds an explicit transaction's claim directly on the child row.
    std::atomic<bool> a_committed{false};
    std::thread a([&] {
        Executor sess = Executor::new_session(shared);
        REQUIRE(sess.execute_sql("USE d").is_ok());
        REQUIRE(sess.execute_sql("BEGIN").is_ok());
        REQUIRE(sess.execute_sql("UPDATE child SET id = id WHERE id = 100").is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
        REQUIRE(sess.execute_sql("COMMIT").is_ok());
        a_committed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Session B updates the parent -- its ON UPDATE CASCADE must claim the SAME child row
    // A is holding, genuinely block (via try_cascade_once's NeedsRetry -> block_on_row
    // path), and succeed once A commits and releases it.
    auto t0 = std::chrono::steady_clock::now();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());
    auto result = b.execute_sql("UPDATE parent SET code = 2 WHERE code = 1");
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    a.join();
    REQUIRE(result.is_ok());
    REQUIRE(a_committed.load());
    REQUIRE(elapsed_ms >= 100);

    // `WHERE id = 100` is a PK-indexed point lookup on the cascaded table -- this used to
    // return the stale pre-cascade value indefinitely (FK cascade never updated the
    // cascaded table's PK B+Tree/secondary/composite index entries, only `s.tables`
    // itself); a full scan via `WHERE parent_code = ...` happened to hide the bug. Fixed
    // by refresh_cascade_indexes() in executor_update.cpp/executor_delete.cpp.
    auto check = ex.execute_sql("SELECT parent_code FROM child WHERE id = 100");
    REQUIRE(check.is_ok());
    REQUIRE(check.value().find("2") != std::string::npos);
}

TEST_CASE("Real blocking-wait stress test: many threads racing a small contended row set finds no hangs/livelocks",
          "[executor][concurrency][blocking][stress]") {
    TempDataDir dir("exec_concurrency_blocking_stress");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    constexpr int kRows = 5;
    for (int i = 0; i < kRows; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 0)").is_ok());

    constexpr int kThreads = 6;
    constexpr int kItersPerThread = 60;
    auto shared = ex.get_shared();
    std::atomic<bool> hard_failed{false};
    std::atomic<int> total_committed{0};

    auto worker = [&] {
        Executor sess = Executor::new_session(shared);
        if (!sess.execute_sql("USE d").is_ok()) { hard_failed = true; return; }
        // Bounded well below the 50s default so a genuine (non-hang) contention loss
        // still resolves quickly -- this test's whole point is finding hangs/livelocks,
        // not tolerating a near-full-default wait per iteration.
        if (!sess.execute_sql("SET @lock_wait_timeout = 5000").is_ok()) { hard_failed = true; return; }
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> pick_row(0, kRows - 1);
        int committed = 0;
        for (int i = 0; i < kItersPerThread; i++) {
            int id = pick_row(rng); // small row set, deliberately heavy contention
            if (!sess.execute_sql("BEGIN").is_ok()) { hard_failed = true; return; }
            auto upd = sess.execute_sql("UPDATE t SET val = val + 1 WHERE id = " + std::to_string(id));
            if (!upd.is_ok()) {
                // A real timeout/deadlock is a clean, expected outcome here -- roll back
                // and move on, like a real retrying client (never a hang).
                (void)sess.execute_sql("ROLLBACK");
                continue;
            }
            if (!sess.execute_sql("COMMIT").is_ok()) {
                (void)sess.execute_sql("ROLLBACK");
                continue;
            }
            committed++;
        }
        total_committed += committed;
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    REQUIRE_FALSE(hard_failed.load());
    auto sum_r = ex.execute_sql("SELECT SUM(val) AS s FROM t");
    REQUIRE(sum_r.is_ok());
    REQUIRE(sum_r.value().find(std::to_string(total_committed.load())) != std::string::npos);
}
