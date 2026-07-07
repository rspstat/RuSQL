#include "catch.hpp"
#include "engine/catalog/schema.hpp"

using namespace engine;

TEST_CASE("Catalog create/get/drop table", "[schema]") {
    Catalog cat;

    ColumnDef id_col;
    id_col.name = "id";
    id_col.data_type = DataType::Int{};
    id_col.primary_key = true;
    id_col.auto_increment = true;

    ColumnDef name_col;
    name_col.name = "name";
    name_col.data_type = DataType::Varchar{50};
    name_col.not_null = true;

    auto res = cat.create_table("employee", {id_col, name_col});
    REQUIRE(res.is_ok());

    auto dup = cat.create_table("employee", {});
    REQUIRE(dup.is_err());
    REQUIRE(dup.error() == "Table 'employee' already exists");

    const TableSchema* schema = cat.get_table("employee");
    REQUIRE(schema != nullptr);
    REQUIRE(schema->columns.size() == 2);
    REQUIRE(schema->columns[0].name == "id");

    REQUIRE(cat.get_table("missing") == nullptr);

    auto drop_ok = cat.drop_table("employee");
    REQUIRE(drop_ok.is_ok());
    REQUIRE(cat.get_table("employee") == nullptr);

    auto drop_missing = cat.drop_table("employee");
    REQUIRE(drop_missing.is_err());
    REQUIRE(drop_missing.error() == "Table 'employee' not found");
}

TEST_CASE("TableSchema JSON round trip preserves all fields", "[schema][json]") {
    ColumnDef dept_id;
    dept_id.name = "department_id";
    dept_id.data_type = DataType::Int{};
    ForeignKey fk;
    fk.column = "department_id";
    fk.ref_table = "department";
    fk.ref_column = "id";
    fk.on_delete = FkAction::SetNull;
    fk.on_update = FkAction::Cascade;
    dept_id.foreign_key = fk;

    ColumnDef salary;
    salary.name = "salary";
    salary.data_type = DataType::Decimal{12, 2};
    salary.check_expr = "salary > 0";

    ColumnDef emp_type;
    emp_type.name = "emp_type";
    emp_type.data_type = DataType::Enum{{"full_time", "part_time", "contract"}};
    emp_type.default_value = "full_time";

    TableSchema schema;
    schema.name = "employee";
    schema.columns = {dept_id, salary, emp_type};
    schema.auto_increment_counters["id"] = 42;
    schema.primary_key_columns = {"id"};
    schema.check_constraints.push_back(CheckConstraint{"chk_salary", "salary > 0"});

    nlohmann::json j = schema;
    std::string serialized = j.dump();

    TableSchema restored = nlohmann::json::parse(serialized).get<TableSchema>();

    REQUIRE(restored.name == "employee");
    REQUIRE(restored.columns.size() == 3);
    REQUIRE(restored.auto_increment_counters.at("id") == 42);
    REQUIRE(restored.primary_key_columns == std::vector<std::string>{"id"});
    REQUIRE(restored.check_constraints.size() == 1);
    REQUIRE(restored.check_constraints[0].name == "chk_salary");

    const ColumnDef& restored_fk_col = restored.columns[0];
    REQUIRE(restored_fk_col.foreign_key.has_value());
    REQUIRE(restored_fk_col.foreign_key->ref_table == "department");
    REQUIRE(restored_fk_col.foreign_key->on_delete == FkAction::SetNull);
    REQUIRE(restored_fk_col.foreign_key->on_update == FkAction::Cascade);

    const ColumnDef& restored_salary = restored.columns[1];
    REQUIRE(std::holds_alternative<DataType::Decimal>(restored_salary.data_type.data));
    REQUIRE(std::get<DataType::Decimal>(restored_salary.data_type.data).precision == 12);
    REQUIRE(std::get<DataType::Decimal>(restored_salary.data_type.data).scale == 2);
    REQUIRE(restored_salary.check_expr == "salary > 0");

    const ColumnDef& restored_enum = restored.columns[2];
    REQUIRE(std::holds_alternative<DataType::Enum>(restored_enum.data_type.data));
    REQUIRE(std::get<DataType::Enum>(restored_enum.data_type.data).values.size() == 3);
    REQUIRE(restored_enum.default_value == "full_time");
}

TEST_CASE("TableSchema JSON tolerates missing #[serde(default)] fields", "[schema][json]") {
    // Simulates an older on-disk schema.json written before primary_key_columns /
    // check_constraints existed.
    nlohmann::json j = {
        {"name", "legacy"},
        {"columns", nlohmann::json::array()},
        {"auto_increment_counters", nlohmann::json::object()},
    };

    TableSchema restored = j.get<TableSchema>();
    REQUIRE(restored.name == "legacy");
    REQUIRE(restored.primary_key_columns.empty());
    REQUIRE(restored.check_constraints.empty());
}
