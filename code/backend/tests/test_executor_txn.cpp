#include <filesystem>

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

TEST_CASE("BEGIN/COMMIT persists changes made during the transaction", "[executor][txn]") {
    TempDataDir dir("exec_txn_data_1");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    auto begin = ex.execute_sql("BEGIN");
    REQUIRE(begin.is_ok());
    REQUIRE(begin.value().find("started") != std::string::npos);

    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    auto commit = ex.execute_sql("COMMIT");
    REQUIRE(commit.is_ok());
    REQUIRE(commit.value() == "Transaction committed.");

    auto s = ex.get_shared()->read();
    REQUIRE(s->tables.at("company.t").size() == 1);
}

TEST_CASE("BEGIN/ROLLBACK discards changes made during the transaction", "[executor][txn]") {
    TempDataDir dir("exec_txn_data_2");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(ex.execute_sql("UPDATE t SET val = 999 WHERE id = 1").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 200)").is_ok());

    auto rollback = ex.execute_sql("ROLLBACK");
    REQUIRE(rollback.is_ok());
    REQUIRE(rollback.value() == "Transaction rolled back.");

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.t");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].at("val") == "100"); // update rolled back, insert discarded
}

TEST_CASE("SELECT inside an active transaction sees uncommitted writes from the same session", "[executor][txn]") {
    TempDataDir dir("exec_txn_data_3");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(ex.execute_sql("UPDATE t SET val = 500 WHERE id = 1").is_ok());

    auto r = ex.execute_sql("SELECT val FROM t WHERE id = 1");
    REQUIRE(r.is_ok());
    REQUIRE(r.value().find("500") != std::string::npos);

    REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
}

TEST_CASE("SAVEPOINT / ROLLBACK TO / RELEASE SAVEPOINT", "[executor][txn]") {
    TempDataDir dir("exec_txn_data_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT)").is_ok());

    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 100)").is_ok());

    auto sp = ex.execute_sql("SAVEPOINT sp1");
    REQUIRE(sp.is_ok());
    REQUIRE(sp.value().find("sp1") != std::string::npos);

    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (2, 200)").is_ok());
    REQUIRE(ex.execute_sql("UPDATE t SET val = 999 WHERE id = 1").is_ok());

    auto rollback_to = ex.execute_sql("ROLLBACK TO sp1");
    REQUIRE(rollback_to.is_ok());

    REQUIRE(ex.execute_sql("COMMIT").is_ok());

    auto s = ex.get_shared()->read();
    auto& rows = s->tables.at("company.t");
    // Row 2 (inserted after the savepoint) and the update to row 1 should both be undone;
    // only the original row 1 insert (before the savepoint) survives.
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].at("val") == "100");
}

TEST_CASE("SET ISOLATION LEVEL / SHOW ISOLATION LEVEL", "[executor][txn]") {
    TempDataDir dir("exec_txn_data_6");
    Executor ex(dir.path);

    auto show0 = ex.execute_sql("SHOW ISOLATION LEVEL");
    REQUIRE(show0.is_ok());
    REQUIRE(show0.value() == "Current isolation level: READ COMMITTED");

    auto set1 = ex.execute_sql("SET ISOLATION LEVEL SERIALIZABLE");
    REQUIRE(set1.is_ok());
    REQUIRE(set1.value() == "Isolation level set to SERIALIZABLE.");

    auto show1 = ex.execute_sql("SHOW ISOLATION LEVEL");
    REQUIRE(show1.is_ok());
    REQUIRE(show1.value() == "Current isolation level: SERIALIZABLE");

    REQUIRE(ex.execute_sql("SET ISOLATION LEVEL READ UNCOMMITTED").is_ok());
    REQUIRE(ex.execute_sql("SHOW ISOLATION LEVEL").value() == "Current isolation level: READ UNCOMMITTED");

    REQUIRE(ex.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
    REQUIRE(ex.execute_sql("SHOW ISOLATION LEVEL").value() == "Current isolation level: REPEATABLE READ");
}

TEST_CASE("SHOW BUFFER POOL / SHOW WAL / SHOW LOCKS / CHECKPOINT", "[executor][txn]") {
    TempDataDir dir("exec_txn_data_7");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
    REQUIRE(ex.execute_sql("USE company").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());

    auto pool = ex.execute_sql("SHOW BUFFER POOL");
    REQUIRE(pool.is_ok());
    REQUIRE(pool.value().find("적중률") != std::string::npos);

    auto wal = ex.execute_sql("SHOW WAL");
    REQUIRE(wal.is_ok());
    REQUIRE(wal.value().find("WAL") != std::string::npos);

    // Outside any explicit transaction, log_insert/log_update/log_delete write nothing
    // to the WAL (matches Rust: they're gated behind `if self.active`), so there
    // should be no locks held either.
    auto locks = ex.execute_sql("SHOW LOCKS");
    REQUIRE(locks.is_ok());
    REQUIRE(locks.value() == "No active row locks.");

    auto checkpoint = ex.execute_sql("CHECKPOINT");
    REQUIRE(checkpoint.is_ok());
    REQUIRE(checkpoint.value().find("Checkpoint completed.") != std::string::npos);
}

TEST_CASE("Crash recovery rebuilds secondary/hash indexes for a redone commit, not just the table rows",
          "[executor][txn][regression]") {
    // Regression, faithfully preserved from Rust (legacy/rusql-core/src/engine/executor.rs
    // recover_from_wal): the REDO/UNDO replay loop only ever patched s.tables (and, for a
    // REDO INSERT only, the PK B+Tree) -- secondary/hash/composite indexes were left
    // exactly as they were before the crash, stale relative to the just-recovered rows.
    // To reproduce without a real process crash: write the INSERT + an *unfinalized*
    // COMMIT record directly via txn.commit_write_record() (mirrors real 2-phase commit's
    // first phase) and drop the Executor without ever calling commit_finalize()/COMMIT,
    // so the WAL retains a "committed but never applied" transaction exactly like a crash
    // between those two steps would.
    TempDataDir dir("exec_txn_data_recovery_idx");
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
        REQUIRE(ex.execute_sql("USE company").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR(20))").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX idx_val ON t (val) USING HASH").is_ok());

        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'hello')").is_ok());
        REQUIRE(ex.txn.commit_write_record().is_ok());
    }

    // recover_from_wal() runs in the constructor and must REDO the committed insert into
    // s.tables *and* rebuild idx_val's hash index so a lookup on it finds the new row.
    Executor ex2(dir.path);
    {
        auto s = ex2.get_shared()->read();
        REQUIRE(s->tables.at("company.t").size() == 1);
    }

    auto plan = ex2.execute_sql("EXPLAIN SELECT * FROM t WHERE val = 'hello'");
    REQUIRE(plan.is_ok());
    REQUIRE(plan.value().find("Hash Index") != std::string::npos);

    auto sel = ex2.execute_sql("SELECT * FROM t WHERE val = 'hello'");
    REQUIRE(sel.is_ok());
    REQUIRE(sel.value().find("hello") != std::string::npos);
    REQUIRE(sel.value().find("0 rows") == std::string::npos);
}

TEST_CASE("A fresh Executor recovers cleanly when there is no WAL to replay", "[executor][txn]") {
    TempDataDir dir("exec_txn_data_5");
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE company").is_ok());
        REQUIRE(ex.execute_sql("USE company").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY)").is_ok());
        // A plain autocommit INSERT never calls buffer_pool::write_page/flush_page
        // (matches Rust executor.rs exec_insert_inner, which deliberately skips that
        // write-back as an optimization -- see its "불필요한 O(n) clone/write_page
        // 제거" comment). Persistence to disk only happens via an explicit COMMIT
        // flush, maybe_auto_vacuum, or maybe_auto_checkpoint, so wrap this in an
        // explicit transaction to guarantee the row is actually durable before we
        // reopen the Executor below.
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1)").is_ok());
        REQUIRE(ex.execute_sql("COMMIT").is_ok());
    }
    // Recover_from_wal runs in the constructor; this must not crash or corrupt data
    // when there's no crash to recover from (the normal, common case).
    Executor ex2(dir.path);
    auto s = ex2.get_shared()->read();
    REQUIRE(s->tables.at("company.t").size() == 1);
}
