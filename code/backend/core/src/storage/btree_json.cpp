#include "engine/storage/btree_json.hpp"

namespace engine {

void to_json(nlohmann::json& j, const Node& node) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            nlohmann::json obj;
            if constexpr (std::is_same_v<T, InternalNode>) {
                nlohmann::json inner;
                inner["keys"] = alt.keys;
                nlohmann::json children = nlohmann::json::array();
                for (auto& c : alt.children) children.push_back(*c);
                inner["children"] = children;
                obj["Internal"] = inner;
            } else {
                nlohmann::json inner;
                inner["keys"] = alt.keys;
                inner["values"] = alt.values;
                obj["Leaf"] = inner;
            }
            j = obj;
        },
        node.data);
}

void from_json(const nlohmann::json& j, Node& node) {
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid Node JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& payload = it.value();
    if (tag == "Internal") {
        InternalNode n;
        payload.at("keys").get_to(n.keys);
        for (auto& child_json : payload.at("children")) {
            n.children.push_back(std::make_unique<Node>(child_json.get<Node>()));
        }
        node = Node(std::move(n));
    } else if (tag == "Leaf") {
        LeafNode n;
        payload.at("keys").get_to(n.keys);
        payload.at("values").get_to(n.values);
        node = Node(std::move(n));
    } else {
        throw std::runtime_error("unknown Node tag: " + tag);
    }
}

void to_json(nlohmann::json& j, const BPlusTree& tree) {
    nlohmann::json obj;
    if (tree.root_ptr()) obj["root"] = *tree.root_ptr();
    else obj["root"] = nullptr;
    j = obj;
}

void from_json(const nlohmann::json& j, BPlusTree& tree) {
    const auto& r = j.at("root");
    if (r.is_null()) {
        tree.set_root(nullptr);
    } else {
        tree.set_root(std::make_unique<Node>(r.get<Node>()));
    }
}

} // namespace engine
