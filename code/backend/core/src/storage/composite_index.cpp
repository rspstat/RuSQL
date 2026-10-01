#include "engine/storage/composite_index.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "engine/storage/numeric_key.hpp"

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
    parts.reserve(columns.size() + std::max<std::size_t>(pk_columns.size(), 1));
    for (auto& col : columns) {
        auto it = row.find(col);
        if (it == row.end()) return std::nullopt;
        parts.push_back(normalize_numeric_key(it->second));
    }
    if (!pk_columns.empty()) {
        for (auto& col : pk_columns) {
            auto it = row.find(col);
            parts.push_back(it != row.end() ? it->second : std::string());
        }
    } else {
        std::vector<const std::pair<const std::string, std::string>*> all;
        for (auto& kv : row) {
            if (kv.first != "_xmin" && kv.first != "_xmax") all.push_back(&kv);
        }
        std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->first < b->first; });
        for (auto* kv : all) parts.push_back(kv->second);
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

std::vector<std::string> CompositeIndex::rows_with_prefix(const std::vector<std::string>& values) const {
    if (values.empty() || values.size() > columns.size()) return {};
    std::vector<std::string> norm;
    norm.reserve(values.size());
    for (auto& v : values) norm.push_back(normalize_numeric_key(v));
    // Entries with these leading columns sort between "v1\0..\0vk\0" and the same followed by a segment that is
    // greater than any real one (0xFF never occurs in UTF-8).
    std::string lo = make_key(norm) + '\x00';
    std::string hi = lo + '\xff';
    return tree_.range_search(lo, hi);
}

std::optional<std::string> CompositeIndex::search_exact(const std::vector<std::string>& values) const {
    auto rows = rows_with_prefix(values);
    if (rows.empty()) return std::nullopt;
    std::string out = "[";
    for (std::size_t i = 0; i < rows.size(); i++) {
        if (i) out += ',';
        out += rows[i];
    }
    return out + "]";
}

bool CompositeIndex::matches_conditions(const std::unordered_map<std::string, std::string>& eq_map) const {
    for (auto& col : columns) {
        if (eq_map.find(col) == eq_map.end()) return false;
    }
    return true;
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

std::optional<std::vector<Row>> CompositeIndex::lookup(const std::unordered_map<std::string, std::string>& eq_map) const {
    std::vector<std::string> values;
    for (auto& col : columns) {
        auto it = eq_map.find(col);
        if (it == eq_map.end()) break;
        values.push_back(it->second);
    }
    if (values.empty()) return std::nullopt;
    std::vector<Row> out;
    for (auto& j : rows_with_prefix(values)) out.push_back(nlohmann::json::parse(j).get<Row>());
    return out;
}

void CompositeIndex::rebuild(const std::vector<Row>& rows) {
    tree_ = BPlusTree();
    for (auto& row : rows) insert_row(row);
}

} // namespace engine
