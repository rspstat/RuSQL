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
    return row_to_json(row);
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
        s.buffer_pool.write_through(table, it->second, s.disk);
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

// Bytes of log a table row is "worth" when deciding how large the redo log may grow (see maybe_checkpoint_redo).
// RUSQL_REDO_CHECKPOINT_ROW_BYTES=0 turns the size-awareness off (the crash fuzzers do, to checkpoint at tiny sizes).
static std::uint64_t redo_checkpoint_row_bytes() {
    static const std::uint64_t v = [] {
        if (const char* e = std::getenv("RUSQL_REDO_CHECKPOINT_ROW_BYTES")) {
            try {
                return static_cast<std::uint64_t>(std::stoull(e));
            } catch (...) {
            }
        }
        return static_cast<std::uint64_t>(128);
    }();
    return v;
}

void Executor::maybe_checkpoint_redo() {
    auto threshold_of = [](const RedoLog& log) { return log.checkpoint_at() ? log.checkpoint_at() : redo_checkpoint_bytes(); };
    if (!redo_log_ || redo_log_->bytes() < threshold_of(*redo_log_)) return;
    auto s = shared->write();
    RedoLog& log = *s->redo_log;
    if (log.bytes() < threshold_of(log)) return;
    // A checkpoint rewrites the files of every table with logged changes, so its cost grows with those tables. With a
    // fixed trigger (4 MB of log ~ 50,000 changed rows) a table of a million rows was rewritten after every 50,000
    // changes -- each change paid for ~20 rows of rewriting. The log is allowed to grow in proportion to the rows a
    // checkpoint has to write (128 bytes per row, so a change costs a constant ~1 row), which keeps recovery (a replay of
    // that log) cheaper than loading the tables it protects.
    std::uint64_t want = redo_checkpoint_bytes();
    {
        std::uint64_t rows = 0;
        auto dirty = s->redo_dirty->lock();
        for (auto& table : *dirty) {
            if (auto it = s->tables.find(table); it != s->tables.end()) rows += it->second.size();
        }
        want = std::max<std::uint64_t>(want, rows * redo_checkpoint_row_bytes());
    }
    if (log.bytes() >= want) {
        checkpoint_redo_locked(*s);
    } else {
        log.set_checkpoint_at(want); // not yet: do not take the write lock again until the log is this big
    }
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
        s.buffer_pool.write_through(table, it->second, s.disk);
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
    // Identical row images are NOT one version: a table without a primary key can hold several rows with the very same
    // image (same values, same _xmin when one transaction inserted them), and a replay that treated "identical image" as
    // "already there" kept one of them -- `BEGIN; INSERT (1,'x'),(1,'x'); COMMIT` came back with ONE row after a crash
    // (autocommit statements escaped only because every row of one used to get its own _xmin). So the replay counts:
    // the k-th logged copy of an image is only added if fewer than k copies are present, and the k-th logged kill of
    // an image by one transaction only stamps a row if fewer than k copies are already killed by it.
    // table -> identity -> positions in sw->tables[table] (ascending) of every row with that identity
    using Positions = std::unordered_map<std::string, std::vector<std::size_t>>;
    std::unordered_map<std::string, Positions> pos;
    auto ensure_index = [&](const std::string& table) -> Positions* {
        auto tit = sw->tables.find(table);
        if (tit == sw->tables.end()) return nullptr; // table no longer exists
        auto pit = pos.find(table);
        if (pit == pos.end()) {
            auto& m = pos[table];
            for (std::size_t i = 0; i < tit->second.size(); i++) m[version_identity(tit->second[i])].push_back(i);
            return &m;
        }
        return &pit->second;
    };

    // Phase 1: every version that was created (idempotent: not added again if already on disk).
    std::unordered_map<std::string, std::size_t> inserts_seen; // table \0 identity -> logged copies seen so far
    for (auto& op : ops) {
        if (op.kind != RedoOp::Kind::InsertVersion) continue;
        auto* idx = ensure_index(op.table);
        if (!idx) continue;
        try {
            Row row = row_from_json(op.row_json);
            std::string id = version_identity(row);
            std::size_t k = ++inserts_seen[op.table + '\x00' + id];
            auto& at = (*idx)[id];
            if (k <= at.size()) continue; // this copy is already in the table
            auto& rows = sw->tables[op.table];
            at.push_back(rows.size());
            rows.push_back(std::move(row));
            touched.insert(op.table);
        } catch (...) {
        }
    }
    // Phase 2: every version that was killed. Done after all inserts so batch order (which
    // may differ from logical commit order) cannot matter.
    std::unordered_map<std::string, std::size_t> kills_seen;    // table \0 identity \0 xmax -> logged kills seen so far
    std::unordered_map<std::string, std::size_t> kills_present; // ... -> copies already killed by that transaction
    for (auto& op : ops) {
        if (op.kind != RedoOp::Kind::SetXmax) continue;
        auto* idx = ensure_index(op.table);
        if (!idx) continue;
        try {
            Row old = row_from_json(op.row_json);
            auto it = idx->find(version_identity(old));
            if (it == idx->end()) continue; // garbage-collected before the crash
            auto& rows = sw->tables[op.table];
            std::string key = op.table + '\x00' + it->first + '\x00' + op.xmax;
            auto present = kills_present.find(key);
            if (present == kills_present.end()) {
                std::size_t n = 0;
                for (std::size_t p : it->second) {
                    auto x = rows[p].find("_xmax");
                    if (x != rows[p].end() && x->second == op.xmax) n++;
                }
                present = kills_present.emplace(key, n).first;
            }
            if (++kills_seen[key] <= present->second) continue; // this kill is already applied
            for (std::size_t p : it->second) {
                auto xit = rows[p].find("_xmax");
                if (xit == rows[p].end() || xit->second == "0") {
                    rows[p]["_xmax"] = op.xmax;
                    touched.insert(op.table);
                    break;
                }
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
        sw->buffer_pool.write_through(table, rows, sw->disk);
    }
    sw->redo_log->clear();
}

} // namespace engine
