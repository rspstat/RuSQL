#pragma once

#include <string>
#include <unordered_map>

namespace engine {

// Mirrors Rust's `pub type Row = HashMap<String, String>` used throughout rusql-core.
//
// NOTE: std::unordered_map's iteration order is unspecified, just like Rust's HashMap.
// This is intentionally preserved rather than switched to an ordered container: the
// migration plan's "faithful port, fix bugs later" philosophy carries this
// nondeterminism over as-is (see plan doc: the original project's own PK-corruption-
// via-VACUUM bug is rooted in exactly this HashMap nondeterminism upstream). Silently
// "fixing" it here by switching to std::map would mask whether a future behavior
// mismatch is a pre-existing bug or a new porting bug.
using Row = std::unordered_map<std::string, std::string>;

} // namespace engine
