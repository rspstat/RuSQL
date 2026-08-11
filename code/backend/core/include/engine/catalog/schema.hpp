#pragma once

// Faithful port of rusql-core/src/catalog/schema.rs.
//
// Simplification vs. the Rust original: ast.rs and catalog/schema.rs each define their
// own field-identical ColumnDef/ForeignKey/FkAction (the Rust code re-exports only
// DataType across that boundary). Since the two are structurally identical and
// duplicating them would add pure busywork with no behavioral difference, this port
// uses a single engine::ColumnDef/ForeignKey/FkAction (from ast.hpp) for both.

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/parser/ast_json.hpp"
#include "engine/json_support.hpp"
#include "engine/result.hpp"

namespace engine {

struct CheckConstraint {
    std::optional<std::string> name;
    std::string expression; // raw SQL string, e.g. "age > 0"
};

void to_json(nlohmann::json& j, const CheckConstraint& c);
void from_json(const nlohmann::json& j, CheckConstraint& c);

struct TableSchema {
    std::string name;
    std::vector<ColumnDef> columns;
    std::unordered_map<std::string, std::int64_t> auto_increment_counters;
    // 복합 PK 컬럼 순서 (비어있으면 단일 PK 또는 PK 없음)
    std::vector<std::string> primary_key_columns;
    // 테이블 레벨 CHECK 제약
    std::vector<CheckConstraint> check_constraints;
    // 파티셔닝 (PARTITION BY) 정보 -- 있으면 이 TableSchema는 "논리" 테이블이고 실제 행은
    // partition_info->partitions[i].child_table 이름의 평범한 물리 테이블에 저장됨
    // (executor_partition.cpp의 라우터가 처리, executor_ddl.cpp의 exec_create가 채움).
    std::optional<PartitionBy> partition_info;
};

void to_json(nlohmann::json& j, const TableSchema& s);
void from_json(const nlohmann::json& j, TableSchema& s);

class Catalog {
public:
    Catalog() = default;

    Result<void, std::string> create_table(std::string name, std::vector<ColumnDef> columns);

    Result<void, std::string> create_table_full(std::string name,
                                                 std::vector<ColumnDef> columns,
                                                 std::vector<std::string> primary_key_columns,
                                                 std::vector<CheckConstraint> check_constraints);

    Result<void, std::string> drop_table(const std::string& name);

    const TableSchema* get_table(const std::string& name) const;
    TableSchema* get_table_mut(const std::string& name);

    // Public, matching the Rust original's `pub tables: HashMap<String, TableSchema>`
    // — executor.rs manipulates this map directly (rename-table key juggling, index
    // bookkeeping, etc.) rather than going through narrower accessors.
    std::unordered_map<std::string, TableSchema> tables;
};

} // namespace engine
