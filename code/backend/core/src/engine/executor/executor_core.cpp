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

bool SharedDatabase::verify_mysql_native_password(const std::string& user, const std::vector<std::uint8_t>& nonce,
                                                   const std::vector<std::uint8_t>& auth_response) const {
    // Fail-closed: no "users empty -> allow everyone" open-mode fallback (see
    // ensure_default_user()'s boot-time guarantee that this table is never empty in practice).
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
    std::unordered_map<std::string, std::shared_ptr<std::shared_mutex>> table_locks;
    // Stage 4: pre-populated here for the same reason as table_locks -- see exec_create's
    // comment (executor_ddl.cpp). Populated once at startup for every table loaded from
    // disk so later per-table-locked DML never inserts a fresh key into these maps.
    std::unordered_map<std::string, TableStats> table_stats;
    std::unordered_map<std::string, std::size_t> dml_since_vacuum;
    std::unordered_map<std::string, std::size_t> dml_since_analyze;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> row_pk_pos;

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
        table_locks[qualified_key] = std::make_shared<std::shared_mutex>();
        table_stats[qualified_key] = TableStats{};
        dml_since_vacuum[qualified_key] = 0;
        dml_since_analyze[qualified_key] = 0;
        row_pk_pos[qualified_key] = {};
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
        .table_stats = std::move(table_stats),
        .procedures = std::move(procedures),
        .triggers = std::move(triggers),
        .dml_since_vacuum = std::move(dml_since_vacuum),
        .dml_since_analyze = std::move(dml_since_analyze),
        .user_functions = std::move(user_functions),
        .process_list = std::make_shared<Mutex<std::unordered_map<std::size_t, ProcessInfo>>>(),
        .next_session_id = std::make_shared<std::atomic<std::size_t>>(1),
        .row_pk_pos = std::move(row_pk_pos),
        .query_cache = QueryResultCache(),
        .txn_io = txn_io,
        .active_txn_ids = std::make_shared<Mutex<std::unordered_set<std::uint64_t>>>(),
        .table_locks = std::move(table_locks),
    };

    shared = std::make_shared<RwLock<SharedDatabase>>(std::move(db_value));
    txn = TransactionManager(dir, txn_io);
    current_db = current_db_val;
    session_id = 0;
    lock_wait_timeout_ms = 50000;

    recover_from_wal();

    // MVCC: _xmin/_xmax bake real transaction ids into rows persisted on disk, but
    // TxnIoShared's counter always restarts at 1 on a fresh process -- without this, a
    // freshly-issued id could collide with (or, worse, be numerically LESS than) an id
    // already sitting in a loaded/recovered row's _xmin/_xmax, which would make
    // is_visible_for_read's cutoff comparison wrongly treat that already-committed row
    // as "didn't exist yet" for every session from the moment this Executor starts. Scan
    // once at startup (after WAL redo/undo has finished mutating s.tables) and bump the
    // counter past the highest id found.
    {
        std::uint64_t max_id = 0;
        auto sr = shared->read();
        for (auto& [table_name, rows] : sr->tables) {
            (void)table_name;
            for (auto& row : rows) {
                for (const char* col : {"_xmin", "_xmax"}) {
                    auto it = row.find(col);
                    if (it == row.end()) continue;
                    try {
                        std::uint64_t id = std::stoull(it->second);
                        if (id > max_id) max_id = id;
                    } catch (...) {
                    }
                }
            }
        }
        if (max_id > 0) txn_io->ensure_next_id_at_least(max_id + 1);
    }
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
// is_pure_read_only(): classifies a statement as safe to execute under
// shared->read() (concurrently with other readers) instead of shared->write().
//
// A statement is read-only only if NOTHING reachable from executing it ever
// mutates any field of SharedDatabase. A WHERE/HAVING/JOIN-ON subquery, or a
// SELECT-list scalar subquery, can itself carry a FROM-derived-table (which
// materializes into s.tables via exec_select_with_subquery) even when the
// *top-level* statement has none — so this must recurse into every nested
// Statement, not just inspect the top-level Select's own fields. Keep this
// exhaustive and conservative (default to "not read-only") whenever
// execute_with_s's dispatch changes; see Executor::execute()'s use of this.
// ---------------------------------------------------------------------------
namespace {
bool cond_value_is_read_only(const ConditionValue& v) {
    if (auto* sq = std::get_if<ConditionValue::Subquery>(&v.data)) return Executor::is_pure_read_only(*sq->query);
    return true;
}
bool condexpr_is_read_only(const CondExpr& e) {
    if (auto* v = std::get_if<CondExpr::And>(&e.data)) return condexpr_is_read_only(*v->lhs) && condexpr_is_read_only(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Or>(&e.data)) return condexpr_is_read_only(*v->lhs) && condexpr_is_read_only(*v->rhs);
    if (auto* v = std::get_if<CondExpr::Not>(&e.data)) return condexpr_is_read_only(*v->inner);
    if (auto* v = std::get_if<CondExpr::Leaf>(&e.data)) return cond_value_is_read_only(v->condition.value);
    return true;
}
bool opt_condexpr_is_read_only(const std::optional<CondExpr>& e) { return !e || condexpr_is_read_only(*e); }
} // namespace

bool Executor::is_pure_read_only(const Statement& stmt) {
    return std::visit(
        [](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Statement::Select>) {
                if (v.for_update || v.for_share || v.subquery.has_value()) return false;
                if (!opt_condexpr_is_read_only(v.condition) || !opt_condexpr_is_read_only(v.having)) return false;
                for (auto& j : v.joins) {
                    if (!condexpr_is_read_only(j.on_expr)) return false;
                }
                for (auto& c : v.columns) {
                    if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                        if (!is_pure_read_only(*sq->query)) return false;
                    }
                }
                return true;
            } else if constexpr (std::is_same_v<T, Statement::Union> || std::is_same_v<T, Statement::Intersect> ||
                                  std::is_same_v<T, Statement::Except>) {
                return is_pure_read_only(*v.left) && is_pure_read_only(*v.right);
            } else if constexpr (std::is_same_v<T, Statement::Explain>) {
                return true; // never actually executes v.inner
            } else if constexpr (std::is_same_v<T, Statement::ExplainAnalyze>) {
                return is_pure_read_only(*v.inner); // DOES execute v.inner for real
            } else if constexpr (std::is_same_v<T, Statement::ShowTables> || std::is_same_v<T, Statement::Describe> ||
                                  std::is_same_v<T, Statement::ShowBufferPool> || std::is_same_v<T, Statement::ShowWal> ||
                                  std::is_same_v<T, Statement::ShowIsolationLevel> || std::is_same_v<T, Statement::ShowLocks> ||
                                  std::is_same_v<T, Statement::ShowGrants> || std::is_same_v<T, Statement::ShowRoles> ||
                                  std::is_same_v<T, Statement::ShowSynonyms> || std::is_same_v<T, Statement::ShowDatabases> ||
                                  std::is_same_v<T, Statement::ShowCreateTable> || std::is_same_v<T, Statement::ShowCreateView> ||
                                  std::is_same_v<T, Statement::ShowIndex> || std::is_same_v<T, Statement::ShowProcessList>) {
                return true; // all provably const-qualified handlers (exec_show_*/exec_describe)
            } else {
                // Default: everything else (INSERT/UPDATE/DELETE/DDL/DCL, BEGIN/COMMIT/
                // ROLLBACK/SAVEPOINT, With (CTE — always materializes into s.tables even
                // with no subquery), procedure CALL, SET forms, VACUUM/ANALYZE/CHECKPOINT,
                // ...) stays write-required. Safe default: unknown => not read-only.
                return false;
            }
        },
        stmt.data);
}

// ---------------------------------------------------------------------------
// Stage 4 (table-level concurrency): table-set discovery for the per-table locking
// path. See Executor::table_lock_set_for's doc comment (executor.hpp) for what this
// covers and why every fallback (nullopt) case exists.
// ---------------------------------------------------------------------------
namespace {

std::string qualify_local(const std::string& name, const std::string& current_db) {
    if (name.find('.') != std::string::npos) return name;
    return current_db + "." + name;
}

bool cond_tables_ok(const CondExpr& e, const std::string& current_db, const SharedDatabase& s, std::vector<std::string>& out);

bool select_tables_ok(const Statement::Select& sel, const std::string& current_db, const SharedDatabase& s,
                      std::vector<std::string>& out) {
    // FROM-derived subquery / view reference: both insert/erase a table-name KEY into
    // s.tables/s.catalog/s.indexes at execution time (a synthetic alias, or the view's own
    // name reused as a temp key) -- not safe to pre-lock a fixed table set for, since that
    // key doesn't exist before execution starts and two concurrent statements could pick
    // the same one. See the Stage 4 plan's audit.
    if (sel.subquery.has_value()) return false;
    if (sel.for_update || sel.for_share) return false; // keep today's shared->write() behavior unchanged for these
    std::string qtable = qualify_local(sel.table, current_db);
    if (s.views.count(qtable)) return false;
    out.push_back(qtable);
    for (auto& j : sel.joins) {
        std::string qj = qualify_local(j.table, current_db);
        if (s.views.count(qj)) return false;
        out.push_back(qj);
        if (!cond_tables_ok(j.on_expr, current_db, s, out)) return false;
    }
    if (sel.condition && !cond_tables_ok(*sel.condition, current_db, s, out)) return false;
    // HAVING subqueries are a non-issue: eval_single's ConditionValue::Subquery case
    // always returns false without executing anything (executor_eval.cpp) -- no table is
    // actually touched, so there's nothing to lock and no need to walk into it here.
    for (auto& c : sel.columns) {
        if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
            auto* nested = std::get_if<Statement::Select>(&sq->query->data);
            if (!nested || !select_tables_ok(*nested, current_db, s, out)) return false;
        }
    }
    return true;
}

// Real bug found via live multi-threaded stress testing: is_pure_read_only() classifies
// Select/Union/Intersect/Except/Explain/ExplainAnalyze as safe under structural-shared
// alone, which was true under the single whole-database exclusive lock (a concurrent
// writer could never be running at all) but is NOT true anymore -- these all read actual
// row data (Union/Intersect/Except execute their branches for real; Explain/
// ExplainAnalyze consult Planner::table_size(), which reads s.tables[table].size())
// without holding that specific table's own lock, racing a per-table-locked concurrent
// writer on the exact same std::vector<Row>. Recurses through every shape
// table_lock_set_for's "candidate" statements can nest via Union/Intersect/Except/
// Explain/ExplainAnalyze so all of them get proper per-table locks too, not just plain
// SELECT.
bool stmt_tables_ok(const Statement& stmt, const std::string& current_db, const SharedDatabase& s, std::vector<std::string>& out) {
    if (auto* sel = std::get_if<Statement::Select>(&stmt.data)) return select_tables_ok(*sel, current_db, s, out);
    if (auto* v = std::get_if<Statement::Union>(&stmt.data)) {
        return stmt_tables_ok(*v->left, current_db, s, out) && stmt_tables_ok(*v->right, current_db, s, out);
    }
    if (auto* v = std::get_if<Statement::Intersect>(&stmt.data)) {
        return stmt_tables_ok(*v->left, current_db, s, out) && stmt_tables_ok(*v->right, current_db, s, out);
    }
    if (auto* v = std::get_if<Statement::Except>(&stmt.data)) {
        return stmt_tables_ok(*v->left, current_db, s, out) && stmt_tables_ok(*v->right, current_db, s, out);
    }
    if (auto* v = std::get_if<Statement::Explain>(&stmt.data)) return stmt_tables_ok(*v->inner, current_db, s, out);
    if (auto* v = std::get_if<Statement::ExplainAnalyze>(&stmt.data)) return stmt_tables_ok(*v->inner, current_db, s, out);
    return false; // unknown/unexpected shape -- fall back, don't guess
}

bool cond_tables_ok(const CondExpr& e, const std::string& current_db, const SharedDatabase& s, std::vector<std::string>& out) {
    if (auto* v = std::get_if<CondExpr::And>(&e.data)) return cond_tables_ok(*v->lhs, current_db, s, out) && cond_tables_ok(*v->rhs, current_db, s, out);
    if (auto* v = std::get_if<CondExpr::Or>(&e.data)) return cond_tables_ok(*v->lhs, current_db, s, out) && cond_tables_ok(*v->rhs, current_db, s, out);
    if (auto* v = std::get_if<CondExpr::Not>(&e.data)) return cond_tables_ok(*v->inner, current_db, s, out);
    if (auto* v = std::get_if<CondExpr::Leaf>(&e.data)) {
        if (auto* sq = std::get_if<ConditionValue::Subquery>(&v->condition.value.data)) {
            auto* nested = std::get_if<Statement::Select>(&sq->query->data);
            if (!nested) return false; // unexpected shape -- fall back, don't guess
            return select_tables_ok(*nested, current_db, s, out);
        }
        return true;
    }
    return true;
}

bool has_firing_trigger(const SharedDatabase& s, const std::string& qtable, const char* event) {
    for (auto& [name, def] : s.triggers) {
        (void)name;
        auto& [t, ti, ev, body] = def;
        (void)ti;
        (void)body;
        if (t == qtable && ev == event) return true;
    }
    return false;
}

// FK parents of `qtable` (its own schema's foreign_key->ref_table -- INSERT's existence
// check reads these) and/or FK children (other tables whose foreign_key->ref_table ==
// qtable -- UPDATE/DELETE cascade reads/writes these). Both are exactly one hop, matching
// this codebase's actual cascade behavior (confirmed non-recursive, non-trigger-firing).
void add_fk_neighbors(const SharedDatabase& s, const std::string& qtable, bool parents, bool children, std::vector<std::string>& out) {
    if (parents) {
        if (auto* schema = s.catalog.get_table(qtable)) {
            for (auto& c : schema->columns) {
                if (c.foreign_key) out.push_back(c.foreign_key->ref_table);
            }
        }
    }
    if (children) {
        for (auto& [tname, tschema] : s.catalog.tables) {
            for (auto& c : tschema.columns) {
                if (c.foreign_key && c.foreign_key->ref_table == qtable) out.push_back(tname);
            }
        }
    }
}

} // namespace

std::optional<std::vector<std::string>> Executor::table_lock_set_for(const SharedDatabase& s, const Statement& stmt) const {
    std::vector<std::string> out;

    if (auto* v = std::get_if<Statement::Insert>(&stmt.data)) {
        std::string qtable = qualify_local(v->table, current_db);
        if (s.views.count(qtable)) return std::nullopt; // updatable-view redirect -- fall back
        if (has_firing_trigger(s, qtable, "INSERT")) return std::nullopt;
        out.push_back(qtable);
        add_fk_neighbors(s, qtable, /*parents=*/true, /*children=*/false, out);
    } else if (auto* v = std::get_if<Statement::InsertSelect>(&stmt.data)) {
        std::string qtable = qualify_local(v->table, current_db);
        if (s.views.count(qtable)) return std::nullopt;
        if (has_firing_trigger(s, qtable, "INSERT")) return std::nullopt;
        out.push_back(qtable);
        add_fk_neighbors(s, qtable, true, false, out);
        if (!v->query || !stmt_tables_ok(*v->query, current_db, s, out)) return std::nullopt;
    } else if (std::holds_alternative<Statement::Select>(stmt.data) || std::holds_alternative<Statement::Union>(stmt.data) ||
               std::holds_alternative<Statement::Intersect>(stmt.data) || std::holds_alternative<Statement::Except>(stmt.data) ||
               std::holds_alternative<Statement::Explain>(stmt.data) || std::holds_alternative<Statement::ExplainAnalyze>(stmt.data)) {
        if (!stmt_tables_ok(stmt, current_db, s, out)) return std::nullopt;
    } else if (auto* v = std::get_if<Statement::Update>(&stmt.data)) {
        std::string qtable = qualify_local(v->table, current_db);
        if (s.views.count(qtable)) return std::nullopt;
        if (has_firing_trigger(s, qtable, "UPDATE")) return std::nullopt;
        out.push_back(qtable);
        add_fk_neighbors(s, qtable, true, true, out);
        if (v->condition && !cond_tables_ok(*v->condition, current_db, s, out)) return std::nullopt;
    } else if (auto* v = std::get_if<Statement::Delete>(&stmt.data)) {
        std::string qtable = qualify_local(v->table, current_db);
        if (s.views.count(qtable)) return std::nullopt;
        if (has_firing_trigger(s, qtable, "DELETE")) return std::nullopt;
        out.push_back(qtable);
        add_fk_neighbors(s, qtable, false, true, out);
        if (v->condition && !cond_tables_ok(*v->condition, current_db, s, out)) return std::nullopt;
    } else if (auto* v = std::get_if<Statement::MultiUpdate>(&stmt.data)) {
        for (auto& t : v->tables) out.push_back(qualify_local(t, current_db));
        for (auto& j : v->joins) out.push_back(qualify_local(j.table, current_db));
        for (auto& t : out) {
            if (s.views.count(t)) return std::nullopt;
            if (has_firing_trigger(s, t, "UPDATE")) return std::nullopt;
        }
        std::vector<std::string> base = out;
        for (auto& t : base) add_fk_neighbors(s, t, true, true, out);
        for (auto& j : v->joins) {
            if (!cond_tables_ok(j.on_expr, current_db, s, out)) return std::nullopt;
        }
        if (v->condition && !cond_tables_ok(*v->condition, current_db, s, out)) return std::nullopt;
    } else if (auto* v = std::get_if<Statement::MultiDelete>(&stmt.data)) {
        for (auto& t : v->delete_tables) out.push_back(qualify_local(t, current_db));
        out.push_back(qualify_local(v->from_table, current_db));
        for (auto& j : v->joins) out.push_back(qualify_local(j.table, current_db));
        for (auto& t : out) {
            if (s.views.count(t)) return std::nullopt;
            if (has_firing_trigger(s, t, "DELETE")) return std::nullopt;
        }
        std::vector<std::string> base = out;
        for (auto& t : base) add_fk_neighbors(s, t, false, true, out);
        for (auto& j : v->joins) {
            if (!cond_tables_ok(j.on_expr, current_db, s, out)) return std::nullopt;
        }
        if (v->condition && !cond_tables_ok(*v->condition, current_db, s, out)) return std::nullopt;
    } else if (auto* v = std::get_if<Statement::Merge>(&stmt.data)) {
        // exec_merge's own cross-table footprint is exactly {source, target} -- no FK
        // existence/cascade checks, no triggers fired (confirmed in the Stage 4 audit).
        std::string qtarget = qualify_local(v->target, current_db);
        std::string qsource = qualify_local(v->source, current_db);
        if (s.views.count(qtarget) || s.views.count(qsource)) return std::nullopt;
        out.push_back(qtarget);
        out.push_back(qsource);
        if (!cond_tables_ok(v->on, current_db, s, out)) return std::nullopt;
        if (v->when_matched_delete_cond && !cond_tables_ok(*v->when_matched_delete_cond, current_db, s, out)) return std::nullopt;
    } else {
        // Not a "candidate" type -- caller falls through to the unchanged
        // is_pure_read_only()-based dispatch (DDL/DCL/SHOW*/session-variable statements,
        // VACUUM/CHECKPOINT, BEGIN/COMMIT/ROLLBACK/SAVEPOINT-family, CALL, etc.).
        return std::nullopt;
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Executor::TableLockGuard Executor::acquire_table_locks(RwLock<SharedDatabase>::ReadGuard structural, const std::vector<std::string>& tables,
                                                        bool exclusive) {
    // Takes an ALREADY-acquired structural shared guard (rather than acquiring its own)
    // so the caller can look up the table set (table_lock_set_for) and acquire the actual
    // per-table locks under the SAME hold -- calling shared->read() twice in a row here
    // would be a TOCTOU gap (a DDL statement could run in between) and is also not
    // guaranteed deadlock-free for a std::shared_mutex re-acquired shared on the same
    // thread.
    TableLockGuard guard{std::move(structural), {}, {}};
    for (auto& t : tables) {
        // A name with no entry is either a table that doesn't exist (the statement will
        // fail naturally downstream with "not found", same as today) or a virtual/
        // computed table (e.g. INFORMATION_SCHEMA.*) that never has one -- either way,
        // there's nothing real to protect, so skip it rather than throwing.
        auto it = guard.structural->table_locks.find(t);
        if (it == guard.structural->table_locks.end()) continue;
        std::shared_mutex& m = *it->second;
        if (exclusive) guard.exclusive_locks.emplace_back(m);
        else guard.shared_locks.emplace_back(m);
    }
    return guard;
}

// ---------------------------------------------------------------------------
// execute() / execute_sql() / execute_with_s()
// ---------------------------------------------------------------------------

StringResult Executor::execute(Statement stmt) {
    subquery_cache_.clear();
    if (std::holds_alternative<Statement::Commit>(stmt.data)) return execute_commit_grouped();

    // BEGIN/SAVEPOINT/RELEASE SAVEPOINT touch only TransactionManager's own session-local
    // state, never any table data -- structural shared is enough, no table locks needed.
    if (std::holds_alternative<Statement::Begin>(stmt.data) || std::holds_alternative<Statement::Savepoint>(stmt.data) ||
        std::holds_alternative<Statement::ReleaseSavepoint>(stmt.data)) {
        auto s = shared->read();
        return execute_with_s(const_cast<SharedDatabase&>(*s), std::move(stmt));
    }

    if (std::holds_alternative<Statement::Rollback>(stmt.data)) {
        // dirty_tables() is a pure read of TransactionManager's own undo log -- safe to
        // call before touching SharedDatabase at all, and apply_rollback (Stage 2) already
        // re-derives the same list itself before txn.abort() clears it.
        auto tables = txn.dirty_tables();
        std::sort(tables.begin(), tables.end());
        auto guard = acquire_table_locks(shared->read(), tables, /*exclusive=*/true);
        return execute_with_s(const_cast<SharedDatabase&>(*guard.structural), std::move(stmt));
    }

    // Single structural-shared acquisition, reused for both the table-set classification
    // and (if it doesn't fall back) the actual per-table lock acquisition -- calling
    // shared->read() twice in a row here would be a TOCTOU gap against a concurrent DDL
    // statement and isn't guaranteed deadlock-free for the same thread either.
    {
        auto s = shared->read();
        if (auto tables = table_lock_set_for(*s, stmt)) {
            bool exclusive = !is_pure_read_only(stmt);
            auto guard = acquire_table_locks(std::move(s), *tables, exclusive);
            // SAFETY: table_lock_set_for()'s fallback conditions (see its doc comment)
            // guarantee that, for any statement reaching here, every table it will read
            // or write is already locked (exclusive for anything that writes, shared for
            // a plain read) for the statement's whole execution -- no exec_* function
            // needs to (or should) acquire any further lock itself. Re-audit
            // table_lock_set_for whenever execute_with_s's dispatch changes, exactly
            // like is_pure_read_only().
            return execute_with_s(const_cast<SharedDatabase&>(*guard.structural), std::move(stmt));
        }
        if (is_pure_read_only(stmt)) {
            // SAFETY: is_pure_read_only() guarantees this call graph never mutates any
            // SharedDatabase field except BufferPool's own cache/LRU bookkeeping, which
            // has its own internal mutex (buffer_pool.hpp) precisely because it's
            // reachable from here. Multiple sessions may hold shared->read() at the same
            // time — re-audit is_pure_read_only() whenever execute_with_s's dispatch
            // changes.
            return execute_with_s(const_cast<SharedDatabase&>(*s), std::move(stmt));
        }
        // `s` (structural shared) released here, at the end of this block -- must happen
        // before acquiring shared->write() below (a shared_mutex has no upgrade, and a
        // thread holding a shared lock then trying to also acquire the same mutex
        // exclusively would deadlock).
    }
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

// PLAN.md P0 fix: the query cache had no notion of non-determinism, so a SELECT
// calling NOW()/RAND()/UUID()/etc. got cached on first execution and then kept
// returning that same stale value forever (nothing ever invalidates a cache entry
// that doesn't reference a table whose DML/DDL changed). Word-boundary matching
// (not a bare substring search) avoids false positives on ordinary identifiers
// that merely contain one of these names, e.g. a `brand` or `uuid_col` column.
// USER()/CURRENT_USER()/SESSION_USER()/SYSTEM_USER() joined this list once USER()
// started reflecting the real per-session Executor::auth_user instead of a fixed
// literal -- otherwise two different sessions running the identical SQL text (e.g.
// "SELECT USER()") could get back whichever user's result was cached first.
bool contains_nondeterministic_func(const std::string& lower_sql) {
    static const char* kFuncs[] = {"now",         "curdate",     "curtime",        "current_time", "current_timestamp",
                                   "localtime",   "localtimestamp", "unix_timestamp", "rand",      "uuid",
                                   "user",        "current_user", "session_user",   "system_user"};
    auto is_ident_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    for (const char* name_c : kFuncs) {
        std::string name = name_c;
        std::size_t pos = 0;
        while ((pos = lower_sql.find(name, pos)) != std::string::npos) {
            bool left_ok = pos == 0 || !is_ident_char(lower_sql[pos - 1]);
            std::size_t after = pos + name.size();
            bool right_ok = after >= lower_sql.size() || !is_ident_char(lower_sql[after]);
            if (left_ok && right_ok) return true;
            pos = after;
        }
    }
    return false;
}
} // namespace

StringResult Executor::execute_sql(const std::string& sql) {
    std::string trimmed = trim(sql);

    bool looks_like_select = trimmed.size() >= 6 && ieq_ascii(trimmed.substr(0, 6), "select");
    bool in_txn = txn.current_txn_id() != 0;

    if (looks_like_select && !in_txn) {
        std::string cache_key = current_db + "::" + trimmed;
        auto s = shared->read();
        // MVCC regression guard: once any transaction is open anywhere, the identical
        // SQL text can legitimately produce different results for different sessions
        // (a REPEATABLE READ transaction's frozen snapshot, READ UNCOMMITTED seeing an
        // in-progress write, or even plain READ COMMITTED sessions straddling another
        // session's still-open commit) -- the cache key (db + SQL text) has no way to
        // capture that, so a hit here could silently serve one session's view to
        // another. Only trust/populate the cache when the DB is fully quiescent.
        if (s->active_txn_ids->lock()->empty()) {
            if (auto cached = s->query_cache.get(cache_key)) return StringResult::Ok(std::move(*cached));
        }
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
    bool has_nondeterministic = looks_like_select && contains_nondeterministic_func(to_ascii_lower(trimmed));

    StringResult result = execute(std::move(stmt));

    if (looks_like_select && !in_txn && !has_subquery && !has_nondeterministic && result.is_ok()) {
        auto s = shared->write();
        // Same guard as the lookup above -- don't let a result computed under one
        // session's in-progress-transaction-aware view get cached and served to another
        // session that would have computed something different.
        if (s->active_txn_ids->lock()->empty()) {
            std::string cache_key = current_db + "::" + trimmed;
            s->query_cache.put(cache_key, result.value(), cache_tables);
        }
    }
    if (dml_table && result.is_ok()) {
        auto s = shared->write();
        s->query_cache.invalidate_table(*dml_table);
    }
    return result;
}

StringResult Executor::execute_with_s(SharedDatabase& s, Statement stmt) {
    sync_udf_context(s.user_functions, current_db, auth_user);

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
