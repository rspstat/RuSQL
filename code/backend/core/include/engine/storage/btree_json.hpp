#pragma once

// JSON (de)serialization for BPlusTree — needed by DiskManager::save_btree_index /
// load_btree_index (persisted as JSON, not the binary .rdb format).

#include "engine/storage/btree.hpp"
#include "engine/json_support.hpp"

namespace engine {

void to_json(nlohmann::json& j, const Node& node);
void from_json(const nlohmann::json& j, Node& node);

void to_json(nlohmann::json& j, const BPlusTree& tree);
void from_json(const nlohmann::json& j, BPlusTree& tree);

} // namespace engine
