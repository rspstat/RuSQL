#pragma once

// Faithful port of rusql-core/src/transaction/txn_manager.rs.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/result.hpp"
#include "engine/row.hpp"
#include "engine/transaction/wal.hpp"

namespace engine {

struct UndoEntry {
    std::uint64_t txn_id; // 이 엔트리를 발생시킨 트랜잭션의 전역 유일 ID.
    std::string operation; // "INSERT" | "UPDATE" | "DELETE"
    std::string table;
    std::string key;
    std::optional<std::string> old_data;
};

/// 미완료 트랜잭션의 Undo Log를 디스크에 영속화하는 관리자 (크래시 복구용).
/// Rust에서는 모듈 비공개(pub 없음) — 여기서도 TransactionManager 내부 용도로만 사용한다.
class UndoLogFile {
public:
    UndoLogFile(const std::string& dir, std::shared_ptr<TxnIoShared> io);

    void append(const UndoEntry& entry);
    std::vector<UndoEntry> read_all();
    void clear();
    bool exists() const;
    void remove_txn(std::uint64_t txn_id);
    void rewrite_txn(std::uint64_t txn_id, const std::vector<UndoEntry>& entries);

    static std::vector<std::uint8_t> encode(const UndoEntry& entry);
    static std::optional<UndoEntry> decode(const std::vector<std::uint8_t>& buf, std::size_t& pos);

private:
    std::string path_;
    std::shared_ptr<TxnIoShared> io_;

    void append_locked(const UndoEntry& entry) const;
    std::vector<UndoEntry> read_all_locked() const;
    void clear_locked() const;
};

class TransactionManager {
public:
    TransactionManager() : TransactionManager("data", std::make_shared<TxnIoShared>()) {}
    explicit TransactionManager(const std::string& dir) : TransactionManager(dir, std::make_shared<TxnIoShared>()) {}
    TransactionManager(const std::string& dir, std::shared_ptr<TxnIoShared> io);

    std::uint64_t current_txn_id() const { return active_ ? txn_id_ : 0; }
    void set_isolation_level(IsolationLevel level);
    IsolationLevel isolation_level() const { return isolation_level_; }

    Result<std::uint64_t, std::string> begin_with_snapshot(const std::unordered_map<std::string, std::vector<Row>>& tables);
    const std::vector<Row>* get_snapshot_table(const std::string& table) const;
    Result<void, std::string> validate_serializable(const std::unordered_map<std::string, std::vector<Row>>& live_tables) const;

    Result<std::uint64_t, std::string> begin();
    std::vector<std::string> dirty_tables() const;

    Result<void, std::string> commit();
    Result<void, std::string> commit_write_record();
    void commit_finalize();
    std::vector<UndoEntry> rollback();
    Result<std::vector<UndoEntry>, std::string> abort();

    Result<void, std::string> create_savepoint(const std::string& name);
    Result<std::vector<UndoEntry>, std::string> rollback_to_savepoint(const std::string& name);
    Result<void, std::string> release_savepoint(const std::string& name);

    void log_insert(const std::string& table, const std::string& key, const std::string& data);
    void log_update(const std::string& table, const std::string& key, const std::string& old_data, const std::string& new_data);
    void log_delete(const std::string& table, const std::string& key, const std::string& old_data);

    bool is_active() const { return active_; }
    std::uint64_t txn_id() const { return txn_id_; }

    std::vector<WalRecord> wal_records() const;
    std::uint64_t wal_size() const;
    void wal_clear();
    void do_checkpoint(bool safe_to_truncate);
    bool needs_auto_checkpoint() const;

    bool has_undo_log_file() const;
    std::vector<UndoEntry> read_undo_log_file();
    void clear_undo_log_file();

private:
    bool active_ = false;
    std::uint64_t txn_id_ = 0;
    std::shared_ptr<TxnIoShared> io_;
    std::vector<UndoEntry> undo_log_;
    WalManager wal_;
    UndoLogFile undo_log_file_;
    IsolationLevel isolation_level_ = IsolationLevel::ReadCommitted;
    std::optional<std::unordered_map<std::string, std::vector<Row>>> snapshot_;
    std::vector<std::pair<std::string, std::size_t>> savepoints_;
};

} // namespace engine
