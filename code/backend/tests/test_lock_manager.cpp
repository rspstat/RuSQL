#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

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
    auto r = lm.register_gap_conflict("t", 2, 1);
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
    auto r = lm.register_gap_conflict("t", 2, 1);
    REQUIRE(r.kind == LockResult::Kind::Deadlock);
}

TEST_CASE("register_gap_conflict with a timeout blocks until release() then grants", "[lock_manager][gap_lock][blocking]") {
    LockManager lm;
    lm.acquire_gap("t", std::nullopt, true, std::nullopt, true, 1);
    REQUIRE(lm.register_gap_conflict("t", 2, 1).kind == LockResult::Kind::Conflict);

    std::atomic<bool> waiter_done{false};
    LockResult result{LockResult::Kind::Conflict, 0};
    std::thread waiter([&] {
        result = lm.register_gap_conflict("t", 2, 1, std::chrono::milliseconds(5000));
        waiter_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE_FALSE(waiter_done.load()); // still genuinely blocked, not instant-failed

    lm.release(1); // holder's txn commits/rolls back -- sweeps its gap locks too
    waiter.join();
    REQUIRE(result.kind == LockResult::Kind::Granted);
    REQUIRE(lm.gap_locks_for("t").empty());
}

TEST_CASE("register_gap_conflict with a timeout returns Timeout after the deadline when never released",
          "[lock_manager][gap_lock][blocking]") {
    LockManager lm;
    lm.acquire_gap("t", std::nullopt, true, std::nullopt, true, 1);

    auto t0 = std::chrono::steady_clock::now();
    auto r = lm.register_gap_conflict("t", 2, 1, std::chrono::milliseconds(200));
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE(r.kind == LockResult::Kind::Timeout);
    REQUIRE(r.holder == 1);
    REQUIRE(elapsed_ms >= 180);
    REQUIRE(elapsed_ms < 2000);
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

TEST_CASE("register_predicate_read never conflicts and predicate_reads_for returns it", "[lock_manager][predicate_lock]") {
    LockManager lm;
    lm.register_predicate_read("t", "10", true, "20", true, 1);
    lm.register_predicate_read("t", "15", true, "25", true, 2); // overlapping range, different txn -- fine
    auto reads = lm.predicate_reads_for("t");
    REQUIRE(reads.size() == 2);
}

TEST_CASE("predicate_reads_for returns empty for untouched table", "[lock_manager][predicate_lock]") {
    LockManager lm;
    REQUIRE(lm.predicate_reads_for("nope").empty());
}

TEST_CASE("flag_predicate_violation/take_predicate_violation round-trip exactly once", "[lock_manager][predicate_lock]") {
    LockManager lm;
    REQUIRE_FALSE(lm.take_predicate_violation(1)); // nothing flagged yet
    lm.flag_predicate_violation(1);
    REQUIRE(lm.take_predicate_violation(1)); // consumes it
    REQUIRE_FALSE(lm.take_predicate_violation(1)); // already consumed
}

TEST_CASE("release sweeps predicate reads and any pending violation flag for the released txn only",
          "[lock_manager][predicate_lock]") {
    LockManager lm;
    lm.register_predicate_read("t", "1", true, "10", true, 1);
    lm.register_predicate_read("t", "5", true, "15", true, 2);
    lm.flag_predicate_violation(1);
    lm.release(1);
    auto reads = lm.predicate_reads_for("t");
    REQUIRE(reads.size() == 1);
    REQUIRE(reads[0].holder == 2);
    REQUIRE_FALSE(lm.take_predicate_violation(1)); // cleared by release(), not just consumed
}

// Row-level-concurrency prep: RowClaimGuard is the release point for autocommit-scoped
// (one-off id) claims, since autocommit statements have no COMMIT/ROLLBACK of their own
// to call release() at. Not wired into any executor code yet at this stage -- these
// tests only confirm the guard's own release-on-destruct contract.
TEST_CASE("RowClaimGuard releases claims on destruction when it owns the id", "[lock_manager][row_claim_guard]") {
    LockManager lm;
    lm.acquire("t", "1", 42);
    {
        RowClaimGuard guard(lm, 42, /*owns=*/true);
        REQUIRE(lm.holder("t", "1") == 42u);
    }
    REQUIRE_FALSE(lm.holder("t", "1").has_value());
}

TEST_CASE("RowClaimGuard does nothing on destruction when it does not own the id", "[lock_manager][row_claim_guard]") {
    // Mirrors reusing an explicit transaction's own long-lived id -- that transaction's
    // claims must survive past this one statement, only released later by its real
    // COMMIT/ROLLBACK (lm.release(cur_txn) called directly, not via the guard).
    LockManager lm;
    lm.acquire("t", "1", 7);
    {
        RowClaimGuard guard(lm, 7, /*owns=*/false);
        REQUIRE(lm.holder("t", "1") == 7u);
    }
    REQUIRE(lm.holder("t", "1") == 7u);
    lm.release(7);
    REQUIRE_FALSE(lm.holder("t", "1").has_value());
}

TEST_CASE("RowClaimGuard survives an early return (the leak scenario it exists to prevent)", "[lock_manager][row_claim_guard]") {
    LockManager lm;
    auto do_statement = [&lm](bool fail_early) -> bool {
        RowClaimGuard guard(lm, 99, /*owns=*/true);
        lm.acquire("t", "1", 99);
        if (fail_early) return false; // e.g. a constraint violation's early `return Err(...)`
        lm.acquire("t", "2", 99);
        return true;
    };
    REQUIRE_FALSE(do_statement(true));
    REQUIRE_FALSE(lm.holder("t", "1").has_value()); // released even though we returned early
}

TEST_CASE("RowClaimGuard move transfers ownership -- the moved-from guard doesn't double-release", "[lock_manager][row_claim_guard]") {
    LockManager lm;
    lm.acquire("t", "1", 5);
    {
        RowClaimGuard g1(lm, 5, /*owns=*/true);
        RowClaimGuard g2(std::move(g1));
        (void)g2;
        // g1 destructs first (reverse declaration order) but no longer owns anything;
        // g2 destructs second and performs the one real release.
    }
    REQUIRE_FALSE(lm.holder("t", "1").has_value());
}

// Real-blocking-wait stage: acquire()/acquire_shared()'s new `timeout` parameter (default
// 0, tested above via every pre-existing case) makes the calling thread genuinely sleep on
// a condition_variable instead of instant-failing. These tests use real std::thread pairs
// -- the whole point is proving actual blocking/wakeup/timeout behavior, which a
// single-threaded call-and-inspect-return-value test cannot exercise.
TEST_CASE("acquire() with a timeout blocks until release() then grants", "[lock_manager][blocking]") {
    LockManager lm;
    REQUIRE(lm.acquire("t", "1", 1).kind == LockResult::Kind::Granted);

    std::atomic<bool> waiter_done{false};
    LockResult result{LockResult::Kind::Conflict, 0};
    std::thread waiter([&] {
        result = lm.acquire("t", "1", 2, std::chrono::milliseconds(5000));
        waiter_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE_FALSE(waiter_done.load()); // still genuinely blocked, not instant-failed

    lm.release(1);
    waiter.join();
    REQUIRE(result.kind == LockResult::Kind::Granted);
    REQUIRE(lm.holder("t", "1") == 2u);
}

TEST_CASE("acquire() with a timeout returns Timeout after the deadline when never released", "[lock_manager][blocking]") {
    LockManager lm;
    REQUIRE(lm.acquire("t", "1", 1).kind == LockResult::Kind::Granted);

    auto t0 = std::chrono::steady_clock::now();
    auto r = lm.acquire("t", "1", 2, std::chrono::milliseconds(200));
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE(r.kind == LockResult::Kind::Timeout);
    REQUIRE(r.holder == 1);
    REQUIRE(elapsed_ms >= 180);  // actually waited roughly the requested duration...
    REQUIRE(elapsed_ms < 2000);  // ...not an instant fail, and not wildly over either.
}

TEST_CASE("a deadlock forming while one transaction sleeps is caught by the completing transaction, not the sleeper",
          "[lock_manager][blocking]") {
    LockManager lm;
    REQUIRE(lm.acquire("t", "A", 1).kind == LockResult::Kind::Granted); // txn 1 holds A
    REQUIRE(lm.acquire("t", "B", 2).kind == LockResult::Kind::Granted); // txn 2 holds B

    std::atomic<bool> t1_done{false};
    LockResult t1_result{LockResult::Kind::Conflict, 0};
    std::thread t1_thread([&] {
        // txn 1 blocks waiting for B, held by txn 2 -- no cycle yet at this point.
        t1_result = lm.acquire("t", "B", 1, std::chrono::milliseconds(5000));
        t1_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE_FALSE(t1_done.load());

    // txn 2 now requests A, held by txn 1 (who is asleep waiting on txn 2) -- this
    // request is the one that completes the cycle, so txn 2 must fail IMMEDIATELY
    // (the "victim"), never waiting itself.
    auto t2_result = lm.acquire("t", "A", 2);
    REQUIRE(t2_result.kind == LockResult::Kind::Deadlock);
    REQUIRE_FALSE(t1_done.load()); // txn 1 is untouched, still correctly asleep

    // Simulate txn 2 rolling back after receiving its deadlock error.
    lm.release(2);
    t1_thread.join();
    REQUIRE(t1_result.kind == LockResult::Kind::Granted);
}

TEST_CASE("a sleeping waiter's wait_for_ edge tracks the current holder after it changes hands",
          "[lock_manager][blocking]") {
    LockManager lm;
    // Two shared holders on the same row -- releasing just one never frees the row (the
    // other still holds it), so txn 1's blocking exclusive request stays genuinely
    // unsatisfiable throughout this test with no race window where it could sneak in and
    // get granted early.
    REQUIRE(lm.acquire_shared("t", "1", 2).kind == LockResult::Kind::Granted);
    REQUIRE(lm.acquire_shared("t", "1", 5).kind == LockResult::Kind::Granted);

    std::thread waiter([&] { lm.acquire("t", "1", 1, std::chrono::milliseconds(5000)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto wf = lm.wait_for_rows();
    REQUIRE(wf.size() == 1);
    REQUIRE(wf[0].first == 1);
    std::uint64_t initial_holder = wf[0].second;
    REQUIRE((initial_holder == 2 || initial_holder == 5));
    std::uint64_t remaining_holder = (initial_holder == 2) ? 5 : 2;

    // Release exactly the holder txn 1's edge currently points to -- the row stays held
    // (shared, by `remaining_holder`), so txn 1 must still be blocked, but its wait_for_
    // edge must now point at `remaining_holder` instead of the stale released one.
    lm.release(initial_holder);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto wf2 = lm.wait_for_rows();
    REQUIRE(wf2.size() == 1);
    REQUIRE(wf2[0].first == 1);
    REQUIRE(wf2[0].second == remaining_holder);

    lm.release(remaining_holder);
    waiter.join();
    REQUIRE(lm.holder("t", "1") == 1u);
}
