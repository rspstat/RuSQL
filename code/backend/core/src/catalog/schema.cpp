#include "engine/catalog/schema.hpp"

namespace engine {

void to_json(nlohmann::json& j, const CheckConstraint& c) {
    j = nlohmann::json{{"name", c.name}, {"expression", c.expression}};
}

void from_json(const nlohmann::json& j, CheckConstraint& c) {
    j.at("name").get_to(c.name);
    j.at("expression").get_to(c.expression);
}

void to_json(nlohmann::json& j, const TableSchema& s) {
    j = nlohmann::json{
        {"name", s.name},
        {"columns", s.columns},
        {"auto_increment_counters", s.auto_increment_counters},
        {"primary_key_columns", s.primary_key_columns},
        {"check_constraints", s.check_constraints},
        {"partition_info", s.partition_info},
    };
}

void from_json(const nlohmann::json& j, TableSchema& s) {
    j.at("name").get_to(s.name);
    j.at("columns").get_to(s.columns);
    j.at("auto_increment_counters").get_to(s.auto_increment_counters);
    // #[serde(default)]-style fields — tolerate absence for backward compatibility with
    // schema files saved before these fields existed.
    if (j.contains("primary_key_columns")) j.at("primary_key_columns").get_to(s.primary_key_columns);
    else s.primary_key_columns.clear();
    if (j.contains("check_constraints")) j.at("check_constraints").get_to(s.check_constraints);
    else s.check_constraints.clear();
    if (j.contains("partition_info")) j.at("partition_info").get_to(s.partition_info);
    else s.partition_info = std::nullopt;
}

Result<void, std::string> Catalog::create_table(std::string name, std::vector<ColumnDef> columns) {
    return create_table_full(std::move(name), std::move(columns), {}, {});
}

Result<void, std::string> Catalog::create_table_full(std::string name,
                                                       std::vector<ColumnDef> columns,
                                                       std::vector<std::string> primary_key_columns,
                                                       std::vector<CheckConstraint> check_constraints) {
    if (tables.find(name) != tables.end()) {
        return Result<void, std::string>::Err("Table '" + name + "' already exists");
    }
    TableSchema schema;
    schema.name = name;
    schema.columns = std::move(columns);
    schema.primary_key_columns = std::move(primary_key_columns);
    schema.check_constraints = std::move(check_constraints);
    tables.emplace(std::move(name), std::move(schema));
    return Result<void, std::string>::Ok();
}

Result<void, std::string> Catalog::drop_table(const std::string& name) {
    auto it = tables.find(name);
    if (it == tables.end()) {
        return Result<void, std::string>::Err("Table '" + name + "' not found");
    }
    tables.erase(it);
    return Result<void, std::string>::Ok();
}

const TableSchema* Catalog::get_table(const std::string& name) const {
    auto it = tables.find(name);
    return it == tables.end() ? nullptr : &it->second;
}

TableSchema* Catalog::get_table_mut(const std::string& name) {
    auto it = tables.find(name);
    return it == tables.end() ? nullptr : &it->second;
}

} // namespace engine
