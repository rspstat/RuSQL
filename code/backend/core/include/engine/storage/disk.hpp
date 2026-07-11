#pragma once

// Faithful port of rusql-core/src/storage/disk.rs.
//
// save_users/load_users/save_grants/.../save_procedures/save_triggers/save_functions
// are templates here for the same reason they're generic (`fn load_users<T:
// Deserialize>`) in the Rust original: DiskManager (storage layer) must stay
// independent of UserRecord/GrantRecord/etc., which are defined alongside
// SharedDatabase in the executor (Phase 8). The template is only instantiated once
// the executor includes this header with those concrete types already visible.

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/storage/btree.hpp"
#include "engine/storage/btree_json.hpp"
#include "engine/json_support.hpp"
#include "engine/row.hpp"
#include "engine/catalog/schema.hpp"

namespace engine {

struct IndexMeta {
    std::string name;
    std::string table;
    std::vector<std::string> columns;
    std::string index_type = "btree"; // "btree" | "hash"
};

void to_json(nlohmann::json& j, const IndexMeta& m);
void from_json(const nlohmann::json& j, IndexMeta& m);

class DiskManager {
public:
    DiskManager() : DiskManager("data") {}
    explicit DiskManager(const std::string& dir);

    const std::string& data_dir() const { return data_dir_; }

    void create_db_dir(const std::string& db) const;
    void drop_db_dir(const std::string& db) const;
    std::vector<std::string> list_databases() const;

    void save_schema(const std::string& table, const TableSchema& schema) const;
    std::optional<TableSchema> load_schema(const std::string& table) const;
    void save_schema_columns(const std::string& table, const std::vector<std::string>& columns) const;

    void save_table(const std::string& table, const std::vector<Row>& rows) const;
    std::vector<Row> load_table(const std::string& table) const;
    void delete_table(const std::string& table) const;
    std::vector<std::string> list_tables() const;

    void save_btree_index(const std::string& key, const BPlusTree& tree) const;
    std::optional<BPlusTree> load_btree_index(const std::string& key) const;
    void delete_btree_index(const std::string& key) const;

    void save_index_meta(const std::string& db, const std::vector<IndexMeta>& meta_list) const;
    std::vector<IndexMeta> load_index_meta(const std::string& db) const;

    // ── Views (per-db) ───────────────────────────────────────────────────
    void save_views(const std::string& db, const std::unordered_map<std::string, Statement>& views) const;
    std::unordered_map<std::string, Statement> load_views(const std::string& db) const;
    void save_view_raw_sql(const std::string& db, const std::unordered_map<std::string, std::string>& view_sql) const;
    std::unordered_map<std::string, std::string> load_view_raw_sql(const std::string& db) const;

    // ── Global (_system/) records: generic over T, exactly like the Rust
    // original's `fn save_users<T: Serialize>` — see file header comment.
    template <typename T>
    void save_users(const T& users) const { save_sys_json("_users.json", users); }
    template <typename T>
    T load_users() const { return load_sys_json<T>("_users.json"); }

    template <typename T>
    void save_grants(const T& grants) const { save_sys_json("_grants.json", grants); }
    template <typename T>
    T load_grants() const { return load_sys_json<T>("_grants.json"); }

    template <typename T>
    void save_roles(const T& roles) const { save_sys_json("_roles.json", roles); }
    template <typename T>
    T load_roles() const { return load_sys_json<T>("_roles.json"); }

    template <typename T>
    void save_role_grants(const T& rg) const { save_sys_json("_role_grants.json", rg); }
    template <typename T>
    T load_role_grants() const { return load_sys_json<T>("_role_grants.json"); }

    template <typename T>
    void save_synonyms(const T& synonyms) const { save_sys_json("_synonyms.json", synonyms); }
    template <typename T>
    T load_synonyms() const { return load_sys_json<T>("_synonyms.json"); }

    template <typename T>
    void save_procedures(const T& procs) const { save_sys_json("_procedures.json", procs); }
    template <typename T>
    T load_procedures() const { return load_sys_json<T>("_procedures.json"); }

    template <typename T>
    void save_triggers(const T& triggers) const { save_sys_json("_triggers.json", triggers); }
    template <typename T>
    T load_triggers() const { return load_sys_json<T>("_triggers.json"); }

    template <typename T>
    void save_functions(const T& funcs) const { save_sys_json("_functions.json", funcs); }
    template <typename T>
    T load_functions() const { return load_sys_json<T>("_functions.json"); }

private:
    std::string data_dir_;

    std::string sys_dir() const { return data_dir_ + "/_system"; }
    std::string table_dir(const std::string& db) const { return data_dir_ + "/" + db; }
    void ensure_db_dir(const std::string& db) const;
    static std::pair<std::string, std::string> parse_key(const std::string& key);
    std::vector<Row> load_rdb(const std::string& path) const;

    bool read_text_file(const std::string& path, std::string& out) const;
    void write_text_file(const std::string& path, const std::string& content) const;
    // Migrates a legacy root-level `data/{filename}` file into `data/_system/{filename}`
    // the first time it's touched, mirroring Rust's load_sys_json migration step.
    void migrate_sys_json_path(const std::string& new_path, const std::string& old_path) const;

    template <typename T>
    void save_sys_json(const std::string& filename, const T& value) const {
        nlohmann::json j = value;
        write_text_file(sys_dir() + "/" + filename, j.dump(2));
    }

    template <typename T>
    T load_sys_json(const std::string& filename) const {
        std::string new_path = sys_dir() + "/" + filename;
        std::string old_path = data_dir_ + "/" + filename;
        migrate_sys_json_path(new_path, old_path);
        std::string content;
        if (!read_text_file(new_path, content)) return T{};
        try {
            return nlohmann::json::parse(content).get<T>();
        } catch (...) {
            return T{};
        }
    }
};

} // namespace engine
