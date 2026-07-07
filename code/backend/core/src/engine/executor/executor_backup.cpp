// Faithful port of BACKUP / RESTORE from rusql-core/src/engine/executor.rs (Phase 8f):
// exec_backup, exec_restore, build_create_table_ddl.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {

std::string to_lower_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string bare_name(const std::string& qualified) {
    auto pos = qualified.rfind('.');
    return pos == std::string::npos ? qualified : qualified.substr(pos + 1);
}

std::string sql_escape_str(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

// build_create_table_ddl's own type-name mapping — distinct from exec_show_create_table's
// sql_type_str (Unknown maps to "TEXT" here, and DEFAULT is always single-quoted with no
// conditional logic), matching the Rust original's independent closure.
std::string backup_type_str(const DataType& dt) {
    return std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int>) return "INT";
            else if constexpr (std::is_same_v<T, DataType::BigInt>) return "BIGINT";
            else if constexpr (std::is_same_v<T, DataType::SmallInt>) return "SMALLINT";
            else if constexpr (std::is_same_v<T, DataType::TinyInt>) return "TINYINT";
            else if constexpr (std::is_same_v<T, DataType::Float>) return "FLOAT";
            else if constexpr (std::is_same_v<T, DataType::Varchar>) return "VARCHAR(" + std::to_string(alt.length) + ")";
            else if constexpr (std::is_same_v<T, DataType::Text>) return "TEXT";
            else if constexpr (std::is_same_v<T, DataType::Boolean>) return "BOOLEAN";
            else if constexpr (std::is_same_v<T, DataType::Date>) return "DATE";
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
            else if constexpr (std::is_same_v<T, DataType::DateTime>)
                return "DATETIME";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>)
                return "TIMESTAMP";
            else if constexpr (std::is_same_v<T, DataType::Decimal>)
                return "DECIMAL(" + std::to_string(alt.precision) + "," + std::to_string(alt.scale) + ")";
            else if constexpr (std::is_same_v<T, DataType::Double>)
                return "DOUBLE";
            else if constexpr (std::is_same_v<T, DataType::Time>)
                return "TIME";
            else if constexpr (std::is_same_v<T, DataType::Year>)
                return "YEAR";
            else
                return "TEXT"; // Unknown
        },
        dt.data);
}

std::string backup_fk_action_str(FkAction a) {
    switch (a) {
        case FkAction::Restrict: return "RESTRICT";
        case FkAction::Cascade: return "CASCADE";
        case FkAction::SetNull: return "SET NULL";
        case FkAction::SetDefault: return "SET DEFAULT";
    }
    return "";
}

} // namespace

std::string Executor::build_create_table_ddl(const SharedDatabase& s, const std::string& qkey) const {
    std::string bare = bare_name(qkey);
    const TableSchema* schema = s.catalog.get_table(qkey);
    if (!schema) return "-- (schema not found for " + bare + ")";

    std::vector<std::string> lines;
    for (auto& col : schema->columns) {
        std::vector<std::string> parts;
        parts.push_back("`" + col.name + "`");
        parts.push_back(backup_type_str(col.data_type));
        if (col.not_null || col.primary_key) parts.push_back("NOT NULL");
        if (col.auto_increment) parts.push_back("AUTO_INCREMENT");
        if (col.default_value && *col.default_value != NULL_DEFAULT) parts.push_back("DEFAULT '" + *col.default_value + "'");
        if (col.primary_key && schema->primary_key_columns.empty()) parts.push_back("PRIMARY KEY");

        std::string line = "  ";
        for (std::size_t i = 0; i < parts.size(); i++) {
            if (i) line += " ";
            line += parts[i];
        }
        lines.push_back(line);
    }
    if (!schema->primary_key_columns.empty()) {
        std::string pkc;
        for (std::size_t i = 0; i < schema->primary_key_columns.size(); i++) {
            if (i) pkc += ", ";
            pkc += "`" + schema->primary_key_columns[i] + "`";
        }
        lines.push_back("  PRIMARY KEY (" + pkc + ")");
    }
    for (auto& col : schema->columns) {
        if (col.foreign_key) {
            auto& fk = *col.foreign_key;
            std::string ref_bare = bare_name(fk.ref_table);
            lines.push_back("  FOREIGN KEY (`" + fk.column + "`) REFERENCES `" + ref_bare + "`(`" + fk.ref_column + "`) ON DELETE " +
                             backup_fk_action_str(fk.on_delete) + " ON UPDATE " + backup_fk_action_str(fk.on_update));
        }
    }

    std::string body;
    for (std::size_t i = 0; i < lines.size(); i++) {
        if (i) body += ",\n";
        body += lines[i];
    }
    return "CREATE TABLE `" + bare + "` (\n" + body + "\n);";
}

StringResult Executor::exec_backup(const SharedDatabase& s, std::optional<std::string> database, std::optional<std::string> output_file) const {
    std::string target_db = to_lower_str(database.value_or(current_db));
    std::string out;

    out += "-- RuSQL backup of database `" + target_db + "`\n";
    {
        auto secs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04llu-%02llu-%02llu", static_cast<unsigned long long>(1970 + secs / 31536000),
                      static_cast<unsigned long long>((secs % 31536000) / 2628000 + 1),
                      static_cast<unsigned long long>((secs % 2628000) / 86400 + 1));
        out += "-- Generated: " + std::string(buf) + "\n\n";
    }
    out += "CREATE DATABASE IF NOT EXISTS `" + target_db + "`;\nUSE `" + target_db + "`;\n\n";

    std::vector<std::string> sorted_keys;
    std::string prefix = target_db + ".";
    for (auto& [k, _] : s.tables) {
        if (k.compare(0, prefix.size(), prefix) == 0) sorted_keys.push_back(k);
    }
    std::sort(sorted_keys.begin(), sorted_keys.end());

    for (auto& qkey : sorted_keys) {
        std::string bare = bare_name(qkey);
        if (s.catalog.get_table(qkey)) {
            std::string ddl = build_create_table_ddl(s, qkey);
            out += "DROP TABLE IF EXISTS `" + bare + "`;\n";
            out += ddl;
            out += "\n\n";
        }
        if (auto it = s.tables.find(qkey); it != s.tables.end()) {
            std::vector<const Row*> visible;
            for (auto& r : it->second) {
                if (is_visible(r)) visible.push_back(&r);
            }
            if (!visible.empty()) {
                std::vector<std::string> cols;
                for (auto& [k, _] : *visible.front()) {
                    if (!k.empty() && k.front() == '_') continue;
                    cols.push_back(k);
                }
                std::string col_list;
                for (std::size_t i = 0; i < cols.size(); i++) {
                    if (i) col_list += ", ";
                    col_list += "`" + cols[i] + "`";
                }
                for (auto* row : visible) {
                    std::string vals;
                    for (std::size_t i = 0; i < cols.size(); i++) {
                        if (i) vals += ", ";
                        auto vit = row->find(cols[i]);
                        if (vit == row->end() || vit->second == "NULL") vals += "NULL";
                        else vals += "'" + sql_escape_str(vit->second) + "'";
                    }
                    out += "INSERT INTO `" + bare + "` (" + col_list + ") VALUES (" + vals + ");\n";
                }
                out += "\n";
            }
        }
    }

    if (output_file) {
        std::ofstream f(*output_file, std::ios::binary | std::ios::trunc);
        if (!f) return StringResult::Err("Failed to write backup file '" + *output_file + "'");
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f) return StringResult::Err("Failed to write backup file '" + *output_file + "'");
        return StringResult::Ok("Backup of '" + target_db + "' written to '" + *output_file + "' (" + std::to_string(out.size()) + " bytes).");
    }
    return StringResult::Ok(out);
}

StringResult Executor::exec_restore(SharedDatabase& s, std::string source_file, std::optional<std::string> database) {
    std::ifstream f(source_file, std::ios::binary);
    if (!f) return StringResult::Err("Cannot read restore file '" + source_file + "'");
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string sql_text = ss.str();

    if (database) current_db = to_lower_str(*database);

    std::size_t stmts_ok = 0, stmts_err = 0;

    std::size_t start = 0;
    while (start <= sql_text.size()) {
        auto semi = sql_text.find(';', start);
        std::string raw = sql_text.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        start = semi == std::string::npos ? sql_text.size() + 1 : semi + 1;

        std::string stmt;
        {
            std::size_t line_start = 0;
            bool first = true;
            while (line_start <= raw.size()) {
                auto nl = raw.find('\n', line_start);
                std::string line = raw.substr(line_start, nl == std::string::npos ? std::string::npos : nl - line_start);
                if (!line.empty() && line.back() == '\r') line.pop_back(); // matches Rust str::lines()' CRLF handling
                std::size_t ws = line.find_first_not_of(" \t");
                bool is_comment = ws != std::string::npos && line.compare(ws, 2, "--") == 0;
                if (!is_comment) {
                    if (!first) stmt += "\n";
                    stmt += line;
                    first = false;
                }
                if (nl == std::string::npos) break;
                line_start = nl + 1;
            }
        }
        std::size_t t0 = stmt.find_first_not_of(" \t\r\n");
        std::string trimmed = t0 == std::string::npos ? "" : stmt.substr(t0, stmt.find_last_not_of(" \t\r\n") - t0 + 1);
        if (trimmed.empty()) continue;

        Parser parser(trimmed);
        auto parsed = parser.parse();
        if (parsed.is_err()) {
            stmts_err++;
        } else {
            auto res = execute_with_s(s, std::move(parsed).value());
            if (res.is_ok()) stmts_ok++;
            else stmts_err++;
        }
    }

    return StringResult::Ok("Restore from '" + source_file + "' complete: " + std::to_string(stmts_ok) + " statement(s) OK, " +
                             std::to_string(stmts_err) + " error(s).");
}

} // namespace engine
