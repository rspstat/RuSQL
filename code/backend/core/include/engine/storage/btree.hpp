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

#include "engine/value_class.hpp"

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

// How the keys of one column of an index are ordered. A column of numbers is ordered by value (integers exactly), a column of text by its
// bytes -- so '10' < '9' and "007" and "7" are different keys, as a comparison on the column says. Mixed is a tree whose column type is
// not known: numbers by value, before every other key, which are ordered by their bytes.
enum class KeyKind : std::uint8_t { Mixed = 0, Number, Text };

// The ordering a column of this class gets in an index.
inline KeyKind key_kind_of(ValueClass cls) { return cls == ValueClass::Number ? KeyKind::Number : (cls == ValueClass::Text ? KeyKind::Text : KeyKind::Mixed); }

/// 키 비교: 현재 트리의 KeyKind(복합 키는 NUL로 나뉜 세그먼트마다)에 따라 숫자/문자열 순서 (BPlusTree의 공개 메서드가 설정).
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
    // `kinds`: how each column of the key is ordered (one for a plain key, one per column of a composite key); empty = Mixed
    explicit BPlusTree(std::vector<KeyKind> kinds) : kinds_(std::move(kinds)) {}
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

    // Read-only access to the root for the structure-invariant check in test_btree.cpp -- deliberately NOT
    // guarded by mutex_ (it returns a reference, which a lock held only inside this accessor couldn't protect
    // after it returns anyway), so not part of the tree's ordinary per-instance-concurrent API.
    const std::unique_ptr<Node>& root_ptr() const { return root_; }

    // How the (first column of the) key is ordered; fixed when the tree is made.
    const std::vector<KeyKind>& kinds() const { return kinds_; }
    bool text_keyed() const { return !kinds_.empty() && kinds_.front() == KeyKind::Text; }

private:
    mutable std::mutex mutex_;
    std::unique_ptr<Node> root_;
    std::vector<KeyKind> kinds_;
};

} // namespace engine
