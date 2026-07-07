#include "catch.hpp"
#include "engine/query_cache.hpp"

using namespace engine;

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
