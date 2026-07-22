#include "engine/transaction/wal.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

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
} // namespace

std::optional<WalOp> wal_op_from_u8(std::uint8_t v) {
    switch (v) {
        case 0x01: return WalOp::Insert;
        case 0x02: return WalOp::Update;
        case 0x03: return WalOp::Delete;
        case 0xFF: return WalOp::Commit;
        case 0xFE: return WalOp::Rollback;
        case 0xFD: return WalOp::Checkpoint;
        default: return std::nullopt;
    }
}

std::uint64_t TxnIoShared::next_id() {
    std::lock_guard<std::mutex> g(counter_mutex_);
    return next_txn_id_++;
}

WalManager::WalManager(const std::string& dir, std::shared_ptr<TxnIoShared> io)
    : path_(dir + "/rusql.wal"), io_(std::move(io)) {}

std::vector<std::uint8_t> WalManager::encode(const WalRecord& record) {
    std::vector<std::uint8_t> buf;
    buf.push_back(static_cast<std::uint8_t>(record.op));
    push_u64_le(buf, record.txn_id);
    push_u32_le(buf, static_cast<std::uint32_t>(record.table_name.size()));
    buf.insert(buf.end(), record.table_name.begin(), record.table_name.end());
    push_u32_le(buf, static_cast<std::uint32_t>(record.key.size()));
    buf.insert(buf.end(), record.key.begin(), record.key.end());
    push_u32_le(buf, static_cast<std::uint32_t>(record.data.size()));
    buf.insert(buf.end(), record.data.begin(), record.data.end());
    return buf;
}

std::optional<WalRecord> WalManager::decode(const std::vector<std::uint8_t>& buf, std::size_t& pos) {
    if (pos >= buf.size()) return std::nullopt;
    auto op = wal_op_from_u8(buf[pos]);
    if (!op) return std::nullopt;
    pos += 1;

    auto txn_id = read_u64_le(buf, pos);
    if (!txn_id) return std::nullopt;
    pos += 8;

    auto table_name = read_string(buf, pos);
    if (!table_name) return std::nullopt;
    auto key = read_string(buf, pos);
    if (!key) return std::nullopt;
    auto data = read_string(buf, pos);
    if (!data) return std::nullopt;

    return WalRecord{*op, *txn_id, *table_name, *key, *data};
}

// std::fstream has no portable fsync, but its underlying FILE*/fd does -- write via C
// stdio instead so a real disk sync (matching Rust's file.sync_all()) is possible. Every
// step's return value is checked and throws on failure, matching Rust's
// `.expect("WAL 기록 실패")`/`.expect("WAL fsync 실패")` panics -- a silently-swallowed
// write/flush/sync failure here would let a COMMIT report success without the record
// actually being durable, defeating the entire point of this function.
void WalManager::write_encoded_locked(const std::vector<std::uint8_t>& encoded, bool sync) const {
    std::FILE* fp = std::fopen(path_.c_str(), "ab");
    if (!fp) throw std::runtime_error("WAL 파일 열기 실패");
    std::size_t written = std::fwrite(encoded.data(), 1, encoded.size(), fp);
    if (written != encoded.size()) {
        std::fclose(fp);
        throw std::runtime_error("WAL 기록 실패");
    }
    if (std::fflush(fp) != 0) {
        std::fclose(fp);
        throw std::runtime_error("WAL 기록 실패");
    }
    if (sync) {
#ifdef _WIN32
        bool ok = _commit(_fileno(fp)) == 0;
#else
        bool ok = fsync(fileno(fp)) == 0;
#endif
        if (!ok) {
            std::fclose(fp);
            throw std::runtime_error("WAL fsync 실패");
        }
    }
    std::fclose(fp);
}

void WalManager::append(const WalRecord& record) {
    std::lock_guard<std::mutex> g(io_->wal_lock);
    write_encoded_locked(encode(record), false);
}

void WalManager::log_insert(std::uint64_t txn_id, const std::string& table, const std::string& key, const std::string& data) {
    append(WalRecord{WalOp::Insert, txn_id, table, key, data});
}

void WalManager::log_update(std::uint64_t txn_id, const std::string& table, const std::string& key, const std::string& data) {
    append(WalRecord{WalOp::Update, txn_id, table, key, data});
}

void WalManager::log_delete(std::uint64_t txn_id, const std::string& table, const std::string& key) {
    append(WalRecord{WalOp::Delete, txn_id, table, key, ""});
}

void WalManager::log_commit(std::uint64_t txn_id) {
    WalRecord record{WalOp::Commit, txn_id, "", "", ""};
    std::lock_guard<std::mutex> g(io_->wal_lock);
    write_encoded_locked(encode(record), true);
}

void WalManager::log_commit_no_sync(std::uint64_t txn_id) {
    WalRecord record{WalOp::Commit, txn_id, "", "", ""};
    std::lock_guard<std::mutex> g(io_->wal_lock);
    write_encoded_locked(encode(record), false);
}

void WalManager::log_rollback(std::uint64_t txn_id) {
    append(WalRecord{WalOp::Rollback, txn_id, "", "", ""});
}

void WalManager::log_checkpoint() {
    WalRecord record{WalOp::Checkpoint, 0, "", "", ""};
    std::lock_guard<std::mutex> g(io_->wal_lock);
    write_encoded_locked(encode(record), true);
}

std::vector<WalRecord> WalManager::read_all_locked() const {
    if (!fs::exists(path_)) return {};
    std::vector<std::uint8_t> buf;
    if (!read_all_bytes(path_, buf)) return {};
    std::vector<WalRecord> records;
    std::size_t pos = 0;
    while (auto record = decode(buf, pos)) records.push_back(*record);
    return records;
}

std::vector<WalRecord> WalManager::read_all() const {
    std::lock_guard<std::mutex> g(io_->wal_lock);
    return read_all_locked();
}

void WalManager::clear_locked() const {
    std::error_code ec;
    fs::remove(path_, ec);
}

void WalManager::clear() {
    std::lock_guard<std::mutex> g(io_->wal_lock);
    clear_locked();
}

void WalManager::remove_txn(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> g(io_->wal_lock);
    auto all = read_all_locked();
    std::vector<WalRecord> remaining;
    for (auto& r : all) {
        if (r.txn_id != txn_id) remaining.push_back(r);
    }
    if (remaining.empty()) {
        clear_locked();
        return;
    }
    std::vector<std::uint8_t> buf;
    for (auto& r : remaining) {
        auto enc = encode(r);
        buf.insert(buf.end(), enc.begin(), enc.end());
    }
    write_bytes_atomic(path_, buf.data(), buf.size());
}

std::uint64_t WalManager::file_size() const {
    std::error_code ec;
    auto sz = fs::file_size(path_, ec);
    return ec ? 0 : static_cast<std::uint64_t>(sz);
}

void WalManager::truncate_to_last_checkpoint() {
    std::lock_guard<std::mutex> g(io_->wal_lock);
    auto records = read_all_locked();
    if (records.empty()) return;

    std::optional<std::size_t> last_cp;
    for (std::size_t i = records.size(); i-- > 0;) {
        if (records[i].op == WalOp::Checkpoint) { last_cp = i; break; }
    }
    if (!last_cp) return;

    std::vector<WalRecord> remaining(records.begin() + static_cast<std::ptrdiff_t>(*last_cp), records.end());
    if (remaining.size() <= 1) {
        clear_locked();
        return;
    }
    std::vector<std::uint8_t> buf;
    for (auto& r : remaining) {
        auto enc = encode(r);
        buf.insert(buf.end(), enc.begin(), enc.end());
    }
    write_bytes_atomic(path_, buf.data(), buf.size());
}

bool WalManager::needs_auto_checkpoint() const {
    return file_size() >= AUTO_CHECKPOINT_BYTES;
}

} // namespace engine
