#include "engine/storage/composite_index.hpp"

#include <nlohmann/json.hpp>

namespace engine {

std::string CompositeIndex::make_key(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); i++) {
        if (i) out.push_back('\x00');
        out += values[i];
    }
    return out;
}

std::optional<std::string> CompositeIndex::key_from_row(const Row& row) const {
    std::vector<std::string> parts;
    parts.reserve(columns.size());
    for (auto& col : columns) {
        auto it = row.find(col);
        if (it == row.end()) return std::nullopt;
        parts.push_back(it->second);
    }
    return make_key(parts);
}

void CompositeIndex::insert_row(const Row& row) {
    auto key = key_from_row(row);
    if (key) {
        nlohmann::json j = row;
        tree_.insert(*key, j.dump());
    }
}

void CompositeIndex::remove_row(const Row& row) {
    auto key = key_from_row(row);
    if (key) tree_.remove(*key);
}

std::optional<std::string> CompositeIndex::search_exact(const std::vector<std::string>& values) const {
    return tree_.search(make_key(values));
}

bool CompositeIndex::matches_conditions(const std::unordered_map<std::string, std::string>& eq_map) const {
    for (auto& col : columns) {
        if (eq_map.find(col) == eq_map.end()) return false;
    }
    return true;
}

std::optional<std::string> CompositeIndex::search_from_eq_map(const std::unordered_map<std::string, std::string>& eq_map) const {
    std::vector<std::string> values;
    values.reserve(columns.size());
    for (auto& col : columns) {
        auto it = eq_map.find(col);
        values.push_back(it != eq_map.end() ? it->second : "");
    }
    return search_exact(values);
}

std::optional<std::string> CompositeIndex::prefix_key_from_eq_map(
    const std::unordered_map<std::string, std::string>& eq_map) const {
    std::vector<std::string> parts;
    for (auto& col : columns) {
        auto it = eq_map.find(col);
        if (it == eq_map.end()) break;
        parts.push_back(it->second);
    }
    if (parts.empty() || parts.size() == columns.size()) return std::nullopt;
    return make_key(parts) + "\x00";
}

std::vector<std::string> CompositeIndex::prefix_scan(const std::string& prefix) const {
    std::vector<std::string> result;
    for (auto& [k, v] : tree_.scan_from(prefix, true)) {
        if (k.rfind(prefix, 0) == 0) result.push_back(v);
    }
    return result;
}

void CompositeIndex::rebuild(const std::vector<Row>& rows) {
    tree_ = BPlusTree();
    for (auto& row : rows) insert_row(row);
}

} // namespace engine
