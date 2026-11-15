#pragma once

// What the engine keeps of a procedure, function or trigger besides its parsed body (R83, R84): the database it was made in, the text it was written as (SHOW CREATE
// PROCEDURE / FUNCTION / TRIGGER and information_schema give it back), the account that made it and when. Kept in _system/_routine_sources.json.

#include <nlohmann/json.hpp>
#include <string>

namespace engine {

struct RoutineSource {
    std::string database;
    std::string text;
    std::string definer; // `user`@`host`, as SHOW CREATE writes it; empty for one made before R84 (root@localhost is told)
    std::string created; // "YYYY-MM-DD HH:MM:SS" (a trigger: "...SS.ff"); empty for one made before R84 (NULL is told)
    std::string name;    // a function: its name as it was written (the engine looks functions up in lower case); empty for any other and for one made before R84
};

inline void to_json(nlohmann::json& j, const RoutineSource& r) { j = nlohmann::json{{"db", r.database}, {"text", r.text}, {"definer", r.definer}, {"created", r.created}, {"name", r.name}}; }

inline void from_json(const nlohmann::json& j, RoutineSource& r) {
    if (j.is_array()) { // (the file R83 wrote: [database, text])
        r.database = j.at(0).get<std::string>();
        r.text = j.at(1).get<std::string>();
        return;
    }
    r.database = j.value("db", std::string());
    r.text = j.value("text", std::string());
    r.definer = j.value("definer", std::string());
    r.created = j.value("created", std::string());
    r.name = j.value("name", std::string());
}

// `root`@`localhost` -> root@localhost (what information_schema's DEFINER columns say)
inline std::string plain_account(const std::string& quoted) {
    std::string out;
    for (char c : quoted) if (c != '`') out.push_back(c);
    return out;
}

} // namespace engine
