// Synonyms (Phase 8f). The users, grants and roles that used to be here are in executor_privileges.cpp since R78.

#include "engine/executor/executor.hpp"

#include <algorithm>

namespace engine {

StringResult Executor::exec_create_synonym(SharedDatabase& s, std::string name, std::string target, bool or_replace) {
    if (s.synonyms.count(name) && !or_replace) return StringResult::Err("Synonym '" + name + "' already exists");
    s.synonyms[name] = std::move(target);
    s.disk.save_synonyms(s.synonyms);
    return StringResult::Ok("Query OK");
}

StringResult Executor::exec_drop_synonym(SharedDatabase& s, std::string name, bool if_exists) {
    if (s.synonyms.erase(name) == 0 && !if_exists) return StringResult::Err("Synonym '" + name + "' does not exist");
    s.disk.save_synonyms(s.synonyms);
    return StringResult::Ok("Query OK");
}

StringResult Executor::exec_show_synonyms(const SharedDatabase& s) const {
    if (s.synonyms.empty()) return StringResult::Ok("No synonyms defined.");
    std::size_t max_name = 7, max_target = 6;
    for (auto& [k, v] : s.synonyms) {
        max_name = std::max(max_name, k.size());
        max_target = std::max(max_target, v.size());
    }
    std::string sep = "+" + std::string(max_name + 2, '-') + "+" + std::string(max_target + 2, '-') + "+";
    std::string header = "| Synonym" + std::string(max_name - 7, ' ') + " | Target" + std::string(max_target - 6, ' ') + " |";
    std::string out = sep + "\n" + header + "\n" + sep + "\n";
    for (auto& [k, v] : s.synonyms) {
        out += "| " + k + std::string(max_name - k.size(), ' ') + " | " + v + std::string(max_target - v.size(), ' ') + " |\n";
    }
    out += sep;
    return StringResult::Ok(out);
}

} // namespace engine
