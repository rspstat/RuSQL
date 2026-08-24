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

// MVCC Stage 2: the deep-clone snapshot_/get_snapshot_table mechanism this test used to
// exercise was retired (SELECT now filters s.tables directly via is_visible_for_read +
// SnapshotCtx instead of reading from a frozen full-table copy) -- rewritten to test the
// mechanism that replaced it: frozen_ctx(), captured once at BEGIN and never recomputed
// for the rest of the transaction, regardless of what happens afterward.
TEST_CASE("REPEATABLE READ freezes its read ctx at BEGIN, unaffected by later activity", "[txn]") {
    auto dir = test_dir("rr_snapshot");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    a.set_isolation_level(IsolationLevel::RepeatableRead);

    std::unordered_set<std::uint64_t> active_before = {42}; // some other txn open at BEGIN time
    a.begin_with_snapshot(active_before);
    REQUIRE(a.frozen_ctx().has_value());
    REQUIRE(a.frozen_ctx()->in_progress.count(42) == 1);

    // A later capture (what a fresh per-statement ctx would see) must differ from what
    // was frozen at BEGIN -- proving frozen_ctx() really is captured once, not live.
    std::uint64_t cutoff_at_begin = a.frozen_ctx()->cutoff;
    io->next_id(); // simulate other activity issuing more ids after BEGIN
    REQUIRE(a.frozen_ctx()->cutoff == cutoff_at_begin);
    REQUIRE(io->peek_next_id() > cutoff_at_begin);

    a.abort();
    fs::remove_all(dir);
}

// MVCC Stage 3: validate_serializable used to compare table-wide row counts between a
// deep clone taken at BEGIN and the live tables at COMMIT -- a real gap, since a same-
// count interleaving (one row deleted, a different row inserted) passed undetected.
// Replaced with real read-set tracking: record_read() logs which specific rows a
// Serializable transaction observed, and validate_serializable now checks whether any of
// those specific rows' visibility has changed due to another transaction's write that has
// since committed.
TEST_CASE("SERIALIZABLE validate_serializable detects a read row modified by a since-committed transaction",
          "[txn]") {
    auto dir = test_dir("serializable");
    auto io = std::make_shared<TxnIoShared>();
    TransactionManager a(dir, io);
    a.set_isolation_level(IsolationLevel::Serializable);

    a.begin_with_snapshot({});
    a.record_read("t", "id", "1"); // simulates a SELECT that observed row id=1

    Row r1;
    r1["id"] = "1";
    r1["_xmin"] = "0";
    r1["_xmax"] = "0";
    std::unordered_map<std::string, std::vector<Row>> tables{{"t", {r1}}};

    // Row unchanged -- no conflict yet.
    REQUIRE(a.validate_serializable(tables, {}).is_ok());

    // Simulate a different, already-committed transaction (its id isn't in
    // active_txn_ids_now) updating row id=1 sometime after `a` began: old version marked
    // dead, a new version appended -- same physical row *count* as before (still one row
    // for this key, or two if not yet vacuumed), but the content a read is no longer what
    // `a`'s snapshot saw, which the old count-only check could never catch.
    std::uint64_t other_txn = io->next_id(); // allocated after a's frozen cutoff was captured
    Row r1_dead = r1;
    r1_dead["_xmax"] = std::to_string(other_txn);
    Row r1_new = r1;
    r1_new["_xmin"] = std::to_string(other_txn);
    tables["t"] = {r1_dead, r1_new};

    REQUIRE(a.validate_serializable(tables, {}).is_err());

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

TEST_CASE("WAL decode rejects a record whose content was corrupted without breaking its length framing",
          "[txn][wal]") {
    // 손상이 길이 프리픽스가 아니라 문자열 "내용" 바이트를 건드리면, 체크섬 없이는 프레이밍이
    // 그대로 유효해 보여 decode()가 손상된 데이터를 조용히 정상 레코드로 받아들였다. 체크섬
    // 도입 후엔 이런 경우를 명시적으로 감지해 nullopt를 반환해야 한다(PLAN.md: "WAL/Undo
    // 디코딩이 체크섬 없음").
    WalRecord record{WalOp::Insert, 42, "t", "k1", "{\"a\":1}"};
    auto encoded = WalManager::encode(record);

    // data 필드("{\"a\":1}") 안의 한 바이트를 플립 -- 길이는 그대로라 프레이밍은 안 깨짐.
    auto data_pos = std::string(encoded.begin(), encoded.end()).find("{\"a\":1}");
    REQUIRE(data_pos != std::string::npos);
    encoded[data_pos] ^= 0xFF;

    std::size_t pos = 0;
    auto decoded = WalManager::decode(encoded, pos);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("UndoLogFile encode/decode round trip, and rejects content-corrupted entries", "[txn][wal]") {
    UndoEntry entry{7, "UPDATE", "t", "k1", std::optional<std::string>("{\"a\":1}")};
    auto encoded = UndoLogFile::encode(entry);

    std::size_t pos = 0;
    auto decoded = UndoLogFile::decode(encoded, pos);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->txn_id == 7);
    REQUIRE(decoded->operation == "UPDATE");
    REQUIRE(decoded->table == "t");
    REQUIRE(decoded->key == "k1");
    REQUIRE(decoded->old_data == "{\"a\":1}");

    auto data_pos = std::string(encoded.begin(), encoded.end()).find("{\"a\":1}");
    REQUIRE(data_pos != std::string::npos);
    encoded[data_pos] ^= 0xFF;
    pos = 0;
    REQUIRE_FALSE(UndoLogFile::decode(encoded, pos).has_value());
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
