// Faithful port of the INFORMATION_SCHEMA virtual tables from
// rusql-core/src/engine/executor.rs (Phase 8f): info_schema_rows (a select on one of these tables runs on a temporary table of the rows: see exec_select).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace engine {

namespace {

std::pair<std::string, std::string> split_name(const std::string& name) {
    auto pos = name.find('.');
    if (pos == std::string::npos) return {"", name};
    return {name.substr(0, pos), name.substr(pos + 1)};
}

std::string dt_base(const DataType& dt) {
    return std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int>) return "int";
            else if constexpr (std::is_same_v<T, DataType::BigInt>) return "bigint";
            else if constexpr (std::is_same_v<T, DataType::SmallInt>) return "smallint";
            else if constexpr (std::is_same_v<T, DataType::TinyInt>) return "tinyint";
            else if constexpr (std::is_same_v<T, DataType::Text>) return "text";
            else if constexpr (std::is_same_v<T, DataType::Varchar>) return "varchar";
            else if constexpr (std::is_same_v<T, DataType::Float>) return "float";
            else if constexpr (std::is_same_v<T, DataType::Double>) return "double";
            else if constexpr (std::is_same_v<T, DataType::Decimal>) return "decimal";
            else if constexpr (std::is_same_v<T, DataType::Boolean>) return "tinyint";
            else if constexpr (std::is_same_v<T, DataType::Date>) return "date";
            else if constexpr (std::is_same_v<T, DataType::DateTime>) return "datetime";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>) return "timestamp";
            else if constexpr (std::is_same_v<T, DataType::Time>) return "time";
            else if constexpr (std::is_same_v<T, DataType::Year>) return "year";
            else if constexpr (std::is_same_v<T, DataType::Blob>) return "blob";
            else if constexpr (std::is_same_v<T, DataType::Enum>) return "enum";
            else if constexpr (std::is_same_v<T, DataType::Set>) return "set";
            else if constexpr (std::is_same_v<T, DataType::Json>) return "json";
            else return "varchar";
        },
        dt.data);
}

std::string dt_full(const DataType& dt) {
    if (auto* v = std::get_if<DataType::Varchar>(&dt.data)) return "varchar(" + std::to_string(v->length) + ")";
    if (auto* v = std::get_if<DataType::Decimal>(&dt.data))
        return "decimal(" + std::to_string(v->precision) + "," + std::to_string(v->scale) + ")";
    if (std::holds_alternative<DataType::Boolean>(dt.data)) return "tinyint(1)";
    return dt_base(dt);
}

} // namespace

namespace {
using IC = InfoSchemaColumn;
// The columns of each information_schema table in the order MySQL has them; `true`: a number (everything else is text).
const std::vector<IC> kSchemata = {{"CATALOG_NAME", false}, {"SCHEMA_NAME", false}, {"DEFAULT_CHARACTER_SET_NAME", false}, {"DEFAULT_COLLATION_NAME", false}, {"SQL_PATH", false}};
const std::vector<IC> kTables = {
    {"TABLE_CATALOG", false}, {"TABLE_SCHEMA", false}, {"TABLE_NAME", false}, {"TABLE_TYPE", false}, {"ENGINE", false}, {"VERSION", true},
    {"ROW_FORMAT", false}, {"TABLE_ROWS", true}, {"AVG_ROW_LENGTH", true}, {"DATA_LENGTH", true}, {"MAX_DATA_LENGTH", true}, {"INDEX_LENGTH", true},
    {"DATA_FREE", true}, {"AUTO_INCREMENT", true}, {"CREATE_TIME", false}, {"UPDATE_TIME", false}, {"CHECK_TIME", false}, {"TABLE_COLLATION", false},
    {"CHECKSUM", true}, {"CREATE_OPTIONS", false}, {"TABLE_COMMENT", false}};
const std::vector<IC> kColumns = {
    {"TABLE_CATALOG", false}, {"TABLE_SCHEMA", false}, {"TABLE_NAME", false}, {"COLUMN_NAME", false}, {"ORDINAL_POSITION", true}, {"COLUMN_DEFAULT", false},
    {"IS_NULLABLE", false}, {"DATA_TYPE", false}, {"CHARACTER_MAXIMUM_LENGTH", true}, {"CHARACTER_OCTET_LENGTH", true}, {"NUMERIC_PRECISION", true},
    {"NUMERIC_SCALE", true}, {"DATETIME_PRECISION", true}, {"CHARACTER_SET_NAME", false}, {"COLLATION_NAME", false}, {"COLUMN_TYPE", false},
    {"COLUMN_KEY", false}, {"EXTRA", false}, {"PRIVILEGES", false}, {"COLUMN_COMMENT", false}, {"GENERATION_EXPRESSION", false}};
const std::vector<IC> kKeyColumnUsage = {
    {"CONSTRAINT_CATALOG", false}, {"CONSTRAINT_SCHEMA", false}, {"CONSTRAINT_NAME", false}, {"TABLE_CATALOG", false}, {"TABLE_SCHEMA", false},
    {"TABLE_NAME", false}, {"COLUMN_NAME", false}, {"ORDINAL_POSITION", true}, {"POSITION_IN_UNIQUE_CONSTRAINT", true}, {"REFERENCED_TABLE_SCHEMA", false},
    {"REFERENCED_TABLE_NAME", false}, {"REFERENCED_COLUMN_NAME", false}};
const std::vector<IC> kTableConstraints = {{"CONSTRAINT_CATALOG", false}, {"CONSTRAINT_SCHEMA", false}, {"CONSTRAINT_NAME", false}, {"TABLE_SCHEMA", false},
                                           {"TABLE_NAME", false}, {"CONSTRAINT_TYPE", false}, {"ENFORCED", false}};
const std::vector<IC> kStatistics = {
    {"TABLE_CATALOG", false}, {"TABLE_SCHEMA", false}, {"TABLE_NAME", false}, {"NON_UNIQUE", true}, {"INDEX_SCHEMA", false}, {"INDEX_NAME", false},
    {"SEQ_IN_INDEX", true}, {"COLUMN_NAME", false}, {"COLLATION", false}, {"CARDINALITY", true}, {"SUB_PART", true}, {"PACKED", false}, {"NULLABLE", false},
    {"INDEX_TYPE", false}, {"COMMENT", false}, {"INDEX_COMMENT", false}, {"IS_VISIBLE", false}};
const std::vector<IC> kViews = {{"TABLE_CATALOG", false}, {"TABLE_SCHEMA", false}, {"TABLE_NAME", false}, {"VIEW_DEFINITION", false}, {"CHECK_OPTION", false},
                                {"IS_UPDATABLE", false}, {"DEFINER", false}, {"SECURITY_TYPE", false}, {"CHARACTER_SET_CLIENT", false}, {"COLLATION_CONNECTION", false}};
const std::vector<IC> kCharacterSets = {{"CHARACTER_SET_NAME", false}, {"DEFAULT_COLLATE_NAME", false}, {"DESCRIPTION", false}, {"MAXLEN", true}};
const std::vector<IC> kCollations = {{"COLLATION_NAME", false}, {"CHARACTER_SET_NAME", false}, {"ID", true}, {"IS_DEFAULT", false}, {"IS_COMPILED", false},
                                     {"SORTLEN", true}, {"PAD_ATTRIBUTE", false}};
const std::vector<IC> kEngines = {{"ENGINE", false}, {"SUPPORT", false}, {"COMMENT", false}, {"TRANSACTIONS", false}, {"XA", false}, {"SAVEPOINTS", false}};
} // namespace

const std::vector<InfoSchemaColumn>* Executor::info_schema_columns(const std::string& which) {
    std::string lower = which;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower == "schemata" || lower == "schemas") return &kSchemata;
    if (lower == "tables") return &kTables;
    if (lower == "columns") return &kColumns;
    if (lower == "key_column_usage") return &kKeyColumnUsage;
    if (lower == "table_constraints") return &kTableConstraints;
    if (lower == "statistics") return &kStatistics;
    if (lower == "views") return &kViews;
    if (lower == "character_sets") return &kCharacterSets;
    if (lower == "collations") return &kCollations;
    if (lower == "engines") return &kEngines;
    return nullptr;
}

std::optional<std::string> Executor::info_schema_bare(const std::string& qualified_name) {
    static const std::string prefix = "information_schema.";
    if (qualified_name.size() <= prefix.size()) return std::nullopt;
    std::string lower = qualified_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    lower.erase(0, prefix.size());
    if (!info_schema_columns(lower)) return std::nullopt;
    return lower;
}

std::vector<Row> Executor::info_schema_rows(const SharedDatabase& s, const std::string& which) {
    std::string lower = which;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    std::vector<Row> rows;

    if (lower == "schemata" || lower == "schemas") {
        for (auto& db : s.databases) {
            Row r;
            r["CATALOG_NAME"] = "def";
            r["SCHEMA_NAME"] = db;
            r["DEFAULT_CHARACTER_SET_NAME"] = "utf8mb4";
            r["DEFAULT_COLLATION_NAME"] = "utf8mb4_0900_ai_ci";
            r["SQL_PATH"] = EXECUTOR_NULL_VALUE;
            rows.push_back(std::move(r));
        }
        return rows;
    }

    if (lower == "tables") {
        for (auto& [name, data] : s.tables) {
            auto [db, tbl] = split_name(name);
            if (db.empty() || db == "information_schema") continue; // (a temporary table of the statement that is running)
            Row r;
            r["TABLE_CATALOG"] = "def";
            r["TABLE_SCHEMA"] = db;
            r["TABLE_NAME"] = tbl;
            r["TABLE_TYPE"] = "BASE TABLE";
            r["ENGINE"] = "InnoDB";
            r["VERSION"] = "10";
            r["ROW_FORMAT"] = "Dynamic";
            r["TABLE_ROWS"] = std::to_string(data.size());
            r["AVG_ROW_LENGTH"] = "0";
            r["DATA_LENGTH"] = "0";
            r["MAX_DATA_LENGTH"] = "0";
            r["INDEX_LENGTH"] = "0";
            r["DATA_FREE"] = "0";
            r["AUTO_INCREMENT"] = EXECUTOR_NULL_VALUE;
            r["CREATE_TIME"] = EXECUTOR_NULL_VALUE;
            r["UPDATE_TIME"] = EXECUTOR_NULL_VALUE;
            r["CHECK_TIME"] = EXECUTOR_NULL_VALUE;
            r["TABLE_COLLATION"] = "utf8mb4_0900_ai_ci";
            r["CHECKSUM"] = EXECUTOR_NULL_VALUE;
            r["CREATE_OPTIONS"] = "";
            r["TABLE_COMMENT"] = "";
            rows.push_back(std::move(r));
        }
        for (auto& [name, _] : s.views) {
            auto [db, tbl] = split_name(name);
            Row r;
            r["TABLE_CATALOG"] = "def";
            r["TABLE_SCHEMA"] = db;
            r["TABLE_NAME"] = tbl;
            r["TABLE_TYPE"] = "VIEW";
            r["ENGINE"] = EXECUTOR_NULL_VALUE;
            r["VERSION"] = EXECUTOR_NULL_VALUE;
            r["ROW_FORMAT"] = EXECUTOR_NULL_VALUE;
            r["TABLE_ROWS"] = EXECUTOR_NULL_VALUE;
            r["TABLE_COMMENT"] = "VIEW";
            rows.push_back(std::move(r));
        }
        return rows;
    }

    if (lower == "columns") {
        for (auto& [_, schema] : s.catalog.tables) {
            auto [db, tbl] = split_name(schema.name);
            if (db.empty() || db == "information_schema") continue;
            for (std::size_t i = 0; i < schema.columns.size(); i++) {
                auto& col = schema.columns[i];
                std::string char_max = EXECUTOR_NULL_VALUE;
                if (auto* v = std::get_if<DataType::Varchar>(&col.data_type.data)) char_max = std::to_string(v->length);
                else if (std::holds_alternative<DataType::Text>(col.data_type.data) || std::holds_alternative<DataType::Blob>(col.data_type.data) ||
                         std::holds_alternative<DataType::Json>(col.data_type.data))
                    char_max = "65535";

                std::string num_prec = EXECUTOR_NULL_VALUE;
                if (std::holds_alternative<DataType::Int>(col.data_type.data) || std::holds_alternative<DataType::SmallInt>(col.data_type.data) ||
                    std::holds_alternative<DataType::TinyInt>(col.data_type.data))
                    num_prec = "10";
                else if (std::holds_alternative<DataType::BigInt>(col.data_type.data))
                    num_prec = "19";
                else if (std::holds_alternative<DataType::Float>(col.data_type.data))
                    num_prec = "12";
                else if (std::holds_alternative<DataType::Double>(col.data_type.data))
                    num_prec = "22";
                else if (auto* v = std::get_if<DataType::Decimal>(&col.data_type.data))
                    num_prec = std::to_string(v->precision);

                std::string num_scale = EXECUTOR_NULL_VALUE;
                if (auto* v = std::get_if<DataType::Decimal>(&col.data_type.data)) num_scale = std::to_string(v->scale);

                bool has_charset = std::holds_alternative<DataType::Varchar>(col.data_type.data) ||
                                    std::holds_alternative<DataType::Text>(col.data_type.data) ||
                                    std::holds_alternative<DataType::Enum>(col.data_type.data) ||
                                    std::holds_alternative<DataType::Set>(col.data_type.data) ||
                                    std::holds_alternative<DataType::Json>(col.data_type.data);

                Row r;
                r["TABLE_CATALOG"] = "def";
                r["TABLE_SCHEMA"] = db;
                r["TABLE_NAME"] = tbl;
                r["COLUMN_NAME"] = col.name;
                r["ORDINAL_POSITION"] = std::to_string(i + 1);
                r["COLUMN_DEFAULT"] = col.default_value.value_or(EXECUTOR_NULL_VALUE);
                r["IS_NULLABLE"] = col.not_null ? "NO" : "YES";
                r["DATA_TYPE"] = dt_base(col.data_type);
                r["CHARACTER_MAXIMUM_LENGTH"] = char_max;
                r["CHARACTER_OCTET_LENGTH"] = EXECUTOR_NULL_VALUE;
                r["NUMERIC_PRECISION"] = num_prec;
                r["NUMERIC_SCALE"] = num_scale;
                r["DATETIME_PRECISION"] = EXECUTOR_NULL_VALUE;
                r["CHARACTER_SET_NAME"] = has_charset ? "utf8mb4" : EXECUTOR_NULL_VALUE;
                r["COLLATION_NAME"] = has_charset ? "utf8mb4_0900_ai_ci" : EXECUTOR_NULL_VALUE;
                r["COLUMN_TYPE"] = dt_full(col.data_type) + (col.is_unsigned ? " unsigned" : "");
                const bool in_composite_key = std::find(schema.primary_key_columns.begin(), schema.primary_key_columns.end(), col.name) != schema.primary_key_columns.end();
                bool first_of_index = false;
                for (auto& [unused, meta] : s.index_meta) first_of_index = first_of_index || (meta.first == schema.name && meta.second == col.name);
                for (auto& [unused, meta] : s.hash_index_meta) first_of_index = first_of_index || (meta.first == schema.name && meta.second == col.name);
                for (auto& [unused, index] : s.composite_indexes) first_of_index = first_of_index || (index.table == schema.name && !index.columns.empty() && index.columns.front() == col.name);
                r["COLUMN_KEY"] = (col.primary_key || in_composite_key) ? "PRI" : (col.unique ? "UNI" : (first_of_index ? "MUL" : ""));
                r["EXTRA"] = col.auto_increment ? "auto_increment" : "";
                r["PRIVILEGES"] = "select,insert,update,references";
                r["COLUMN_COMMENT"] = "";
                r["GENERATION_EXPRESSION"] = "";
                rows.push_back(std::move(r));
            }
        }
        return rows;
    }

    if (lower == "key_column_usage") {
        for (auto& [_, schema] : s.catalog.tables) {
            auto [db, tbl] = split_name(schema.name);
            if (db.empty() || db == "information_schema") continue;
            std::vector<std::string> pk_cols = schema.primary_key_columns;
            if (pk_cols.empty()) {
                for (auto& c : schema.columns) {
                    if (c.primary_key) pk_cols.push_back(c.name);
                }
            }
            for (std::size_t pos = 0; pos < pk_cols.size(); pos++) {
                Row r;
                r["CONSTRAINT_CATALOG"] = "def";
                r["CONSTRAINT_SCHEMA"] = db;
                r["CONSTRAINT_NAME"] = "PRIMARY";
                r["TABLE_CATALOG"] = "def";
                r["TABLE_SCHEMA"] = db;
                r["TABLE_NAME"] = tbl;
                r["COLUMN_NAME"] = pk_cols[pos];
                r["ORDINAL_POSITION"] = std::to_string(pos + 1);
                r["POSITION_IN_UNIQUE_CONSTRAINT"] = EXECUTOR_NULL_VALUE;
                r["REFERENCED_TABLE_SCHEMA"] = EXECUTOR_NULL_VALUE;
                r["REFERENCED_TABLE_NAME"] = EXECUTOR_NULL_VALUE;
                r["REFERENCED_COLUMN_NAME"] = EXECUTOR_NULL_VALUE;
                rows.push_back(std::move(r));
            }
            for (std::size_t i = 0; i < schema.columns.size(); i++) {
                auto& col = schema.columns[i];
                if (!col.foreign_key) continue;
                auto& fk = *col.foreign_key;
                auto [ref_db0, ref_tbl] = split_name(fk.ref_table);
                std::string ref_db = ref_db0.empty() ? db : ref_db0;
                Row r;
                r["CONSTRAINT_CATALOG"] = "def";
                r["CONSTRAINT_SCHEMA"] = db;
                r["CONSTRAINT_NAME"] = tbl + "_ibfk_" + std::to_string(i + 1);
                r["TABLE_CATALOG"] = "def";
                r["TABLE_SCHEMA"] = db;
                r["TABLE_NAME"] = tbl;
                r["COLUMN_NAME"] = col.name;
                r["ORDINAL_POSITION"] = "1";
                r["POSITION_IN_UNIQUE_CONSTRAINT"] = "1";
                r["REFERENCED_TABLE_SCHEMA"] = ref_db;
                r["REFERENCED_TABLE_NAME"] = ref_tbl;
                r["REFERENCED_COLUMN_NAME"] = fk.ref_column;
                rows.push_back(std::move(r));
            }
        }
        return rows;
    }

    if (lower == "table_constraints") {
        for (auto& [_, schema] : s.catalog.tables) {
            auto [db, tbl] = split_name(schema.name);
            if (db.empty() || db == "information_schema") continue;
            bool has_pk =
                std::any_of(schema.columns.begin(), schema.columns.end(), [](const ColumnDef& c) { return c.primary_key; }) ||
                !schema.primary_key_columns.empty();
            if (has_pk) {
                Row r;
                r["CONSTRAINT_CATALOG"] = "def";
                r["CONSTRAINT_SCHEMA"] = db;
                r["CONSTRAINT_NAME"] = "PRIMARY";
                r["TABLE_SCHEMA"] = db;
                r["TABLE_NAME"] = tbl;
                r["CONSTRAINT_TYPE"] = "PRIMARY KEY";
                r["ENFORCED"] = "YES";
                rows.push_back(std::move(r));
            }
            for (auto& col : schema.columns) {
                if (col.unique && !col.primary_key) {
                    Row r;
                    r["CONSTRAINT_CATALOG"] = "def";
                    r["CONSTRAINT_SCHEMA"] = db;
                    r["CONSTRAINT_NAME"] = col.unique_constraint_name.value_or(col.name);
                    r["TABLE_SCHEMA"] = db;
                    r["TABLE_NAME"] = tbl;
                    r["CONSTRAINT_TYPE"] = "UNIQUE";
                    r["ENFORCED"] = "YES";
                    rows.push_back(std::move(r));
                }
                if (col.foreign_key) {
                    Row r;
                    r["CONSTRAINT_CATALOG"] = "def";
                    r["CONSTRAINT_SCHEMA"] = db;
                    r["CONSTRAINT_NAME"] = tbl + "_ibfk_" + col.name;
                    r["TABLE_SCHEMA"] = db;
                    r["TABLE_NAME"] = tbl;
                    r["CONSTRAINT_TYPE"] = "FOREIGN KEY";
                    r["ENFORCED"] = "YES";
                    rows.push_back(std::move(r));
                }
            }
            for (auto& cc : schema.check_constraints) {
                Row r;
                r["CONSTRAINT_CATALOG"] = "def";
                r["CONSTRAINT_SCHEMA"] = db;
                r["CONSTRAINT_NAME"] = cc.name.value_or("chk");
                r["TABLE_SCHEMA"] = db;
                r["TABLE_NAME"] = tbl;
                r["CONSTRAINT_TYPE"] = "CHECK";
                r["ENFORCED"] = "YES";
                rows.push_back(std::move(r));
            }
        }
        return rows;
    }

    if (lower == "statistics") {
        // every index of every table, one row per column of it: the PRIMARY KEY, the UNIQUE columns and keys, then the ones CREATE INDEX made
        for (auto& [_, schema] : s.catalog.tables) {
            auto [db, tbl] = split_name(schema.name);
            if (db.empty() || db == "information_schema") continue;
            auto nullable = [&](const std::string& column) {
                for (auto& c : schema.columns) {
                    if (c.name == column) return !(c.not_null || c.primary_key);
                }
                return true;
            };
            // how many different values the first `count` columns of an index hold (a NULL is one value), over the rows nobody has deleted
            std::vector<std::string> index_columns;
            auto cardinality = [&](std::size_t count) {
                auto data = s.tables.find(schema.name);
                std::size_t distinct = 0;
                if (data != s.tables.end()) {
                    std::unordered_set<std::string> seen;
                    for (auto& row : data->second) {
                        if (auto x = row.find("_xmax"); x != row.end() && x->second != "0") continue;
                        std::string key;
                        for (std::size_t c = 0; c < count && c < index_columns.size(); c++) {
                            auto v = row.find(index_columns[c]);
                            key += collation::key(v != row.end() ? v->second : std::string(EXECUTOR_NULL_VALUE));
                            key += '';
                        }
                        seen.insert(std::move(key));
                    }
                    distinct = seen.size();
                }
                return std::to_string(distinct);
            };
            auto add = [&](const std::string& index_name, bool non_unique, std::size_t seq, const std::string& column, const char* type) {
                Row r;
                r["TABLE_CATALOG"] = "def";
                r["TABLE_SCHEMA"] = db;
                r["TABLE_NAME"] = tbl;
                r["NON_UNIQUE"] = non_unique ? "1" : "0";
                r["INDEX_SCHEMA"] = db;
                r["INDEX_NAME"] = index_name;
                r["SEQ_IN_INDEX"] = std::to_string(seq);
                r["COLUMN_NAME"] = column;
                r["COLLATION"] = "A";
                r["CARDINALITY"] = cardinality(seq);
                r["SUB_PART"] = EXECUTOR_NULL_VALUE;
                r["PACKED"] = EXECUTOR_NULL_VALUE;
                r["NULLABLE"] = nullable(column) ? "YES" : "";
                r["INDEX_TYPE"] = type;
                r["COMMENT"] = "";
                r["INDEX_COMMENT"] = "";
                r["IS_VISIBLE"] = "YES";
                rows.push_back(std::move(r));
            };
            std::vector<std::string> pk_cols = schema.primary_key_columns;
            if (pk_cols.empty()) {
                for (auto& c : schema.columns) {
                    if (c.primary_key) pk_cols.push_back(c.name);
                }
            }
            index_columns = pk_cols;
            for (std::size_t k = 0; k < pk_cols.size(); k++) add("PRIMARY", false, k + 1, pk_cols[k], "BTREE");
            for (auto& c : schema.columns) {
                if (!c.unique || c.primary_key) continue;
                index_columns = {c.name};
                add(c.unique_constraint_name.value_or(c.name), false, 1, c.name, "BTREE");
            }
            for (auto& key : schema.unique_keys) {
                index_columns = key.columns;
                for (std::size_t k = 0; k < key.columns.size(); k++) add(key.name.value_or(key.columns.front()), false, k + 1, key.columns[k], "BTREE");
            }
            // (the indexes made by CREATE INDEX are kept as "<table>_<name>")
            const std::string key_prefix = schema.name + "_";
            auto bare_index_name = [&](const std::string& key) { return key.compare(0, key_prefix.size(), key_prefix) == 0 ? key.substr(key_prefix.size()) : key; };
            std::vector<std::pair<std::string, std::vector<std::string>>> secondary; // (name, columns), by name
            std::vector<bool> hashed;
            for (auto& [key, meta] : s.index_meta) {
                if (meta.first == schema.name) { secondary.push_back({bare_index_name(key), {meta.second}}); hashed.push_back(false); }
            }
            for (auto& [key, meta] : s.hash_index_meta) {
                if (meta.first == schema.name) { secondary.push_back({bare_index_name(key), {meta.second}}); hashed.push_back(true); }
            }
            for (auto& [key, comp] : s.composite_indexes) {
                if (comp.table == schema.name) { secondary.push_back({bare_index_name(key), comp.columns}); hashed.push_back(false); }
            }
            std::vector<std::size_t> order(secondary.size());
            for (std::size_t k = 0; k < order.size(); k++) order[k] = k;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return secondary[a].first < secondary[b].first; });
            for (std::size_t k : order) {
                index_columns = secondary[k].second;
                for (std::size_t c = 0; c < secondary[k].second.size(); c++) add(secondary[k].first, true, c + 1, secondary[k].second[c], hashed[k] ? "HASH" : "BTREE");
            }
        }
        return rows;
    }

    if (lower == "views") {
        for (auto& [name, _] : s.views) {
            auto [db, vw] = split_name(name);
            Row r;
            r["TABLE_CATALOG"] = "def";
            r["TABLE_SCHEMA"] = db;
            r["TABLE_NAME"] = vw;
            r["VIEW_DEFINITION"] = "";
            r["CHECK_OPTION"] = "NONE";
            r["IS_UPDATABLE"] = "NO";
            r["DEFINER"] = "root@%";
            r["SECURITY_TYPE"] = "DEFINER";
            r["CHARACTER_SET_CLIENT"] = "utf8mb4";
            r["COLLATION_CONNECTION"] = "utf8mb4_0900_ai_ci";
            rows.push_back(std::move(r));
        }
        return rows;
    }

    if (lower == "character_sets" || lower == "collations" || lower == "engines") {
        Row r;
        if (lower == "character_sets") {
            r["CHARACTER_SET_NAME"] = "utf8mb4";
            r["DEFAULT_COLLATE_NAME"] = "utf8mb4_0900_ai_ci";
            r["DESCRIPTION"] = "UTF-8 Unicode";
            r["MAXLEN"] = "4";
        } else if (lower == "collations") {
            r["COLLATION_NAME"] = "utf8mb4_0900_ai_ci";
            r["CHARACTER_SET_NAME"] = "utf8mb4";
            r["ID"] = "255";
            r["IS_DEFAULT"] = "Yes";
            r["IS_COMPILED"] = "Yes";
            r["SORTLEN"] = "0";
            r["PAD_ATTRIBUTE"] = "NO PAD";
        } else {
            r["ENGINE"] = "InnoDB";
            r["SUPPORT"] = "DEFAULT";
            r["COMMENT"] = "Custom Rust RDBMS";
            r["TRANSACTIONS"] = "YES";
            r["XA"] = "NO";
            r["SAVEPOINTS"] = "YES";
        }
        rows.push_back(std::move(r));
        return rows;
    }

    return rows; // unknown IS table -> empty result
}

} // namespace engine
