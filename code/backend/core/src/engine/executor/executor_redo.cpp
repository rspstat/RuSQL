// Redo-log durability (see engine/transaction/redo_log.hpp for the design and the
// order-independence argument): COMMIT and "covered" autocommit statements make their
// changes durable by appending one small batch to rusql.redo + a group-committed fsync,
// instead of rewriting the whole table file. Table files are rewritten only at checkpoints
// (size threshold, DDL, boot recovery).

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

#include "engine/executor/executor.hpp"

namespace engine {

namespace {
// A row version's identity for replay: its full image minus _xmax (the only field that
// changes after the version is created -- a version is stamped dead exactly once).
std::string version_identity(Row row) {
    row.erase("_xmax");
    nlohmann::json j = row;
    return j.dump();
}
} // namespace

// Statement kinds whose every mutation goes through TransactionManager::log_insert/update/
// delete: plain single-table INSERT/UPDATE/DELETE. (Cascades, MERGE, multi-table forms,
// ON DUPLICATE KEY/REPLACE and INSERT..SELECT mutate rows without logging, so they keep the
// legacy "flush the table" persistence.) The dispatcher additionally requires that the
// statement's table lock set be exactly one table -- i.e. no FK neighbours that could be
// cascaded into.
bool Executor::is_redo_covered_kind(const Statement& stmt) {
    if (auto* ins = std::get_if<Statement::Insert>(&stmt.data)) {
        return std::holds_alternative<InsertConflict::Abort>(ins->on_conflict.data);
    }
    return std::holds_alternative<Statement::Update>(stmt.data) || std::holds_alternative<Statement::Delete>(stmt.data);
}

void Executor::write_redo_batch(SharedDatabase& s, std::vector<RedoOp> ops) {
    if (ops.empty()) return;
    {
        auto dirty = s.redo_dirty->lock();
        for (auto& op : ops) dirty->insert(op.table);
    }
    std::uint64_t seq = s.redo_log->append_batch(ops);
    s.redo_log->sync(seq); // group commit: one fsync shared by concurrent committers
}

// Call right after a legacy flush of `table` while still holding that table's data lock
// exclusively (so no logged mutation of the table can fall between the flush's snapshot and
// this marker). Makes replay ignore the table's older ops -- see RedoOp::Kind::TableFlushed.
void Executor::redo_mark_flushed(SharedDatabase& s, const std::string& table) {
    if (s.redo_log->bytes() == 0) return; // nothing logged that could contradict the file
    RedoOp op;
    op.kind = RedoOp::Kind::TableFlushed;
    op.table = table;
    std::uint64_t seq = s.redo_log->append_batch({op});
    s.redo_log->sync(seq); // must be durable before we rely on the flushed file alone
}

void Executor::checkpoint_redo_locked(SharedDatabase& s) {
    std::vector<std::string> dirty;
    {
        auto d = s.redo_dirty->lock();
        dirty.assign(d->begin(), d->end());
    }
    // Flush every table that has committed-but-unflushed changes, THEN drop the log: a crash
    // between the two just replays already-applied (idempotent) batches on top of the files.
    for (auto& table : dirty) {
        auto it = s.tables.find(table);
        if (it == s.tables.end()) continue; // dropped since
        s.buffer_pool.write_page(table, it->second);
        s.buffer_pool.flush_page(table, s.disk);
    }
    s.redo_dirty->lock()->clear();
    s.redo_log->clear();
}

// Called at the very top of execute() while NO lock is held: if the log has grown past the
// threshold, take the structural write lock (which waits out every in-flight statement,
// so the tables are quiescent) and checkpoint.
std::uint64_t Executor::redo_checkpoint_bytes() {
    static const std::uint64_t v = [] {
        if (const char* e = std::getenv("RUSQL_REDO_CHECKPOINT_BYTES")) {
            try {
                auto n = std::stoull(e);
                if (n > 0) return static_cast<std::uint64_t>(n);
            } catch (...) {
            }
        }
        return static_cast<std::uint64_t>(4ull * 1024 * 1024);
    }();
    return v;
}

void Executor::maybe_checkpoint_redo() {
    if (!redo_log_ || redo_log_->bytes() < redo_checkpoint_bytes()) return;
    auto s = shared->write();
    if (s->redo_log->bytes() >= redo_checkpoint_bytes()) checkpoint_redo_locked(*s);
}

void Executor::persist_autocommit(SharedDatabase& s, const std::vector<std::string>& lock_tables, bool covered, bool mutating) {
    if (txn.is_active()) return;
    if (covered) {
        write_redo_batch(s, txn.take_redo_ops(std::to_string(s.txn_io->next_id())));
        return;
    }
    // Legacy persistence for everything the redo ops don't fully describe: flush the
    // statement's tables like COMMIT used to. (Previously autocommit INSERT/MERGE/... never
    // reached disk at all -- a crash right after the "OK" lost the rows.)
    // Any ops that were logged by such a statement are redundant with the flush below.
    txn.discard_redo_ops();
    if (!mutating) return;
    for (auto& table : lock_tables) {
        auto data_lock = acquire_table_data_locks(s, {table}, /*exclusive=*/true);
        auto it = s.tables.find(table);
        if (it == s.tables.end()) continue;
        std::vector<Row> copy = it->second;
        s.buffer_pool.write_page(table, std::move(copy));
        s.buffer_pool.flush_page(table, s.disk);
        redo_mark_flushed(s, table);
    }
}

void Executor::recover_from_redo() {
    auto sw = shared->write();
    auto ops = sw->redo_log->read_all();
    if (ops.empty()) {
        sw->redo_log->clear(); // also removes an empty/fully-torn file
        return;
    }

    // Drop every op that precedes its table's last TableFlushed marker (already in the file).
    {
        std::unordered_map<std::string, std::size_t> last_flush;
        for (std::size_t i = 0; i < ops.size(); i++) {
            if (ops[i].kind == RedoOp::Kind::TableFlushed) last_flush[ops[i].table] = i;
        }
        std::vector<RedoOp> kept;
        for (std::size_t i = 0; i < ops.size(); i++) {
            if (ops[i].kind == RedoOp::Kind::TableFlushed) continue;
            auto lf = last_flush.find(ops[i].table);
            if (lf != last_flush.end() && i < lf->second) continue;
            kept.push_back(std::move(ops[i]));
        }
        ops = std::move(kept);
    }

    std::unordered_set<std::string> touched;
    // table -> identity -> position in sw->tables[table]
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> pos;
    auto ensure_index = [&](const std::string& table) -> std::unordered_map<std::string, std::size_t>* {
        auto tit = sw->tables.find(table);
        if (tit == sw->tables.end()) return nullptr; // table no longer exists
        auto pit = pos.find(table);
        if (pit == pos.end()) {
            auto& m = pos[table];
            for (std::size_t i = 0; i < tit->second.size(); i++) m.emplace(version_identity(tit->second[i]), i);
            return &m;
        }
        return &pit->second;
    };

    // Phase 1: every version that was created (idempotent: skip if already on disk).
    for (auto& op : ops) {
        if (op.kind != RedoOp::Kind::InsertVersion) continue;
        auto* idx = ensure_index(op.table);
        if (!idx) continue;
        try {
            Row row = nlohmann::json::parse(op.row_json).get<Row>();
            std::string id = version_identity(row);
            if (idx->count(id)) continue;
            auto& rows = sw->tables[op.table];
            idx->emplace(id, rows.size());
            rows.push_back(std::move(row));
            touched.insert(op.table);
        } catch (...) {
        }
    }
    // Phase 2: every version that was killed. Done after all inserts so batch order (which
    // may differ from logical commit order) cannot matter.
    for (auto& op : ops) {
        if (op.kind != RedoOp::Kind::SetXmax) continue;
        auto* idx = ensure_index(op.table);
        if (!idx) continue;
        try {
            Row old = nlohmann::json::parse(op.row_json).get<Row>();
            auto it = idx->find(version_identity(old));
            if (it == idx->end()) continue; // garbage-collected before the crash
            Row& live = sw->tables[op.table][it->second];
            auto xit = live.find("_xmax");
            if (xit == live.end() || xit->second == "0") {
                live["_xmax"] = op.xmax;
                touched.insert(op.table);
            }
        } catch (...) {
        }
    }

    // Same index repair the WAL-undo recovery does for every table it touched: secondary/
    // hash/composite/PK indexes were loaded from the pre-crash state.
    for (auto& table : touched) {
        auto tit = sw->tables.find(table);
        if (tit == sw->tables.end()) continue;
        const std::vector<Row>& rows = tit->second;
        std::string pk_col = "id";
        if (auto* sc = sw->catalog.get_table(table)) {
            for (auto& c : sc->columns) {
                if (c.primary_key) {
                    pk_col = c.name;
                    break;
                }
            }
        }
        if (auto idx_it = sw->indexes.find(table); idx_it != sw->indexes.end()) idx_it->second = build_pk_tree(rows, pk_col);
        rebuild_secondary_indexes(*sw, table, rows);
        for (auto& [k, ci] : sw->composite_indexes) {
            if (ci.table == table) ci.rebuild(rows);
        }
        if (auto mit = sw->row_pk_pos.find(table); mit != sw->row_pk_pos.end()) mit->second.clear();
        sw->buffer_pool.write_page(table, rows);
        sw->buffer_pool.flush_page(table, sw->disk);
    }
    sw->redo_log->clear();
}

} // namespace engine
