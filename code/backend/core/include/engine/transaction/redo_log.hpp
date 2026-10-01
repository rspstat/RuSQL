#pragma once

// Committed-change redo log (rusql.redo): the durability mechanism for COMMIT and for
// autocommit statements, replacing "rewrite the whole table file + fsync on every commit".
//
// A commit appends ONE small batch describing the row versions it created/killed and fsyncs
// the log (group-commit batched); table files (.rdb) are only rewritten at checkpoints.
// Recovery loads the table files and re-applies the surviving batches.
//
// Why a physical, order-independent op set instead of replaying SQL: rows are MVCC version
// chains (_xmin/_xmax). A version is immutable except for the one-time stamp of its _xmax by
// the single transaction that killed it, so replay can be made commutative and idempotent:
//   - InsertVersion: add this exact row image unless an identical version is already present.
//   - SetXmax: stamp `xmax` on the version whose image equals `row_json` (ignoring _xmax) if
//     it is still alive; if the version is absent (garbage-collected) the op is a no-op.
// Replaying all InsertVersion ops first and all SetXmax ops second therefore yields the same
// end state regardless of the order batches were appended in (which can differ from the
// logical commit order because row claims are released before the batch is written).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace engine {

struct RedoOp {
    // TableFlushed: written right after a statement-level ("legacy") flush rewrote `table`'s
    // file from memory. Every earlier op for that table is already reflected in the file --
    // and, crucially, so are mutations that were never logged (in-place ON DUPLICATE KEY
    // UPDATE, REPLACE's physical delete, cascades) which replaying those older ops on top of
    // the file would contradict (resurrecting deleted rows). Replay skips them.
    enum class Kind : std::uint8_t { InsertVersion = 1, SetXmax = 2, TableFlushed = 3 };
    Kind kind = Kind::InsertVersion;
    std::string table;
    std::string row_json; // InsertVersion: the new row image. SetXmax: the (pre-stamp) image of the version to kill.
    std::string xmax;     // SetXmax only: id of the committing transaction. Empty = "fill in at batch time".
};

class RedoLog {
public:
    explicit RedoLog(std::string path) : path_(std::move(path)) {}
    ~RedoLog();
    RedoLog(const RedoLog&) = delete;
    RedoLog& operator=(const RedoLog&) = delete;

    // Appends one batch (checksummed as a unit, so a torn tail is detected and dropped) and
    // fflush()es it to the OS. Returns a sequence number to pass to sync().
    std::uint64_t append_batch(const std::vector<RedoOp>& ops);

    // Makes every batch up to `seq` durable (fsync on the already-open append handle -- no
    // reopen). Group commit: committers that arrive while another fsync is running wait on
    // sync_mu_, then find their batch already covered by that fsync and return without one.
    void sync(std::uint64_t seq);

    // Every intact batch in file order, flattened. Stops at the first corrupt/torn batch.
    std::vector<RedoOp> read_all() const;

    // Closes the append handle and deletes the file (a checkpoint has just made it redundant).
    void clear();

    std::uint64_t bytes() const { return bytes_.load(); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    mutable std::mutex mu_;
    std::FILE* fp_ = nullptr;
    std::atomic<std::uint64_t> bytes_{0};
    bool size_known_ = false;
    std::atomic<std::uint64_t> appended_{0};
    std::atomic<std::uint64_t> synced_{0};
    std::mutex sync_mu_;
};

} // namespace engine
