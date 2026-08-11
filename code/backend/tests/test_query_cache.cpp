#include <filesystem>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/query_cache.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};
} // namespace

TEST_CASE("QueryResultCache basic put/get", "[query_cache]") {
    QueryResultCache cache;
    REQUIRE_FALSE(cache.get("k1").has_value());
    cache.put("k1", "result1", {{"employee", cache.table_generation("employee")}});
    auto v = cache.get("k1");
    REQUIRE(v.has_value());
    REQUIRE(*v == "result1");
    REQUIRE(cache.len() == 1);
}

TEST_CASE("QueryResultCache put does not overwrite existing key", "[query_cache]") {
    QueryResultCache cache;
    cache.put("k1", "first", {{"t", cache.table_generation("t")}});
    cache.put("k1", "second", {{"t", cache.table_generation("t")}});
    REQUIRE(*cache.get("k1") == "first");
}

TEST_CASE("QueryResultCache invalidate_table removes only matching entries", "[query_cache]") {
    QueryResultCache cache;
    cache.put("k1", "r1", {{"employee", cache.table_generation("employee")}});
    cache.put("k2", "r2", {{"department", cache.table_generation("department")}});
    cache.put("k3", "r3", {{"employee", cache.table_generation("employee")}, {"department", cache.table_generation("department")}});

    cache.invalidate_table("employee");
    REQUIRE_FALSE(cache.get("k1").has_value());
    REQUIRE(cache.get("k2").has_value());
    REQUIRE_FALSE(cache.get("k3").has_value());
    REQUIRE(cache.len() == 1);
}

TEST_CASE("QueryResultCache evicts oldest entry when over capacity", "[query_cache]") {
    QueryResultCache cache(2);
    cache.put("k1", "r1", {{"t", cache.table_generation("t")}});
    cache.put("k2", "r2", {{"t", cache.table_generation("t")}});
    cache.put("k3", "r3", {{"t", cache.table_generation("t")}}); // should evict k1 (oldest)

    REQUIRE(cache.len() == 2);
    REQUIRE_FALSE(cache.get("k1").has_value());
    REQUIRE(cache.get("k2").has_value());
    REQUIRE(cache.get("k3").has_value());
}

TEST_CASE("QueryResultCache clear resets everything", "[query_cache]") {
    QueryResultCache cache;
    cache.put("k1", "r1", {{"t", cache.table_generation("t")}});
    cache.clear();
    REQUIRE(cache.len() == 0);
    REQUIRE_FALSE(cache.get("k1").has_value());
}

TEST_CASE("QueryResultCache get() rejects an entry whose table generation moved on since it was cached",
          "[query_cache]") {
    // Row-level-concurrency Stage 4/5 correctness fix regression: a put() stamped with
    // a generation captured before invalidate_table() ran (e.g. because a concurrent
    // writer's invalidate landed between this entry's read and its put()) must be
    // treated as a miss on the very next get(), not resurrect stale data.
    QueryResultCache cache;
    std::uint64_t stale_gen = cache.table_generation("t");
    cache.invalidate_table("t"); // bumps generation past `stale_gen`
    cache.put("k1", "stale", {{"t", stale_gen}});
    REQUIRE_FALSE(cache.get("k1").has_value());
}

TEST_CASE("execute_sql never caches a query calling a non-deterministic function", "[query_cache][executor]") {
    // PLAN.md P0 regression test: a SELECT calling NOW()/RAND()/UUID()/etc. used to
    // get cached on first execution and then return that exact same stale value
    // forever, since nothing ever invalidates a cache entry with no table reference.
    TempDataDir dir("query_cache_data_nondeterministic");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE t").is_ok());
    REQUIRE(ex.execute_sql("USE t").is_ok());

    auto r1 = ex.execute_sql("SELECT RAND() AS r");
    auto r2 = ex.execute_sql("SELECT RAND() AS r");
    REQUIRE(r1.is_ok());
    REQUIRE(r2.is_ok());
    REQUIRE(r1.value() != r2.value());
}

TEST_CASE("execute_sql still caches a deterministic query, incl. one mentioning a column named like 'brand'",
          "[query_cache][executor]") {
    // Regression guard for the fix above: the non-deterministic-function detector
    // must not false-positive on an identifier that merely *contains* one of those
    // function names (e.g. a `brand` column contains "rand") and disable caching
    // for an otherwise perfectly cacheable, deterministic query.
    TempDataDir dir("query_cache_data_deterministic_stays_cached");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE t").is_ok());
    REQUIRE(ex.execute_sql("USE t").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE p (id INT PRIMARY KEY, brand VARCHAR(20))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1,'acme')").is_ok());

    auto r1 = ex.execute_sql("SELECT id, brand FROM p WHERE id = 1");
    REQUIRE(r1.is_ok());
    REQUIRE(ex.get_shared()->read()->query_cache.len() == 1);
}

// Stage 5 prep: put()/invalidate_table()/clear() became `const` (backed entirely by the
// cache's own mutex_) so execute_sql's cache population/invalidation could move from
// shared->write() (whole database exclusive, on every successful DML regardless of
// table) to shared->read(). Confirms the cache itself is genuinely safe under real
// concurrent callers, not just callable through a const reference.
TEST_CASE("QueryResultCache is safe under real concurrent put/get/invalidate_table", "[query_cache][concurrency]") {
    QueryResultCache cache(1000);
    constexpr int kThreads = 8;
    constexpr int kIterations = 500;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < kIterations; i++) {
                std::string key = "k" + std::to_string(t) + "_" + std::to_string(i);
                std::string tbl = "tbl" + std::to_string(t % 3);
                cache.put(key, "v" + std::to_string(i), {{tbl, cache.table_generation(tbl)}});
                (void)cache.get(key);
                if (i % 10 == 0) cache.invalidate_table("tbl" + std::to_string((t + 1) % 3));
            }
        });
    }
    for (auto& th : threads) th.join();
    // No crash/UB is the primary assertion here (TSan/ASan would be the real judge);
    // a sane post-condition check that the cache is still internally consistent.
    REQUIRE(cache.len() <= 1000);
}
