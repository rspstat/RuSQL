// Faithful port of DESCRIBE / SHOW CREATE TABLE / SHOW CREATE VIEW / SHOW INDEX /
// SHOW PROCESSLIST from rusql-core/src/engine/executor.rs (Phase 8f).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <chrono>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {

std::string pad_right(const std::string& v, std::size_t width) {
    std::string out = v;
    if (out.size() < width) out.append(width - out.size(), ' ');
    return out;
}

std::string sql_type_str(const DataType& dt) {
    return std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int>) return "INT";
            else if constexpr (std::is_same_v<T, DataType::BigInt>) return "BIGINT";
            else if constexpr (std::is_same_v<T, DataType::SmallInt>) return "SMALLINT";
            else if constexpr (std::is_same_v<T, DataType::TinyInt>) return "TINYINT";
            else if constexpr (std::is_same_v<T, DataType::Text>) return "TEXT";
            else if constexpr (std::is_same_v<T, DataType::Float>) return "FLOAT";
            else if constexpr (std::is_same_v<T, DataType::Boolean>) return "BOOLEAN";
            else if constexpr (std::is_same_v<T, DataType::Date>) return "DATE";
            else if constexpr (std::is_same_v<T, DataType::DateTime>) return "DATETIME";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>) return "TIMESTAMP";
            else if constexpr (std::is_same_v<T, DataType::Varchar>) return "VARCHAR(" + std::to_string(alt.length) + ")";
            else if constexpr (std::is_same_v<T, DataType::Decimal>)
                return "DECIMAL(" + std::to_string(alt.precision) + "," + std::to_string(alt.scale) + ")";
            else if constexpr (std::is_same_v<T, DataType::Double>) return "DOUBLE";
            else if constexpr (std::is_same_v<T, DataType::Time>) return "TIME";
            else if constexpr (std::is_same_v<T, DataType::Year>) return "YEAR";
            else if constexpr (std::is_same_v<T, DataType::Enum>) {
                std::string out = "ENUM(";
                for (std::size_t i = 0; i < alt.values.size(); i++) {
                    if (i) out += ",";
                    out += "'" + alt.values[i] + "'";
                }
                return out + ")";
            } else if constexpr (std::is_same_v<T, DataType::Set>) {
                std::string out = "SET(";
                for (std::size_t i = 0; i < alt.values.size(); i++) {
                    if (i) out += ",";
                    out += "'" + alt.values[i] + "'";
                }
                return out + ")";
            } else if constexpr (std::is_same_v<T, DataType::Blob>)
                return "BLOB";
            else if constexpr (std::is_same_v<T, DataType::Json>)
                return "JSON";
            else
                return "UNKNOWN";
        },
        dt.data);
}

std::string sql_type_str_ddl(const DataType& dt) {
    // SHOW CREATE TABLE joins ENUM/SET values with ", " rather than DESCRIBE's ",".
    return std::visit(
        [&dt](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Enum>) {
                std::string out = "ENUM(";
                for (std::size_t i = 0; i < alt.values.size(); i++) {
                    if (i) out += ", ";
                    out += "'" + alt.values[i] + "'";
                }
                return out + ")";
            } else if constexpr (std::is_same_v<T, DataType::Set>) {
                std::string out = "SET(";
                for (std::size_t i = 0; i < alt.values.size(); i++) {
                    if (i) out += ", ";
                    out += "'" + alt.values[i] + "'";
                }
                return out + ")";
            } else {
                return sql_type_str(dt);
            }
        },
        dt.data);
}

std::string fk_action_str(FkAction a) {
    switch (a) {
        case FkAction::Restrict: return "RESTRICT";
        case FkAction::Cascade: return "CASCADE";
        case FkAction::SetNull: return "SET NULL";
        case FkAction::SetDefault: return "SET DEFAULT";
    }
    return "";
}

std::string bare_name(const std::string& qualified) {
    auto pos = qualified.rfind('.');
    return pos == std::string::npos ? qualified : qualified.substr(pos + 1);
}

} // namespace

StringResult Executor::exec_describe(const SharedDatabase& s, const std::string& table) const {
    const TableSchema* schema = s.catalog.get_table(table);
    if (!schema) return StringResult::Err("Table '" + table + "' not found");

    std::string sep = "+------------------+---------+-----+-----+----------------+-----------------+";
    std::string out = sep + "\n";
    out += "| Field            | Type    | PK  | NN  | Auto Increment | Default         |\n";
    out += sep + "\n";
    for (auto& col : schema->columns) {
        std::string type_str = sql_type_str(col.data_type);
        std::string def_str = "NULL";
        if (col.default_value && *col.default_value != NULL_DEFAULT) def_str = *col.default_value;

        out += "| " + pad_right(col.name, 16) + " | " + pad_right(type_str, 7) + " | " + pad_right(col.primary_key ? "YES" : "NO", 3) + " | " +
               pad_right(col.not_null ? "YES" : "NO", 3) + " | " + pad_right(col.auto_increment ? "YES" : "NO", 14) + " | " +
               pad_right(def_str, 15) + " |\n";
    }
    out += sep;
    return StringResult::Ok(out);
}

StringResult Executor::exec_show_create_table(const SharedDatabase& s, const std::string& table) const {
    const TableSchema* schema = s.catalog.get_table(table);
    if (!schema) return StringResult::Err("Table '" + table + "' not found");

    std::string bare = bare_name(table);
    auto& composite_pk = schema->primary_key_columns;

    std::vector<std::string> lines;
    for (auto& col : schema->columns) {
        std::vector<std::string> parts;
        parts.push_back("`" + col.name + "`");
        parts.push_back(sql_type_str_ddl(col.data_type));
        if (col.not_null || col.primary_key) parts.push_back("NOT NULL");
        if (col.auto_increment) parts.push_back("AUTO_INCREMENT");
        if (col.default_value && *col.default_value != NULL_DEFAULT) {
            bool is_enum_set_varchar_text = std::holds_alternative<DataType::Enum>(col.data_type.data) ||
                                             std::holds_alternative<DataType::Set>(col.data_type.data) ||
                                             std::holds_alternative<DataType::Varchar>(col.data_type.data) ||
                                             std::holds_alternative<DataType::Text>(col.data_type.data);
            bool needs_quotes = is_enum_set_varchar_text && !col.default_value->empty() && col.default_value->front() != '\'' &&
                                col.default_value->front() != '"';
            std::string display = needs_quotes ? "'" + *col.default_value + "'" : *col.default_value;
            parts.push_back("DEFAULT " + display);
        }
        if (col.primary_key && composite_pk.empty()) parts.push_back("PRIMARY KEY");
        if (col.unique && !col.unique_constraint_name) parts.push_back("UNIQUE");

        std::string line = "  ";
        for (std::size_t i = 0; i < parts.size(); i++) {
            if (i) line += " ";
            line += parts[i];
        }
        lines.push_back(line);
    }

    if (!composite_pk.empty()) {
        std::string cols_str;
        for (std::size_t i = 0; i < composite_pk.size(); i++) {
            if (i) cols_str += ", ";
            cols_str += "`" + composite_pk[i] + "`";
        }
        lines.push_back("  PRIMARY KEY (" + cols_str + ")");
    }

    for (auto& col : schema->columns) {
        if (col.unique_constraint_name) lines.push_back("  UNIQUE KEY `" + *col.unique_constraint_name + "` (`" + col.name + "`)");
    }

    for (auto& col : schema->columns) {
        if (col.foreign_key) {
            auto& fk = *col.foreign_key;
            std::string ref_bare = bare_name(fk.ref_table);
            lines.push_back("  FOREIGN KEY (`" + fk.column + "`) REFERENCES `" + ref_bare + "`(`" + fk.ref_column + "`) ON DELETE " +
                             fk_action_str(fk.on_delete) + " ON UPDATE " + fk_action_str(fk.on_update));
        }
    }

    for (auto& cc : schema->check_constraints) {
        if (cc.name) lines.push_back("  CONSTRAINT `" + *cc.name + "` CHECK (" + cc.expression + ")");
        else lines.push_back("  CHECK (" + cc.expression + ")");
    }

    std::string body;
    for (std::size_t i = 0; i < lines.size(); i++) {
        if (i) body += ",\n";
        body += lines[i];
    }
    std::string ddl = "CREATE TABLE `" + bare + "` (\n" + body + "\n)";
    if (schema->partition_info) {
        auto& info = *schema->partition_info;
        std::string kind_str = info.kind == PartitionKind::Range ? "RANGE" : info.kind == PartitionKind::List ? "LIST" : "HASH";
        ddl += "\nPARTITION BY " + kind_str + " (`" + info.column + "`)";
        if (info.kind == PartitionKind::Hash) {
            ddl += " PARTITIONS " + std::to_string(info.partitions.size());
        } else {
            ddl += " (\n";
            for (std::size_t i = 0; i < info.partitions.size(); i++) {
                auto& def = info.partitions[i];
                ddl += "  PARTITION `" + def.name + "` VALUES ";
                if (info.kind == PartitionKind::Range) {
                    ddl += def.range_is_maxvalue ? "LESS THAN MAXVALUE" : "LESS THAN (" + def.range_upper_bound.value_or("") + ")";
                } else {
                    ddl += "IN (";
                    for (std::size_t j = 0; j < def.list_values.size(); j++) {
                        if (j) ddl += ", ";
                        ddl += "'" + def.list_values[j] + "'";
                    }
                    ddl += ")";
                }
                ddl += (i + 1 < info.partitions.size()) ? ",\n" : "\n";
            }
            ddl += ")";
        }
    }
    ddl += ";";
    return StringResult::Ok("Table: " + bare + "\n" + ddl);
}

StringResult Executor::exec_show_create_view(const SharedDatabase& s, const std::string& view) const {
    std::string q_view = view.find('.') != std::string::npos ? view : current_db + "." + view;
    std::string bare = bare_name(view);

    if (!s.views.count(q_view)) return StringResult::Err("View '" + bare + "' not found");

    std::string select_sql = "<view definition not available>";
    if (auto it = s.view_raw_sql.find(q_view); it != s.view_raw_sql.end()) select_sql = it->second;

    std::string ddl = "CREATE VIEW `" + bare + "` AS " + select_sql;
    return StringResult::Ok("View: " + bare + "\nCreate View: " + ddl);
}

StringResult Executor::exec_show_index(const SharedDatabase& s, const std::string& table) const {
    std::string q_table = table.find('.') != std::string::npos ? table : current_db + "." + table;
    std::string bare = bare_name(table);

    const TableSchema* schema = s.catalog.get_table(q_table);
    if (!schema) return StringResult::Err("Table '" + bare + "' not found");

    std::vector<std::string> rows;
    rows.push_back("Table\tKey_name\tColumn_name\tIndex_type");

    // PRIMARY KEY B+Tree — every table's own PK index, tracked separately from the
    // secondary/composite index metadata iterated below (s.index_meta/composite_indexes
    // only ever holds CREATE INDEX-created indexes, never the implicit PK one).
    if (!schema->primary_key_columns.empty()) {
        for (auto& col : schema->primary_key_columns) rows.push_back(bare + "\tPRIMARY\t" + col + "\tBTREE");
    } else {
        for (auto& col : schema->columns) {
            if (col.primary_key) rows.push_back(bare + "\tPRIMARY\t" + col.name + "\tBTREE");
        }
    }

    // Keys are stored as "<table>_<index_name>" (to allow the same bare index name to be
    // reused across tables/databases) -- strip the table prefix back off for display.
    std::string key_prefix = q_table + "_";
    auto strip_prefix = [&](const std::string& key) {
        return key.compare(0, key_prefix.size(), key_prefix) == 0 ? key.substr(key_prefix.size()) : key;
    };

    for (auto& [idx_name, meta] : s.index_meta) {
        if (meta.first == q_table) rows.push_back(bare + "\t" + strip_prefix(idx_name) + "\t" + meta.second + "\tBTREE");
    }
    for (auto& [idx_name, comp] : s.composite_indexes) {
        if (comp.table == q_table) {
            std::string cols;
            for (std::size_t i = 0; i < comp.columns.size(); i++) {
                if (i) cols += ", ";
                cols += comp.columns[i];
            }
            rows.push_back(bare + "\t" + strip_prefix(idx_name) + "\t" + cols + "\tBTREE");
        }
    }

    if (rows.size() == 1) return StringResult::Ok("No indexes found on table '" + bare + "'");
    std::string out;
    for (std::size_t i = 0; i < rows.size(); i++) {
        if (i) out += "\n";
        out += rows[i];
    }
    return StringResult::Ok(out);
}

StringResult Executor::exec_show_processlist(const SharedDatabase& s) const {
    auto list = s.process_list->lock();
    auto now = std::chrono::steady_clock::now();
    std::string sep =
        "+------+------------------+-----------------------+------------+---------+------+"
        "----------------------------------------------------------------------+";
    std::string header = "| " + pad_right("Id", 4) + " | " + pad_right("User", 16) + " | " + pad_right("Host", 21) + " | " +
                          pad_right("db", 10) + " | " + pad_right("Command", 7) + " | " + pad_right("Time", 4) + " | " +
                          pad_right("Info", 68) + " |";

    std::vector<std::string> rows;
    for (auto& [id, p] : *list) {
        std::string db_str = p.db.empty() ? "NULL" : p.db;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - p.state_since).count();
        std::string info_str = p.info.empty() ? "NULL" : p.info.substr(0, std::min<std::size_t>(68, p.info.size()));
        rows.push_back("| " + pad_right(std::to_string(p.id), 4) + " | " + pad_right(p.user, 16) + " | " + pad_right(p.host, 21) + " | " +
                        pad_right(db_str, 10) + " | " + pad_right(p.command, 7) + " | " + pad_right(std::to_string(secs), 4) + " | " +
                        pad_right(info_str, 68) + " |");
    }
    std::sort(rows.begin(), rows.end());

    std::string out = sep + "\n" + header + "\n" + sep + "\n";
    for (auto& r : rows) out += r + "\n";
    out += sep;
    return StringResult::Ok(out);
}

} // namespace engine
