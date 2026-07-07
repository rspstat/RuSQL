#include "engine/storage/btree.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace engine {

namespace {
constexpr std::size_t ORDER = 16; // 노드당 최대 키 수

// Rust's `s.parse::<f64>()` validates the WHOLE string slice (embedded null bytes
// included, e.g. composite-index keys use '\x00' as a column separator). Using
// strtod()/c_str() here would stop at the first embedded '\0' and misreport a
// composite key like "1\x0050000" as the plain number 1 — use from_chars instead,
// which works on an explicit [first,last) range with no null-termination assumption.
bool try_parse_f64(const std::string& s, double& out) {
    if (s.empty()) return false;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

std::unique_ptr<Node> clone_node(const std::unique_ptr<Node>& p) {
    return p ? std::make_unique<Node>(*p) : nullptr;
}
} // namespace

int cmp_keys(const std::string& a, const std::string& b) {
    double af, bf;
    if (try_parse_f64(a, af) && try_parse_f64(b, bf)) {
        if (af < bf) return -1;
        if (af > bf) return 1;
        return 0;
    }
    return a.compare(b) < 0 ? -1 : (a.compare(b) > 0 ? 1 : 0);
}

Node::Node(const Node& other)
    : data(std::visit(
          [](const auto& alt) -> Data {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, InternalNode>) {
                  InternalNode n;
                  n.keys = alt.keys;
                  n.children.reserve(alt.children.size());
                  for (auto& c : alt.children) n.children.push_back(clone_node(c));
                  return Data(std::move(n));
              } else {
                  return Data(alt);
              }
          },
          other.data)) {}

Node& Node::operator=(const Node& other) {
    if (this != &other) {
        Node tmp(other);
        *this = std::move(tmp);
    }
    return *this;
}

namespace {

// 첫 index로, keys[idx] > key인 첫 위치 (search / internal-child 선택용)
std::size_t first_greater(const std::vector<std::string>& keys, const std::string& key) {
    return static_cast<std::size_t>(
        std::partition_point(keys.begin(), keys.end(), [&](const std::string& k) { return cmp_keys(k, key) <= 0; }) -
        keys.begin());
}

// 첫 index로, keys[idx] >= key인 첫 위치 (leaf 삽입 위치 / internal split-key 삽입 위치용)
std::size_t first_geq(const std::vector<std::string>& keys, const std::string& key) {
    return static_cast<std::size_t>(
        std::partition_point(keys.begin(), keys.end(), [&](const std::string& k) { return cmp_keys(k, key) < 0; }) -
        keys.begin());
}

std::optional<std::string> search_node(const Node& node, const std::string& key) {
    return std::visit(
        [&](const auto& alt) -> std::optional<std::string> {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LeafNode>) {
                for (std::size_t i = 0; i < alt.keys.size(); i++) {
                    if (cmp_keys(alt.keys[i], key) == 0) return alt.values[i];
                }
                return std::nullopt;
            } else {
                std::size_t idx = first_greater(alt.keys, key);
                idx = std::min(idx, alt.children.size() - 1);
                return search_node(*alt.children[idx], key);
            }
        },
        node.data);
}

// Returns (new_node, optional split (mid_key, right_node)).
std::pair<std::unique_ptr<Node>, std::optional<std::pair<std::string, std::unique_ptr<Node>>>> insert_node(
    std::unique_ptr<Node> node, std::string key, std::string value) {
    if (std::holds_alternative<LeafNode>(node->data)) {
        LeafNode leaf = std::move(std::get<LeafNode>(node->data));
        std::size_t pos = first_geq(leaf.keys, key);
        if (pos < leaf.keys.size() && cmp_keys(leaf.keys[pos], key) == 0) {
            leaf.values[pos] = std::move(value);
            return {std::make_unique<Node>(Node(std::move(leaf))), std::nullopt};
        }
        leaf.keys.insert(leaf.keys.begin() + static_cast<std::ptrdiff_t>(pos), key);
        leaf.values.insert(leaf.values.begin() + static_cast<std::ptrdiff_t>(pos), value);

        if (leaf.keys.size() >= ORDER) {
            std::size_t mid = leaf.keys.size() / 2;
            LeafNode right;
            right.keys.assign(leaf.keys.begin() + static_cast<std::ptrdiff_t>(mid), leaf.keys.end());
            right.values.assign(leaf.values.begin() + static_cast<std::ptrdiff_t>(mid), leaf.values.end());
            leaf.keys.resize(mid);
            leaf.values.resize(mid);
            std::string mid_key = right.keys[0];
            auto right_node = std::make_unique<Node>(Node(std::move(right)));
            return {std::make_unique<Node>(Node(std::move(leaf))), std::make_pair(mid_key, std::move(right_node))};
        }
        return {std::make_unique<Node>(Node(std::move(leaf))), std::nullopt};
    }

    InternalNode internal = std::move(std::get<InternalNode>(node->data));
    std::size_t idx = first_greater(internal.keys, key);
    idx = std::min(idx, internal.children.size() - 1);

    std::unique_ptr<Node> child = std::move(internal.children[idx]);
    internal.children.erase(internal.children.begin() + static_cast<std::ptrdiff_t>(idx));
    auto [new_child, split] = insert_node(std::move(child), key, value);
    internal.children.insert(internal.children.begin() + static_cast<std::ptrdiff_t>(idx), std::move(new_child));

    if (split) {
        auto [split_key, right_child] = std::move(*split);
        std::size_t pos = first_geq(internal.keys, split_key);
        internal.keys.insert(internal.keys.begin() + static_cast<std::ptrdiff_t>(pos), split_key);
        internal.children.insert(internal.children.begin() + static_cast<std::ptrdiff_t>(pos + 1), std::move(right_child));

        if (internal.keys.size() >= ORDER) {
            std::size_t mid = internal.keys.size() / 2;
            std::string up_key = internal.keys[mid];
            InternalNode right;
            right.keys.assign(internal.keys.begin() + static_cast<std::ptrdiff_t>(mid + 1), internal.keys.end());
            internal.keys.resize(mid); // drops keys[mid] (the up_key) from the left side
            right.children.reserve(internal.children.size() - (mid + 1));
            for (std::size_t i = mid + 1; i < internal.children.size(); i++) right.children.push_back(std::move(internal.children[i]));
            internal.children.resize(mid + 1);

            auto right_node = std::make_unique<Node>(Node(std::move(right)));
            return {std::make_unique<Node>(Node(std::move(internal))), std::make_pair(up_key, std::move(right_node))};
        }
        return {std::make_unique<Node>(Node(std::move(internal))), std::nullopt};
    }
    return {std::make_unique<Node>(Node(std::move(internal))), std::nullopt};
}

void range_collect(const Node& node, const std::string& start, const std::string& end, std::vector<std::string>& result) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LeafNode>) {
                for (std::size_t i = 0; i < alt.keys.size(); i++) {
                    if (cmp_keys(alt.keys[i], end) > 0) break;
                    if (cmp_keys(alt.keys[i], start) >= 0) result.push_back(alt.values[i]);
                }
            } else {
                for (std::size_t i = 0; i < alt.children.size(); i++) {
                    if (i < alt.keys.size() && cmp_keys(alt.keys[i], start) <= 0) continue;
                    if (i > 0 && cmp_keys(alt.keys[i - 1], end) > 0) break;
                    range_collect(*alt.children[i], start, end, result);
                }
            }
        },
        node.data);
}

void scan_from_node(const Node& node, const std::string& start, bool inclusive,
                     std::vector<std::pair<std::string, std::string>>& result) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LeafNode>) {
                for (std::size_t i = 0; i < alt.keys.size(); i++) {
                    int ord = cmp_keys(alt.keys[i], start);
                    bool include = inclusive ? ord >= 0 : ord > 0;
                    if (include) result.emplace_back(alt.keys[i], alt.values[i]);
                }
            } else {
                for (std::size_t i = 0; i < alt.children.size(); i++) {
                    if (i < alt.keys.size() && cmp_keys(alt.keys[i], start) <= 0) continue;
                    scan_from_node(*alt.children[i], start, inclusive, result);
                }
            }
        },
        node.data);
}

void scan_to_node(const Node& node, const std::string& end, bool inclusive,
                   std::vector<std::pair<std::string, std::string>>& result) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LeafNode>) {
                for (std::size_t i = 0; i < alt.keys.size(); i++) {
                    int ord = cmp_keys(alt.keys[i], end);
                    if (ord > 0) break;
                    if (inclusive || ord < 0) result.emplace_back(alt.keys[i], alt.values[i]);
                }
            } else {
                for (std::size_t i = 0; i < alt.children.size(); i++) {
                    if (i > 0) {
                        int ord = cmp_keys(alt.keys[i - 1], end);
                        bool stop = inclusive ? ord > 0 : ord >= 0;
                        if (stop) break;
                    }
                    scan_to_node(*alt.children[i], end, inclusive, result);
                }
            }
        },
        node.data);
}

void collect_all(const Node& node, std::vector<std::string>& result) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LeafNode>) {
                result.insert(result.end(), alt.values.begin(), alt.values.end());
            } else {
                for (auto& c : alt.children) collect_all(*c, result);
            }
        },
        node.data);
}

void collect_kv_node(const Node& node, std::vector<std::pair<std::string, std::string>>& result) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LeafNode>) {
                for (std::size_t i = 0; i < alt.keys.size(); i++) result.emplace_back(alt.keys[i], alt.values[i]);
            } else {
                for (auto& c : alt.children) collect_kv_node(*c, result);
            }
        },
        node.data);
}

void range_collect_keys(const Node& node, const std::string& start, const std::string& end, std::vector<std::string>& result) {
    std::visit(
        [&](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, LeafNode>) {
                for (auto& k : alt.keys) {
                    if (cmp_keys(k, end) > 0) break;
                    if (cmp_keys(k, start) >= 0) result.push_back(k);
                }
            } else {
                for (std::size_t i = 0; i < alt.children.size(); i++) {
                    if (i < alt.keys.size() && cmp_keys(alt.keys[i], start) <= 0) continue;
                    if (i > 0 && cmp_keys(alt.keys[i - 1], end) > 0) break;
                    range_collect_keys(*alt.children[i], start, end, result);
                }
            }
        },
        node.data);
}

std::unique_ptr<Node> remove_node(std::unique_ptr<Node> node, const std::string& key) {
    if (std::holds_alternative<LeafNode>(node->data)) {
        LeafNode leaf = std::move(std::get<LeafNode>(node->data));
        auto it = std::find_if(leaf.keys.begin(), leaf.keys.end(), [&](const std::string& k) { return cmp_keys(k, key) == 0; });
        if (it != leaf.keys.end()) {
            std::size_t pos = static_cast<std::size_t>(it - leaf.keys.begin());
            leaf.keys.erase(leaf.keys.begin() + static_cast<std::ptrdiff_t>(pos));
            leaf.values.erase(leaf.values.begin() + static_cast<std::ptrdiff_t>(pos));
        }
        if (leaf.keys.empty()) return nullptr;
        return std::make_unique<Node>(Node(std::move(leaf)));
    }

    InternalNode internal = std::move(std::get<InternalNode>(node->data));
    std::size_t idx = first_greater(internal.keys, key);
    idx = std::min(idx, internal.children.size() - 1);

    std::unique_ptr<Node> child = std::move(internal.children[idx]);
    internal.children.erase(internal.children.begin() + static_cast<std::ptrdiff_t>(idx));
    std::unique_ptr<Node> updated = remove_node(std::move(child), key);

    if (updated) {
        internal.children.insert(internal.children.begin() + static_cast<std::ptrdiff_t>(idx), std::move(updated));
        return std::make_unique<Node>(Node(std::move(internal)));
    }

    std::size_t key_idx = idx == 0 ? 0 : idx - 1;
    if (key_idx < internal.keys.size()) internal.keys.erase(internal.keys.begin() + static_cast<std::ptrdiff_t>(key_idx));

    if (internal.children.empty()) return nullptr;
    if (internal.children.size() == 1) return std::move(internal.children[0]);
    return std::make_unique<Node>(Node(std::move(internal)));
}

} // namespace

BPlusTree::BPlusTree(const BPlusTree& other) : root_(clone_node(other.root_)) {}

BPlusTree& BPlusTree::operator=(const BPlusTree& other) {
    if (this != &other) root_ = clone_node(other.root_);
    return *this;
}

std::optional<std::string> BPlusTree::search(const std::string& key) const {
    if (!root_) return std::nullopt;
    return search_node(*root_, key);
}

void BPlusTree::insert(std::string key, std::string value) {
    if (!root_) {
        LeafNode leaf;
        leaf.keys.push_back(key);
        leaf.values.push_back(value);
        root_ = std::make_unique<Node>(Node(std::move(leaf)));
        return;
    }

    auto [new_root, split] = insert_node(std::move(root_), std::move(key), std::move(value));
    if (!split) {
        root_ = std::move(new_root);
    } else {
        auto [mid_key, right_node] = std::move(*split);
        InternalNode top;
        top.keys.push_back(mid_key);
        top.children.push_back(std::move(new_root));
        top.children.push_back(std::move(right_node));
        root_ = std::make_unique<Node>(Node(std::move(top)));
    }
}

void BPlusTree::remove(const std::string& key) {
    if (!root_) return;
    root_ = remove_node(std::move(root_), key);
}

std::vector<std::string> BPlusTree::range_search(const std::string& start, const std::string& end) const {
    std::vector<std::string> result;
    if (root_) range_collect(*root_, start, end, result);
    return result;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::scan_from(const std::string& start, bool inclusive) const {
    std::vector<std::pair<std::string, std::string>> result;
    if (root_) scan_from_node(*root_, start, inclusive, result);
    return result;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::scan_to(const std::string& end, bool inclusive) const {
    std::vector<std::pair<std::string, std::string>> result;
    if (root_) scan_to_node(*root_, end, inclusive, result);
    return result;
}

std::vector<std::string> BPlusTree::all_values() const {
    std::vector<std::string> result;
    if (root_) collect_all(*root_, result);
    return result;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::collect_all_kv() const {
    std::vector<std::pair<std::string, std::string>> result;
    if (root_) collect_kv_node(*root_, result);
    return result;
}

std::vector<std::string> BPlusTree::range_keys(const std::string& start, const std::string& end) const {
    std::vector<std::string> result;
    if (root_) range_collect_keys(*root_, start, end, result);
    return result;
}

} // namespace engine
