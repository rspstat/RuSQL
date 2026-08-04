#include "catch.hpp"
#include "engine/lock_manager.hpp"

using namespace engine;

TEST_CASE("grant lock", "[lock_manager]") {
    LockManager lm;
    REQUIRE(lm.acquire("t", "1", 1).kind == LockResult::Kind::Granted);
}

TEST_CASE("reentrant lock", "[lock_manager]") {
    LockManager lm;
    lm.acquire("t", "1", 1);
    REQUIRE(lm.acquire("t", "1", 1).kind == LockResult::Kind::Granted);
}

TEST_CASE("conflict", "[lock_manager]") {
    LockManager lm;
    lm.acquire("t", "1", 1);
    auto r = lm.acquire("t", "1", 2);
    REQUIRE(r.kind == LockResult::Kind::Conflict);
    REQUIRE(r.holder == 1);
}

TEST_CASE("deadlock detection", "[lock_manager]") {
    LockManager lm;
    lm.acquire("t", "row1", 1);
    lm.acquire("t", "row2", 2);
    auto r1 = lm.acquire("t", "row2", 1);
    REQUIRE(r1.kind == LockResult::Kind::Conflict);
    REQUIRE(r1.holder == 2);
    auto r2 = lm.acquire("t", "row1", 2);
    REQUIRE(r2.kind == LockResult::Kind::Deadlock);
    REQUIRE(r2.holder == 1);
    REQUIRE(lm.deadlock_history().size() == 1);
    REQUIRE(lm.deadlock_history()[0] == std::make_pair(std::uint64_t{2}, std::uint64_t{1}));
}

TEST_CASE("release", "[lock_manager]") {
    LockManager lm;
    lm.acquire("t", "1", 1);
    lm.acquire("t", "2", 1);
    lm.release(1);
    REQUIRE(lm.is_empty());
    REQUIRE(lm.wait_for_rows().empty());
}

TEST_CASE("shared lock multi readers", "[lock_manager]") {
    LockManager lm;
    REQUIRE(lm.acquire_shared("t", "1", 1).kind == LockResult::Kind::Granted);
    REQUIRE(lm.acquire_shared("t", "1", 2).kind == LockResult::Kind::Granted);
    REQUIRE(lm.acquire_shared("t", "1", 3).kind == LockResult::Kind::Granted);
}

TEST_CASE("shared exclusive conflict", "[lock_manager]") {
    LockManager lm;
    lm.acquire_shared("t", "1", 1);
    lm.acquire_shared("t", "1", 2);
    REQUIRE(lm.acquire("t", "1", 3).kind == LockResult::Kind::Conflict);
}

TEST_CASE("exclusive blocks shared", "[lock_manager]") {
    LockManager lm;
    lm.acquire("t", "1", 1);
    auto r = lm.acquire_shared("t", "1", 2);
    REQUIRE(r.kind == LockResult::Kind::Conflict);
    REQUIRE(r.holder == 1);
}

TEST_CASE("single shared holder upgrades to exclusive", "[lock_manager]") {
    LockManager lm;
    lm.acquire_shared("t", "1", 1);
    REQUIRE(lm.acquire("t", "1", 1).kind == LockResult::Kind::Granted);
    REQUIRE(lm.holder("t", "1") == 1u);
}

TEST_CASE("gap locks never conflict with each other", "[lock_manager][gap_lock]") {
    LockManager lm;
    lm.acquire_gap("t", "10", true, "20", true, 1);
    lm.acquire_gap("t", "15", true, "25", true, 2); // overlapping range, different txn
    lm.acquire_gap("t", "10", true, "20", true, 1); // same txn, same range again -- harmless
    auto gaps = lm.gap_locks_for("t");
    REQUIRE(gaps.size() == 3);
}

TEST_CASE("gap_locks_for returns empty for untouched table", "[lock_manager][gap_lock]") {
    LockManager lm;
    REQUIRE(lm.gap_locks_for("nope").empty());
}

TEST_CASE("register_gap_conflict reports conflict", "[lock_manager][gap_lock]") {
    LockManager lm;
    lm.acquire_gap("t", std::nullopt, true, std::nullopt, true, 1);
    auto r = lm.register_gap_conflict(2, 1);
    REQUIRE(r.kind == LockResult::Kind::Conflict);
    REQUIRE(r.holder == 1);
}

TEST_CASE("register_gap_conflict detects deadlock via shared wait_for_ graph", "[lock_manager][gap_lock]") {
    LockManager lm;
    // txn 1 is already waiting on txn 2 (via an ordinary row lock)...
    lm.acquire("t", "row", 2);
    REQUIRE(lm.acquire("t", "row", 1).kind == LockResult::Kind::Conflict);
    // ...so txn 2 registering a gap-lock conflict against txn 1 closes the cycle.
    lm.acquire_gap("t", std::nullopt, true, std::nullopt, true, 1);
    auto r = lm.register_gap_conflict(2, 1);
    REQUIRE(r.kind == LockResult::Kind::Deadlock);
}

TEST_CASE("release sweeps gap locks for the released txn only", "[lock_manager][gap_lock]") {
    LockManager lm;
    lm.acquire_gap("t", "1", true, "10", true, 1);
    lm.acquire_gap("t", "5", true, "15", true, 2);
    lm.release(1);
    auto gaps = lm.gap_locks_for("t");
    REQUIRE(gaps.size() == 1);
    REQUIRE(gaps[0].holder == 2);
}

TEST_CASE("gap_lock_rows formats ranges and is sorted", "[lock_manager][gap_lock]") {
    LockManager lm;
    lm.acquire_gap("z", "10", true, "20", false, 1);
    lm.acquire_gap("a", std::nullopt, true, std::nullopt, true, 2);
    auto rows = lm.gap_lock_rows();
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<0>(rows[0]) == "a");
    REQUIRE(std::get<1>(rows[0]) == "[-inf, +inf]");
    REQUIRE(std::get<0>(rows[1]) == "z");
    REQUIRE(std::get<1>(rows[1]) == "[10, 20)");
}
