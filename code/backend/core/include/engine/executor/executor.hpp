#pragma once

// Faithful port of rusql-core/src/engine/executor.rs — SharedDatabase/Executor state,
// and (incrementally, phase by phase per the migration plan) the ~176 exec_* functions.
// Only Phase 8a (DDL) is implemented so far; unimplemented Statement variants fall
// through execute_with_s to an explicit "not yet implemented" error rather than being
// silently mishandled, so partial coverage is always honestly observable.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/parser/ast_json.hpp"
#include "engine/storage/buffer_pool.hpp"
#include "engine/storage/composite_index.hpp"
#include "engine/storage/disk.hpp"
#include "engine/transaction/group_commit.hpp"
#include "engine/storage/hash_index.hpp"
#include "engine/json_support.hpp"
#include "engine/lock_manager.hpp"
#include "engine/query_cache.hpp"
#include "engine/result.hpp"
#include "engine/row.hpp"
#include "engine/catalog/schema.hpp"
#include "engine/sync.hpp"
#include "engine/table_stats.hpp"
#include "engine/transaction/txn_manager.hpp"

namespace engine {

constexpr const char* EXECUTOR_NULL_VALUE = "NULL";

// Password hashing helpers (native TCP auth: SHA-256; MySQL wire protocol: SHA1),
// shared by SharedDatabase's auth methods (executor_core.cpp) and DCL user management
// (executor_dcl.cpp).
std::string hash_password(const std::string& password);
std::string mysql_native_hash_compute(const std::string& password);

struct UserRecord {
    std::string user;
    std::string host;
    std::optional<std::string> password_hash;
    // mysql_native_password verification: SHA1(SHA1(password)) hex string.
    std::optional<std::string> mysql_native_hash;
};
void to_json(nlohmann::json& j, const UserRecord& u);
void from_json(const nlohmann::json& j, UserRecord& u);

struct GrantRecord {
    std::string user;
    std::string host;
    std::string object_type;
    std::string object;
    std::vector<std::string> privileges;
    bool with_grant_option = false;
};
void to_json(nlohmann::json& j, const GrantRecord& g);
void from_json(const nlohmann::json& j, GrantRecord& g);

struct RoleRecord {
    std::string name;
};
void to_json(nlohmann::json& j, const RoleRecord& r);
void from_json(const nlohmann::json& j, RoleRecord& r);

struct RoleGrant {
    std::string role;
    std::string user;
    std::string host;
    bool with_admin_option = false;
};
void to_json(nlohmann::json& j, const RoleGrant& rg);
void from_json(const nlohmann::json& j, RoleGrant& rg);

struct ProcessInfo {
    std::size_t id = 0;
    std::string user;
    std::string host;
    std::string db;
    std::string command;
    std::string info;
    std::chrono::steady_clock::time_point connected_at;
    std::chrono::steady_clock::time_point state_since;
};

// Procedure: (params: [(IN/OUT/INOUT, name, type)], body statements).
using ProcedureDef = std::pair<std::vector<std::tuple<std::string, std::string, std::string>>, std::vector<Statement>>;
// Trigger: (table, timing, event, body statements) — stored as strings for
// timing/event to mirror the Rust tuple's `(String, String, String, Vec<Statement>)`.
using TriggerDef = std::tuple<std::string, std::string, std::string, std::vector<Statement>>;
// User-defined scalar function: (param names, body expression string).
using UserFunctionDef = std::pair<std::vector<std::string>, std::string>;

struct SharedDatabase {
    Catalog catalog;
    std::unordered_map<std::string, std::vector<Row>> tables;
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
    std::unordered_map<std::string, Statement> views;
    std::unordered_map<std::string, std::string> view_raw_sql;
    BufferPool buffer_pool;
    DiskManager disk;
    LockManager lock_mgr;
    std::unordered_set<std::string> databases;
    std::vector<UserRecord> users;
    std::vector<GrantRecord> grants;
    std::vector<RoleRecord> roles;
    std::vector<RoleGrant> role_grants;
    std::unordered_map<std::string, std::string> synonyms;
    std::shared_ptr<GroupCommitCoordinator> group_commit_coord;
    std::string data_dir;
    std::unordered_map<std::string, TableStats> table_stats;
    std::unordered_map<std::string, ProcedureDef> procedures;
    std::unordered_map<std::string, TriggerDef> triggers;
    std::size_t dml_since_vacuum = 0;
    std::unordered_map<std::string, std::size_t> dml_since_analyze;
    std::unordered_map<std::string, UserFunctionDef> user_functions;
    // SHOW PROCESS LIST state, guarded by its own lock (matches Rust's separate
    // `Arc<Mutex<HashMap<...>>>`, independent of the outer SharedDatabase RwLock).
    std::shared_ptr<Mutex<std::unordered_map<std::size_t, ProcessInfo>>> process_list;
    std::shared_ptr<std::atomic<std::size_t>> next_session_id;
    // PK value -> Vec position index: O(1) single-row DELETE optimization outside txns.
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> row_pk_pos;
    QueryResultCache query_cache;
    std::shared_ptr<TxnIoShared> txn_io;
    std::shared_ptr<Mutex<std::unordered_set<std::uint64_t>>> active_txn_ids;

    // mysql_native_password challenge-response verification (nonce is a 20-byte challenge)
    // -- used by both the MySQL wire protocol listener and the native TCP protocol's own
    // AUTH handshake (server/src/main.cpp), so no plaintext password is ever transmitted
    // by either protocol.
    bool verify_mysql_native_password(const std::string& user, const std::vector<std::uint8_t>& nonce,
                                       const std::vector<std::uint8_t>& auth_response) const;
    // Creates a default root/root account if `users` is empty; returns true if created.
    bool ensure_default_user();
};

class Executor {
public:
    std::shared_ptr<RwLock<SharedDatabase>> shared;
    TransactionManager txn;
    std::string current_db;
    // Session-local table buffer holding in-flight DML changes during a transaction;
    // applied to s.tables on COMMIT, discarded on ROLLBACK.
    std::unordered_map<std::string, std::vector<Row>> session_tables;
    std::unordered_map<std::string, std::string> proc_vars;
    std::unordered_map<std::string, std::string> user_vars;
    std::unordered_map<std::string, std::string> prepared_stmts;
    std::size_t session_id = 0;
    std::uint64_t lock_wait_timeout_ms = 50000;

    Executor() : Executor("data", 64) {}
    explicit Executor(std::size_t buffer_pool_capacity) : Executor("data", buffer_pool_capacity) {}
    explicit Executor(const std::string& dir) : Executor(dir, 64) {}
    Executor(const std::string& dir, std::size_t buffer_pool_capacity);

    static Executor new_session(std::shared_ptr<RwLock<SharedDatabase>> shared);

    void register_process(const std::string& user, const std::string& host) const;
    void update_process_command(const std::string& command, const std::string& info) const;
    void deregister_process() const;
    std::shared_ptr<RwLock<SharedDatabase>> get_shared() const { return shared; }

    StringResult execute(Statement stmt);
    StringResult execute_sql(const std::string& sql);

private:
    // Tag-dispatched constructor for new_session(): builds a session Executor that
    // reuses an existing SharedDatabase instead of loading one from disk.
    struct SessionTag {};
    Executor(SessionTag, std::shared_ptr<RwLock<SharedDatabase>> shared_db, std::string dir,
             std::shared_ptr<TxnIoShared> txn_io_shared, std::size_t sess_id, std::string db);

    // Control-flow signal for stored-procedure LEAVE/ITERATE (ported ahead of the
    // procedure interpreter itself since it's a plain field on Executor).
    struct ProcSignal {
        struct Leave { std::optional<std::string> label; };
        struct Iterate { std::optional<std::string> label; };
        using Data = std::variant<Leave, Iterate>;
        Data data;
        ProcSignal() : data(Leave{}) {}
        template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, ProcSignal>>>
        ProcSignal(Alt alt) : data(std::move(alt)) {}
    };
    std::optional<ProcSignal> proc_signal_;
    // Uncorrelated IN/NOT IN subquery result cache (Phase 8c).
    std::unordered_map<std::string, std::unordered_set<std::string>> subquery_cache_;
    // Trigger recursion depth (a trigger body's own DML can fire further triggers,
    // directly or via a chain through another table) -- see fire_triggers().
    std::size_t trigger_depth_ = 0;

    static std::pair<std::string, std::string> split_key(const std::string& key);
    std::string qualify_name(const std::string& name) const;
    std::string qualify_name_with_synonyms(const SharedDatabase& s, const std::string& name) const;
    static std::string strip_db_prefix(const std::string& name);
    static std::optional<CondExpr> merge_conditions(std::optional<CondExpr> a, std::optional<CondExpr> b);
    std::string display_name(const std::string& key) const;
    static std::string qualify_static(const std::string& name, const std::string& current_db);
    static std::vector<std::string> select_tables(const Statement& stmt, const std::string& current_db);
    // MVCC row visibility: a row is visible unless it's been deleted (_xmax != "0").
    static bool is_visible(const Row& row);
    // Concurrency: true iff executing `stmt` is guaranteed to never mutate any
    // SharedDatabase field, so it's safe to run under shared->read() concurrently with
    // other readers. See executor_core.cpp for the exhaustive classification and why it
    // must recurse into nested subqueries. Used by execute().
public:
    static bool is_pure_read_only(const Statement& stmt);
private:

    StringResult execute_with_s(SharedDatabase& s, Statement stmt);

    Statement qualify_stmt(const SharedDatabase& s, Statement stmt) const;
    CondExpr qualify_condexpr(const SharedDatabase& s, CondExpr expr) const;
    Join qualify_join_(const SharedDatabase& s, const Join& j) const;

    // ── Phase 8a: DDL ────────────────────────────────────────────────────
    StringResult exec_create(SharedDatabase& s, std::string name, std::vector<ColumnDef> columns, bool if_not_exists,
                              std::vector<std::string> primary_key_columns,
                              std::vector<std::pair<std::optional<std::string>, std::string>> check_constraints);
    StringResult exec_drop(SharedDatabase& s, const std::string& name, bool if_exists);
    StringResult exec_truncate(SharedDatabase& s, const std::string& name);
    StringResult exec_alter(SharedDatabase& s, const std::string& table, AlterAction action);
    StringResult exec_create_database(SharedDatabase& s, const std::string& name, bool if_not_exists);
    StringResult exec_drop_database(SharedDatabase& s, const std::string& name, bool if_exists);
    StringResult exec_create_index(SharedDatabase& s, const std::string& index_name, const std::string& table,
                                    const std::vector<std::string>& columns, bool using_hash);
    StringResult exec_drop_index(SharedDatabase& s, const std::string& index_name);
    StringResult exec_create_view(SharedDatabase& s, const std::string& name, Statement query, const std::string& raw_sql);
    StringResult exec_drop_view(SharedDatabase& s, const std::string& name);
    void persist_views_for_db(const SharedDatabase& s, const std::string& db) const;
    static void index_insert_row(SharedDatabase& s, const std::string& table, const Row& row);
    static void index_remove_row(SharedDatabase& s, const std::string& table, const Row& row, const std::string& pk_col);
    void rebuild_secondary_indexes(SharedDatabase& s, const std::string& table, const std::vector<Row>& rows);
    void persist_index_meta(const SharedDatabase& s) const;
    StringResult exec_use(SharedDatabase& s, const std::string& database);
    StringResult exec_show_tables(const SharedDatabase& s) const;

    // ── Phase 8b: expression/condition evaluation ───────────────────────
    // "table.col" or "col" lookup with dotted-suffix/bare-column fallback.
    static const std::string* get_col(const Row& row, const std::string& col);
    static std::string eval_arith(const Row& row, const ArithExpr& expr);
    static std::string format_arith_result(double f);
    // Scalar function dispatcher (MD5/string/date/JSON/math functions, plus
    // user-defined functions and DATABASE()/SCHEMA()). Mirrors the Rust original's use
    // of thread_local USER_FUNCTIONS/CURRENT_DB_CTX to give this otherwise-static
    // function session context: sync_udf_context() (executor_scalar_func.cpp) sets
    // those thread_locals and is called at the top of execute_with_s, exactly where
    // the Rust original syncs them.
    static std::string apply_scalar_func(const std::string& func_name, const std::vector<std::string>& args, const Row& row);
    static void sync_udf_context(const std::unordered_map<std::string, UserFunctionDef>& user_functions, const std::string& current_db);
    static bool matches_condexpr(const Row& row, const std::optional<CondExpr>& condition);
    static bool eval_condexpr(const Row& row, const CondExpr& expr);
    static bool eval_single(const Row& row, const Condition& cond);
    static bool eval_check_expr(const std::string& expr, const Row& row);
    static CondExpr substitute_correlated_condexpr(const CondExpr& expr, const Row& outer_row);
    static ArithExpr substitute_arith_outer_refs(const ArithExpr& expr, const Row& outer_row);
    static std::string format_returning_rows(const std::vector<Row>& rows, const std::vector<SelectColumn>& cols);
    static void update_stat_rows(SharedDatabase& s, const std::string& table, std::int64_t delta);
    static std::pair<std::vector<std::string>, std::vector<Row>> parse_table_output(const std::string& output);

    // ── Phase 8b: shared DML infrastructure ─────────────────────────────
    void maybe_auto_checkpoint(SharedDatabase& s);
    static void maybe_auto_vacuum(SharedDatabase& s);
    void maybe_auto_analyze(SharedDatabase& s, const std::string& table);
    std::vector<Row> session_swap_in(SharedDatabase& s, const std::string& table);
    void session_swap_out(SharedDatabase& s, const std::string& table, std::vector<Row> committed);
    // Returns Err only if trigger recursion exceeds the depth cap; a failing trigger-body
    // statement is otherwise ignored (matches the pre-existing best-effort behavior).
    StringResult fire_triggers(SharedDatabase& s, const std::string& table, const std::string& timing, const std::string& event);
    static std::optional<std::pair<std::string, std::optional<CondExpr>>> resolve_updatable_view(const SharedDatabase& s,
                                                                                                    const std::string& name);

    // ── Phase 8b: INSERT ─────────────────────────────────────────────────
    StringResult exec_insert_select(SharedDatabase& s, std::string table, std::optional<std::vector<std::string>> columns,
                                     Statement query, InsertConflict on_conflict, std::optional<std::vector<SelectColumn>> returning);
    StringResult exec_insert(SharedDatabase& s, std::string table, std::optional<std::vector<std::string>> col_list,
                              std::vector<std::vector<std::string>> all_values, InsertConflict on_conflict,
                              std::optional<std::vector<SelectColumn>> returning);
    StringResult exec_insert_inner(SharedDatabase& s, const std::string& table, const std::optional<std::vector<std::string>>& col_list,
                                    std::vector<std::vector<std::string>> all_values, const InsertConflict& on_conflict,
                                    const std::optional<std::vector<SelectColumn>>& returning);

    // ── Phase 8b: UPDATE ─────────────────────────────────────────────────
    StringResult exec_update(SharedDatabase& s, std::string table, std::vector<std::pair<std::string, ArithExpr>> assignments,
                              std::optional<CondExpr> condition, std::optional<std::vector<SelectColumn>> returning);
    StringResult exec_update_inner(SharedDatabase& s, const std::string& table, const std::vector<std::pair<std::string, ArithExpr>>& assignments,
                                    const std::optional<CondExpr>& condition, const std::optional<std::vector<SelectColumn>>& returning);

    // ── Phase 8b: DELETE ─────────────────────────────────────────────────
    static bool condition_has_subquery(const std::optional<CondExpr>& condition);
    static std::optional<std::string> extract_pk_eq_value(const std::optional<CondExpr>& condition, const std::string& pk_col);
    static std::optional<std::pair<std::string, std::string>> extract_pk_between_value(const std::optional<CondExpr>& condition,
                                                                                          const std::string& pk_col);
    StringResult exec_delete(SharedDatabase& s, const std::string& table, std::optional<CondExpr> condition,
                              std::optional<std::vector<SelectColumn>> returning);
    StringResult exec_delete_inner(SharedDatabase& s, const std::string& table, const std::optional<CondExpr>& condition,
                                    const std::optional<std::vector<SelectColumn>>& returning);

    // ── Phase 8c: subquery-aware WHERE evaluation ───────────────────────
    // These re-execute exec_select per row (or once, cached, for uncorrelated
    // IN/NOT IN) whenever a condition's ConditionValue is a Subquery; every other
    // leaf falls through to the plain (static) eval_single/eval_condexpr.
    bool matches_condition_with_subquery(SharedDatabase& s, const Row& row, const std::optional<CondExpr>& condition);
    bool eval_condexpr_with_subquery(SharedDatabase& s, const Row& row, const CondExpr& expr);
    static bool has_outer_ref(const CondExpr& expr);
    bool eval_single_with_subquery(SharedDatabase& s, const Row& row, const Condition& cond);
    std::vector<std::string> extract_values_from_output(const std::string& output) const;

    // ── Phase 8c: CTE (WITH ... [RECURSIVE]) ────────────────────────────
    StringResult exec_with(SharedDatabase& s, std::vector<std::pair<std::string, std::unique_ptr<Statement>>> ctes, Statement query,
                            bool recursive);

    // ── Phase 8c: UNION / INTERSECT / EXCEPT ────────────────────────────
    StringResult exec_union(SharedDatabase& s, Statement left, Statement right, bool all, std::vector<OrderBy> order_by,
                             std::optional<std::size_t> limit, std::optional<std::size_t> offset);
    StringResult exec_intersect(SharedDatabase& s, Statement left, Statement right, bool all, std::vector<OrderBy> order_by,
                                 std::optional<std::size_t> limit, std::optional<std::size_t> offset);
    StringResult exec_except(SharedDatabase& s, Statement left, Statement right, bool all, std::vector<OrderBy> order_by,
                              std::optional<std::size_t> limit, std::optional<std::size_t> offset);
    static void apply_set_postprocess(std::vector<Row>& result, const std::vector<std::string>& cols, const std::vector<OrderBy>& order_by,
                                       std::optional<std::size_t> limit, std::optional<std::size_t> offset);
    static std::string format_set_result(const std::vector<std::string>& cols, const std::vector<Row>& result);

    // ── Phase 8b: SELECT ─────────────────────────────────────────────────
    // NOTE ON SCOPE: the Rust original's exec_select additionally has (a) ~10
    // planner-driven AccessPath fast paths (PK/secondary/hash/composite index point
    // and range scans, a Top-K path) that are pure execution-strategy optimizations
    // over the same WHERE-filtered row set computed below — skipping them changes
    // nothing observable, matching this port's established "sequential/simple first"
    // stance on performance-only code (see join.hpp's rayon note); (b) FROM `_dual_`
    // scalar SELECT and INFORMATION_SCHEMA virtual tables, deferred to Phase 8f; and
    // (c) scalar subqueries in the SELECT list specifically (SelectColumn::Subquery —
    // WHERE-clause subqueries, via matches_condition_with_subquery, ARE supported).
    // Everything else — joins (always via nested_loop_join; algorithm selection is
    // likewise a performance-only choice), WHERE (including subqueries), GROUP
    // BY/aggregates, HAVING, ORDER BY, OFFSET/LIMIT, DISTINCT, views, FROM-subqueries,
    // FOR UPDATE/FOR SHARE, window functions — is faithfully ported.
    StringResult exec_select(SharedDatabase& s, std::string table, std::optional<std::pair<std::unique_ptr<Statement>, std::string>> subquery,
                              bool distinct, std::vector<SelectColumn> columns, std::optional<CondExpr> condition, std::vector<Join> joins,
                              std::vector<OrderBy> order_by, std::optional<std::vector<std::string>> group_by, std::optional<CondExpr> having,
                              std::optional<std::size_t> limit, std::optional<std::size_t> offset, bool for_update, bool for_share);
    StringResult exec_select_with_subquery(SharedDatabase& s, Statement inner_stmt, const std::string& alias, bool distinct,
                                            std::vector<SelectColumn> columns, std::optional<CondExpr> condition, std::vector<Join> joins,
                                            std::vector<OrderBy> order_by, std::optional<std::vector<std::string>> group_by,
                                            std::optional<CondExpr> having, std::optional<std::size_t> limit, std::optional<std::size_t> offset,
                                            bool for_update, bool for_share);
    StringResult format_result(SharedDatabase& s, std::vector<Row> result, const std::vector<SelectColumn>& columns, const std::string& table,
                                const std::vector<Join>& joins);
    static std::string agg_label(const AggFunc& func, const std::string& col);
    static std::vector<std::string> extract_agg_refs_from_cond(const CondExpr& expr);
    static void collect_agg_refs_cond(const CondExpr& expr, std::vector<std::string>& out);
    static void collect_agg_refs_arith(const ArithExpr& expr, std::vector<std::string>& out);
    static std::string compute_agg_from_key(const std::string& key, const std::vector<Row>& grp);
    // Computes every Agg/AggAlias column in `columns` over `grp`, keyed by each
    // column's display label — shared by the GROUP BY path (one call per group) and
    // the whole-result aggregate path (one call over all rows), exactly as the two
    // near-identical loops in the Rust original do it separately.
    static Row compute_aggregates(const std::vector<Row>& grp, const std::vector<SelectColumn>& columns, bool allow_parallel = false);
    static std::string window_func_default_label(WindowFunc func);

    // ── Phase 8d: transactions/MVCC/savepoints ──────────────────────────
    StringResult exec_begin(SharedDatabase& s);
    StringResult exec_commit(SharedDatabase& s);
    StringResult execute_commit_grouped();
    StringResult exec_commit_phase1(SharedDatabase& s);
    void apply_rollback(SharedDatabase& s);
    StringResult exec_rollback(SharedDatabase& s);
    StringResult exec_savepoint(const std::string& name);
    StringResult exec_release_savepoint(const std::string& name);
    StringResult exec_rollback_to(SharedDatabase& s, const std::string& name);
    // Boot-time WAL crash recovery (Phase 8d): replays committed transactions'
    // records since the last checkpoint and undoes anything left uncommitted.
    void recover_from_wal();

    // ── Phase 8d: isolation level / WAL / lock / buffer-pool monitoring ─────
    StringResult exec_show_buffer_pool(const SharedDatabase& s) const;
    StringResult exec_show_wal() const;
    StringResult exec_set_isolation_level(IsolationLevel level);
    StringResult exec_show_isolation_level() const;
    StringResult exec_show_locks(const SharedDatabase& s) const;
    StringResult exec_checkpoint(SharedDatabase& s);

    // ── Phase 8e: stored procedures / triggers / UDF ────────────────────
    StringResult exec_create_procedure(SharedDatabase& s, std::string name,
                                        std::vector<std::tuple<std::string, std::string, std::string>> params,
                                        std::vector<Statement> body);
    // Runs a list of statements in procedure context; stops early once proc_signal_ is set.
    StringResult exec_proc_stmts(SharedDatabase& s, std::vector<Statement> stmts);
    StringResult exec_proc_if(SharedDatabase& s, CondExpr condition, std::vector<Statement> then_body,
                               std::vector<std::pair<CondExpr, std::vector<Statement>>> elseif_branches,
                               std::optional<std::vector<Statement>> else_body);
    StringResult exec_proc_while(SharedDatabase& s, std::optional<std::string> label, CondExpr condition, std::vector<Statement> body);
    StringResult exec_proc_loop(SharedDatabase& s, std::optional<std::string> label, std::vector<Statement> body);
    StringResult exec_proc_repeat(SharedDatabase& s, std::optional<std::string> label, std::vector<Statement> body, CondExpr until);
    StringResult exec_call_procedure(SharedDatabase& s, std::string name, std::vector<std::string> args);
    StringResult exec_drop_procedure(SharedDatabase& s, std::string name, bool if_exists);
    StringResult exec_create_function(SharedDatabase& s, std::string name, std::vector<std::string> params, std::string body);
    StringResult exec_drop_function(SharedDatabase& s, std::string name, bool if_exists);
    StringResult exec_create_trigger(SharedDatabase& s, std::string name, TriggerTiming timing, TriggerEvent event, std::string table,
                                      std::vector<Statement> body);
    StringResult exec_drop_trigger(SharedDatabase& s, std::string name, bool if_exists);
    StringResult exec_execute(SharedDatabase& s, const std::string& name, const std::vector<std::string>& using_vars);

    // ── Phase 8f: DCL (users/grants/roles/synonyms) ─────────────────────
    StringResult exec_create_user(SharedDatabase& s, std::string user, std::string host, std::optional<std::string> password,
                                   bool if_not_exists);
    StringResult exec_drop_user(SharedDatabase& s, std::string user, std::string host, bool if_exists);
    StringResult exec_grant(SharedDatabase& s, std::vector<std::string> privileges, std::string object_type, std::string object,
                             std::string user, std::string host, bool with_grant_option);
    StringResult exec_revoke(SharedDatabase& s, std::vector<std::string> privileges, std::string object_type, std::string object,
                              std::string user, std::string host);
    StringResult exec_show_grants(const SharedDatabase& s, std::optional<std::string> user, std::optional<std::string> host) const;
    StringResult exec_show_databases(const SharedDatabase& s) const;
    StringResult exec_create_role(SharedDatabase& s, std::string name);
    StringResult exec_drop_role(SharedDatabase& s, std::string name, bool if_exists);
    StringResult exec_grant_role(SharedDatabase& s, std::string role, std::string user, std::string host, bool with_admin_option);
    StringResult exec_revoke_role(SharedDatabase& s, std::string role, std::string user, std::string host);
    StringResult exec_show_roles(const SharedDatabase& s) const;
    StringResult exec_create_synonym(SharedDatabase& s, std::string name, std::string target, bool or_replace);
    StringResult exec_drop_synonym(SharedDatabase& s, std::string name, bool if_exists);
    StringResult exec_show_synonyms(const SharedDatabase& s) const;

    // ── Phase 8f: DESCRIBE / SHOW CREATE / SHOW INDEX / SHOW PROCESSLIST ─
    StringResult exec_describe(const SharedDatabase& s, const std::string& table) const;
    StringResult exec_show_create_table(const SharedDatabase& s, const std::string& table) const;
    StringResult exec_show_create_view(const SharedDatabase& s, const std::string& view) const;
    StringResult exec_show_index(const SharedDatabase& s, const std::string& table) const;
    StringResult exec_show_processlist(const SharedDatabase& s) const;

    // ── Phase 8f: VACUUM / ANALYZE TABLE ─────────────────────────────────
    StringResult exec_vacuum(SharedDatabase& s, std::optional<std::string> table);
    StringResult exec_analyze_table(SharedDatabase& s, const std::string& table) const;

    // ── Phase 8f: EXPLAIN / EXPLAIN ANALYZE ──────────────────────────────
    StringResult exec_explain(const SharedDatabase& s, Statement stmt) const;
    StringResult exec_explain_analyze(SharedDatabase& s, Statement stmt);

    // ── Phase 8f: BACKUP / RESTORE ────────────────────────────────────────
    StringResult exec_backup(const SharedDatabase& s, std::optional<std::string> database, std::optional<std::string> output_file) const;
    StringResult exec_restore(SharedDatabase& s, std::string source_file, std::optional<std::string> database);
    std::string build_create_table_ddl(const SharedDatabase& s, const std::string& qkey) const;

    // ── Phase 8f: multi-table UPDATE / DELETE ────────────────────────────
    StringResult exec_multi_update(SharedDatabase& s, std::vector<std::string> tables, std::vector<Join> joins,
                                    std::vector<std::pair<std::string, ArithExpr>> assignments, std::optional<CondExpr> condition);
    StringResult exec_multi_delete(SharedDatabase& s, std::vector<std::string> delete_tables, std::string from_table, std::vector<Join> joins,
                                    std::optional<CondExpr> condition);

    // ── Phase 8f: MERGE ───────────────────────────────────────────────────
    StringResult exec_merge(SharedDatabase& s, std::string target, std::optional<std::string> target_alias, std::string source,
                             std::optional<std::string> source_alias, CondExpr on,
                             std::optional<std::vector<std::pair<std::string, ArithExpr>>> when_matched_update, bool when_matched_delete,
                             std::optional<CondExpr> when_matched_delete_cond, std::optional<std::vector<std::string>> when_not_matched_columns,
                             std::vector<std::string> when_not_matched_values);

    // ── Phase 8f: INFORMATION_SCHEMA virtual tables ──────────────────────
    static std::vector<Row> info_schema_rows(const SharedDatabase& s, const std::string& which);
    StringResult exec_information_schema(SharedDatabase& s, const std::string& which, std::vector<SelectColumn> columns,
                                          std::optional<CondExpr> condition, std::vector<OrderBy> order_by, std::optional<std::size_t> limit,
                                          std::optional<std::size_t> offset) const;

    // ── Phase 8c: window functions ───────────────────────────────────────
    static std::pair<std::size_t, std::size_t> frame_bounds(std::size_t pos, std::size_t len, const std::optional<WindowFrame>& frame,
                                                              bool has_order);
    static bool win_order_eq(const Row& a, const Row& b, const std::vector<OrderBy>& order_by);
    static std::vector<Row> compute_window_functions(std::vector<Row> rows, const std::vector<SelectColumn>& columns);
};

} // namespace engine
