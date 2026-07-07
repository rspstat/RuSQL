#pragma once

// Faithful port of rusql-core/src/transaction/wal.rs — binary write-ahead log.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace engine {

constexpr const char* WAL_PATH = "rusql.wal";
constexpr std::uint64_t AUTO_CHECKPOINT_BYTES = 512 * 1024;

enum class WalOp : std::uint8_t {
    Insert = 0x01,
    Update = 0x02,
    Delete = 0x03,
    Commit = 0xFF,
    Rollback = 0xFE,
    Checkpoint = 0xFD,
};

std::optional<WalOp> wal_op_from_u8(std::uint8_t v);

struct WalRecord {
    WalOp op;
    std::uint64_t txn_id; // Checkpoint 레코드는 0 고정
    std::string table_name;
    std::string key;
    std::string data; // JSON
};

// 세션 간 공유되는 WAL/Undo 파일 접근 직렬화 락 + 전역 유일 txn_id 발급기.
class TxnIoShared {
public:
    TxnIoShared() = default;
    TxnIoShared(const TxnIoShared&) = delete;
    TxnIoShared& operator=(const TxnIoShared&) = delete;

    std::uint64_t next_id();

    std::mutex wal_lock;
    std::mutex undo_lock;

private:
    std::uint64_t next_txn_id_ = 1;
    std::mutex counter_mutex_;
};

class WalManager {
public:
    WalManager(const std::string& dir, std::shared_ptr<TxnIoShared> io);

    void append(const WalRecord& record);
    void log_insert(std::uint64_t txn_id, const std::string& table, const std::string& key, const std::string& data);
    void log_update(std::uint64_t txn_id, const std::string& table, const std::string& key, const std::string& data);
    void log_delete(std::uint64_t txn_id, const std::string& table, const std::string& key);
    void log_commit(std::uint64_t txn_id);
    void log_commit_no_sync(std::uint64_t txn_id);
    void log_rollback(std::uint64_t txn_id);
    void log_checkpoint();

    std::vector<WalRecord> read_all() const;
    void clear();
    void remove_txn(std::uint64_t txn_id);
    std::uint64_t file_size() const;
    void truncate_to_last_checkpoint();
    bool needs_auto_checkpoint() const;

    static std::vector<std::uint8_t> encode(const WalRecord& record);
    static std::optional<WalRecord> decode(const std::vector<std::uint8_t>& buf, std::size_t& pos);

private:
    std::string path_;
    std::shared_ptr<TxnIoShared> io_;

    void write_encoded_locked(const std::vector<std::uint8_t>& encoded, bool sync) const;
    std::vector<WalRecord> read_all_locked() const;
    void clear_locked() const;
};

} // namespace engine
