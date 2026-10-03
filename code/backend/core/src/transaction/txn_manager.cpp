#include "engine/row_json.hpp"
#include "engine/transaction/txn_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "engine/storage/atomic_write.hpp"

namespace fs = std::filesystem;

namespace engine {

namespace {
void push_u64_le(std::vector<std::uint8_t>& buf, std::uint64_t v) {
    for (int i = 0; i < 8; i++) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void push_u32_le(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    for (int i = 0; i < 4; i++) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
std::optional<std::uint64_t> read_u64_le(const std::vector<std::uint8_t>& buf, std::size_t pos) {
    if (pos + 8 > buf.size()) return std::nullopt;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<std::uint64_t>(buf[pos + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}
// FNV-1a 32비트 -- wal.cpp의 동일 헬퍼와 같은 이유로 여기도 중복 정의(이 파일 전체의 기존
// 관례). 디스크 손상으로 Undo 레코드 내용은 바뀌었지만 길이 프리픽스는 우연히 그대로인 경우를
// decode()가 명시적으로 감지하도록 함.
std::uint32_t fnv1a32(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}
std::uint64_t parse_txn_id(const Row& row, const char* col) {
    auto it = row.find(col);
    if (it == row.end()) return 0;
    try {
        return std::stoull(it->second);
    } catch (...) {
        return 0;
    }
}

// Mirrors Executor::is_visible_for_read (executor_ddl.cpp) exactly -- duplicated here
// rather than shared, since this transaction-module file has no dependency on the engine
// module and Row/SnapshotCtx are the only pieces it actually needs.
bool visible_under(const Row& row, const SnapshotCtx& ctx) {
    auto committed_as_of = [&](std::uint64_t id) { return id == 0 || (id < ctx.cutoff && !ctx.in_progress.count(id)); };
    std::uint64_t xmin = parse_txn_id(row, "_xmin");
    if (xmin != ctx.self_txn_id && !committed_as_of(xmin)) return false;
    auto xmax_it = row.find("_xmax");
    if (xmax_it == row.end() || xmax_it->second == "0") return true;
    std::uint64_t xmax = parse_txn_id(row, "_xmax");
    if (xmax == ctx.self_txn_id) return false;
    if (committed_as_of(xmax)) return false;
    return true;
}

std::optional<std::uint32_t> read_u32_le(const std::vector<std::uint8_t>& buf, std::size_t pos) {
    if (pos + 4 > buf.size()) return std::nullopt;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<std::uint32_t>(buf[pos + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}
std::optional<std::string> read_string(const std::vector<std::uint8_t>& buf, std::size_t& pos) {
    auto len_opt = read_u32_le(buf, pos);
    if (!len_opt) return std::nullopt;
    std::size_t len = *len_opt;
    pos += 4;
    if (pos + len > buf.size()) return std::nullopt;
    std::string s(reinterpret_cast<const char*>(buf.data() + pos), len);
    pos += len;
    return s;
}
bool read_all_bytes(const std::string& path, std::vector<std::uint8_t>& out) {
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
// The tracking record of `contents` for the file at `path` (reset when a different file is in use).
TxnIoShared::LogContents& undo_tracked(TxnIoShared::LogContents& contents, const std::string& path) {
    if (contents.path != path) {
        contents.path = path;
        contents.known = false;
        contents.txns.clear();
    }
    return contents;
}
bool undo_truncate_to_zero(std::FILE* fp) {
    if (std::fflush(fp) != 0) return false;
#ifdef _WIN32
    return _chsize_s(_fileno(fp), 0) == 0;
#else
    return ftruncate(fileno(fp), 0) == 0;
#endif
}
} // namespace

// ─── UndoLogFile ──────────────────────────────────────────────────────────

UndoLogFile::UndoLogFile(const std::string& dir, std::shared_ptr<TxnIoShared> io)
    : path_(dir + "/_undo.log"), io_(std::move(io)) {}

std::vector<std::uint8_t> UndoLogFile::encode(const UndoEntry& entry) {
    std::uint8_t op = entry.operation == "INSERT" ? 0x01 : entry.operation == "UPDATE" ? 0x02
                       : entry.operation == "DELETE" ? 0x03 : 0x00;
    std::vector<std::uint8_t> buf;
    buf.push_back(op);
    push_u64_le(buf, entry.txn_id);
    push_u32_le(buf, static_cast<std::uint32_t>(entry.table.size()));
    buf.insert(buf.end(), entry.table.begin(), entry.table.end());
    push_u32_le(buf, static_cast<std::uint32_t>(entry.key.size()));
    buf.insert(buf.end(), entry.key.begin(), entry.key.end());
    if (entry.old_data) {
        buf.push_back(1);
        push_u32_le(buf, static_cast<std::uint32_t>(entry.old_data->size()));
        buf.insert(buf.end(), entry.old_data->begin(), entry.old_data->end());
    } else {
        buf.push_back(0);
    }
    push_u32_le(buf, fnv1a32(buf.data(), buf.size())); // 체크섬은 그 앞의 전체 레코드 바이트를 커버
    return buf;
}

std::optional<UndoEntry> UndoLogFile::decode(const std::vector<std::uint8_t>& buf, std::size_t& pos) {
    std::size_t start = pos;
    if (pos >= buf.size()) return std::nullopt;
    std::uint8_t op_byte = buf[pos];
    pos += 1;
    std::string operation;
    switch (op_byte) {
        case 0x01: operation = "INSERT"; break;
        case 0x02: operation = "UPDATE"; break;
        case 0x03: operation = "DELETE"; break;
        default: return std::nullopt;
    }
    auto txn_id = read_u64_le(buf, pos);
    if (!txn_id) return std::nullopt;
    pos += 8;
    auto table = read_string(buf, pos);
    if (!table) return std::nullopt;
    auto key = read_string(buf, pos);
    if (!key) return std::nullopt;
    if (pos >= buf.size()) return std::nullopt;
    std::uint8_t has_data = buf[pos];
    pos += 1;
    std::optional<std::string> old_data;
    if (has_data == 1) {
        old_data = read_string(buf, pos);
        if (!old_data) return std::nullopt;
    }

    auto stored_checksum = read_u32_le(buf, pos);
    if (!stored_checksum) return std::nullopt;
    std::uint32_t actual = fnv1a32(buf.data() + start, pos - start);
    if (actual != *stored_checksum) return std::nullopt; // 손상 감지 -- 기존과 동일하게 이 지점에서 중단
    pos += 4;

    return UndoEntry{*txn_id, operation, *table, *key, old_data};
}

void UndoLogFile::append_locked(const UndoEntry& entry) const {
    auto encoded = encode(entry);
    // Persistent append handle (see TxnIoShared::undo_append_handle_locked) -- reopening the
    // file for every single record dominated per-statement transaction cost.
    auto& contents = undo_tracked(io_->undo_contents, path_);
    std::FILE* fp = io_->undo_append_handle_locked(path_);
    if (!fp) throw std::runtime_error("Undo log 파일 열기 실패");
    if (std::fwrite(encoded.data(), 1, encoded.size(), fp) != encoded.size() || std::fflush(fp) != 0) {
        contents.known = false; // part of a record may be in the file
        io_->close_undo_handle_locked();
        throw std::runtime_error("Undo log 기록 실패");
    }
    if (contents.known) contents.txns.insert(entry.txn_id);
}

void UndoLogFile::append(const UndoEntry& entry) {
    std::lock_guard<std::mutex> g(io_->undo_lock);
    append_locked(entry);
}

std::vector<UndoEntry> UndoLogFile::read_all_locked() const {
    if (!fs::exists(path_)) return {};
    std::vector<std::uint8_t> buf;
    if (!read_all_bytes(path_, buf)) return {};
    std::vector<UndoEntry> entries;
    std::size_t pos = 0;
    while (auto e = decode(buf, pos)) entries.push_back(*e);
    return entries;
}

std::vector<UndoEntry> UndoLogFile::read_all() {
    std::lock_guard<std::mutex> g(io_->undo_lock);
    return read_all_locked();
}

void UndoLogFile::clear_locked() const {
    io_->close_undo_handle_locked(); // Windows can't delete a file that still has an open handle
    std::error_code ec;
    fs::remove(path_, ec);
    auto& contents = undo_tracked(io_->undo_contents, path_);
    contents.known = true; // the file is gone: nothing in it
    contents.txns.clear();
}

void UndoLogFile::clear() {
    std::lock_guard<std::mutex> g(io_->undo_lock);
    clear_locked();
}

void UndoLogFile::remove_txn(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(io_->undo_lock);
    auto& contents = undo_tracked(io_->undo_contents, path_);
    if (contents.known) {
        if (!contents.txns.count(txn_id)) return; // none of its entries are in the file
        if (contents.txns.size() == 1) {
            // Alone in the file: empty it in place without reading it (see TxnIoShared::LogContents).
            std::FILE* fp = io_->undo_append_handle_locked(path_);
            if (fp && undo_truncate_to_zero(fp)) {
                contents.txns.clear();
                return;
            }
        }
    }
    auto all = read_all_locked();
    std::vector<UndoEntry> remaining;
    for (auto& e : all) {
        if (e.txn_id != txn_id) remaining.push_back(e);
    }
    if (remaining.empty()) {
        // Empty in place through the persistent handle (see WalManager::remove_txn).
        if (fs::exists(path_)) {
            std::FILE* fp = io_->undo_append_handle_locked(path_);
            if (fp && undo_truncate_to_zero(fp)) {
                contents.known = true;
                contents.txns.clear();
                return;
            }
        }
        clear_locked();
        return;
    }
    std::vector<std::uint8_t> buf;
    for (auto& e : remaining) {
        auto enc = encode(e);
        buf.insert(buf.end(), enc.begin(), enc.end());
    }
    io_->close_undo_handle_locked(); // can't rename over a file with an open handle on Windows
    write_bytes_atomic(path_, buf.data(), buf.size());
    contents.known = true;
    contents.txns.clear();
    for (auto& e : remaining) contents.txns.insert(e.txn_id);
}

void UndoLogFile::rewrite_txn(std::uint64_t txn_id, const std::vector<UndoEntry>& entries) {
    std::lock_guard<std::mutex> g(io_->undo_lock);
    auto all_existing = read_all_locked();
    std::vector<UndoEntry> all;
    for (auto& e : all_existing) {
        if (e.txn_id != txn_id) all.push_back(e);
    }
    all.insert(all.end(), entries.begin(), entries.end());
    if (all.empty()) {
        clear_locked();
        return;
    }
    std::vector<std::uint8_t> buf;
    for (auto& e : all) {
        auto enc = encode(e);
        buf.insert(buf.end(), enc.begin(), enc.end());
    }
    io_->close_undo_handle_locked(); // can't rename over a file with an open handle on Windows
    write_bytes_atomic(path_, buf.data(), buf.size());
    auto& contents = undo_tracked(io_->undo_contents, path_);
    contents.known = true;
    contents.txns.clear();
    for (auto& e : all) contents.txns.insert(e.txn_id);
}

// ─── TransactionManager ───────────────────────────────────────────────────

TransactionManager::TransactionManager(const std::string& dir, std::shared_ptr<TxnIoShared> io)
    : io_(io), wal_(dir, io), undo_log_file_(dir, io) {}

void TransactionManager::set_isolation_level(IsolationLevel level) {
    isolation_level_ = level;
}

Result<std::uint64_t, std::string> TransactionManager::begin_with_snapshot(const std::unordered_set<std::uint64_t>& active_txn_ids) {
    if (active_) return Result<std::uint64_t, std::string>::Err("Transaction already active. COMMIT or ROLLBACK first.");
    txn_id_ = io_->next_id();
    active_ = true;
    undo_log_.clear();
    read_set_.clear();
    discard_redo_ops();
    redo_incomplete_ = false;

    if (isolation_level_ == IsolationLevel::RepeatableRead || isolation_level_ == IsolationLevel::Serializable) {
        frozen_ctx_ = SnapshotCtx{txn_id_, io_->peek_next_id(), active_txn_ids};
    } else {
        frozen_ctx_ = std::nullopt;
    }
    return Result<std::uint64_t, std::string>::Ok(txn_id_);
}

void TransactionManager::record_read(const std::string& table, const std::string& pk_col, const std::string& key) {
    if (!active_ || isolation_level_ != IsolationLevel::Serializable) return;
    read_set_.insert(table + '\x00' + pk_col + '\x00' + key);
}

Result<void, std::string> TransactionManager::validate_serializable(
    const std::unordered_map<std::string, std::vector<Row>>& live_tables,
    const std::unordered_set<std::uint64_t>& active_txn_ids_now) const {
    if (isolation_level_ != IsolationLevel::Serializable) return Result<void, std::string>::Ok();
    if (!frozen_ctx_) return Result<void, std::string>::Ok();

    // "Right now" ctx, for the *committed* side of the comparison: an xmin/xmax id not in
    // active_txn_ids_now has definitely terminated by now (Stage 2's apply_rollback
    // physically erases/reverts a rolled-back transaction's own marks, so mere presence
    // means it committed) -- cutoff is a generous upper bound, id membership in
    // active_txn_ids_now is what actually matters here.
    SnapshotCtx now_ctx{txn_id_, std::numeric_limits<std::uint64_t>::max(), active_txn_ids_now};

    for (auto& encoded : read_set_) {
        auto sep1 = encoded.find('\x00');
        auto sep2 = encoded.find('\x00', sep1 + 1);
        std::string table = encoded.substr(0, sep1);
        std::string pk_col = encoded.substr(sep1 + 1, sep2 - sep1 - 1);
        std::string key = encoded.substr(sep2 + 1);

        auto it = live_tables.find(table);
        if (it == live_tables.end()) continue;

        for (auto& row : it->second) {
            auto pit = row.find(pk_col);
            if (pit == row.end() || pit->second != key) continue;
            // A physical version of a row this transaction read whose visibility differs
            // between what was frozen at BEGIN and what's true right now (excluding
            // still-in-flight writers, which aren't a conflict yet) means some other
            // transaction wrote (inserted/updated/deleted) this exact key after this
            // transaction's snapshot began, and that write has since committed.
            if (visible_under(row, *frozen_ctx_) != visible_under(row, now_ctx)) {
                return Result<void, std::string>::Err("Serialization failure: table '" + table + "' row '" + key +
                                                        "' was modified by another transaction since this one started. ROLLBACK required.");
            }
        }
    }
    return Result<void, std::string>::Ok();
}

Result<std::uint64_t, std::string> TransactionManager::begin() {
    if (active_) return Result<std::uint64_t, std::string>::Err("Transaction already active. COMMIT or ROLLBACK first.");
    txn_id_ = io_->next_id();
    active_ = true;
    undo_log_.clear();
    discard_redo_ops();
    redo_incomplete_ = false;
    return Result<std::uint64_t, std::string>::Ok(txn_id_);
}

std::vector<std::string> TransactionManager::dirty_tables() const {
    std::vector<std::string> tables;
    tables.reserve(undo_log_.size());
    for (auto& e : undo_log_) tables.push_back(e.table);
    std::sort(tables.begin(), tables.end());
    tables.erase(std::unique(tables.begin(), tables.end()), tables.end());
    return tables;
}

Result<void, std::string> TransactionManager::commit() {
    if (!active_) return Result<void, std::string>::Err("No active transaction.");
    wal_.log_commit(txn_id_);
    wal_.remove_txn(txn_id_);
    undo_log_.clear();
    undo_log_file_.remove_txn(txn_id_);
    frozen_ctx_ = std::nullopt;
    read_set_.clear();
    discard_redo_ops();
    redo_incomplete_ = false;
    savepoints_.clear();
    active_ = false;
    return Result<void, std::string>::Ok();
}

Result<void, std::string> TransactionManager::commit_write_record() {
    if (!active_) return Result<void, std::string>::Err("No active transaction.");
    wal_.log_commit_no_sync(txn_id_);
    return Result<void, std::string>::Ok();
}

void TransactionManager::commit_finalize() {
    wal_.remove_txn(txn_id_);
    undo_log_.clear();
    undo_log_file_.remove_txn(txn_id_);
    frozen_ctx_ = std::nullopt;
    read_set_.clear();
    discard_redo_ops();
    redo_incomplete_ = false;
    savepoints_.clear();
    active_ = false;
}

std::vector<UndoEntry> TransactionManager::rollback() {
    wal_.log_rollback(txn_id_);
    wal_.remove_txn(txn_id_);
    std::vector<UndoEntry> entries(undo_log_.rbegin(), undo_log_.rend());
    undo_log_.clear();
    undo_log_file_.remove_txn(txn_id_);
    frozen_ctx_ = std::nullopt;
    read_set_.clear();
    discard_redo_ops();
    redo_incomplete_ = false;
    savepoints_.clear();
    active_ = false;
    return entries;
}

Result<std::vector<UndoEntry>, std::string> TransactionManager::abort() {
    if (!active_) return Result<std::vector<UndoEntry>, std::string>::Err("No active transaction.");
    wal_.log_rollback(txn_id_);
    wal_.remove_txn(txn_id_);
    std::vector<UndoEntry> entries(undo_log_.rbegin(), undo_log_.rend());
    undo_log_.clear();
    undo_log_file_.remove_txn(txn_id_);
    frozen_ctx_ = std::nullopt;
    read_set_.clear();
    discard_redo_ops();
    redo_incomplete_ = false;
    savepoints_.clear();
    active_ = false;
    return Result<std::vector<UndoEntry>, std::string>::Ok(entries);
}

Result<void, std::string> TransactionManager::create_savepoint(const std::string& name) {
    if (!active_) return Result<void, std::string>::Err("No active transaction. Use BEGIN first.");
    savepoints_.erase(std::remove_if(savepoints_.begin(), savepoints_.end(),
                                      [&](auto& p) { return p.first == name; }),
                       savepoints_.end());
    savepoints_.emplace_back(name, undo_log_.size());
    return Result<void, std::string>::Ok();
}

Result<std::vector<UndoEntry>, std::string> TransactionManager::rollback_to_savepoint(const std::string& name) {
    if (!active_) return Result<std::vector<UndoEntry>, std::string>::Err("No active transaction.");
    auto it = std::find_if(savepoints_.rbegin(), savepoints_.rend(), [&](auto& p) { return p.first == name; });
    if (it == savepoints_.rend()) return Result<std::vector<UndoEntry>, std::string>::Err("Savepoint '" + name + "' not found");
    std::size_t undo_len = it->second;
    std::size_t pos = static_cast<std::size_t>(std::distance(it, savepoints_.rend()) - 1);

    std::vector<UndoEntry> entries(undo_log_.begin() + static_cast<std::ptrdiff_t>(undo_len), undo_log_.end());
    std::reverse(entries.begin(), entries.end());
    undo_log_.resize(undo_len);
    redo_ops_.resize(undo_len == 0 ? 0 : redo_marks_[undo_len - 1]);
    redo_marks_.resize(undo_len);
    savepoints_.resize(pos + 1);
    undo_log_file_.rewrite_txn(txn_id_, undo_log_);
    return Result<std::vector<UndoEntry>, std::string>::Ok(entries);
}

Result<void, std::string> TransactionManager::release_savepoint(const std::string& name) {
    if (!active_) return Result<void, std::string>::Err("No active transaction.");
    auto it = std::find_if(savepoints_.rbegin(), savepoints_.rend(), [&](auto& p) { return p.first == name; });
    if (it == savepoints_.rend()) return Result<void, std::string>::Err("Savepoint '" + name + "' not found");
    std::size_t pos = static_cast<std::size_t>(std::distance(it, savepoints_.rend()) - 1);
    savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(pos));
    return Result<void, std::string>::Ok();
}

std::vector<RedoOp> TransactionManager::take_redo_ops(const std::string& fallback_xmax) {
    std::vector<RedoOp> out = std::move(redo_ops_);
    for (auto& op : out) {
        if (op.kind == RedoOp::Kind::SetXmax && op.xmax.empty()) op.xmax = fallback_xmax;
    }
    discard_redo_ops();
    return out;
}

void TransactionManager::discard_redo_ops() {
    redo_ops_.clear();
    redo_marks_.clear();
}

namespace {
// The _xmin of a row image (the id of the transaction that created that version) -- the
// matching _xmax value for the version it superseded.
std::string xmin_of_image(const std::string& json) {
    try {
        Row row = row_from_json(json); // (the same text nlohmann::json::parse reads, without building a json tree per row)
        if (auto it = row.find("_xmin"); it != row.end()) return it->second;
    } catch (...) {
    }
    return "";
}
} // namespace

void TransactionManager::log_insert(const std::string& table, const std::string& key, const std::string& data) {
    redo_ops_.push_back({RedoOp::Kind::InsertVersion, table, data, ""});
    if (active_) redo_marks_.push_back(redo_ops_.size());
    if (!active_) return;
    wal_.log_insert(txn_id_, table, key, data);
    UndoEntry entry{txn_id_, "INSERT", table, key, std::nullopt};
    undo_log_file_.append(entry);
    undo_log_.push_back(entry);
}

void TransactionManager::log_update(const std::string& table, const std::string& key, const std::string& old_data,
                                     const std::string& new_data) {
    redo_ops_.push_back({RedoOp::Kind::SetXmax, table, old_data, xmin_of_image(new_data)});
    redo_ops_.push_back({RedoOp::Kind::InsertVersion, table, new_data, ""});
    if (active_) redo_marks_.push_back(redo_ops_.size());
    if (!active_) return;
    wal_.log_update(txn_id_, table, key, new_data);
    UndoEntry entry{txn_id_, "UPDATE", table, key, old_data};
    undo_log_file_.append(entry);
    undo_log_.push_back(entry);
}

void TransactionManager::log_delete(const std::string& table, const std::string& key, const std::string& old_data) {
    redo_ops_.push_back({RedoOp::Kind::SetXmax, table, old_data, ""}); // xmax = committing txn id, filled at batch time
    if (active_) redo_marks_.push_back(redo_ops_.size());
    if (!active_) return;
    wal_.log_delete(txn_id_, table, key);
    UndoEntry entry{txn_id_, "DELETE", table, key, old_data};
    undo_log_file_.append(entry);
    undo_log_.push_back(entry);
}

std::vector<WalRecord> TransactionManager::wal_records() const { return wal_.read_all(); }
std::uint64_t TransactionManager::wal_size() const { return wal_.file_size(); }
void TransactionManager::wal_clear() { wal_.clear(); }

void TransactionManager::do_checkpoint(bool safe_to_truncate) {
    if (!safe_to_truncate) return;
    wal_.log_checkpoint();
    wal_.truncate_to_last_checkpoint();
}

bool TransactionManager::needs_auto_checkpoint() const { return wal_.needs_auto_checkpoint(); }

void TransactionManager::release_file_handles() {
    {
        std::lock_guard<std::mutex> g(io_->wal_lock);
        io_->close_wal_handle_locked();
    }
    std::lock_guard<std::mutex> g(io_->undo_lock);
    io_->close_undo_handle_locked();
}

std::vector<UndoEntry> TransactionManager::read_undo_log_file() { return undo_log_file_.read_all(); }
void TransactionManager::clear_undo_log_file() { undo_log_file_.clear(); }

} // namespace engine
