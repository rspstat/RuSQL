// Faithful port of the DCL (user/grant/role/synonym) management functions from
// rusql-core/src/engine/executor.rs (Phase 8f): exec_create_user/exec_drop_user,
// exec_grant/exec_revoke/exec_show_grants, exec_show_databases, exec_create_role/
// exec_drop_role/exec_grant_role/exec_revoke_role/exec_show_roles, and
// exec_create_synonym/exec_drop_synonym/exec_show_synonyms.

#include "engine/executor/executor.hpp"

#include <algorithm>

namespace engine {

StringResult Executor::exec_create_user(SharedDatabase& s, std::string user, std::string host, std::optional<std::string> password,
                                         bool if_not_exists) {
    bool exists = std::any_of(s.users.begin(), s.users.end(), [&](const UserRecord& u) { return u.user == user && u.host == host; });
    if (exists) {
        if (if_not_exists) return StringResult::Ok("User '" + user + "@" + host + "' already exists (IF NOT EXISTS — skipped).");
        return StringResult::Err("User '" + user + "@" + host + "' already exists.");
    }
    UserRecord rec;
    rec.user = user;
    rec.host = host;
    if (password) {
        rec.password_hash = hash_password(*password);
        rec.mysql_native_hash = mysql_native_hash_compute(*password);
    }
    s.users.push_back(std::move(rec));
    s.disk.save_users(s.users);
    return StringResult::Ok("User '" + user + "@" + host + "' created.");
}

StringResult Executor::exec_drop_user(SharedDatabase& s, std::string user, std::string host, bool if_exists) {
    std::size_t before = s.users.size();
    s.users.erase(std::remove_if(s.users.begin(), s.users.end(), [&](const UserRecord& u) { return u.user == user && u.host == host; }),
                  s.users.end());
    if (s.users.size() == before) {
        if (if_exists) return StringResult::Ok("User '" + user + "@" + host + "' does not exist (IF EXISTS — skipped).");
        return StringResult::Err("User '" + user + "@" + host + "' does not exist.");
    }
    s.grants.erase(std::remove_if(s.grants.begin(), s.grants.end(), [&](const GrantRecord& g) { return g.user == user && g.host == host; }),
                   s.grants.end());
    s.disk.save_users(s.users);
    s.disk.save_grants(s.grants);
    return StringResult::Ok("User '" + user + "@" + host + "' dropped.");
}

StringResult Executor::exec_grant(SharedDatabase& s, std::vector<std::string> privileges, std::string object_type, std::string object,
                                   std::string user, std::string host, bool with_grant_option) {
    auto it = std::find_if(s.grants.begin(), s.grants.end(), [&](const GrantRecord& g) {
        return g.user == user && g.host == host && g.object == object && g.object_type == object_type;
    });
    if (it != s.grants.end()) {
        for (auto& priv_name : privileges) {
            if (std::find(it->privileges.begin(), it->privileges.end(), priv_name) == it->privileges.end()) it->privileges.push_back(priv_name);
        }
        if (with_grant_option) it->with_grant_option = true;
    } else {
        GrantRecord rec;
        rec.user = user;
        rec.host = host;
        rec.object_type = std::move(object_type);
        rec.object = object;
        rec.privileges = privileges;
        rec.with_grant_option = with_grant_option;
        s.grants.push_back(std::move(rec));
    }
    s.disk.save_grants(s.grants);

    std::string priv_str;
    for (std::size_t i = 0; i < privileges.size(); i++) {
        if (i) priv_str += ", ";
        priv_str += privileges[i];
    }
    return StringResult::Ok("Granted " + priv_str + " on " + object + " to '" + user + "@" + host + "'.");
}

StringResult Executor::exec_revoke(SharedDatabase& s, std::vector<std::string> privileges, std::string object_type, std::string object,
                                    std::string user, std::string host) {
    bool changed = false;
    bool revoke_all = std::find(privileges.begin(), privileges.end(), "ALL PRIVILEGES") != privileges.end();
    for (auto& g : s.grants) {
        if (g.user == user && g.host == host && g.object == object && g.object_type == object_type) {
            std::size_t before = g.privileges.size();
            if (revoke_all) {
                g.privileges.clear();
            } else {
                g.privileges.erase(std::remove_if(g.privileges.begin(), g.privileges.end(),
                                                    [&](const std::string& p) {
                                                        return std::find(privileges.begin(), privileges.end(), p) != privileges.end();
                                                    }),
                                    g.privileges.end());
            }
            if (g.privileges.size() != before) changed = true;
        }
    }
    s.grants.erase(std::remove_if(s.grants.begin(), s.grants.end(), [](const GrantRecord& g) { return g.privileges.empty(); }), s.grants.end());
    s.disk.save_grants(s.grants);

    std::string priv_str;
    for (std::size_t i = 0; i < privileges.size(); i++) {
        if (i) priv_str += ", ";
        priv_str += privileges[i];
    }
    if (changed) return StringResult::Ok("Revoked " + priv_str + " on " + object + " from '" + user + "@" + host + "'.");
    return StringResult::Ok("No matching grants found for '" + user + "@" + host + "'.");
}

StringResult Executor::exec_show_grants(const SharedDatabase& s, std::optional<std::string> user, std::optional<std::string> host) const {
    std::string filter_user = user.value_or("");
    std::string filter_host = host.value_or("");
    bool show_all = !user.has_value();

    std::vector<std::string> lines;
    for (auto& g : s.grants) {
        if (show_all || (g.user == filter_user && g.host == filter_host)) {
            std::string priv_str;
            for (std::size_t i = 0; i < g.privileges.size(); i++) {
                if (i) priv_str += ", ";
                priv_str += g.privileges[i];
            }
            std::string grant_opt = g.with_grant_option ? " WITH GRANT OPTION" : "";
            lines.push_back("GRANT " + priv_str + " ON " + g.object + " TO '" + g.user + "'@'" + g.host + "'" + grant_opt + ";");
        }
    }

    if (lines.empty()) return StringResult::Ok("No grants found.");

    std::size_t max_len = 0;
    for (auto& l : lines) max_len = std::max(max_len, l.size());
    std::string sep = "+" + std::string(max_len + 2, '-') + "+";
    std::string header = "| Grants" + std::string(max_len - 6, ' ') + " |";
    std::string out = sep + "\n" + header + "\n" + sep + "\n";
    for (auto& line : lines) out += "| " + line + std::string(max_len - line.size(), ' ') + " |\n";
    out += sep;
    return StringResult::Ok(out);
}

StringResult Executor::exec_show_databases(const SharedDatabase& s) const {
    std::vector<std::string> dbs(s.databases.begin(), s.databases.end());
    std::sort(dbs.begin(), dbs.end());
    if (dbs.empty()) return StringResult::Ok("No databases.");

    std::size_t max_len = 8;
    for (auto& d : dbs) max_len = std::max(max_len, d.size());
    std::string sep = "+" + std::string(max_len + 2, '-') + "+";
    std::string header = "| Database" + std::string(max_len - 8, ' ') + " |";
    std::string out = sep + "\n" + header + "\n" + sep + "\n";
    for (auto& db : dbs) out += "| " + db + std::string(max_len - db.size(), ' ') + " |\n";
    out += sep;
    return StringResult::Ok(out);
}

StringResult Executor::exec_create_role(SharedDatabase& s, std::string name) {
    if (std::any_of(s.roles.begin(), s.roles.end(), [&](const RoleRecord& r) { return r.name == name; })) {
        return StringResult::Err("Role '" + name + "' already exists");
    }
    s.roles.push_back(RoleRecord{std::move(name)});
    s.disk.save_roles(s.roles);
    return StringResult::Ok("Query OK");
}

StringResult Executor::exec_drop_role(SharedDatabase& s, std::string name, bool if_exists) {
    std::size_t before = s.roles.size();
    s.roles.erase(std::remove_if(s.roles.begin(), s.roles.end(), [&](const RoleRecord& r) { return r.name == name; }), s.roles.end());
    if (s.roles.size() == before && !if_exists) return StringResult::Err("Role '" + name + "' does not exist");
    s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == name; }),
                         s.role_grants.end());
    s.disk.save_roles(s.roles);
    s.disk.save_role_grants(s.role_grants);
    return StringResult::Ok("Query OK");
}

StringResult Executor::exec_grant_role(SharedDatabase& s, std::string role, std::string user, std::string host, bool with_admin_option) {
    if (!std::any_of(s.roles.begin(), s.roles.end(), [&](const RoleRecord& r) { return r.name == role; })) {
        return StringResult::Err("Role '" + role + "' does not exist");
    }
    s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(),
                                        [&](const RoleGrant& rg) { return rg.role == role && rg.user == user && rg.host == host; }),
                         s.role_grants.end());
    s.role_grants.push_back(RoleGrant{std::move(role), std::move(user), std::move(host), with_admin_option});
    s.disk.save_role_grants(s.role_grants);
    return StringResult::Ok("Query OK");
}

StringResult Executor::exec_revoke_role(SharedDatabase& s, std::string role, std::string user, std::string host) {
    std::size_t before = s.role_grants.size();
    s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(),
                                        [&](const RoleGrant& rg) { return rg.role == role && rg.user == user && rg.host == host; }),
                         s.role_grants.end());
    if (s.role_grants.size() == before) {
        return StringResult::Err("Role '" + role + "' not granted to '" + user + "'@'" + host + "'");
    }
    s.disk.save_role_grants(s.role_grants);
    return StringResult::Ok("Query OK");
}

StringResult Executor::exec_show_roles(const SharedDatabase& s) const {
    if (s.roles.empty()) return StringResult::Ok("No roles defined.");
    std::size_t max_len = 4;
    for (auto& r : s.roles) max_len = std::max(max_len, r.name.size());
    std::string sep = "+" + std::string(max_len + 2, '-') + "+";
    std::string header = "| Role" + std::string(max_len - 4, ' ') + " |";
    std::string out = sep + "\n" + header + "\n" + sep + "\n";
    for (auto& r : s.roles) out += "| " + r.name + std::string(max_len - r.name.size(), ' ') + " |\n";
    out += sep;
    return StringResult::Ok(out);
}

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
