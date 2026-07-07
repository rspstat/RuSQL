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
