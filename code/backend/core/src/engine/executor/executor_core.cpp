// Faithful port of the shared/session infrastructure in rusql-core/src/engine/executor.rs:
// UserRecord/GrantRecord/RoleRecord/RoleGrant JSON, SharedDatabase auth helpers, the
// Executor constructors (new_with_options / new_session), process-list bookkeeping,
// qualify_name/qualify_stmt/qualify_condexpr, and execute()/execute_sql()/execute_with_s()
// (the last with only the Phase 8a DDL cases wired up so far).

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <thread>

#include <picosha2/picosha2.h>
#include <sha1/sha1.hpp>

#include "engine/parser/parser.hpp"
#include "engine/thread_pool.hpp"

namespace engine {

// ---------------------------------------------------------------------------
// Password hashing helpers (native TCP auth: SHA-256; MySQL wire protocol: SHA1)
// ---------------------------------------------------------------------------
namespace {

std::string sha1_hex(const std::string& data) {
    SHA1 hasher;
    hasher.update(data);
    return hasher.final();
}

std::vector<std::uint8_t> hex_decode(const std::string& hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

} // namespace

std::string hash_password(const std::string& password) { return picosha2::hash256_hex_string(password); }

// SHA1(SHA1(password)) hex — mysql_native_password verification.
std::string mysql_native_hash_compute(const std::string& password) {
    auto first_bytes = hex_decode(sha1_hex(password));
    std::string first_str(first_bytes.begin(), first_bytes.end());
    return sha1_hex(first_str);
}

void to_json(nlohmann::json& j, const UserRecord& u) {
    j = nlohmann::json{{"user", u.user}, {"host", u.host}, {"password_hash", u.password_hash}, {"mysql_native_hash", u.mysql_native_hash}};
}

void from_json(const nlohmann::json& j, UserRecord& u) {
    j.at("user").get_to(u.user);
    j.at("host").get_to(u.host);
    j.at("password_hash").get_to(u.password_hash);
    if (j.contains("mysql_native_hash")) j.at("mysql_native_hash").get_to(u.mysql_native_hash);
    else u.mysql_native_hash = std::nullopt;
}

void to_json(nlohmann::json& j, const GrantRecord& g) {
    j = nlohmann::json{{"user", g.user},
                       {"host", g.host},
                       {"object_type", g.object_type},
                       {"object", g.object},
                       {"privileges", g.privileges},
                       {"with_grant_option", g.with_grant_option}};
}

void from_json(const nlohmann::json& j, GrantRecord& g) {
    j.at("user").get_to(g.user);
    j.at("host").get_to(g.host);
    j.at("object_type").get_to(g.object_type);
    j.at("object").get_to(g.object);
    j.at("privileges").get_to(g.privileges);
    j.at("with_grant_option").get_to(g.with_grant_option);
}

void to_json(nlohmann::json& j, const RoleRecord& r) { j = nlohmann::json{{"name", r.name}}; }

void from_json(const nlohmann::json& j, RoleRecord& r) { j.at("name").get_to(r.name); }

void to_json(nlohmann::json& j, const RoleGrant& rg) {
    j = nlohmann::json{{"role", rg.role}, {"user", rg.user}, {"host", rg.host}, {"with_admin_option", rg.with_admin_option}};
}

void from_json(const nlohmann::json& j, RoleGrant& rg) {
    j.at("role").get_to(rg.role);
    j.at("user").get_to(rg.user);
    j.at("host").get_to(rg.host);
    j.at("with_admin_option").get_to(rg.with_admin_option);
}

bool SharedDatabase::validate_credentials(const std::string& user, const std::string& password) const {
    if (users.empty()) return true;
    for (auto& u : users) {
        if (u.user != user) continue;
        if (!u.password_hash.has_value()) {
            if (password.empty()) return true;
            continue;
        }
        std::string hashed = hash_password(password);
        if (*u.password_hash == hashed || *u.password_hash == password) return true;
    }
    return false;
}

bool SharedDatabase::verify_mysql_native_password(const std::string& user, const std::vector<std::uint8_t>& nonce,
                                                   const std::vector<std::uint8_t>& auth_response) const {
    if (users.empty()) return true; // open mode

    const UserRecord* record = nullptr;
    for (auto& u : users) {
        if (u.user == user && (u.host == "%" || u.host == "localhost" || u.host == "127.0.0.1" || u.host == "::1")) {
            record = &u;
            break;
        }
    }
    if (!record) return false;

    if (!record->mysql_native_hash.has_value() && !record->password_hash.has_value()) {
        return auth_response.empty();
    }
    if (!record->mysql_native_hash.has_value()) return false; // SHA-256-only legacy account

    auto stored = hex_decode(*record->mysql_native_hash);
    if (stored.size() != 20 || auth_response.size() < 20) return false;

    std::string concat(nonce.begin(), nonce.end());
    concat.append(stored.begin(), stored.end());
    auto xor_key = hex_decode(sha1_hex(concat));

    std::vector<std::uint8_t> claimed(20);
    for (std::size_t i = 0; i < 20; i++) claimed[i] = auth_response[i] ^ xor_key[i];
    std::string claimed_str(claimed.begin(), claimed.end());
    auto verified = hex_decode(sha1_hex(claimed_str));
    return verified == stored;
}

void SharedDatabase::migrate_mysql_hash(const std::string& user, const std::string& password) {
    bool needs = false;
    for (auto& u : users) {
        if (u.user == user && !u.mysql_native_hash.has_value()) { needs = true; break; }
    }
    if (!needs) return;
    for (auto& u : users) {
        if (u.user == user) {
            u.mysql_native_hash = mysql_native_hash_compute(password);
            break;
        }
    }
    disk.save_users(users);
}

bool SharedDatabase::ensure_default_user() {
    if (!users.empty()) return false;
    UserRecord root;
    root.user = "root";
    root.host = "%";
    root.password_hash = hash_password("root");
    root.mysql_native_hash = mysql_native_hash_compute("root");
    users.push_back(root);
    disk.save_users(users);
    return true;
}

// ---------------------------------------------------------------------------
// Executor construction / session management
// ---------------------------------------------------------------------------

Executor::Executor(const std::string& dir, std::size_t buffer_pool_capacity) {
    DiskManager disk(dir);
    Catalog catalog;
    std::unordered_map<std::string, std::vector<Row>> tables;
    std::unordered_map<std::string, BPlusTree> indexes;

    std::unordered_set<std::string> databases;
    for (auto& db : disk.list_databases()) {
        std::string lower = db;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        databases.insert(lower);
    }

    // Tables whose PK B+Tree wasn't found persisted on disk need a from-scratch rebuild;
    // collected here (instead of rebuilt inline) so Phase 2 below can do it in parallel,
    // matching the Rust original's into_par_iter() over rebuild-needed tables.
    struct RebuildEntry {
        std::string key;
        std::string first_col;
        const std::vector<Row>* rows;
    };
    std::vector<RebuildEntry> rebuild_needed;

    for (auto& qualified_key : disk.list_tables()) {
        auto schema_opt = disk.load_schema(qualified_key);
        if (!schema_opt) continue;
        TableSchema schema = std::move(*schema_opt);
        auto [db, tbl] = split_key(qualified_key);
        (void)tbl;
        std::string db_lower = db;
        std::transform(db_lower.begin(), db_lower.end(), db_lower.begin(), [](unsigned char c) { return std::tolower(c); });
        databases.insert(db_lower);

        for (auto& col : schema.columns) {
            if (col.foreign_key && col.foreign_key->ref_table.find('.') == std::string::npos) {
                col.foreign_key->ref_table = db + "." + col.foreign_key->ref_table;
            }
        }

        auto auto_inc_counters = schema.auto_increment_counters;
        catalog.create_table_full(qualified_key, schema.columns, schema.primary_key_columns, schema.check_constraints);
        if (auto* ts = catalog.get_table_mut(qualified_key)) ts->auto_increment_counters = auto_inc_counters;

        auto rows = disk.load_table(qualified_key);
        auto loaded_index = disk.load_btree_index(qualified_key);
        bool needs_rebuild = !loaded_index.has_value();
        if (loaded_index) indexes.insert({qualified_key, std::move(*loaded_index)});

        auto [tit, inserted] = tables.insert({qualified_key, std::move(rows)});
        (void)inserted;
        if (needs_rebuild) {
            std::string first_col = schema.columns.empty() ? std::string() : schema.columns.front().name;
            rebuild_needed.push_back({qualified_key, std::move(first_col), &tit->second});
        }
    }

    // ── Phase 2: PK 인덱스 재빌드 병렬화 (영속화 인덱스가 없는 테이블만) ──────
    if (!rebuild_needed.empty()) {
        std::vector<BPlusTree> built(rebuild_needed.size());
        ThreadPool::global().parallel_for(rebuild_needed.size(), [&](std::size_t i) {
            auto& entry = rebuild_needed[i];
            BPlusTree tree;
            if (!entry.first_col.empty()) {
                for (auto& row : *entry.rows) {
                    auto it = row.find(entry.first_col);
                    if (it != row.end()) {
                        nlohmann::json j = row;
                        tree.insert(it->second, j.dump());
                    }
                }
            }
            built[i] = std::move(tree);
        });
        for (std::size_t i = 0; i < rebuild_needed.size(); i++) {
            indexes.insert({rebuild_needed[i].key, std::move(built[i])});
        }
    }

    std::unordered_map<std::string, Statement> views;
    std::unordered_map<std::string, std::string> view_raw_sql;
    for (auto& db : databases) {
        for (auto& [k, v] : disk.load_views(db)) {
            std::string qk = (k.find('.') != std::string::npos) ? k : (db + "." + k);
            views.insert({qk, v});
        }
        for (auto& [k, v] : disk.load_view_raw_sql(db)) {
            std::string qk = (k.find('.') != std::string::npos) ? k : (db + "." + k);
            view_raw_sql.insert({qk, v});
        }
    }

    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;

    // 보조 인덱스 재빌드 작업 수집: 영속화 인덱스가 없는 것만 Phase 3에서 병렬 처리.
    struct SecIdxWork {
        std::string index_key, meta_name, q_table, column;
        std::vector<Row> rows;
    };
    std::vector<SecIdxWork> sec_rebuild_work;

    for (auto& db : databases) {
        for (auto& meta : disk.load_index_meta(db)) {
            std::string q_table = (meta.table.find('.') != std::string::npos) ? meta.table : (db + "." + meta.table);
            if (meta.index_type == "hash" && meta.columns.size() == 1) {
                const std::string& column = meta.columns.front();
                HashIndex hi(q_table, column);
                if (auto it = tables.find(q_table); it != tables.end()) hi.rebuild(it->second);
                hash_indexes.insert({q_table + "_" + meta.name, std::move(hi)});
                hash_index_meta.insert({meta.name, {q_table, column}});
            } else if (meta.columns.size() == 1) {
                const std::string& column = meta.columns.front();
                std::string key = q_table + "_" + meta.name;
                if (auto tree = disk.load_btree_index(key)) {
                    indexes.insert({key, std::move(*tree)});
                    index_meta.insert({meta.name, {q_table, column}});
                } else {
                    std::vector<Row> rows;
                    if (auto it = tables.find(q_table); it != tables.end()) rows = it->second;
                    sec_rebuild_work.push_back({std::move(key), meta.name, q_table, column, std::move(rows)});
                    continue;
                }
            } else {
                CompositeIndex comp(q_table, meta.columns);
                if (auto it = tables.find(q_table); it != tables.end()) comp.rebuild(it->second);
                composite_indexes.insert({meta.name, std::move(comp)});
            }
        }
    }

    // ── Phase 3: 보조 인덱스 재빌드 병렬화 (영속화 인덱스가 없는 것만) ──────
    if (!sec_rebuild_work.empty()) {
        std::vector<BPlusTree> built(sec_rebuild_work.size());
        ThreadPool::global().parallel_for(sec_rebuild_work.size(), [&](std::size_t i) {
            auto& work = sec_rebuild_work[i];
            std::unordered_map<std::string, std::vector<Row>> bucket;
            for (auto& row : work.rows) {
                if (auto vit = row.find(work.column); vit != row.end()) bucket[vit->second].push_back(row);
            }
            BPlusTree t;
            for (auto& [k, bucket_rows] : bucket) {
                nlohmann::json j = bucket_rows;
                t.insert(k, j.dump());
            }
            built[i] = std::move(t);
        });
        for (std::size_t i = 0; i < sec_rebuild_work.size(); i++) {
            auto& work = sec_rebuild_work[i];
            indexes.insert({work.index_key, std::move(built[i])});
            index_meta.insert({work.meta_name, {work.q_table, work.column}});
        }
    }

    auto users = disk.load_users<std::vector<UserRecord>>();
    auto grants = disk.load_grants<std::vector<GrantRecord>>();
    auto roles = disk.load_roles<std::vector<RoleRecord>>();
    auto role_grants = disk.load_role_grants<std::vector<RoleGrant>>();
    auto synonyms = disk.load_synonyms<std::unordered_map<std::string, std::string>>();
    auto procedures = disk.load_procedures<std::unordered_map<std::string, ProcedureDef>>();
    auto triggers = disk.load_triggers<std::unordered_map<std::string, TriggerDef>>();
    auto user_functions = disk.load_functions<std::unordered_map<std::string, UserFunctionDef>>();

    std::string current_db_val =
        databases.empty() ? std::string("rusql") : *std::min_element(databases.begin(), databases.end());
    auto txn_io = std::make_shared<TxnIoShared>();

    SharedDatabase db_value{
        .catalog = std::move(catalog),
        .tables = std::move(tables),
        .indexes = std::move(indexes),
        .index_meta = std::move(index_meta),
        .composite_indexes = std::move(composite_indexes),
        .hash_indexes = std::move(hash_indexes),
        .hash_index_meta = std::move(hash_index_meta),
        .views = std::move(views),
        .view_raw_sql = std::move(view_raw_sql),
        .buffer_pool = BufferPool(buffer_pool_capacity),
        .disk = std::move(disk),
        .lock_mgr = LockManager(),
        .databases = std::move(databases),
        .users = std::move(users),
        .grants = std::move(grants),
        .roles = std::move(roles),
        .role_grants = std::move(role_grants),
        .synonyms = std::move(synonyms),
        .group_commit_coord = std::make_shared<GroupCommitCoordinator>(dir + "/rusql.wal"),
        .data_dir = dir,
        .table_stats = {},
        .procedures = std::move(procedures),
        .triggers = std::move(triggers),
        .dml_since_vacuum = 0,
        .dml_since_analyze = {},
        .user_functions = std::move(user_functions),
        .process_list = std::make_shared<Mutex<std::unordered_map<std::size_t, ProcessInfo>>>(),
        .next_session_id = std::make_shared<std::atomic<std::size_t>>(1),
        .row_pk_pos = {},
        .query_cache = QueryResultCache(),
        .txn_io = txn_io,
        .active_txn_ids = std::make_shared<Mutex<std::unordered_set<std::uint64_t>>>(),
    };

    shared = std::make_shared<RwLock<SharedDatabase>>(std::move(db_value));
    txn = TransactionManager(dir, txn_io);
    current_db = current_db_val;
    session_id = 0;
    lock_wait_timeout_ms = 50000;

    recover_from_wal();
}

Executor::Executor(SessionTag, std::shared_ptr<RwLock<SharedDatabase>> shared_db, std::string dir,
                    std::shared_ptr<TxnIoShared> txn_io_shared, std::size_t sess_id, std::string db)
    : shared(std::move(shared_db)), txn(dir, std::move(txn_io_shared)), current_db(std::move(db)), session_id(sess_id) {}

Executor Executor::new_session(std::shared_ptr<RwLock<SharedDatabase>> shared_db) {
    std::string current_db_val;
    std::string data_dir_val;
    std::size_t session_id_val;
    std::shared_ptr<TxnIoShared> txn_io_val;
    {
        auto s = shared_db->read();
        current_db_val = s->databases.empty() ? std::string("rusql") : *std::min_element(s->databases.begin(), s->databases.end());
        data_dir_val = s->data_dir;
        session_id_val = s->next_session_id->fetch_add(1);
        txn_io_val = s->txn_io;
    }
    return Executor(SessionTag{}, std::move(shared_db), data_dir_val, std::move(txn_io_val), session_id_val, current_db_val);
}

void Executor::register_process(const std::string& user, const std::string& host) const {
    auto now = std::chrono::steady_clock::now();
    auto s = shared->read();
    ProcessInfo info;
    info.id = session_id;
    info.user = user;
    info.host = host;
    info.db = current_db;
    info.command = "Sleep";
    info.connected_at = now;
    info.state_since = now;
    (*s->process_list->lock())[session_id] = std::move(info);
}

void Executor::update_process_command(const std::string& command, const std::string& info) const {
    auto s = shared->read();
    auto list = s->process_list->lock();
    auto it = list->find(session_id);
    if (it != list->end()) {
        it->second.command = command;
        it->second.info = info.substr(0, 100);
        it->second.state_since = std::chrono::steady_clock::now();
        it->second.db = current_db;
    }
}

void Executor::deregister_process() const {
    auto s = shared->read();
    s->process_list->lock()->erase(session_id);
}

// ---------------------------------------------------------------------------
// Naming/qualification helpers
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> Executor::split_key(const std::string& key) {
    auto pos = key.find('.');
    if (pos != std::string::npos) return {key.substr(0, pos), key.substr(pos + 1)};
    return {"rusql", key};
}

std::string Executor::qualify_name(const std::string& name) const {
    if (name.find('.') != std::string::npos) return name;
    return current_db + "." + name;
}

std::string Executor::qualify_name_with_synonyms(const SharedDatabase& s, const std::string& name) const {
    std::string resolved = name;
    if (auto it = s.synonyms.find(name); it != s.synonyms.end()) resolved = it->second;
    if (resolved.find('.') != std::string::npos) return resolved;
    return current_db + "." + resolved;
}

std::string Executor::strip_db_prefix(const std::string& name) {
    auto pos = name.rfind('.');
    return pos == std::string::npos ? name : name.substr(pos + 1);
}

std::optional<CondExpr> Executor::merge_conditions(std::optional<CondExpr> a, std::optional<CondExpr> b) {
    if (!a) return b;
    if (!b) return a;
    return CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(*a)), std::make_unique<CondExpr>(std::move(*b))});
}

std::string Executor::display_name(const std::string& key) const {
    std::string prefix = current_db + ".";
    if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0) return key.substr(prefix.size());
    return key;
}

std::string Executor::qualify_static(const std::string& name, const std::string& current_db_val) {
    if (name.find('.') != std::string::npos) return name;
    return current_db_val + "." + name;
}

std::vector<std::string> Executor::select_tables(const Statement& stmt, const std::string& current_db_val) {
    std::vector<std::string> result;
    if (auto* v = std::get_if<Statement::Select>(&stmt.data)) {
        result.push_back(qualify_static(v->table, current_db_val));
        for (auto& j : v->joins) result.push_back(qualify_static(j.table, current_db_val));
    }
    return result;
}

// ---------------------------------------------------------------------------
// execute() / execute_sql() / execute_with_s()
// ---------------------------------------------------------------------------

StringResult Executor::execute(Statement stmt) {
    subquery_cache_.clear();
    if (std::holds_alternative<Statement::Commit>(stmt.data)) return execute_commit_grouped();
    auto s = shared->write();
    return execute_with_s(*s, std::move(stmt));
}

namespace {
bool ieq_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string to_ascii_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0, pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}
} // namespace

StringResult Executor::execute_sql(const std::string& sql) {
    std::string trimmed = trim(sql);

    bool looks_like_select = trimmed.size() >= 6 && ieq_ascii(trimmed.substr(0, 6), "select");
    bool in_txn = txn.current_txn_id() != 0;

    if (looks_like_select && !in_txn) {
        std::string cache_key = current_db + "::" + trimmed;
        auto s = shared->read();
        if (const std::string* cached = s->query_cache.get(cache_key)) return StringResult::Ok(*cached);
    }

    Parser parser(trimmed);
    auto parsed = parser.parse();
    if (parsed.is_err()) return StringResult::Err(parsed.error());
    Statement stmt = std::move(parsed).value();

    std::optional<std::string> dml_table;
    if (auto* v = std::get_if<Statement::Insert>(&stmt.data)) dml_table = qualify_static(v->table, current_db);
    else if (auto* v = std::get_if<Statement::InsertSelect>(&stmt.data)) dml_table = qualify_static(v->table, current_db);
    else if (auto* v = std::get_if<Statement::Update>(&stmt.data)) dml_table = qualify_static(v->table, current_db);
    else if (auto* v = std::get_if<Statement::Delete>(&stmt.data)) dml_table = qualify_static(v->table, current_db);
    else if (auto* v = std::get_if<Statement::TruncateTable>(&stmt.data)) dml_table = qualify_static(v->name, current_db);
    else if (auto* v = std::get_if<Statement::DropTable>(&stmt.data)) dml_table = qualify_static(v->name, current_db);
    else if (auto* v = std::get_if<Statement::MultiUpdate>(&stmt.data)) {
        if (!v->tables.empty()) dml_table = qualify_static(v->tables.front(), current_db);
    } else if (auto* v = std::get_if<Statement::MultiDelete>(&stmt.data)) {
        if (!v->delete_tables.empty()) dml_table = qualify_static(v->delete_tables.front(), current_db);
    } else if (auto* v = std::get_if<Statement::Merge>(&stmt.data)) dml_table = qualify_static(v->target, current_db);
    else if (auto* v = std::get_if<Statement::AlterTable>(&stmt.data)) dml_table = qualify_static(v->table, current_db);

    std::vector<std::string> cache_tables;
    if (looks_like_select && !in_txn) cache_tables = select_tables(stmt, current_db);

    bool has_subquery = looks_like_select && count_occurrences(to_ascii_lower(trimmed), "select") > 1;

    StringResult result = execute(std::move(stmt));

    if (looks_like_select && !in_txn && !has_subquery && result.is_ok()) {
        std::string cache_key = current_db + "::" + trimmed;
        auto s = shared->write();
        s->query_cache.put(cache_key, result.value(), cache_tables);
    }
    if (dml_table && result.is_ok()) {
        auto s = shared->write();
        s->query_cache.invalidate_table(*dml_table);
    }
    return result;
}

StringResult Executor::execute_with_s(SharedDatabase& s, Statement stmt) {
    sync_udf_context(s.user_functions, current_db);

    if (auto* v = std::get_if<Statement::Use>(&stmt.data)) return exec_use(s, v->database);
    if (auto* v = std::get_if<Statement::CreateDatabase>(&stmt.data)) return exec_create_database(s, v->name, v->if_not_exists);
    if (auto* v = std::get_if<Statement::DropDatabase>(&stmt.data)) return exec_drop_database(s, v->name, v->if_exists);

    // User management / privileges — qualification not needed
    if (auto* v = std::get_if<Statement::CreateUser>(&stmt.data)) return exec_create_user(s, v->user, v->host, v->password, v->if_not_exists);
    if (auto* v = std::get_if<Statement::DropUser>(&stmt.data)) return exec_drop_user(s, v->user, v->host, v->if_exists);
    if (auto* v = std::get_if<Statement::Grant>(&stmt.data))
        return exec_grant(s, v->privileges, v->object_type, v->object, v->user, v->host, v->with_grant_option);
    if (auto* v = std::get_if<Statement::Revoke>(&stmt.data)) return exec_revoke(s, v->privileges, v->object_type, v->object, v->user, v->host);
    if (auto* v = std::get_if<Statement::ShowGrants>(&stmt.data)) return exec_show_grants(s, v->user, v->host);
    if (std::holds_alternative<Statement::ShowDatabases>(stmt.data)) return exec_show_databases(s);
    // ROLE 관리
    if (auto* v = std::get_if<Statement::CreateRole>(&stmt.data)) return exec_create_role(s, v->name);
    if (auto* v = std::get_if<Statement::DropRole>(&stmt.data)) return exec_drop_role(s, v->name, v->if_exists);
    if (auto* v = std::get_if<Statement::GrantRole>(&stmt.data)) return exec_grant_role(s, v->role, v->user, v->host, v->with_admin_option);
    if (auto* v = std::get_if<Statement::RevokeRole>(&stmt.data)) return exec_revoke_role(s, v->role, v->user, v->host);
    if (std::holds_alternative<Statement::ShowRoles>(stmt.data)) return exec_show_roles(s);
    // SYNONYM 관리
    if (auto* v = std::get_if<Statement::CreateSynonym>(&stmt.data)) return exec_create_synonym(s, v->name, v->target, v->or_replace);
    if (auto* v = std::get_if<Statement::DropSynonym>(&stmt.data)) return exec_drop_synonym(s, v->name, v->if_exists);
    if (std::holds_alternative<Statement::ShowSynonyms>(stmt.data)) return exec_show_synonyms(s);

    stmt = qualify_stmt(s, std::move(stmt));

    if (std::holds_alternative<Statement::Begin>(stmt.data)) return exec_begin(s);
    if (std::holds_alternative<Statement::Commit>(stmt.data)) return exec_commit(s);
    if (std::holds_alternative<Statement::Rollback>(stmt.data)) return exec_rollback(s);
    if (auto* v = std::get_if<Statement::Savepoint>(&stmt.data)) return exec_savepoint(v->name);
    if (auto* v = std::get_if<Statement::ReleaseSavepoint>(&stmt.data)) return exec_release_savepoint(v->name);
    if (auto* v = std::get_if<Statement::RollbackTo>(&stmt.data)) return exec_rollback_to(s, v->name);
    if (std::holds_alternative<Statement::ShowBufferPool>(stmt.data)) return exec_show_buffer_pool(s);
    if (std::holds_alternative<Statement::ShowWal>(stmt.data)) return exec_show_wal();
    if (std::holds_alternative<Statement::Checkpoint>(stmt.data)) return exec_checkpoint(s);
    if (auto* v = std::get_if<Statement::SetIsolationLevel>(&stmt.data)) return exec_set_isolation_level(v->level);
    if (std::holds_alternative<Statement::ShowIsolationLevel>(stmt.data)) return exec_show_isolation_level();
    if (std::holds_alternative<Statement::ShowLocks>(stmt.data)) return exec_show_locks(s);

    if (auto* v = std::get_if<Statement::CreateTable>(&stmt.data))
        return exec_create(s, v->name, v->columns, v->if_not_exists, v->primary_key_columns, v->check_constraints);
    if (auto* v = std::get_if<Statement::DropTable>(&stmt.data)) return exec_drop(s, v->name, v->if_exists);
    if (auto* v = std::get_if<Statement::TruncateTable>(&stmt.data)) return exec_truncate(s, v->name);
    if (auto* v = std::get_if<Statement::AlterTable>(&stmt.data)) return exec_alter(s, v->table, v->action);
    if (auto* v = std::get_if<Statement::CreateIndex>(&stmt.data))
        return exec_create_index(s, v->index_name, v->table, v->columns, v->using_hash);
    if (auto* v = std::get_if<Statement::DropIndex>(&stmt.data)) return exec_drop_index(s, v->index_name);
    if (auto* v = std::get_if<Statement::CreateView>(&stmt.data)) return exec_create_view(s, v->name, *v->query, v->raw_sql);
    if (auto* v = std::get_if<Statement::DropView>(&stmt.data)) return exec_drop_view(s, v->name);
    if (std::holds_alternative<Statement::ShowTables>(stmt.data)) return exec_show_tables(s);
    if (auto* v = std::get_if<Statement::Describe>(&stmt.data)) return exec_describe(s, v->table);
    if (auto* v = std::get_if<Statement::ShowCreateTable>(&stmt.data)) return exec_show_create_table(s, v->table);
    if (auto* v = std::get_if<Statement::ShowCreateView>(&stmt.data)) return exec_show_create_view(s, v->view);
    if (auto* v = std::get_if<Statement::ShowIndex>(&stmt.data)) return exec_show_index(s, v->table);
    if (std::holds_alternative<Statement::ShowProcessList>(stmt.data)) return exec_show_processlist(s);
    if (auto* v = std::get_if<Statement::Vacuum>(&stmt.data)) return exec_vacuum(s, v->table);
    if (auto* v = std::get_if<Statement::AnalyzeTable>(&stmt.data)) return exec_analyze_table(s, v->table);
    if (auto* v = std::get_if<Statement::Explain>(&stmt.data)) return exec_explain(s, std::move(*v->inner));
    if (auto* v = std::get_if<Statement::ExplainAnalyze>(&stmt.data)) return exec_explain_analyze(s, std::move(*v->inner));
    if (auto* v = std::get_if<Statement::Backup>(&stmt.data)) return exec_backup(s, v->database, v->output_file);
    if (auto* v = std::get_if<Statement::Restore>(&stmt.data)) return exec_restore(s, v->source_file, v->database);
    if (auto* v = std::get_if<Statement::MultiUpdate>(&stmt.data)) return exec_multi_update(s, v->tables, v->joins, v->assignments, v->condition);
    if (auto* v = std::get_if<Statement::MultiDelete>(&stmt.data))
        return exec_multi_delete(s, v->delete_tables, v->from_table, v->joins, v->condition);
    if (auto* v = std::get_if<Statement::Merge>(&stmt.data))
        return exec_merge(s, v->target, v->target_alias, v->source, v->source_alias, v->on, v->when_matched_update, v->when_matched_delete,
                           v->when_matched_delete_cond, v->when_not_matched_columns, v->when_not_matched_values);
    if (auto* v = std::get_if<Statement::Insert>(&stmt.data))
        return exec_insert(s, v->table, v->columns, v->values, v->on_conflict, v->returning);
    if (auto* v = std::get_if<Statement::InsertSelect>(&stmt.data))
        return exec_insert_select(s, v->table, v->columns, std::move(*v->query), v->on_conflict, v->returning);
    if (auto* v = std::get_if<Statement::Update>(&stmt.data)) return exec_update(s, v->table, v->assignments, v->condition, v->returning);
    if (auto* v = std::get_if<Statement::Delete>(&stmt.data)) return exec_delete(s, v->table, v->condition, v->returning);
    if (auto* v = std::get_if<Statement::Select>(&stmt.data)) {
        return exec_select(s, v->table, std::move(v->subquery), v->distinct, v->columns, v->condition, v->joins, v->order_by, v->group_by,
                            v->having, v->limit, v->offset, v->for_update, v->for_share);
    }
    if (auto* v = std::get_if<Statement::With>(&stmt.data)) return exec_with(s, std::move(v->ctes), std::move(*v->query), v->recursive);
    if (auto* v = std::get_if<Statement::Union>(&stmt.data))
        return exec_union(s, std::move(*v->left), std::move(*v->right), v->all, v->order_by, v->limit, v->offset);
    if (auto* v = std::get_if<Statement::Intersect>(&stmt.data))
        return exec_intersect(s, std::move(*v->left), std::move(*v->right), v->all, v->order_by, v->limit, v->offset);
    if (auto* v = std::get_if<Statement::Except>(&stmt.data))
        return exec_except(s, std::move(*v->left), std::move(*v->right), v->all, v->order_by, v->limit, v->offset);

    if (auto* v = std::get_if<Statement::CreateProcedure>(&stmt.data))
        return exec_create_procedure(s, v->name, v->params, std::move(v->body));
    if (auto* v = std::get_if<Statement::CallProcedure>(&stmt.data)) return exec_call_procedure(s, v->name, v->args);
    if (auto* v = std::get_if<Statement::CreateTrigger>(&stmt.data))
        return exec_create_trigger(s, v->name, v->timing, v->event, v->table, std::move(v->body));
    if (auto* v = std::get_if<Statement::DropTrigger>(&stmt.data)) return exec_drop_trigger(s, v->name, v->if_exists);
    if (auto* v = std::get_if<Statement::DropProcedure>(&stmt.data)) return exec_drop_procedure(s, v->name, v->if_exists);
    if (auto* v = std::get_if<Statement::CreateFunction>(&stmt.data)) return exec_create_function(s, v->name, v->params, v->body);
    if (auto* v = std::get_if<Statement::DropFunction>(&stmt.data)) return exec_drop_function(s, v->name, v->if_exists);

    // 저장 프로시저 제어문
    if (auto* v = std::get_if<Statement::ProcDeclare>(&stmt.data)) {
        proc_vars[v->name] = v->default_value.value_or("NULL");
        return StringResult::Ok("");
    }
    if (auto* v = std::get_if<Statement::ProcSet>(&stmt.data)) {
        proc_vars[v->name] = eval_arith(proc_vars, v->expr);
        return StringResult::Ok("");
    }
    if (auto* v = std::get_if<Statement::ProcIf>(&stmt.data))
        return exec_proc_if(s, v->condition, std::move(v->then_body), std::move(v->elseif_branches), std::move(v->else_body));
    if (auto* v = std::get_if<Statement::ProcWhile>(&stmt.data)) return exec_proc_while(s, v->label, v->condition, std::move(v->body));
    if (auto* v = std::get_if<Statement::ProcLoop>(&stmt.data)) return exec_proc_loop(s, v->label, std::move(v->body));
    if (auto* v = std::get_if<Statement::ProcRepeat>(&stmt.data)) return exec_proc_repeat(s, v->label, std::move(v->body), v->until);
    if (auto* v = std::get_if<Statement::ProcLeave>(&stmt.data)) {
        proc_signal_ = ProcSignal{ProcSignal::Leave{v->label}};
        return StringResult::Ok("");
    }
    if (auto* v = std::get_if<Statement::ProcIterate>(&stmt.data)) {
        proc_signal_ = ProcSignal{ProcSignal::Iterate{v->label}};
        return StringResult::Ok("");
    }
    if (auto* v = std::get_if<Statement::PrepareStmt>(&stmt.data)) {
        std::string upper = v->name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        prepared_stmts[upper] = v->query;
        return StringResult::Ok("Query OK");
    }
    if (auto* v = std::get_if<Statement::ExecuteStmt>(&stmt.data)) {
        std::string upper = v->name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        return exec_execute(s, upper, v->using_vars);
    }
    if (auto* v = std::get_if<Statement::DeallocatePrepare>(&stmt.data)) {
        std::string upper = v->name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        if (prepared_stmts.erase(upper) > 0) return StringResult::Ok("Query OK");
        return StringResult::Err("Unknown prepared statement: " + v->name);
    }
    if (auto* v = std::get_if<Statement::SetUserVar>(&stmt.data)) {
        Row vars = proc_vars;
        for (auto& [k, val] : user_vars) vars["@" + k] = val;
        std::string result_val = eval_arith(vars, v->expr);

        std::string lower_name = v->name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower_name == "rusql_parallel") {
            _putenv_s("RUSTDB_PARALLEL", result_val.c_str());
        }
        if (lower_name == "lock_wait_timeout") {
            try {
                lock_wait_timeout_ms = std::stoull(result_val);
            } catch (...) {
            }
        }
        user_vars[v->name] = result_val;
        return StringResult::Ok("");
    }

    return StringResult::Err("This statement type is not yet implemented in the C++ port (pending a later Phase 8 sub-step).");
}

} // namespace engine
