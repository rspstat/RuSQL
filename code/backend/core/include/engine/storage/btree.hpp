#pragma once

// Faithful port of rusql-core/src/storage/btree.rs — an in-memory B+Tree (order 16)
// mapping string keys to string values (values are JSON-serialized rows elsewhere).
//
// Node = Internal(InternalNode) | Leaf(LeafNode) becomes the same variant-of-structs
// cookbook pattern as ast.hpp; InternalNode::children (Vec<Box<Node>>) becomes
// std::vector<std::unique_ptr<Node>>. Deep-copy is needed (BPlusTree is
// Clone-derived in Rust and this port preserves that), implemented the same way as
// ast.hpp's recursive types: a custom copy constructor via std::visit + clone_ptr.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace engine {

struct Node;

struct InternalNode {
    std::vector<std::string> keys;
    std::vector<std::unique_ptr<Node>> children;
};

struct LeafNode {
    std::vector<std::string> keys;
    std::vector<std::string> values; // JSON-serialized Row
};

struct Node {
    using Data = std::variant<InternalNode, LeafNode>;
    Data data;

    Node() : data(LeafNode{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, Node>>>
    Node(Alt alt) : data(std::move(alt)) {}

    Node(const Node& other);
    Node& operator=(const Node& other);
    Node(Node&&) noexcept = default;
    Node& operator=(Node&&) noexcept = default;
    ~Node() = default;
};

/// 수치 인식 키 비교: 두 키가 모두 숫자로 파싱되면 수치 비교, 아니면 문자열 비교.
int cmp_keys(const std::string& a, const std::string& b);

// Row-level-concurrency Stage 2: guarded by its own mutex_ so that two threads
// operating on DIFFERENT rows/keys of the same table (and therefore the same index
// instance) can safely call insert/remove/search concurrently -- one mutex per
// instance, held only for the short duration of a single call (not true node-level
// fine-grained locking; a B+Tree's node split/merge/rebalance touches an
// unpredictable number of nodes per operation, so latch-crabbing-style concurrency
// isn't attempted here). Every public method already returns by value, so nothing
// is ever exposed as a reference into mutex_-guarded state.
class BPlusTree {
public:
    BPlusTree() = default;
    BPlusTree(const BPlusTree& other);
    BPlusTree& operator=(const BPlusTree& other);
    BPlusTree(BPlusTree&& other) noexcept;
    BPlusTree& operator=(BPlusTree&& other) noexcept;
    ~BPlusTree() = default;

    std::optional<std::string> search(const std::string& key) const;
    void insert(std::string key, std::string value);
    void remove(const std::string& key);

    std::vector<std::string> range_search(const std::string& start, const std::string& end) const;
    std::vector<std::pair<std::string, std::string>> scan_from(const std::string& start, bool inclusive) const;
    std::vector<std::pair<std::string, std::string>> scan_to(const std::string& end, bool inclusive) const;
    std::vector<std::string> all_values() const;
    std::vector<std::pair<std::string, std::string>> collect_all_kv() const;
    std::vector<std::string> range_keys(const std::string& start, const std::string& end) const;

    std::size_t len() const { return all_values().size(); } // delegates -- already locked
    bool is_empty() const {
        std::lock_guard<std::mutex> g(mutex_);
        return root_ == nullptr;
    }

    // Internal accessors used only by btree_json.cpp for (de)serialization, which only
    // ever runs under the outer structural-exclusive lock (CREATE INDEX / startup load /
    // ALTER) -- deliberately NOT guarded by mutex_ (root_ptr() returns a reference, which
    // a lock held only inside this accessor couldn't protect after it returns anyway).
    // Not meant as part of the tree's ordinary per-instance-concurrent public API.
    const std::unique_ptr<Node>& root_ptr() const { return root_; }
    void set_root(std::unique_ptr<Node> r) { root_ = std::move(r); }

private:
    mutable std::mutex mutex_;
    std::unique_ptr<Node> root_;
};

} // namespace engine
