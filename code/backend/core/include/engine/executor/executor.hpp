#pragma once

// Faithful port of rusql-core/src/engine/executor.rs — SharedDatabase/Executor state,
// and (incrementally, phase by phase per the migration plan) the ~176 exec_* functions.
// Only Phase 8a (DDL) is implemented so far; unimplemented Statement variants fall
// through execute_with_s to an explicit "not yet implemented" error rather than being
// silently mishandled, so partial coverage is always honestly observable.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
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

// LOCK TABLES / UNLOCK TABLES holder record (V1 scope -- see executor_dcl.cpp's
// exec_lock_tables design note: session-to-session LOCK TABLES cooperation only).
struct ExplicitTableLockHolder { std::size_t session_id; bool exclusive; };

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
    // Stage 4: per-table (not global) so a table's auto-vacuum threshold is driven only
    // by DML on that table, and firing it only ever needs that one table's own lock
    // (mirrors dml_since_analyze, which was already correctly scoped this way).
    std::unordered_map<std::string, std::size_t> dml_since_vacuum;
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
    // Stage 4 (table-level concurrency): one shared_mutex per real, catalog-registered
    // table, keyed the same way as s.tables. Populated on CREATE TABLE (and at startup
    // for every table loaded from disk) and erased on DROP TABLE -- entries are only ever
    // added/removed while the outer RwLock<SharedDatabase> is held exclusively (DDL is
    // always "structural"), so looking up an *existing* entry while the outer lock is
    // only held shared is safe (no concurrent insert/erase can be racing that lookup).
    // Ephemeral tables (CTE/FROM-subquery-alias/view-materialization keys transiently
    // inserted into s.tables) deliberately have NO entry here -- statements that create
    // them always fall back to holding the outer lock exclusively instead of using this
    // map at all (see Executor::execute()'s dispatch).
    std::unordered_map<std::string, std::shared_ptr<FairSharedMutex>> table_locks;

    // Row-level-concurrency Stage 4: same key/lifecycle as table_locks (populated on
    // CREATE TABLE / startup load, erased on DROP TABLE, renamed alongside RENAME TABLE,
    // only ever touched while the outer lock is exclusive) but a SEPARATE mutex serving a
    // different purpose. table_locks now stays SHARED for plain single-table
    // INSERT/UPDATE/DELETE/SELECT (only MultiUpdate/MultiDelete/Merge and the structural
    // fallback path still take it exclusive) -- table_data_locks is what actually
    // protects s.tables[table]'s vector SHAPE (push_back/insert/erase/pop_back/clear) and
    // the paired row_pk_pos[table] positions: held SHARED for a full-table scan (WHERE
    // matching, FK existence checks), held EXCLUSIVE only for the brief moment the
    // vector's shape actually changes. In-place mutation of an EXISTING row's fields
    // (e.g. `row["_xmax"] = ...`) needs only table_data_locks SHARED -- the guarantee that
    // no two threads mutate the SAME row concurrently comes from LockManager's per-row
    // claim (see RowClaimGuard), not from this mutex.
    std::unordered_map<std::string, std::shared_ptr<FairSharedMutex>> table_data_locks;

    // LOCK TABLES / UNLOCK TABLES holder registry (V1 scope -- see executor_dcl.cpp's
    // exec_lock_tables). Guarded by its own dedicated Mutex, independent of the outer
    // RwLock<SharedDatabase>: a session's held lock must stay checkable/waitable even
    // between statements, when this session may not be holding any statement-level lock
    // on `s` at all (LOCK TABLES ... UNLOCK TABLES spans multiple separate client
    // round-trips, unlike every other lock in this codebase).
    std::shared_ptr<Mutex<std::unordered_map<std::string, std::vector<ExplicitTableLockHolder>>>> explicit_table_locks;

    // mysql_native_password challenge-response verification (nonce is a 20-byte challenge)
    // -- used by both the MySQL wire protocol listener and the native TCP protocol's own
    // AUTH handshake (server/src/main.cpp), so no plaintext password is ever transmitted
    // by either protocol.
    bool verify_mysql_native_password(const std::string& user, const std::vector<std::uint8_t>& nonce,
                                       const std::vector<std::uint8_t>& auth_response) const;
    // Creates a default root/root account if `users` is empty; returns true if created.
    bool ensure_default_user();
};

// Gap lock range (InnoDB-style phantom-read prevention), extracted from a WHERE clause's
// PK-column leaves. nullopt bound == unbounded on that side. See gap_lock.cpp.
struct GapRange {
    std::optional<std::string> lo;
    std::optional<std::string> hi;
    bool lo_inclusive = true;
    bool hi_inclusive = true;
};

class Executor {
public:
    std::shared_ptr<RwLock<SharedDatabase>> shared;
    TransactionManager txn;
    std::string current_db;
    std::unordered_map<std::string, std::string> proc_vars;
    std::unordered_map<std::string, std::string> user_vars;
    std::unordered_map<std::string, std::string> prepared_stmts;
    std::size_t session_id = 0;
    std::uint64_t lock_wait_timeout_ms = 50000;
    // The username that authenticated this session (native/MySQL protocol servers set
    // this after a successful AUTH handshake; defaults to "root" for the CLI/in-process
    // embedding, which has no network auth step of its own). Read by USER()/CURRENT_USER().
    std::string auth_user = "root";
    // exec_restore()-only escape hatch (executor_backup.cpp) -- NOT a general SQL feature
    // (there's no SET FOREIGN_KEY_CHECKS statement). A backup dumps tables alphabetically
    // and rows in physical vector order, neither of which is guaranteed to respect
    // FK/self-referential-FK dependency order; replaying INSERTs with FK checks on can
    // therefore reject rows whose referenced row simply hasn't been replayed yet, even
    // though the full dump is internally consistent. Set for the duration of a single
    // restore's replay loop via an RAII guard, exactly like mysql.exe's dump format
    // bracketing itself with SET FOREIGN_KEY_CHECKS=0/1.
    bool skip_fk_checks = false;
    // Tables this session currently holds via LOCK TABLES (V1 scope, see
    // executor_dcl.cpp's exec_lock_tables) -- released by UNLOCK TABLES or
    // release_explicit_table_locks() at connection teardown.
    std::vector<std::pair<std::string, bool>> held_explicit_locks;

    Executor() : Executor("data", 64) {}
    explicit Executor(std::size_t buffer_pool_capacity) : Executor("data", buffer_pool_capacity) {}
    explicit Executor(const std::string& dir) : Executor(dir, 64) {}
    Executor(const std::string& dir, std::size_t buffer_pool_capacity);

    static Executor new_session(std::shared_ptr<RwLock<SharedDatabase>> shared);

    void register_process(const std::string& user, const std::string& host) const;
    void update_process_command(const std::string& command, const std::string& info) const;
    void deregister_process() const;
    std::shared_ptr<RwLock<SharedDatabase>> get_shared() const { return shared; }
    // Releases this session's LOCK TABLES holds (V1 scope) without an explicit UNLOCK
    // TABLES statement -- call at connection teardown, alongside deregister_process()
    // (mirrors its explicit-call convention; Executor doesn't own SharedDatabase's
    // lifetime so this can't be a destructor).
    void release_explicit_table_locks();

    StringResult execute(Statement stmt);
    StringResult execute_sql(const std::string& sql);

private:
    // Row-level-concurrency Stage 4/5 correctness fix (found via concurrent-reader
    // stress testing): execute_sql's actual body -- pulled out so execute_sql itself
    // can wrap the call in a try/catch. Before this fix, an exception escaping from
    // deep inside (e.g. DiskManager's schema-file atomic-replace throwing on a Windows
    // file-handle/antivirus timing conflict -- a real, if rare, pre-existing condition)
    // would propagate all the way out of execute_sql. On the thread that owns the
    // top-level test/request loop that's usually survivable (caught by a REQUIRE macro
    // or an RPC layer's own try/catch), but execute_sql is also called directly from
    // spawned std::threads in this codebase's own concurrency tests -- and a C++
    // exception escaping a std::thread's function with no catch of its own calls
    // std::terminate() immediately, crashing the entire process. execute_sql must never
    // let that happen regardless of which thread calls it.
    StringResult execute_sql_inner(const std::string& sql);
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
    // Uncorrelated IN/NOT IN subquery result cache (Phase 8c). Keyed by the subquery
    // AST's own address (sub->query.get()) rather than a serialized string -- see
    // eval_single_with_subquery's comment for why this matters (found via concurrent-
    // reader stress testing): a JSON-dump string key means copying and re-serializing
    // the ENTIRE subquery AST on every single row evaluated by the OUTER query, even on
    // a cache HIT, since that work has to happen before the lookup even knows whether
    // it's a hit. The AST is parsed once per SQL statement and reused unchanged across
    // every row of that statement's scan (condition is passed once to exec_select, not
    // rebuilt per row), and subquery_cache_ itself is cleared at the start of every new
    // statement (execute()'s first line) -- so the raw pointer is a valid, stable,
    // dramatically cheaper identity for "which subquery is this" for as long as the
    // cache entry can possibly be looked up, with no risk of a stale/reused-address
    // collision across different statements.
    std::unordered_map<const void*, std::unordered_set<std::string>> subquery_cache_;
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
    // MVCC row visibility: a row is visible unless it's been deleted (_xmax != "0"). This
    // permissive check ignores the reading transaction's own snapshot entirely -- it's
    // what PK/UNIQUE/FK constraint-checking call sites must keep using (two sessions
    // must each see the other's uncommitted same-PK insert as "live" for duplicate
    // detection to work at all, since writes stay fully serialized under the exclusive
    // per-statement lock -- see is_visible_for_read's doc comment for the read-path
    // counterpart).
    static bool is_visible(const Row& row);
    // Strict MVCC visibility for read paths that must respect a specific reading
    // transaction's snapshot (index-based fast-path SELECTs in particular, and every
    // other read of s.tables -- without this a concurrent read could see another
    // session's still-open transaction's uncommitted INSERT/UPDATE/DELETE).
    static bool is_visible_for_read(const Row& row, const SnapshotCtx& ctx);
    // Builds the SnapshotCtx for the current statement's reads: the frozen BEGIN-time
    // ctx for an open RepeatableRead/Serializable transaction, or a fresh "right now"
    // capture otherwise (ReadCommitted, autocommit, and ReadUncommitted -- which always
    // treats everything as committed regardless of timing, i.e. a genuine dirty read).
    SnapshotCtx current_read_ctx(SharedDatabase& s) const;
    // Real transaction id to tag _xmin/_xmax with: the active explicit transaction's id,
    // or a freshly-allocated one-off id for an autocommit statement (never 0 -- 0 is
    // reserved as the "pre-MVCC legacy row, always visible" sentinel).
    std::uint64_t tagging_txn_id(SharedDatabase& s) const;
    // GC horizon: the smallest txn id any currently open transaction's frozen snapshot
    // could still need (min of active_txn_ids, or peek_next_id() if none are open --
    // matching "nothing has an open snapshot, anything already dead is fair game").
    static std::uint64_t oldest_active_txn_id(SharedDatabase& s);
    // True iff a dead row (_xmax != "0") can be physically purged: its deletion must have
    // committed strictly before the oldest currently open transaction's snapshot could
    // have started, i.e. _xmax < oldest_active_txn_id(s) -- replaces the old permissive
    // !is_visible(r) VACUUM predicate, which purged a row the instant _xmax was set with
    // no regard for any other session's still-open snapshot.
    static bool is_vacuumable(const Row& row, std::uint64_t oldest_active_txn_id);
    // Concurrency: true iff executing `stmt` is guaranteed to never mutate any
    // SharedDatabase field, so it's safe to run under shared->read() concurrently with
    // other readers. See executor_core.cpp for the exhaustive classification and why it
    // must recurse into nested subqueries. Used by execute().
public:
    static bool is_pure_read_only(const Statement& stmt);
private:

    // Stage 4 (table-level concurrency): holds the outer RwLock<SharedDatabase>'s shared
    // "structural" lock (protects the SHAPE of every table-keyed map -- e.g. against a
    // concurrent CREATE/DROP TABLE resizing/rehashing them while this statement runs) plus
    // a specific, sorted set of per-table locks, all acquired in the SAME mode (shared for
    // a statement that only reads, exclusive for anything that writes -- this stage
    // doesn't mix read/write granularity within one statement's lock set). Destructor
    // order releases the table locks first, then the structural shared lock.
    struct TableLockGuard {
        RwLock<SharedDatabase>::ReadGuard structural;
        std::vector<std::shared_lock<FairSharedMutex>> shared_locks;
        std::vector<std::unique_lock<FairSharedMutex>> exclusive_locks;
    };
    // Acquires `tables` (already sorted+deduplicated real, catalog-registered table
    // names) in the given mode, under the given already-acquired structural shared guard
    // (moved in -- see the .cpp definition for why this must not acquire its own). Every
    // name must already have an entry in s.table_locks (true for any real table,
    // populated at CREATE TABLE time) -- a missing entry is a bug in the caller's
    // table-set computation, not a runtime condition to handle gracefully.
    TableLockGuard acquire_table_locks(RwLock<SharedDatabase>::ReadGuard structural, const std::vector<std::string>& tables, bool exclusive);

    // Row-level-concurrency Stage 4: analogous to TableLockGuard/acquire_table_locks but
    // for s.table_data_locks instead of s.table_locks. No structural member -- the caller
    // already holds (via the outer TableLockGuard) whatever structural/table_locks
    // protection it needs; this guard only ever nests INSIDE that, never on its own.
    // Unlike table_locks (held for a whole statement), callers are expected to acquire
    // this for the SHORTEST phase that actually needs it (e.g. just the push_back
    // moment for INSERT), except exec_select, which holds it SHARED for its whole body
    // since a read has no shape-changing phase to isolate.
    struct DataLockGuard {
        std::vector<std::shared_lock<FairSharedMutex>> shared_locks;
        std::vector<std::unique_lock<FairSharedMutex>> exclusive_locks;
    };
    // `tables` need not be pre-sorted by the caller -- this function sorts+dedups its own
    // copy, so call sites can pass e.g. {table} or {table, fk_parent} in any order without
    // thinking about lock-ordering themselves. A name with no entry in s.table_data_locks
    // (ephemeral CTE/subquery-alias table) is skipped, same as acquire_table_locks.
    DataLockGuard acquire_table_data_locks(SharedDatabase& s, std::vector<std::string> tables, bool exclusive);

    // Real-blocking-wait stage: thin shared helper for the "probe under table_data_locks
    // (timeout=0), on conflict release table_data_locks, block on just the ONE contested
    // row, then retry the whole scan from scratch" pattern used by exec_insert_inner/
    // exec_update_inner/exec_delete_inner (see executor_update.cpp's UPDATE mutation loop
    // for the fullest example -- probe-then-mutate, since that phase's atomicity is load-
    // bearing for a prior phantom-disappearance fix). Computes the remaining time until
    // `deadline` itself (clamped to zero, never negative) so call sites don't repeat that
    // arithmetic. The CALLER must not be holding any table_data_locks/table_locks when
    // calling this -- blocking while holding either would freeze the whole table for
    // other sessions, exactly the risk this feature's design exists to avoid.
    //
    // Result handling for callers: Kind::Granted means retry the scan now; Kind::Deadlock
    // means fail with the deadlock error immediately (unchanged message). Both
    // Kind::Timeout (a real wait ran out) AND Kind::Conflict (deadline was already exactly
    // exhausted when this was called, so LockManager took its instant-fail path) mean the
    // SAME thing to a caller here -- give up with the standard ERROR 1205 message; treat
    // them identically, don't special-case Conflict.
    static LockResult block_on_row(LockManager& lock_mgr, const std::string& table, const std::string& pk, std::uint64_t txn_id,
                                    bool exclusive, std::chrono::steady_clock::time_point deadline);

    // Real-blocking-wait stage, second correctness fix (found via a live hang, not a
    // static review): releasing table_data_locks before block_on_row is NOT sufficient by
    // itself. execute()'s dispatcher acquires table_locks[table] SHARED (or EXCLUSIVE for
    // MultiUpdate/MultiDelete/Merge) ONCE, for the entire statement, in a TableLockGuard
    // that lives on execute()'s OWN stack frame -- exec_update_inner/exec_insert_inner/
    // exec_delete_inner have no access to it and, without help, cannot release it before
    // blocking. Meanwhile execute_commit_grouped() holds table_locks[table] EXCLUSIVE
    // continuously from phase1 through the active_txn_ids erase (a Phase-27 fix for a
    // separate MVCC visibility race). Put those two facts together: a statement parked in
    // block_on_row() while still holding table_locks SHARED can deadlock against a
    // concurrent COMMIT that needs table_locks EXCLUSIVE on the same table -- neither
    // waits on the other through LockManager, so its row-only wait-for graph never sees
    // it (confirmed via a real, reproducible hang before this fix).
    //
    // Fix: execute() stashes a pointer to its own local TableLockGuard (kept in a
    // std::optional so it can be reset()+re-emplaced in place -- TableLockGuard holds a
    // RwLock<SharedDatabase>::ReadGuard, which has a reference member and so is
    // move-constructible but NOT assignable) here, plus the exact table set/mode used to
    // build it, for the statement's duration. exec_*_inner calls
    // release_table_locks_for_block() immediately before block_on_row() and
    // reacquire_table_locks_after_block() immediately after it returns. Both are no-ops
    // if execute() didn't stash a guard (shouldn't happen for any statement that ever
    // calls block_on_row, but safe regardless). Resetting/re-emplacing the guard destroys
    // and recreates its `structural` ReadGuard too, not just the table_locks entries --
    // safe because `s`
    // (the SharedDatabase& every exec_*_inner already holds) is a reference into the
    // RwLock<SharedDatabase>'s own persistent value_, not into the transient guard, so it
    // stays valid across the whole release/reacquire cycle.
    std::optional<TableLockGuard>* active_table_lock_guard_ = nullptr;
    std::vector<std::string> active_table_lock_tables_;
    bool active_table_lock_exclusive_ = false;
    void release_table_locks_for_block();
    void reacquire_table_locks_after_block();

    // Real-blocking-wait stage, extended to SELECT FOR UPDATE/FOR SHARE: the exact same
    // problem as active_table_lock_guard_ above, but for the DataLockGuard execute()'s
    // dispatcher acquires SHARED for the whole SELECT family (is_select_family, see
    // executor_core.cpp) and holds for the whole recursive execute_with_s call --
    // exec_select's FOR UPDATE/FOR SHARE row-locking loop is deep inside that call and has
    // no way to release it before a genuine block_on_row() wait (which would otherwise
    // freeze the table for every other session's writes, and for a concurrent COMMIT that
    // needs table_data_locks EXCLUSIVE, for the whole wait).
    //
    // Unlike TableLockGuard, DataLockGuard has no reference member (just two vectors of
    // locks) -- it's plain move-assignable (see e.g. executor_dml.cpp's `x = DataLockGuard{};`
    // reset idiom), so no std::optional wrapping is needed here, just a raw pointer.
    DataLockGuard* active_table_data_lock_guard_ = nullptr;
    std::vector<std::string> active_table_data_lock_tables_;
    bool active_table_data_lock_exclusive_ = false;
    void release_table_data_locks_for_block();
    void reacquire_table_data_locks_after_block(SharedDatabase& s);

    // Stage 4: computes the exact set of real tables `stmt` will touch (including FK
    // existence-check parents for INSERT and FK cascade children for UPDATE/DELETE --
    // both statically known from the catalog, see the Stage 4 plan's audit), or nullopt if
    // `stmt` needs the full structural exclusive lock instead. Only handles
    // Insert/InsertSelect/Select/Update/Delete/MultiUpdate/MultiDelete/Merge -- every other
    // Statement variant returns nullopt unconditionally, falling through to today's
    // unchanged is_pure_read_only()-based dispatch in execute(). Falls back (nullopt)
    // whenever `stmt` involves a FROM-derived subquery, a view reference, or a table with
    // a trigger that would fire for this statement's DML type -- none of those are safe to
    // pre-lock a fixed table set for (see the plan's audit findings). Re-audit this
    // whenever a new Statement variant or execute_with_s dispatch case is added, same as
    // is_pure_read_only().
    std::optional<std::vector<std::string>> table_lock_set_for(const SharedDatabase& s, const Statement& stmt) const;

    StringResult execute_with_s(SharedDatabase& s, Statement stmt);

    Statement qualify_stmt(const SharedDatabase& s, Statement stmt) const;
    CondExpr qualify_condexpr(const SharedDatabase& s, CondExpr expr) const;
    Join qualify_join_(const SharedDatabase& s, const Join& j) const;

    // ── Phase 8a: DDL ────────────────────────────────────────────────────
    StringResult exec_create(SharedDatabase& s, std::string name, std::vector<ColumnDef> columns, bool if_not_exists,
                              std::vector<std::string> primary_key_columns,
                              std::vector<std::pair<std::optional<std::string>, std::string>> check_constraints,
                              std::optional<PartitionBy> partition_by = std::nullopt);
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
    // user-defined functions and DATABASE()/SCHEMA()/USER()). Mirrors the Rust original's
    // use of thread_local USER_FUNCTIONS/CURRENT_DB_CTX to give this otherwise-static
    // function session context: sync_udf_context() (executor_scalar_func.cpp) sets
    // those thread_locals and is called at the top of execute_with_s, exactly where
    // the Rust original syncs them.
    static std::string apply_scalar_func(const std::string& func_name, const std::vector<std::string>& args, const Row& row);
    static void sync_udf_context(const std::unordered_map<std::string, UserFunctionDef>& user_functions, const std::string& current_db,
                                  const std::string& current_user);
    static bool matches_condexpr(const Row& row, const std::optional<CondExpr>& condition);
    static bool eval_condexpr(const Row& row, const CondExpr& expr);
    static bool eval_single(const Row& row, const Condition& cond);
    static bool eval_check_expr(const std::string& expr, const Row& row);
    static CondExpr substitute_correlated_condexpr(const CondExpr& expr, const Row& outer_row);
    static ArithExpr substitute_arith_outer_refs(const ArithExpr& expr, const Row& outer_row);
    static std::string format_returning_rows(const std::vector<Row>& rows, const std::vector<SelectColumn>& cols);
    static void update_stat_rows(SharedDatabase& s, const std::string& table, std::int64_t delta);
    static std::pair<std::vector<std::string>, std::vector<Row>> parse_table_output(const std::string& output);
    // Escapes '\', '\n', and '|' in a cell value before it is written into the ASCII
    // table string that parse_table_output() later re-parses (UNION/CTE/subquery/
    // LATERAL/partition-routing all round-trip a SELECT's result through this exact
    // string format) -- without this, a value containing a literal '|' or newline
    // would silently corrupt the round-trip (extra/missing columns, or a row cut off
    // mid-value). Called at every headers/cells insertion site that builds one of
    // these tables; parse_table_output() reverses it after extracting each cell.
    static std::string escape_cell(const std::string& v);

    // ── Phase 8b: shared DML infrastructure ─────────────────────────────
    void maybe_auto_checkpoint(SharedDatabase& s);
    static void maybe_auto_vacuum(SharedDatabase& s, const std::string& table);
    void maybe_auto_analyze(SharedDatabase& s, const std::string& table);
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
    // REPLACE INTO support: before the real INSERT runs, deletes any existing row(s) that
    // would conflict on a PK/UNIQUE column of the incoming row(s) -- via a real DELETE
    // statement through execute_with_s (full locking/index/trigger correctness reused,
    // not reimplemented), not by mutating s.tables directly. See executor_dml.cpp for the
    // V1 scope note (only explicit-valued PK/UNIQUE columns are considered).
    StringResult replace_delete_conflicts(SharedDatabase& s, const std::string& table,
                                           const std::optional<std::vector<std::string>>& col_list,
                                           const std::vector<std::vector<std::string>>& all_values);

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

public:
    // ── Gap Lock (InnoDB-style phantom-read prevention, see gap_lock.cpp) ────
    // Extracts a conservative range covering every PK-column leaf ANDed into `condition`
    // (Eq/Gt/Gte/Lt/Lte/Between); ignores non-PK leaves and anything under an OR/NOT
    // (collect_and_leaves only flattens AND), falling back to a fully unbounded range in
    // those cases -- always safe (over-locking costs concurrency, never correctness).
    // Public (unlike the surrounding DELETE helpers) so it's directly unit-testable.
    static GapRange extract_pk_gap_range(const std::optional<CondExpr>& condition, const std::string& pk_col);
    // Same numeric-priority/lexicographic-fallback comparison semantics as eval_single's
    // BETWEEN/Gt/Lt/Gte/Lte handling (executor_eval.cpp's cmp_num), so a value considered
    // "inside" a gap-locked range matches how the same value would evaluate against the
    // original WHERE clause elsewhere in the engine.
    static bool gap_range_contains(const GapRange& range, const std::string& value);

    // ── Index-assisted existence checks (FK/UNIQUE validation perf, see PLAN.md Section E)
    // Does `table_rows` contain a row where `column` == val? Prefers an existing PK B+Tree /
    // secondary B+Tree / hash index covering (table, column) over a linear scan when one is
    // available. Safe to use even for an existence-ONLY question against a non-unique
    // column: a secondary B+Tree index stores only one value per key, so finding (or not
    // finding) a match through it isn't authoritative on its own for a non-unique column --
    // this function only ever treats an index lookup as the final answer when it provably
    // is one (the PK index, since PK is unique by definition; or a hash index, whose bucket
    // already holds every physical row sharing that value), and otherwise falls through to
    // the next index type or, ultimately, a full linear scan over `table_rows` -- so the
    // result is always exactly what a full scan would have found, just usually faster.
    // `accept` lets each call site apply its own exact pre-existing predicate (MVCC
    // visibility, etc.) on top of the plain equality match, so no call site's semantics
    // change -- only how fast the matching candidate is located.
    static bool index_or_scan_exists(const SharedDatabase& s, const std::string& table, const std::vector<Row>& table_rows,
                                       const std::string& column, const std::string& val, const std::function<bool(const Row&)>& accept);

    // ── Table partitioning (PARTITION BY RANGE/LIST/HASH, V1 -- executor_partition.cpp)
    // A partitioned table's catalog entry (TableSchema::partition_info) is the single
    // source of truth: `s.tables[logical_name]` is a permanently-empty phantom (every
    // real row lives in one of `partitions[i].child_table`, an ordinary table with its
    // own full set of the 8 per-table SharedDatabase maps). Public/static so both DDL
    // (child-table setup) and FK-existence-check code can reuse them without a full
    // Executor instance.
    static std::optional<PartitionBy> partition_info_for(const SharedDatabase& s, const std::string& table);
    // Which child a single partition-key VALUE belongs to (INSERT routing; also HASH's
    // Eq-pruning path). Empty string means "no partition matches" (RANGE only, when the
    // value exceeds every bound and there's no MAXVALUE catch-all) -- caller must error.
    static std::string partition_child_for_value(const PartitionBy& info, const std::string& value);
    // WHERE-clause pruning: returns the (possibly full) subset of child table names that
    // could contain a matching row. Reuses extract_pk_gap_range (already generic on
    // column name, not just PK) for RANGE; scans AND-leaves for Eq/In on LIST/HASH.
    // Always safe to over-return (a extra child just gets scanned and finds nothing).
    static std::vector<std::string> prune_partition_children(const PartitionBy& info, const std::optional<CondExpr>& where);

private:
    // Entry point called from execute(), BEFORE any lock is acquired for the current
    // statement (so it's free to recurse back into execute() once per relevant child --
    // see the design note in executor_partition.cpp for why that's required instead of
    // hooking inside execute_with_s, which may already be running under a lock held by
    // an outer execute() call on the same thread). Returns nullopt if `stmt` doesn't
    // target a partitioned table (caller continues with the normal dispatch, zero
    // overhead beyond one catalog lookup); otherwise returns the final routed result
    // (success, or a clear "not supported on partitioned tables in this version" error
    // for statement kinds this V1 doesn't route).
    std::optional<StringResult> try_route_partitioned(Statement& stmt);
    StringResult route_partitioned_insert(const std::string& table, const PartitionBy& info, Statement::Insert ins);
    StringResult route_partitioned_update(const std::string& table, const PartitionBy& info, Statement::Update upd);
    StringResult route_partitioned_delete(const std::string& table, const PartitionBy& info, Statement::Delete del);
    StringResult route_partitioned_select(const PartitionBy& info, Statement stmt);
    // Runs `child_stmts` in order via execute() (reusing 100% of normal locking/MVCC/undo
    // machinery -- each child is executed exactly as if a client had sent it directly).
    // Wraps them in an implicit BEGIN/COMMIT/ROLLBACK when the caller isn't already in an
    // explicit transaction, so a multi-child autocommit statement is all-or-nothing
    // (matching a single-table statement's atomicity) -- stops and rolls back on the
    // first error.
    struct RoutedRun {
        bool ok = true;
        std::vector<StringResult> child_results; // one per successfully-run child, in order
        StringResult error = StringResult::Ok(""); // meaningful only when !ok
    };
    RoutedRun run_routed_statements(std::vector<Statement> child_stmts);

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

    // ── LOCK TABLES / UNLOCK TABLES (V1 -- session-to-session cooperation only,
    // see executor_txn.cpp's exec_lock_tables for the full design note) ────────
    StringResult exec_lock_tables(SharedDatabase& s, std::vector<std::pair<std::string, bool>> tables);
    StringResult exec_unlock_tables(SharedDatabase& s);
    // Actual release logic given an already-obtained `s` -- used both by the two
    // functions above (which already have `s` from their caller) and by the public
    // no-arg release_explicit_table_locks() (which acquires `s` itself for the
    // connection-teardown case). Never call shared->read() from inside a function that
    // may already be running under execute()'s own outer guard on the same thread --
    // that's a real recursive-lock deadlock risk if a writer happens to be queued at that
    // moment (FairSharedMutex has no reentrancy tracking).
    void release_explicit_table_locks_impl(const SharedDatabase& s);

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
