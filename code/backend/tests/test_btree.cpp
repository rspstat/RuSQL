#include "catch.hpp"
#include "engine/storage/btree.hpp"
#include "engine/storage/btree_json.hpp"

using namespace engine;

TEST_CASE("cmp_keys is numeric-aware", "[btree]") {
    REQUIRE(cmp_keys("10", "9") > 0);
    REQUIRE(cmp_keys("2", "10") < 0);
    REQUIRE(cmp_keys("5", "5") == 0);
    REQUIRE(cmp_keys("abc", "abd") < 0);
}

TEST_CASE("cmp_keys treats differently-formatted equal-valued numeric strings as distinct", "[btree]") {
    // PLAN.md P0 regression test: "007" and "07" both parse to the f64 value 7.0,
    // so cmp_keys used to report them equal — colliding two distinct string PK
    // values (e.g. zip codes, SKUs) into what looked like a single duplicate key.
    REQUIRE(cmp_keys("007", "07") != 0);
    REQUIRE(cmp_keys("07", "007") != 0);
    REQUIRE(cmp_keys("007", "007") == 0);
    // Still numeric-order-consistent: both sit at the same numeric position (7),
    // strictly between the neighboring distinct numeric values 5 and 9.
    REQUIRE(cmp_keys("5", "007") < 0);
    REQUIRE(cmp_keys("007", "9") < 0);
    REQUIRE(cmp_keys("5", "07") < 0);
    REQUIRE(cmp_keys("07", "9") < 0);
}

TEST_CASE("BPlusTree keeps differently-formatted equal-valued numeric string keys distinct", "[btree]") {
    BPlusTree tree;
    tree.insert("007", "agent");
    tree.insert("07", "other");
    REQUIRE(tree.search("007") == "agent");
    REQUIRE(tree.search("07") == "other");
}

TEST_CASE("cmp_keys compares composite-index keys (NUL-joined columns) segment-by-segment, numerically", "[btree]") {
    // PLAN.md P0 regression test: CompositeIndex::make_key joins column values with
    // a '\x00' separator ("val1\x00val2..."). The embedded NUL byte means the whole
    // joined string never parses as a single f64, so cmp_keys always fell back to
    // plain lexicographic comparison of the WHOLE key -- sorting a leading numeric
    // column like "10\x00..." before "9\x00..." (lexicographic '1' < '9'), the
    // opposite of the correct numeric order. Splitting on '\x00' and comparing each
    // column segment independently (like a multi-column ORDER BY) fixes this.
    std::string k9 = std::string("9") + '\x00' + "100";
    std::string k10 = std::string("10") + '\x00' + "200";
    REQUIRE(cmp_keys(k10, k9) > 0);
    REQUIRE(cmp_keys(k9, k10) < 0);

    // Tie on the leading segment falls through to the second segment, numerically.
    std::string k10a = std::string("10") + '\x00' + "9";
    std::string k10b = std::string("10") + '\x00' + "10";
    REQUIRE(cmp_keys(k10a, k10b) < 0);

    // Plain (non-composite) keys are unaffected -- no '\x00' means a single segment,
    // identical behavior to the numeric-aware single-key comparison above.
    REQUIRE(cmp_keys("10", "9") > 0);
}

TEST_CASE("BPlusTree insert/search", "[btree]") {
    BPlusTree tree;
    for (int i : {1, 10, 5, 3, 7, 2, 8, 4, 6, 9}) {
        tree.insert(std::to_string(i), "v" + std::to_string(i));
    }
    REQUIRE(tree.search("10") == "v10");
    REQUIRE(tree.search("1") == "v1");
    REQUIRE(!tree.search("11").has_value());
}

TEST_CASE("BPlusTree range_search", "[btree]") {
    BPlusTree tree;
    for (int i = 1; i <= 10; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    auto r = tree.range_search("3", "7");
    REQUIRE(r.size() == 5);
}

TEST_CASE("BPlusTree scan_from", "[btree]") {
    BPlusTree tree;
    for (int i = 1; i <= 10; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));

    auto r = tree.scan_from("8", true);
    std::vector<std::string> keys;
    for (auto& [k, v] : r) keys.push_back(k);
    REQUIRE(keys == std::vector<std::string>{"8", "9", "10"});

    auto r2 = tree.scan_from("8", false);
    std::vector<std::string> keys2;
    for (auto& [k, v] : r2) keys2.push_back(k);
    REQUIRE(keys2 == std::vector<std::string>{"9", "10"});
}

TEST_CASE("BPlusTree scan_to", "[btree]") {
    BPlusTree tree;
    for (int i = 1; i <= 10; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));

    auto r = tree.scan_to("3", true);
    std::vector<std::string> keys;
    for (auto& [k, v] : r) keys.push_back(k);
    REQUIRE(keys == std::vector<std::string>{"1", "2", "3"});

    auto r2 = tree.scan_to("3", false);
    std::vector<std::string> keys2;
    for (auto& [k, v] : r2) keys2.push_back(k);
    REQUIRE(keys2 == std::vector<std::string>{"1", "2"});
}

TEST_CASE("BPlusTree handles a large number of keys with splits and stays correct", "[btree]") {
    BPlusTree tree;
    for (int i = 0; i < 500; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    for (int i = 0; i < 500; i++) {
        auto v = tree.search(std::to_string(i));
        REQUIRE(v.has_value());
        REQUIRE(*v == "v" + std::to_string(i));
    }
    REQUIRE(tree.len() == 500);

    auto kv = tree.collect_all_kv();
    REQUIRE(kv.size() == 500);
    for (std::size_t i = 1; i < kv.size(); i++) {
        REQUIRE(cmp_keys(kv[i - 1].first, kv[i].first) < 0); // sorted order maintained
    }
}

TEST_CASE("BPlusTree remove", "[btree]") {
    BPlusTree tree;
    for (int i = 1; i <= 10; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    tree.remove("5");
    REQUIRE(!tree.search("5").has_value());
    REQUIRE(tree.search("4").has_value());
    REQUIRE(tree.len() == 9);
}

TEST_CASE("BPlusTree range_keys", "[btree]") {
    BPlusTree tree;
    for (int i = 1; i <= 10; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    auto keys = tree.range_keys("4", "6");
    REQUIRE(keys == std::vector<std::string>{"4", "5", "6"});
}

TEST_CASE("BPlusTree deep-copies on copy construction", "[btree]") {
    BPlusTree tree;
    for (int i = 1; i <= 50; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    BPlusTree copy = tree;
    copy.insert("999", "new");
    REQUIRE(copy.search("999").has_value());
    REQUIRE(!tree.search("999").has_value());
}

TEST_CASE("BPlusTree JSON round trip preserves structure", "[btree][json]") {
    BPlusTree tree;
    for (int i = 1; i <= 50; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));

    nlohmann::json j = tree;
    std::string serialized = j.dump();

    BPlusTree restored = nlohmann::json::parse(serialized).get<BPlusTree>();
    for (int i = 1; i <= 50; i++) {
        auto v = restored.search(std::to_string(i));
        REQUIRE(v.has_value());
        REQUIRE(*v == "v" + std::to_string(i));
    }
    REQUIRE(restored.len() == 50);
}

TEST_CASE("empty BPlusTree JSON round trip", "[btree][json]") {
    BPlusTree tree;
    nlohmann::json j = tree;
    REQUIRE(j.at("root").is_null());
    BPlusTree restored = j.get<BPlusTree>();
    REQUIRE(restored.is_empty());
}
