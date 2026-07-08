#include <filesystem>

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
    REQUIRE(cache.get("k1") == nullptr);
    cache.put("k1", "result1", {"employee"});
    auto* v = cache.get("k1");
    REQUIRE(v != nullptr);
    REQUIRE(*v == "result1");
    REQUIRE(cache.len() == 1);
}

TEST_CASE("QueryResultCache put does not overwrite existing key", "[query_cache]") {
    QueryResultCache cache;
    cache.put("k1", "first", {"t"});
    cache.put("k1", "second", {"t"});
    REQUIRE(*cache.get("k1") == "first");
}

TEST_CASE("QueryResultCache invalidate_table removes only matching entries", "[query_cache]") {
    QueryResultCache cache;
    cache.put("k1", "r1", {"employee"});
    cache.put("k2", "r2", {"department"});
    cache.put("k3", "r3", {"employee", "department"});

    cache.invalidate_table("employee");
    REQUIRE(cache.get("k1") == nullptr);
    REQUIRE(cache.get("k2") != nullptr);
    REQUIRE(cache.get("k3") == nullptr);
    REQUIRE(cache.len() == 1);
}

TEST_CASE("QueryResultCache evicts oldest entry when over capacity", "[query_cache]") {
    QueryResultCache cache(2);
    cache.put("k1", "r1", {"t"});
    cache.put("k2", "r2", {"t"});
    cache.put("k3", "r3", {"t"}); // should evict k1 (oldest)

    REQUIRE(cache.len() == 2);
    REQUIRE(cache.get("k1") == nullptr);
    REQUIRE(cache.get("k2") != nullptr);
    REQUIRE(cache.get("k3") != nullptr);
}

TEST_CASE("QueryResultCache clear resets everything", "[query_cache]") {
    QueryResultCache cache;
    cache.put("k1", "r1", {"t"});
    cache.clear();
    REQUIRE(cache.len() == 0);
    REQUIRE(cache.get("k1") == nullptr);
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
