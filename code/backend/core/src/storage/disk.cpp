#include "engine/storage/disk.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <lz4/lz4.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "engine/parser/ast_json.hpp"
#include "engine/storage/page.hpp"

namespace fs = std::filesystem;

namespace engine {

void to_json(nlohmann::json& j, const IndexMeta& m) {
    j = nlohmann::json{{"name", m.name}, {"table", m.table}, {"columns", m.columns}, {"index_type", m.index_type}};
}

void from_json(const nlohmann::json& j, IndexMeta& m) {
    j.at("name").get_to(m.name);
    j.at("table").get_to(m.table);
    j.at("columns").get_to(m.columns);
    if (j.contains("index_type")) j.at("index_type").get_to(m.index_type);
    else m.index_type = "btree";
}

namespace {

std::vector<std::uint8_t> lz4_compress_prepend_size(const std::vector<std::uint8_t>& raw) {
    int bound = LZ4_compressBound(static_cast<int>(raw.size()));
    std::vector<std::uint8_t> out(4 + static_cast<std::size_t>(bound));
    std::uint32_t orig_size = static_cast<std::uint32_t>(raw.size());
    for (int i = 0; i < 4; i++) out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((orig_size >> (8 * i)) & 0xFF);
    int compressed_size = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),
                                                reinterpret_cast<char*>(out.data() + 4),
                                                static_cast<int>(raw.size()), bound);
    out.resize(4 + static_cast<std::size_t>(compressed_size));
    return out;
}

std::optional<std::vector<std::uint8_t>> lz4_decompress_size_prepended(const std::uint8_t* data, std::size_t len) {
    if (len < 4) return std::nullopt;
    std::uint32_t orig_size = static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
                               (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
    std::vector<std::uint8_t> out(orig_size);
    if (orig_size == 0) return out;
    int written = LZ4_decompress_safe(reinterpret_cast<const char*>(data + 4), reinterpret_cast<char*>(out.data()),
                                       static_cast<int>(len - 4), static_cast<int>(orig_size));
    if (written < 0 || static_cast<std::uint32_t>(written) != orig_size) return std::nullopt;
    return out;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size < 0) return false;
    out.resize(static_cast<std::size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return true;
}

// Writes the full content to a sibling ".tmp" file, fsyncs it, then atomically renames
// it over `path`. Without this, a crash between the pre-existing truncate-on-open and
// the write completing left the file empty/corrupt with the old contents already gone
// (PLAN.md: "테이블 저장이 원자적이지 않음"), and even a clean write was never fsynced
// to physical disk (PLAN.md: "fsync 없이 flush만 됨") -- std::ofstream has no portable
// fsync, so this goes through a C stdio FILE* to reach the real fd/handle.
void write_bytes_atomic(const std::string& path, const void* data, std::size_t size) {
    std::string tmp = path + ".tmp";
    std::FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) throw std::runtime_error("파일 쓰기 실패: " + tmp);
    std::size_t written = std::fwrite(data, 1, size, fp);
    if (written != size) {
        std::fclose(fp);
        throw std::runtime_error("파일 쓰기 실패: " + tmp);
    }
    if (std::fflush(fp) != 0) {
        std::fclose(fp);
        throw std::runtime_error("파일 쓰기 실패: " + tmp);
    }
#ifdef _WIN32
    bool synced = _commit(_fileno(fp)) == 0;
#else
    bool synced = fsync(fileno(fp)) == 0;
#endif
    std::fclose(fp);
    if (!synced) throw std::runtime_error("파일 동기화 실패: " + tmp);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("파일 교체 실패: " + path + " (" + ec.message() + ")");
}

void write_file(const std::string& path, const std::string& content) {
    write_bytes_atomic(path, content.data(), content.size());
}

void write_file_bytes(const std::string& path, const std::vector<std::uint8_t>& content) {
    write_bytes_atomic(path, content.data(), content.size());
}

} // namespace

DiskManager::DiskManager(const std::string& dir) : data_dir_(dir) {
    fs::create_directories(dir);
    fs::create_directories(dir + "/_system");
}

void DiskManager::ensure_db_dir(const std::string& db) const {
    std::error_code ec;
    fs::create_directories(table_dir(db), ec);
}

std::pair<std::string, std::string> DiskManager::parse_key(const std::string& key) {
    auto pos = key.find('.');
    if (pos != std::string::npos) return {key.substr(0, pos), key.substr(pos + 1)};
    return {"rusql", key};
}

void DiskManager::create_db_dir(const std::string& db) const {
    std::error_code ec;
    fs::create_directories(table_dir(db), ec);
}

void DiskManager::drop_db_dir(const std::string& db) const {
    std::error_code ec;
    fs::remove_all(table_dir(db), ec);
}

std::vector<std::string> DiskManager::list_databases() const {
    std::vector<std::string> dbs;
    std::error_code ec;
    if (!fs::exists(data_dir_, ec)) return dbs;
    for (auto& entry : fs::directory_iterator(data_dir_, ec)) {
        if (entry.is_directory()) {
            std::string name = entry.path().filename().string();
            // Leading underscore is reserved for the engine's own top-level directories
            // (_system, _backups, ...), which must never surface as a "database" a client
            // can USE/SHOW -- a real CREATE DATABASE name isn't required to avoid this
            // prefix, but nothing in normal usage would ever name one this way.
            if (!name.empty() && name.front() != '_') dbs.push_back(name);
        }
    }
    return dbs;
}

void DiskManager::save_schema(const std::string& table, const TableSchema& schema) const {
    auto [db, tbl] = parse_key(table);
    ensure_db_dir(db);
    std::string path = table_dir(db) + "/" + tbl + ".schema.json";
    nlohmann::json j = schema;
    write_file(path, j.dump(2));
}

std::optional<TableSchema> DiskManager::load_schema(const std::string& table) const {
    auto [db, tbl] = parse_key(table);
    std::string path = table_dir(db) + "/" + tbl + ".schema.json";
    std::string flat_path = data_dir_ + "/" + tbl + ".schema.json";
    std::string chosen;
    if (fs::exists(path)) chosen = path;
    else if (db == "rusql" && fs::exists(flat_path)) chosen = flat_path;
    else return std::nullopt;

    std::string content;
    if (!read_file(chosen, content)) return std::nullopt;

    // 신버전: TableSchema JSON
    try {
        return nlohmann::json::parse(content).get<TableSchema>();
    } catch (...) {
        // fall through to legacy formats
    }

    // 구버전 폴백: 컬럼명 배열 ["col1", "col2", ...]
    try {
        auto col_names = nlohmann::json::parse(content).get<std::vector<std::string>>();
        TableSchema schema;
        schema.name = table;
        for (auto& c : col_names) {
            ColumnDef col;
            col.name = c;
            col.data_type = DataType(DataType::Text{});
            schema.columns.push_back(col);
        }
        return schema;
    } catch (...) {
        return std::nullopt;
    }
}

void DiskManager::save_schema_columns(const std::string& table, const std::vector<std::string>& columns) const {
    auto [db, tbl] = parse_key(table);
    ensure_db_dir(db);
    std::string path = table_dir(db) + "/" + tbl + ".schema.json";
    nlohmann::json j = columns;
    write_file(path, j.dump());
}

void DiskManager::save_table(const std::string& table, const std::vector<Row>& rows) const {
    auto [db, tbl] = parse_key(table);
    ensure_db_dir(db);
    std::string path = table_dir(db) + "/" + tbl + ".rdb";

    std::vector<std::uint8_t> raw;
    for (auto& row : rows) {
        nlohmann::json j = row;
        std::string json = j.dump();
        std::uint32_t len = static_cast<std::uint32_t>(json.size());
        for (int i = 0; i < 4; i++) raw.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
        raw.insert(raw.end(), json.begin(), json.end());
    }

    std::vector<std::uint8_t> compressed = lz4_compress_prepend_size(raw);

    PageHeader header;
    header.row_count = static_cast<std::uint32_t>(rows.size());
    header.flags = FLAG_COMPRESSED;
    header.page_count = static_cast<std::uint32_t>(
        std::max<std::size_t>((compressed.size() + PAGE_SIZE - 1) / PAGE_SIZE, 1));

    std::vector<std::uint8_t> out = header.to_bytes();
    out.insert(out.end(), compressed.begin(), compressed.end());
    write_file_bytes(path, out);
}

std::vector<Row> DiskManager::load_rdb(const std::string& path) const {
    std::vector<std::uint8_t> buf;
    if (!read_file_bytes(path, buf)) return {};
    if (buf.size() < 32) return {};

    auto header = PageHeader::from_bytes(buf.data(), 32);
    if (!header) return {};

    std::vector<std::uint8_t> raw;
    if (header->is_compressed()) {
        auto decompressed = lz4_decompress_size_prepended(buf.data() + 32, buf.size() - 32);
        if (!decompressed) return {};
        raw = std::move(*decompressed);
    } else {
        raw.assign(buf.begin() + 32, buf.end());
    }

    std::vector<Row> rows;
    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < header->row_count; i++) {
        if (pos + 4 > raw.size()) break;
        std::uint32_t len = static_cast<std::uint32_t>(raw[pos]) | (static_cast<std::uint32_t>(raw[pos + 1]) << 8) |
                             (static_cast<std::uint32_t>(raw[pos + 2]) << 16) | (static_cast<std::uint32_t>(raw[pos + 3]) << 24);
        pos += 4;
        if (pos + len > raw.size()) break;
        std::string json(reinterpret_cast<const char*>(raw.data() + pos), len);
        try {
            rows.push_back(nlohmann::json::parse(json).get<Row>());
        } catch (...) {
            // matches Rust's `.unwrap_or("{}")` + `if let Ok(row) = ...` — skip malformed rows
        }
        pos += len;
    }
    return rows;
}

std::vector<Row> DiskManager::load_table(const std::string& table) const {
    auto [db, tbl] = parse_key(table);
    std::string rdb_path = table_dir(db) + "/" + tbl + ".rdb";
    if (fs::exists(rdb_path)) return load_rdb(rdb_path);

    if (db == "rusql") {
        std::string flat_rdb = data_dir_ + "/" + tbl + ".rdb";
        if (fs::exists(flat_rdb)) return load_rdb(flat_rdb);
        std::string flat_json = data_dir_ + "/" + tbl + ".json";
        if (fs::exists(flat_json)) {
            std::string content;
            read_file(flat_json, content);
            try {
                return nlohmann::json::parse(content).get<std::vector<Row>>();
            } catch (...) {
                return {};
            }
        }
    }
    return {};
}

void DiskManager::delete_table(const std::string& table) const {
    auto [db, tbl] = parse_key(table);
    std::string dir = table_dir(db);
    std::error_code ec;
    fs::remove(dir + "/" + tbl + ".rdb", ec);
    fs::remove(dir + "/" + tbl + ".json", ec);
    fs::remove(dir + "/" + tbl + ".schema.json", ec);
    if (db == "rusql") {
        fs::remove(data_dir_ + "/" + tbl + ".rdb", ec);
        fs::remove(data_dir_ + "/" + tbl + ".json", ec);
        fs::remove(data_dir_ + "/" + tbl + ".schema.json", ec);
    }
}

std::vector<std::string> DiskManager::list_tables() const {
    std::vector<std::string> tables;
    std::error_code ec;
    if (!fs::exists(data_dir_, ec)) return tables;
    for (auto& entry : fs::directory_iterator(data_dir_, ec)) {
        std::string name = entry.path().filename().string();
        if (entry.is_directory()) {
            std::string db = name;
            std::error_code ec2;
            for (auto& sub : fs::directory_iterator(entry.path(), ec2)) {
                std::string fname = sub.path().filename().string();
                const std::string suffix = ".schema.json";
                if (fname.size() > suffix.size() && fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    std::string tbl = fname.substr(0, fname.size() - suffix.size());
                    tables.push_back(db + "." + tbl);
                }
            }
        } else {
            const std::string suffix = ".schema.json";
            if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                std::string tbl = name.substr(0, name.size() - suffix.size());
                std::string qualified = "rusql." + tbl;
                if (std::find(tables.begin(), tables.end(), qualified) == tables.end()) tables.push_back(qualified);
            }
        }
    }
    return tables;
}

void DiskManager::save_btree_index(const std::string& key, const BPlusTree& tree) const {
    auto [db, name] = parse_key(key);
    ensure_db_dir(db);
    std::string path = table_dir(db) + "/" + name + ".idx";
    nlohmann::json j = tree;
    write_file(path, j.dump());
}

std::optional<BPlusTree> DiskManager::load_btree_index(const std::string& key) const {
    auto [db, name] = parse_key(key);
    std::string path = table_dir(db) + "/" + name + ".idx";
    std::string content;
    if (!read_file(path, content)) return std::nullopt;
    try {
        return nlohmann::json::parse(content).get<BPlusTree>();
    } catch (...) {
        return std::nullopt;
    }
}

void DiskManager::delete_btree_index(const std::string& key) const {
    auto [db, name] = parse_key(key);
    std::error_code ec;
    fs::remove(table_dir(db) + "/" + name + ".idx", ec);
}

void DiskManager::save_index_meta(const std::string& db, const std::vector<IndexMeta>& meta_list) const {
    ensure_db_dir(db);
    std::string path = table_dir(db) + "/indexes.json";
    nlohmann::json j = meta_list;
    write_file(path, j.dump(2));
}

std::vector<IndexMeta> DiskManager::load_index_meta(const std::string& db) const {
    std::string path = table_dir(db) + "/indexes.json";
    std::string flat = data_dir_ + "/indexes.json";
    std::string chosen;
    if (fs::exists(path)) chosen = path;
    else if (db == "rusql" && fs::exists(flat)) chosen = flat;
    else return {};
    std::string content;
    read_file(chosen, content);
    try {
        return nlohmann::json::parse(content).get<std::vector<IndexMeta>>();
    } catch (...) {
        return {};
    }
}

void DiskManager::save_views(const std::string& db, const std::unordered_map<std::string, Statement>& views) const {
    ensure_db_dir(db);
    nlohmann::json j = views;
    write_file(table_dir(db) + "/views.json", j.dump(2));
}

std::unordered_map<std::string, Statement> DiskManager::load_views(const std::string& db) const {
    std::string path = table_dir(db) + "/views.json";
    std::string flat = data_dir_ + "/views.json";
    std::string chosen;
    if (fs::exists(path)) chosen = path;
    else if (db == "rusql" && fs::exists(flat)) chosen = flat;
    else return {};
    std::string content;
    read_file(chosen, content);
    try {
        return nlohmann::json::parse(content).get<std::unordered_map<std::string, Statement>>();
    } catch (...) {
        return {};
    }
}

void DiskManager::save_view_raw_sql(const std::string& db, const std::unordered_map<std::string, std::string>& view_sql) const {
    ensure_db_dir(db);
    nlohmann::json j = view_sql;
    write_file(table_dir(db) + "/view_sql.json", j.dump(2));
}

std::unordered_map<std::string, std::string> DiskManager::load_view_raw_sql(const std::string& db) const {
    std::string path = table_dir(db) + "/view_sql.json";
    if (!fs::exists(path)) return {};
    std::string content;
    read_file(path, content);
    try {
        return nlohmann::json::parse(content).get<std::unordered_map<std::string, std::string>>();
    } catch (...) {
        return {};
    }
}

bool DiskManager::read_text_file(const std::string& path, std::string& out) const { return read_file(path, out); }

void DiskManager::write_text_file(const std::string& path, const std::string& content) const {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    write_file(path, content);
}

void DiskManager::migrate_sys_json_path(const std::string& new_path, const std::string& old_path) const {
    std::error_code ec;
    fs::create_directories(sys_dir(), ec);
    if (!fs::exists(new_path) && fs::exists(old_path)) {
        fs::copy_file(old_path, new_path, ec);
        fs::remove(old_path, ec);
    }
}

} // namespace engine
