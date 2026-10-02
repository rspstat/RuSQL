#include <algorithm>
#include <random>
#include <thread>
#include <vector>

#include <charconv>
#include <string>
#include "catch.hpp"
#include "engine/storage/btree.hpp"

using namespace engine;

namespace {
// Mirrors btree.cpp's private MIN_KEYS constant (ORDER=16 => ORDER/2 - 1 = 7). Not part
// of BPlusTree's public API, so duplicated here for the invariant checks below -- if
// ORDER ever changes, this must be updated to match.
constexpr std::size_t TEST_MIN_KEYS = 7;

// Recursively verifies every non-root node has at least TEST_MIN_KEYS keys (the B-tree
// minimum-key invariant a correct delete-side rebalance must maintain -- this is exactly
// what PLAN.md's "no underflow rebalancing" gap meant nothing enforced), and returns the
// total number of leaf keys found (cross-checked against BPlusTree::len() by callers).
std::size_t check_min_keys_invariant(const Node& node, bool is_root) {
    return std::visit(
        [&](const auto& alt) -> std::size_t {
            using T = std::decay_t<decltype(alt)>;
            if (!is_root) REQUIRE(alt.keys.size() >= TEST_MIN_KEYS);
            if constexpr (std::is_same_v<T, LeafNode>) {
                return alt.keys.size();
            } else {
                std::size_t total = 0;
                for (auto& c : alt.children) total += check_min_keys_invariant(*c, false);
                return total;
            }
        },
        node.data);
}

// Checks the invariant across the whole tree (no-op on an empty tree) and cross-checks
// the leaf-key total against len().
void check_tree_balanced(const BPlusTree& tree) {
    if (tree.is_empty()) return;
    std::size_t counted = check_min_keys_invariant(*tree.root_ptr(), /*is_root=*/true);
    REQUIRE(counted == tree.len());
}
} // namespace

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

// Regression tests for PLAN.md's "B+Tree 삭제 시 언더플로우 리밸런싱 없음" (no delete-side
// rebalancing -- a node could shrink to a single key and just stay that way forever,
// with no borrow/merge, letting the tree grow arbitrarily sparse under a heavy-delete
// workload). All of these use enough keys (well beyond ORDER=16 squared) to force
// multi-level internal-node splits, so the deletes below exercise rebalancing at both
// leaf and internal levels, not just a single-node tree.
constexpr int kBigN = 2000;

TEST_CASE("BPlusTree rebalances after deleting a contiguous prefix", "[btree][rebalance]") {
    BPlusTree tree;
    for (int i = 0; i < kBigN; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    for (int i = 0; i < kBigN / 2; i++) tree.remove(std::to_string(i)); // delete the leftmost half

    check_tree_balanced(tree);
    REQUIRE(tree.len() == static_cast<std::size_t>(kBigN / 2));
    for (int i = 0; i < kBigN / 2; i++) REQUIRE(!tree.search(std::to_string(i)).has_value());
    for (int i = kBigN / 2; i < kBigN; i++) {
        auto v = tree.search(std::to_string(i));
        REQUIRE(v.has_value());
        REQUIRE(*v == "v" + std::to_string(i));
    }
}

TEST_CASE("BPlusTree rebalances after deleting a contiguous suffix", "[btree][rebalance]") {
    BPlusTree tree;
    for (int i = 0; i < kBigN; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    for (int i = kBigN / 2; i < kBigN; i++) tree.remove(std::to_string(i)); // delete the rightmost half

    check_tree_balanced(tree);
    REQUIRE(tree.len() == static_cast<std::size_t>(kBigN / 2));
    for (int i = kBigN / 2; i < kBigN; i++) REQUIRE(!tree.search(std::to_string(i)).has_value());
    for (int i = 0; i < kBigN / 2; i++) {
        auto v = tree.search(std::to_string(i));
        REQUIRE(v.has_value());
        REQUIRE(*v == "v" + std::to_string(i));
    }
}

TEST_CASE("BPlusTree rebalances after deleting a contiguous middle chunk", "[btree][rebalance]") {
    BPlusTree tree;
    for (int i = 0; i < kBigN; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    int lo = kBigN / 3, hi = 2 * kBigN / 3;
    for (int i = lo; i < hi; i++) tree.remove(std::to_string(i));

    check_tree_balanced(tree);
    REQUIRE(tree.len() == static_cast<std::size_t>(kBigN - (hi - lo)));
    for (int i = lo; i < hi; i++) REQUIRE(!tree.search(std::to_string(i)).has_value());
    for (int i = 0; i < lo; i++) REQUIRE(tree.search(std::to_string(i)).has_value());
    for (int i = hi; i < kBigN; i++) REQUIRE(tree.search(std::to_string(i)).has_value());
}

TEST_CASE("BPlusTree rebalances after scattered interleaved deletes", "[btree][rebalance]") {
    BPlusTree tree;
    for (int i = 0; i < kBigN; i++) tree.insert(std::to_string(i), "v" + std::to_string(i));
    for (int i = 0; i < kBigN; i += 3) tree.remove(std::to_string(i)); // delete every 3rd key

    check_tree_balanced(tree);
    std::size_t expected_removed = (kBigN + 2) / 3;
    REQUIRE(tree.len() == static_cast<std::size_t>(kBigN) - expected_removed);
    for (int i = 0; i < kBigN; i++) {
        bool was_removed = (i % 3 == 0);
        REQUIRE(tree.search(std::to_string(i)).has_value() == !was_removed);
    }
}

TEST_CASE("BPlusTree rebalances after random-order deletes down to a handful of keys", "[btree][rebalance]") {
    BPlusTree tree;
    std::vector<int> keys(kBigN);
    for (int i = 0; i < kBigN; i++) keys[static_cast<std::size_t>(i)] = i;
    for (int i : keys) tree.insert(std::to_string(i), "v" + std::to_string(i));

    std::mt19937 rng(42); // fixed seed for reproducibility
    std::shuffle(keys.begin(), keys.end(), rng);

    // Remove all but the last 5 keys (in shuffled order), checking the invariant
    // periodically -- not just once at the very end -- so a rebalance bug that only
    // shows up transiently (e.g. right after a merge cascades up several levels) isn't
    // masked by later operations happening to "heal" the structure.
    for (std::size_t removed = 0; removed + 5 < keys.size(); removed++) {
        tree.remove(std::to_string(keys[removed]));
        if (removed % 97 == 0) check_tree_balanced(tree);
    }
    check_tree_balanced(tree);
    REQUIRE(tree.len() == 5);
    for (std::size_t i = keys.size() - 5; i < keys.size(); i++) {
        REQUIRE(tree.search(std::to_string(keys[i])).has_value());
    }
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

// Row-level-concurrency Stage 2: BPlusTree gained its own internal mutex_ (one per
// instance, short critical sections) so genuinely concurrent threads can share a single
// tree instance -- exactly the scenario a table's PK/secondary/composite index sees once
// two sessions insert/update/delete different rows of the same table at the same time.
// Each thread here owns a disjoint key range, so the ONLY way this test can fail is a
// real data race (a lost/corrupted insert, a crash, or a torn read) -- there's no
// legitimate reason for any key to end up missing or wrong.
TEST_CASE("BPlusTree is safe under real concurrent insert/remove/search on disjoint keys", "[btree][concurrency]") {
    BPlusTree tree;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&tree, t] {
            int base = t * kPerThread;
            for (int i = 0; i < kPerThread; i++) {
                std::string key = std::to_string(base + i);
                tree.insert(key, "v" + key);
            }
            // Remove every other key this thread inserted, then confirm the tree agrees
            // with what should still be there -- all within the same thread's own
            // disjoint range, so any wrong answer is necessarily a race with another
            // thread's concurrent operation on ITS range corrupting shared tree state.
            for (int i = 0; i < kPerThread; i += 2) {
                tree.remove(std::to_string(base + i));
            }
            for (int i = 0; i < kPerThread; i++) {
                std::string key = std::to_string(base + i);
                auto v = tree.search(key);
                if (i % 2 == 0) {
                    if (v.has_value()) throw std::runtime_error("key " + key + " should have been removed");
                } else {
                    if (!v.has_value() || *v != "v" + key) throw std::runtime_error("key " + key + " missing or wrong");
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    REQUIRE(tree.len() == static_cast<std::size_t>(kThreads * kPerThread / 2));
    for (int t = 0; t < kThreads; t++) {
        for (int i = 1; i < kPerThread; i += 2) {
            std::string key = std::to_string(t * kPerThread + i);
            auto v = tree.search(key);
            REQUIRE(v.has_value());
            REQUIRE(*v == "v" + key);
        }
    }
}

// cmp_keys is the comparison under every index operation, so it was rewritten without per-call allocations and with a
// parse-free path for plain integers. The ORDER it defines must not change by a single comparison: this keeps a copy of
// the previous implementation and compares the two on random keys -- numbers in many spellings, composite (NUL-joined)
// keys, text, empties, signs, exponents, leading zeros, very long digit strings.
namespace {
bool ref_parse_f64(const std::string& s, double& out) {
    if (s.empty()) return false;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}
int ref_segment(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    double af, bf;
    if (ref_parse_f64(a, af) && ref_parse_f64(b, bf)) {
        if (af < bf) return -1;
        if (af > bf) return 1;
        return a.compare(b) < 0 ? -1 : (a.compare(b) > 0 ? 1 : 0);
    }
    return a.compare(b) < 0 ? -1 : (a.compare(b) > 0 ? 1 : 0);
}
int ref_cmp_keys(const std::string& a, const std::string& b) {
    std::size_t pa = 0, pb = 0;
    for (;;) {
        std::size_t da = a.find('\x00', pa);
        std::size_t db = b.find('\x00', pb);
        std::string sa = a.substr(pa, da == std::string::npos ? std::string::npos : da - pa);
        std::string sb = b.substr(pb, db == std::string::npos ? std::string::npos : db - pb);
        int c = ref_segment(sa, sb);
        if (c != 0) return c;
        if (da == std::string::npos && db == std::string::npos) return 0;
        if (da == std::string::npos) return -1;
        if (db == std::string::npos) return 1;
        pa = da + 1;
        pb = db + 1;
    }
}
int sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

std::string random_segment(std::mt19937& rng) {
    static const char* fixed[] = {"", "0", "00", "-0", "0.0", "7", "07", "7.0", "7.00", "+7", "-7", "1e3", "1E3", "1000", "0x10", "inf", "nan", "abc",
                                  "ab", "a", "Z", " 7", "7 ", "12a", "9223372036854775807", "9223372036854775808", "123456789012345", "1234567890123456",
                                  "999999999999999", "100000000000000", "0.5", ".5", "5.", "-.5", "한글", "x\xff"};
    switch (rng() % 5) {
        case 0: return fixed[rng() % (sizeof fixed / sizeof *fixed)];
        case 1: return std::to_string(rng() % 2000);                    // canonical integers
        case 2: return std::to_string(rng() % 50) + "." + std::to_string(rng() % 100);
        case 3: {                                                       // digit strings of random length
            std::string t;
            for (std::size_t n = rng() % 18, i = 0; i < n; i++) t += static_cast<char>('0' + rng() % 10);
            return t;
        }
        default: return std::string(1, static_cast<char>('a' + rng() % 6)) + std::to_string(rng() % 12);
    }
}
} // namespace

TEST_CASE("cmp_keys orders keys exactly as the previous implementation did", "[btree][cmp_keys]") {
    std::mt19937 rng(12345);
    for (int i = 0; i < 400000; i++) {
        std::string a = random_segment(rng), b = random_segment(rng);
        if (rng() % 4 == 0) { // composite keys: 2-3 segments joined with NUL
            for (std::size_t n = 1 + rng() % 2, k = 0; k < n; k++) {
                a += '\x00' + random_segment(rng);
                b += '\x00' + random_segment(rng);
            }
            if (rng() % 3 == 0) b = a.substr(0, rng() % (a.size() + 1)); // prefix of the other
        }
        INFO("a=[" << a << "] b=[" << b << "]");
        REQUIRE(sign(cmp_keys(a, b)) == sign(ref_cmp_keys(a, b)));
        REQUIRE(sign(cmp_keys(a, a)) == 0);
    }
}
