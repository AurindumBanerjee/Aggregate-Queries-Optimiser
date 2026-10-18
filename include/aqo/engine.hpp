// StorageEngine: ties the disk manager, buffer pool, WAL, recovery and pager together and offers
// single-writer transactions with physical (before/after image) logging. See docs/adr/0006-wal-recovery.md.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "aqo/buffer_pool.hpp"
#include "aqo/disk_btree.hpp"
#include "aqo/pager.hpp"
#include "aqo/recovery.hpp"
#include "aqo/wal.hpp"

namespace aqo {

struct EngineOptions {
  std::string path;  // data file; the log lives at path + ".wal"
  std::size_t pool_frames = 1024;
  EvictionPolicy policy = EvictionPolicy::kLru;
  bool sync_commit = true;  // fsync the log on commit
  bool enable_wal = true;   // false: no atomicity/durability, for benchmark baselines only
  FaultInjector* faults = nullptr;
};

struct EngineStats {
  std::uint64_t commits = 0;
  std::uint64_t aborts = 0;
  std::uint64_t checkpoints = 0;
};

class StorageEngine;

/// A write transaction. Destroying it without commit() rolls it back.
class Txn {
 public:
  Txn() = default;
  Txn(const Txn&) = delete;
  Txn& operator=(const Txn&) = delete;
  Txn(Txn&& o) noexcept : engine_(o.engine_), id_(o.id_) { o.engine_ = nullptr; }
  Txn& operator=(Txn&& o) noexcept;
  ~Txn();
  void commit();
  void abort();
  [[nodiscard]] txn_id_t id() const { return id_; }
  [[nodiscard]] bool active() const { return engine_ != nullptr; }

 private:
  friend class StorageEngine;
  Txn(StorageEngine* e, txn_id_t id) : engine_(e), id_(id) {}
  StorageEngine* engine_ = nullptr;
  txn_id_t id_ = 0;
};

class StorageEngine {
 public:
  /// Opens (creating if needed) the database and runs crash recovery if the log has work to do.
  explicit StorageEngine(EngineOptions opts);
  /// Releases files without flushing anything: dropping the engine models a crash.
  ~StorageEngine();
  StorageEngine(const StorageEngine&) = delete;
  StorageEngine& operator=(const StorageEngine&) = delete;

  /// Starts the (single) write transaction; throws if one is already open.
  Txn begin();
  /// Flushes dirty pages, writes a checkpoint record and (if idle) truncates the log.
  void checkpoint();
  /// Clean shutdown: rolls back any open transaction, then checkpoints.
  void close();

  [[nodiscard]] bool in_txn() const { return active_.has_value(); }
  Pager& pager() { return *pager_; }
  BufferPool& pool() { return pool_; }
  Wal* wal() { return wal_.get(); }
  DiskManager& disk() { return disk_; }
  /// System catalog tree: int64 id -> opaque bytes (used by the SQL layer).
  DiskBTree catalog() { return DiskBTree(*pager_, pager_->catalog_root()); }
  [[nodiscard]] const RecoveryStats& recovery_stats() const { return recovery_; }
  [[nodiscard]] const EngineStats& stats() const { return stats_; }
  [[nodiscard]] const EngineOptions& options() const { return opts_; }
  /// True once an injected crash has fired: the engine must do no further work (tests only).
  [[nodiscard]] bool crashed() const { return opts_.faults != nullptr && opts_.faults->fired(); }

 private:
  friend class Txn;
  struct ActiveTxn {
    txn_id_t id;
    lsn_t last_lsn;
  };
  void commit_active();
  void abort_active();
  void install_hooks();

  EngineOptions opts_;
  DiskManager disk_;
  BufferPool pool_;
  std::unique_ptr<Wal> wal_;
  std::unique_ptr<Pager> pager_;
  std::optional<ActiveTxn> active_;
  std::map<page_id_t, lsn_t> dirty_table_;  // page -> LSN of the first update since it was last written
  txn_id_t next_txn_ = 1;
  RecoveryStats recovery_;
  EngineStats stats_;
};

}  // namespace aqo
