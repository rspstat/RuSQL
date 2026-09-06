// Faithful port of the INFORMATION_SCHEMA virtual tables from
// rusql-core/src/engine/executor.rs (Phase 8f): info_schema_rows, exec_information_schema.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <cmath>

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
            Row r;
            r["TABLE_CATALOG"] = "def";
            r["TABLE_SCHEMA"] = db;
            r["TABLE_NAME"] = tbl;
            r["TABLE_TYPE"] = "BASE TABLE";
            r["ENGINE"] = "RuSQL";
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
                r["COLUMN_TYPE"] = dt_full(col.data_type);
                r["COLUMN_KEY"] = col.primary_key ? "PRI" : (col.unique ? "UNI" : "");
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
        for (auto& [idx_name, meta] : s.index_meta) {
            auto [db, tbl] = split_name(meta.first);
            Row r;
            r["TABLE_CATALOG"] = "def";
            r["TABLE_SCHEMA"] = db;
            r["TABLE_NAME"] = tbl;
            r["NON_UNIQUE"] = "1";
            r["INDEX_SCHEMA"] = db;
            r["INDEX_NAME"] = idx_name;
            r["SEQ_IN_INDEX"] = "1";
            r["COLUMN_NAME"] = meta.second;
            r["COLLATION"] = "A";
            r["CARDINALITY"] = EXECUTOR_NULL_VALUE;
            r["SUB_PART"] = EXECUTOR_NULL_VALUE;
            r["PACKED"] = EXECUTOR_NULL_VALUE;
            r["NULLABLE"] = "YES";
            r["INDEX_TYPE"] = "BTREE";
            r["COMMENT"] = "";
            r["INDEX_COMMENT"] = "";
            r["IS_VISIBLE"] = "YES";
            rows.push_back(std::move(r));
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
            r["ENGINE"] = "RuSQL";
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

StringResult Executor::exec_information_schema(SharedDatabase& s, const std::string& which, std::vector<SelectColumn> columns,
                                                 std::optional<CondExpr> condition, std::vector<OrderBy> order_by,
                                                 std::optional<std::size_t> limit, std::optional<std::size_t> offset) const {
    std::vector<Row> rows;
    for (auto& r0 : info_schema_rows(s, which)) {
        Row r;
        for (auto& [k, v] : r0) {
            std::string lk = k;
            std::transform(lk.begin(), lk.end(), lk.begin(), [](unsigned char c) { return std::tolower(c); });
            r[lk] = v;
        }
        rows.push_back(std::move(r));
    }

    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return !matches_condexpr(r, condition); }), rows.end());

    if (!order_by.empty()) {
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            for (auto& ord : order_by) {
                const std::string* av_p = get_col(a, ord.column);
                const std::string* bv_p = get_col(b, ord.column);
                std::string av = av_p ? *av_p : std::string();
                std::string bv = bv_p ? *bv_p : std::string();
                int cmp;
                try {
                    std::size_t pa, pb;
                    double af = std::stod(av, &pa);
                    double bf = std::stod(bv, &pb);
                    if (pa != av.size() || pb != bv.size()) throw std::invalid_argument("");
                    cmp = af < bf ? -1 : (af > bf ? 1 : 0);
                } catch (...) {
                    cmp = av < bv ? -1 : (av > bv ? 1 : 0);
                }
                if (!ord.ascending) cmp = -cmp;
                if (cmp != 0) return cmp < 0;
            }
            return false;
        });
    }

    std::size_t off = offset.value_or(0);
    if (off > rows.size()) rows.clear();
    else rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(off));
    std::size_t lim = limit.value_or(rows.size());
    if (rows.size() > lim) rows.resize(lim);

    if (rows.empty()) return StringResult::Ok("0 rows returned.");

    bool has_all = std::any_of(columns.begin(), columns.end(), [](const SelectColumn& c) { return std::holds_alternative<SelectColumn::All>(c.data); });
    std::vector<std::string> col_names;
    if (has_all) {
        for (auto& [k, _] : rows[0]) col_names.push_back(k);
        std::sort(col_names.begin(), col_names.end());
    } else {
        for (auto& c : columns) {
            if (auto* v = std::get_if<SelectColumn::Column>(&c.data)) {
                auto dot = v->name.rfind('.');
                col_names.push_back(dot == std::string::npos ? v->name : v->name.substr(dot + 1));
            } else if (auto* v = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
                col_names.push_back(v->alias);
            }
        }
    }

    if (col_names.empty()) return StringResult::Ok("0 rows returned.");

    // Escape headers/cells up front (widths below are computed on the escaped form,
    // matching the visual padding actually written) -- see Executor::escape_cell.
    std::vector<std::string> headers(col_names.size());
    for (std::size_t i = 0; i < col_names.size(); i++) headers[i] = escape_cell(col_names[i]);

    std::vector<std::size_t> col_widths;
    for (std::size_t i = 0; i < col_names.size(); i++) {
        std::size_t w = headers[i].size();
        for (auto& r : rows) {
            const std::string* v = get_col(r, col_names[i]);
            if (v) w = std::max(w, escape_cell(*v).size());
        }
        col_widths.push_back(w);
    }

    std::string sep;
    for (auto w : col_widths) sep += "+" + std::string(w + 2, '-');
    sep += "+";

    std::string hdr;
    for (std::size_t i = 0; i < headers.size(); i++) {
        hdr += "| " + headers[i] + std::string(col_widths[i] - headers[i].size(), ' ') + " ";
    }
    hdr += "|";

    std::string out = sep + "\n" + hdr + "\n" + sep + "\n";
    for (auto& row : rows) {
        std::string line;
        for (std::size_t i = 0; i < col_names.size(); i++) {
            const std::string* v = get_col(row, col_names[i]);
            std::string val = v ? escape_cell(*v) : std::string();
            line += "| " + val + std::string(col_widths[i] - val.size(), ' ') + " ";
        }
        line += "|";
        out += line + "\n";
    }
    out += sep;
    out += "\n" + std::to_string(rows.size()) + " row(s) returned.";
    return StringResult::Ok(out);
}

} // namespace engine
