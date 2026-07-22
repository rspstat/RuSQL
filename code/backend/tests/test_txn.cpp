#include <filesystem>
#include <memory>

#include "catch.hpp"
#include "engine/transaction/group_commit.hpp"
#include "engine/transaction/txn_manager.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
std::string test_dir(const std::string& name) {
    std::string dir = "test_tmp_txn_" + name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("global txn_id is unique across managers sharing TxnIoShared", "[txn]") {
    auto dir = test_dir("unique_id");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    TransactionManager b(dir, io);

    auto id_a = a.begin();
    auto id_b = b.begin();
    REQUIRE(id_a.is_ok());
    REQUIRE(id_b.is_ok());
    REQUIRE(id_a.value() != id_b.value());

    a.abort();
    b.abort();
    fs::remove_all(dir);
}

TEST_CASE("concurrent commit preserves other open transaction's WAL/undo records", "[txn]") {
    auto dir = test_dir("concurrent_commit");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    TransactionManager b(dir, io);

    auto id_a = a.begin().value();
    a.log_insert("t", "k1", "{\"a\":1}");

    auto id_b = b.begin().value();
    b.log_insert("t", "k2", "{\"a\":2}");
    REQUIRE(b.commit().is_ok());

    auto a_records = a.wal_records();
    bool a_survives = false, b_gone = true;
    for (auto& r : a_records) {
        if (r.txn_id == id_a && r.key == "k1") a_survives = true;
        if (r.txn_id == id_b) b_gone = false;
    }
    REQUIRE(a_survives);
    REQUIRE(b_gone);

    auto a_undo = a.read_undo_log_file();
    std::size_t a_count = 0, b_count = 0;
    for (auto& e : a_undo) {
        if (e.txn_id == id_a) a_count++;
        if (e.txn_id == id_b) b_count++;
    }
    REQUIRE(a_count == 1);
    REQUIRE(b_count == 0);

    REQUIRE(a.commit().is_ok());
    fs::remove_all(dir);
}

TEST_CASE("rollback_to_savepoint preserves other transaction's undo", "[txn]") {
    auto dir = test_dir("savepoint_preserve");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    TransactionManager b(dir, io);

    auto id_b = b.begin().value();
    b.log_insert("t", "kb", "{}");

    a.begin();
    a.log_insert("t", "ka1", "{}");
    REQUIRE(a.create_savepoint("sp1").is_ok());
    a.log_insert("t", "ka2", "{}");
    REQUIRE(a.rollback_to_savepoint("sp1").is_ok());

    auto entries = a.read_undo_log_file();
    std::size_t b_count = 0, ka2_count = 0, ka1_count = 0;
    for (auto& e : entries) {
        if (e.txn_id == id_b) b_count++;
        if (e.key == "ka2") ka2_count++;
        if (e.key == "ka1") ka1_count++;
    }
    REQUIRE(b_count == 1);
    REQUIRE(ka2_count == 0);
    REQUIRE(ka1_count == 1);

    REQUIRE(a.commit().is_ok());
    REQUIRE(b.commit().is_ok());
    fs::remove_all(dir);
}

TEST_CASE("do_checkpoint deferred when unsafe is a no-op", "[txn]") {
    auto dir = test_dir("checkpoint_deferred");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    a.begin();
    a.log_insert("t", "k1", "{}");
    auto before = a.wal_records().size();

    a.do_checkpoint(false);

    auto after = a.wal_records();
    REQUIRE(after.size() == before);
    bool has_checkpoint = false;
    for (auto& r : after) if (r.op == WalOp::Checkpoint) has_checkpoint = true;
    REQUIRE_FALSE(has_checkpoint);

    a.abort();
    fs::remove_all(dir);
}

TEST_CASE("REPEATABLE READ snapshot isolates reads from live table changes", "[txn]") {
    auto dir = test_dir("rr_snapshot");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    a.set_isolation_level(IsolationLevel::RepeatableRead);

    std::unordered_map<std::string, std::vector<Row>> tables;
    Row r1;
    r1["id"] = "1";
    tables["t"] = {r1};

    a.begin_with_snapshot(tables);
    REQUIRE(a.get_snapshot_table("t") != nullptr);
    REQUIRE(a.get_snapshot_table("t")->size() == 1);

    // Live table changes after BEGIN must not affect the snapshot.
    tables["t"].push_back(r1);
    REQUIRE(a.get_snapshot_table("t")->size() == 1);

    a.abort();
    fs::remove_all(dir);
}

TEST_CASE("SERIALIZABLE validate_serializable detects row-count drift", "[txn]") {
    auto dir = test_dir("serializable");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    a.set_isolation_level(IsolationLevel::Serializable);

    std::unordered_map<std::string, std::vector<Row>> tables;
    Row r1;
    r1["id"] = "1";
    tables["t"] = {r1};
    a.begin_with_snapshot(tables);

    // No drift yet.
    REQUIRE(a.validate_serializable(tables).is_ok());

    // Simulate a phantom row appearing after BEGIN.
    auto live = tables;
    live["t"].push_back(r1);
    REQUIRE(a.validate_serializable(live).is_err());

    a.abort();
    fs::remove_all(dir);
}

TEST_CASE("GroupCommitCoordinator sync_commit does not deadlock for a single session", "[txn][group_commit]") {
    GroupCommitCoordinator coord("gc_test.wal");
    coord.sync_commit();
    coord.sync_commit();
    SUCCEED("leader path completed without blocking");
}

TEST_CASE("WAL encode/decode round trip with txn_id", "[txn][wal]") {
    WalRecord record{WalOp::Insert, 42, "t", "k1", "{\"a\":1}"};
    auto encoded = WalManager::encode(record);
    std::size_t pos = 0;
    auto decoded = WalManager::decode(encoded, pos);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->txn_id == 42);
    REQUIRE(decoded->table_name == "t");
    REQUIRE(decoded->key == "k1");
    REQUIRE(decoded->data == "{\"a\":1}");
    REQUIRE(decoded->op == WalOp::Insert);
}

TEST_CASE("remove_txn preserves other transactions in WAL", "[txn][wal]") {
    auto dir = test_dir("wal_remove_preserve");
    auto io = std::make_shared<TxnIoShared>();
    WalManager wal(dir, io);

    wal.log_insert(1, "t", "k1", "{}");
    wal.log_insert(2, "t", "k2", "{}");
    wal.log_insert(1, "t", "k3", "{}");

    wal.remove_txn(1);

    auto remaining = wal.read_all();
    REQUIRE(remaining.size() == 1);
    REQUIRE(remaining[0].txn_id == 2);
    REQUIRE(remaining[0].key == "k2");

    fs::remove_all(dir);
}

// Regression: remove_txn/truncate_to_last_checkpoint used to rewrite the whole shared
// WAL file via a plain std::ofstream(..., ios::trunc) -- no fsync, no atomic rename via
// a tmp file. A crash mid-write left the file truncated/corrupt, losing other sessions'
// still-pending records too. Both now go through the same write_bytes_atomic tmp+fsync+
// rename helper disk.cpp already uses for table/schema files -- verify no stray ".tmp"
// survives a successful call (the rename should always consume it).
TEST_CASE("remove_txn leaves no stray .tmp file behind", "[txn][wal]") {
    auto dir = test_dir("wal_remove_no_tmp");
    auto io = std::make_shared<TxnIoShared>();
    WalManager wal(dir, io);

    wal.log_insert(1, "t", "k1", "{}");
    wal.log_insert(2, "t", "k2", "{}");
    wal.remove_txn(1);

    REQUIRE_FALSE(fs::exists(dir + "/rusql.wal.tmp"));
    fs::remove_all(dir);
}

TEST_CASE("truncate_to_last_checkpoint keeps only records from the last checkpoint onward, no stray .tmp file", "[txn][wal]") {
    auto dir = test_dir("wal_truncate_checkpoint");
    auto io = std::make_shared<TxnIoShared>();
    WalManager wal(dir, io);

    wal.log_insert(1, "t", "k1", "{}");
    wal.log_checkpoint();
    wal.log_insert(2, "t", "k2", "{}");
    wal.log_commit(2);

    wal.truncate_to_last_checkpoint();

    auto remaining = wal.read_all();
    REQUIRE(remaining.size() == 3); // checkpoint marker + insert(2) + commit(2)
    REQUIRE(remaining[0].op == WalOp::Checkpoint);
    REQUIRE(remaining[1].txn_id == 2);
    REQUIRE(remaining[2].op == WalOp::Commit);

    REQUIRE_FALSE(fs::exists(dir + "/rusql.wal.tmp"));
    fs::remove_all(dir);
}

TEST_CASE("UndoLogFile remove_txn/rewrite_txn are atomic (no stray .tmp file, no dropped entries)", "[txn][wal]") {
    auto dir = test_dir("undo_atomic");
    auto io = std::make_shared<TxnIoShared>();
    UndoLogFile undo(dir, io);

    UndoEntry e1{1, "INSERT", "t", "k1", std::nullopt};
    UndoEntry e2{2, "INSERT", "t", "k2", std::nullopt};
    undo.append(e1);
    undo.append(e2);

    undo.remove_txn(1);
    auto after_remove = undo.read_all();
    REQUIRE(after_remove.size() == 1);
    REQUIRE(after_remove[0].txn_id == 2);
    REQUIRE_FALSE(fs::exists(dir + "/_undo.log.tmp"));

    UndoEntry e3{2, "UPDATE", "t", "k2", std::string("{}")};
    undo.rewrite_txn(2, {e3});
    auto after_rewrite = undo.read_all();
    REQUIRE(after_rewrite.size() == 1);
    REQUIRE(after_rewrite[0].operation == "UPDATE");
    REQUIRE_FALSE(fs::exists(dir + "/_undo.log.tmp"));

    fs::remove_all(dir);
}
