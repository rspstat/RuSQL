// Faithful port of DESCRIBE / SHOW CREATE TABLE / SHOW CREATE VIEW / SHOW INDEX /
// SHOW PROCESSLIST from rusql-core/src/engine/executor.rs (Phase 8f).

#include "engine/collation.hpp"
#include "engine/executor/executor.hpp"
#include "engine/fk_name.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

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
            else if constexpr (std::is_same_v<T, DataType::DateTime>) return alt.fsp ? "DATETIME(" + std::to_string(alt.fsp) + ")" : "DATETIME";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>) return alt.fsp ? "TIMESTAMP(" + std::to_string(alt.fsp) + ")" : "TIMESTAMP";
            else if constexpr (std::is_same_v<T, DataType::Varchar>) return "VARCHAR(" + std::to_string(alt.length) + ")";
            else if constexpr (std::is_same_v<T, DataType::Decimal>)
                return "DECIMAL(" + std::to_string(alt.precision) + "," + std::to_string(alt.scale) + ")";
            else if constexpr (std::is_same_v<T, DataType::Double>) return "DOUBLE";
            else if constexpr (std::is_same_v<T, DataType::Time>) return alt.fsp ? "TIME(" + std::to_string(alt.fsp) + ")" : "TIME";
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

// `restrict_written`: RESTRICT was spelled for the rule (a rule left out, or NO ACTION, is reported as NO ACTION: the same refusal)
std::string fk_action_str(FkAction a, bool restrict_written) {
    switch (a) {
        case FkAction::Restrict: return restrict_written ? "RESTRICT" : "NO ACTION";
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
        std::string type_str = sql_type_str(col.data_type) + (col.is_unsigned ? " UNSIGNED" : "");
        std::string def_str = "NULL";
        if (col.default_value && *col.default_value != NULL_DEFAULT) def_str = *col.default_value;

        out += "| " + pad_right(col.name, 16) + " | " + pad_right(type_str, 7) + " | " + pad_right(col.primary_key ? "YES" : "NO", 3) + " | " +
               pad_right(col.not_null ? "YES" : "NO", 3) + " | " + pad_right(col.auto_increment ? "YES" : "NO", 14) + " | " +
               pad_right(def_str, 15) + " |\n";
    }
    out += sep;
    return StringResult::Ok(out);
}

namespace {
// the type as MySQL writes it in SHOW CREATE TABLE
std::string mysql_type_text(const DataType& dt, bool is_unsigned) {
    std::string out = std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int>) return "int";
            else if constexpr (std::is_same_v<T, DataType::BigInt>) return "bigint";
            else if constexpr (std::is_same_v<T, DataType::SmallInt>) return "smallint";
            else if constexpr (std::is_same_v<T, DataType::TinyInt>) return "tinyint";
            else if constexpr (std::is_same_v<T, DataType::Text>) return "text";
            else if constexpr (std::is_same_v<T, DataType::Float>) return "float";
            else if constexpr (std::is_same_v<T, DataType::Double>) return "double";
            else if constexpr (std::is_same_v<T, DataType::Boolean>) return "tinyint(1)";
            else if constexpr (std::is_same_v<T, DataType::Varchar>) return "varchar(" + std::to_string(alt.length) + ")";
            else if constexpr (std::is_same_v<T, DataType::Decimal>) return "decimal(" + std::to_string(alt.precision) + "," + std::to_string(alt.scale) + ")";
            else if constexpr (std::is_same_v<T, DataType::Date>) return "date";
            else if constexpr (std::is_same_v<T, DataType::DateTime>) return alt.fsp ? "datetime(" + std::to_string(alt.fsp) + ")" : "datetime";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>) return alt.fsp ? "timestamp(" + std::to_string(alt.fsp) + ")" : "timestamp";
            else if constexpr (std::is_same_v<T, DataType::Time>) return alt.fsp ? "time(" + std::to_string(alt.fsp) + ")" : "time";
            else if constexpr (std::is_same_v<T, DataType::Year>) return "year";
            else if constexpr (std::is_same_v<T, DataType::Blob>) return "blob";
            else if constexpr (std::is_same_v<T, DataType::Json>) return "json";
            else if constexpr (std::is_same_v<T, DataType::Enum> || std::is_same_v<T, DataType::Set>) {
                std::string list = std::is_same_v<T, DataType::Enum> ? "enum(" : "set(";
                for (std::size_t i = 0; i < alt.values.size(); i++) list += (i ? ",'" : "'") + alt.values[i] + "'";
                return list + ")";
            } else return "varchar(255)";
        },
        dt.data);
    return is_unsigned ? out + " unsigned" : out;
}

std::string mysql_quoted(const std::string& text) {
    std::string out = "'";
    for (char c : text) out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}
} // namespace

StringResult Executor::show_create_table_mysql(const std::string& name) {
    Statement named = Statement(Statement::ShowCreateTable{name});
    resolve_names(named);
    const std::string table = qualify_name(std::get<Statement::ShowCreateTable>(named.data).table);
    auto s = shared->read();
    const TableSchema* schema = s->catalog.get_table(table);
    if (!schema) return StringResult::Err("Table '" + table + "' doesn't exist");
    const std::string bare = bare_name(table);
    std::vector<std::string> lines;
    std::vector<std::string> pk = schema->primary_key_columns;
    if (pk.empty()) {
        for (auto& c : schema->columns) {
            if (c.primary_key) pk.push_back(c.name);
        }
    }
    auto in_pk = [&](const std::string& column) { return std::find(pk.begin(), pk.end(), column) != pk.end(); };
    bool has_auto_increment = false;
    for (const ColumnDef& col : schema->columns) {
        const bool is_timestamp = std::holds_alternative<DataType::Timestamp>(col.data_type.data);
        const bool no_default_clause = std::holds_alternative<DataType::Text>(col.data_type.data) || std::holds_alternative<DataType::Blob>(col.data_type.data);
        const bool not_null = col.not_null || in_pk(col.name);
        std::string line = "  `" + col.name + "` " + mysql_type_text(col.data_type, col.is_unsigned);
        if (not_null) line += " NOT NULL";
        else if (is_timestamp) line += " NULL";
        if (col.auto_increment) {
            line += " AUTO_INCREMENT";
            has_auto_increment = true;
        }
        if (col.default_value && *col.default_value != NULL_DEFAULT) {
            std::string upper = *col.default_value;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
            if (upper == "NOW()" || upper == "CURRENT_TIMESTAMP" || upper == "CURRENT_TIMESTAMP()") line += " DEFAULT CURRENT_TIMESTAMP";
            else if (upper == "TRUE" && std::holds_alternative<DataType::Boolean>(col.data_type.data)) line += " DEFAULT '1'";
            else if (upper == "FALSE" && std::holds_alternative<DataType::Boolean>(col.data_type.data)) line += " DEFAULT '0'";
            else {
                std::string value = *col.default_value;
                if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') value = value.substr(1, value.size() - 2);
                line += " DEFAULT " + mysql_quoted(value);
            }
        } else if (!not_null && !no_default_clause) {
            line += " DEFAULT NULL";
        }
        if (col.on_update_now) line += " ON UPDATE CURRENT_TIMESTAMP";
        lines.push_back(std::move(line));
    }
    auto column_list = [](const std::vector<std::string>& columns) {
        std::string out;
        for (std::size_t i = 0; i < columns.size(); i++) out += (i ? ",`" : "`") + columns[i] + "`";
        return out;
    };
    if (!pk.empty()) lines.push_back("  PRIMARY KEY (" + column_list(pk) + ")");
    // the UNIQUE keys, then the others (by name)
    std::vector<std::pair<std::string, std::string>> unique_keys, other_keys;
    for (auto& c : schema->columns) {
        if (c.unique && !c.primary_key) unique_keys.emplace_back(c.unique_constraint_name.value_or(c.name), "  UNIQUE KEY `" + c.unique_constraint_name.value_or(c.name) + "` (`" + c.name + "`)");
    }
    for (auto& key : schema->unique_keys) {
        const std::string key_name = key.name.value_or(key.columns.front());
        unique_keys.emplace_back(key_name, "  UNIQUE KEY `" + key_name + "` (" + column_list(key.columns) + ")");
    }
    const std::string key_prefix = table + "_";
    auto bare_index_name = [&](const std::string& key) { return key.compare(0, key_prefix.size(), key_prefix) == 0 ? key.substr(key_prefix.size()) : key; };
    for (auto& [key, meta] : s->index_meta) {
        if (meta.first == table) other_keys.emplace_back(bare_index_name(key), "  KEY `" + bare_index_name(key) + "` (`" + meta.second + "`)");
    }
    for (auto& [key, meta] : s->hash_index_meta) {
        if (meta.first == table) other_keys.emplace_back(bare_index_name(key), "  KEY `" + bare_index_name(key) + "` (`" + meta.second + "`) USING HASH");
    }
    for (auto& [key, comp] : s->composite_indexes) {
        if (comp.table == table) other_keys.emplace_back(bare_index_name(key), "  KEY `" + bare_index_name(key) + "` (" + column_list(comp.columns) + ")");
    }
    std::sort(unique_keys.begin(), unique_keys.end());
    std::sort(other_keys.begin(), other_keys.end());
    for (auto& k : unique_keys) lines.push_back(k.second);
    for (auto& k : other_keys) lines.push_back(k.second);
    for (std::size_t i = 0; i < schema->columns.size(); i++) {
        const ColumnDef& col = schema->columns[i];
        for (std::size_t k = 0; k < col.foreign_keys.size(); k++) {
            const ForeignKey& fk = col.foreign_keys[k];
            std::string line = "  CONSTRAINT `" + fk_constraint_name(*schema, bare, i, k) + "` FOREIGN KEY (`" + fk.column + "`) REFERENCES `" + bare_name(fk.ref_table) + "` (`" + fk.ref_column + "`)";
            if (fk.on_delete != FkAction::Restrict || fk.restrict_delete) line += " ON DELETE " + fk_action_str(fk.on_delete, fk.restrict_delete);
            if (fk.on_update != FkAction::Restrict || fk.restrict_update) line += " ON UPDATE " + fk_action_str(fk.on_update, fk.restrict_update);
            lines.push_back(std::move(line));
        }
    }
    std::size_t check_number = 0;
    for (auto& cc : schema->check_constraints) {
        check_number++;
        lines.push_back("  CONSTRAINT `" + cc.name.value_or(bare + "_chk_" + std::to_string(check_number)) + "` CHECK ((" + cc.expression + "))");
    }
    std::string ddl = "CREATE TABLE `" + bare + "` (\n";
    for (std::size_t i = 0; i < lines.size(); i++) ddl += lines[i] + (i + 1 < lines.size() ? ",\n" : "\n");
    ddl += ") ENGINE=InnoDB";
    if (has_auto_increment) {
        std::int64_t next = 1;
        for (auto& [column, counter] : schema->auto_increment_counters) next = std::max(next, counter + 1);
        ddl += " AUTO_INCREMENT=" + std::to_string(next);
    }
    ddl += " DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci";
    return StringResult::Ok(ddl);
}

StringResult Executor::show_create_view_mysql(const std::string& name) {
    Statement named = Statement(Statement::ShowCreateView{name});
    resolve_names(named);
    const std::string view = qualify_name(std::get<Statement::ShowCreateView>(named.data).view);
    auto s = shared->read();
    if (!s->views.count(view)) return StringResult::Err("Table '" + view + "' doesn't exist");
    std::string select_text;
    if (auto raw = s->view_raw_sql.find(view); raw != s->view_raw_sql.end()) select_text = raw->second;
    std::string body = select_text;
    // (the stored text may start with SELECT or already with the whole CREATE)
    return StringResult::Ok("CREATE ALGORITHM=UNDEFINED DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `" + bare_name(view) + "` AS " + body);
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
        parts.push_back(sql_type_str_ddl(col.data_type) + (col.is_unsigned ? " UNSIGNED" : ""));
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
    for (auto& key : schema->unique_keys) {
        std::string cols_str;
        for (std::size_t i = 0; i < key.columns.size(); i++) cols_str += (i ? ",`" : "`") + key.columns[i] + "`";
        lines.push_back("  UNIQUE KEY `" + key.name.value_or(key.columns.front()) + "` (" + cols_str + ")");
    }

    for (auto& col : schema->columns) {
        for (auto& fk : col.foreign_keys) {
            std::string ref_bare = bare_name(fk.ref_table);
            lines.push_back(std::string("  ") + (fk.name.empty() ? "" : "CONSTRAINT `" + fk.name + "` ") + "FOREIGN KEY (`" + fk.column + "`) REFERENCES `" + ref_bare + "`(`" + fk.ref_column + "`) ON DELETE " +
                             fk_action_str(fk.on_delete, fk.restrict_delete) + " ON UPDATE " + fk_action_str(fk.on_update, fk.restrict_update));
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

// The rows of SHOW STATUS: the statement counters, the connections, the time the server has been up, the buffer pool and the row locks, by name.
std::vector<std::pair<std::string, std::string>> server_status_values(const ServerCounters& c, std::size_t threads_connected, std::size_t threads_running,
                                                                      std::uint64_t redo_log_bytes, const std::array<std::uint64_t, kStatCount>* session) {
    std::vector<std::pair<std::string, std::string>> rows;
    auto add = [&](const std::string& name, std::uint64_t value) { rows.emplace_back(name, std::to_string(value)); };
    add("Aborted_connects", c.aborted_connects.load());
    for (int i = 0; i < kStatCount; i++) {
        const std::uint64_t value = session ? (*session)[static_cast<std::size_t>(i)] : c.statements[static_cast<std::size_t>(i)].load();
        add(stat_name(static_cast<Stat>(i)), value);
        if (static_cast<Stat>(i) == Stat::Questions) add("Queries", value);
    }
    add("Connections", c.connections.load());
    if (c.buffer_pool) {
        const std::uint64_t hits = c.buffer_pool->hit_count.load(), misses = c.buffer_pool->miss_count.load();
        add("Innodb_buffer_pool_pages_data", c.buffer_pool->usage());
        add("Innodb_buffer_pool_pages_total", c.buffer_pool->capacity);
        add("Innodb_buffer_pool_read_requests", hits + misses);
        add("Innodb_buffer_pool_reads", misses);
    }
    if (c.lock_mgr) {
        add("Innodb_deadlocks", c.lock_mgr->deadlock_history().size());
        add("Innodb_row_lock_current_waits", c.lock_mgr->wait_for_rows().size());
        add("Innodb_row_lock_waits", c.lock_mgr->waits_total());
    }
    add("Innodb_redo_log_bytes", redo_log_bytes); // (the size of the redo log now: a checkpoint empties it)
    add("Max_used_connections", c.max_used_connections.load());
    add("Slow_queries", c.slow_queries.load());
    add("Threads_connected", threads_connected);
    add("Threads_running", threads_running);
    add("Uptime", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - c.started).count()));
    std::sort(rows.begin(), rows.end());
    return rows;
}

// SHOW [GLOBAL | SESSION] STATUS [LIKE 'pattern']
StringResult Executor::exec_show_status(const std::string& sql) {
    std::string upper = sql;
    for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    std::istringstream words(upper);
    std::string show, scope, status;
    words >> show >> scope;
    const bool global = scope == "GLOBAL";
    std::optional<std::string> pattern;
    if (const auto like = upper.find(" LIKE "); like != std::string::npos) {
        const auto open = sql.find('\'', like), close = open == std::string::npos ? std::string::npos : sql.find('\'', open + 1);
        if (close == std::string::npos) return StringResult::Err("ERROR 1064 (42000): You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near '" + sql.substr(like) + "'");
        pattern = sql.substr(open + 1, close - open - 1);
    }
    std::size_t connected, running = 0;
    {
        auto list = process_list()->lock();
        connected = list->size();
        for (auto& [id, p] : *list) {
            if (p.command == "Query") running++;
        }
    }
    if (running == 0) running = 1; // (this statement)
    auto rows = server_status_values(*counters(), connected, running, redo_log_ ? redo_log_->bytes() : 0, global ? nullptr : &session_statements_);
    std::vector<std::vector<std::string>> shown;
    for (auto& [name, value] : rows) {
        if (!pattern || collation::like(name, *pattern)) shown.push_back({name, value});
    }
    std::vector<std::size_t> width = {13, 5};
    for (auto& r : shown) {
        width[0] = std::max(width[0], r[0].size());
        width[1] = std::max(width[1], r[1].size());
    }
    const std::string sep = "+" + std::string(width[0] + 2, '-') + "+" + std::string(width[1] + 2, '-') + "+";
    auto line = [&](const std::string& a, const std::string& b) { return "| " + a + std::string(width[0] - a.size(), ' ') + " | " + b + std::string(width[1] - b.size(), ' ') + " |"; };
    std::string out = sep + "\n" + line("Variable_name", "Value") + "\n" + sep + "\n";
    for (auto& r : shown) out += line(r[0], r[1]) + "\n";
    out += sep + "\n" + std::to_string(shown.size()) + " row(s) returned.";
    return StringResult::Ok(out);
}

// KILL [CONNECTION | QUERY] <id>
StringResult Executor::exec_kill(const std::string& sql) {
    std::string text = sql;
    while (!text.empty() && (text.back() == ';' || std::isspace(static_cast<unsigned char>(text.back())))) text.pop_back();
    std::istringstream words(text);
    std::string kill_word, first, id_text, extra;
    words >> kill_word >> first;
    bool query_only = false;
    auto same_word = [](std::string a, const char* b) {
        for (char& ch : a) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return a == b;
    };
    if (same_word(first, "CONNECTION") || same_word(first, "QUERY")) {
        query_only = same_word(first, "QUERY");
        words >> id_text;
    } else {
        id_text = first;
    }
    words >> extra;
    if (id_text.empty() || id_text.find_first_not_of("0123456789") != std::string::npos || id_text.size() > 18 || !extra.empty()) {
        return StringResult::Err("ERROR 1064 (42000): You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near '" +
                                  text + "'");
    }
    const std::size_t id = static_cast<std::size_t>(std::stoull(id_text));
    auto list = process_list()->lock();
    auto it = list->find(id);
    if (it == list->end()) return StringResult::Err("ERROR 1094 (HY000): Unknown thread id: " + id_text);
    ProcessInfo& target = it->second;
    if (target.user != auth_user && auth_user != "root") return StringResult::Err("ERROR 1095 (HY000): You are not owner of thread " + id_text);
    affected_rows_ = 0;
    if (id == session_id) { // a session that kills itself: the query ends with the error, the connection with the answer
        if (query_only) return StringResult::Err("ERROR 1317 (70100): Query execution was interrupted");
        kill_state_->store(2);
        return StringResult::Ok("Connection " + id_text + " killed.");
    }
    if (query_only) {
        // only a session that is running a statement is asked to stop (one that is asleep has nothing to stop: MySQL does nothing either)
        if (target.command == "Query" && target.kill_state) {
            int none = 0;
            target.kill_state->compare_exchange_strong(none, 1);
        }
        return StringResult::Ok("Query " + id_text + " interrupted.");
    }
    if (target.kill_state) target.kill_state->store(2);
    if (target.close_connection) target.close_connection(); // (a session that waits for its next statement is woken by its connection closing)
    return StringResult::Ok("Connection " + id_text + " killed.");
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
