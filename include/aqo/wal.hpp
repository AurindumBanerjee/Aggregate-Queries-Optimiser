// Write-ahead log: append-only, CRC-framed records. LSN = base + byte offset past the file header, so
// LSNs stay monotonic across log truncation. A torn tail (bad length/CRC) ends the log.
//
// File layout: 64-byte header {magic, base_lsn, checkpoint_lsn, crc} then frames
//   u32 payload_len | u32 crc32(payload) | payload
// payload = u8 type | u64 txn | u64 prev_lsn | type-specific body (see LogRecord).
#pragma once

#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aqo/fault_injector.hpp"
#include "aqo/page.hpp"

namespace aqo {

using txn_id_t = std::uint64_t;

enum class LogType : std::uint8_t { kBegin = 1, kCommit = 2, kAbort = 3, kUpdate = 4, kClr = 5, kCheckpoint = 6 };

/// A changed byte range of a page. CLRs carry no `before`.
struct Segment {
  std::uint16_t offset = 0;
  std::vector<std::byte> before;
  std::vector<std::byte> after;
};

struct LogRecord {
  lsn_t lsn = 0;  // assigned by Wal::append / read
  LogType type = LogType::kBegin;
  txn_id_t txn = 0;
  lsn_t prev_lsn = 0;  // previous record of the same transaction
  // kUpdate / kClr
  page_id_t page = kInvalidPage;
  std::vector<Segment> segments;
  lsn_t undo_next = 0;  // kClr: next record of this txn to undo
  // kCheckpoint
  std::vector<std::pair<txn_id_t, lsn_t>> active_txns;   // txn -> last lsn
  std::vector<std::pair<page_id_t, lsn_t>> dirty_pages;  // page -> rec_lsn
};

class Wal {
 public:
  static constexpr std::size_t kHeaderSize = 64;

  /// Opens or creates the log, scanning for the end of the valid prefix and discarding any torn tail.
  Wal(const std::string& path, bool sync_on_flush, FaultInjector* faults = nullptr);
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  /// Buffers the record (no I/O) and returns its LSN.
  lsn_t append(const LogRecord& rec);
  /// Makes every record with LSN <= `up_to` durable (written, and fsynced if sync_on_flush).
  void flush(lsn_t up_to);
  void flush_all() { flush(end_lsn()); }

  /// LSN the next appended record will get.
  [[nodiscard]] lsn_t end_lsn() const { return base_lsn_ + (file_end_ - kHeaderSize) + buffer_.size(); }
  /// All records with LSN < flushed_lsn() have been handed to the OS.
  [[nodiscard]] lsn_t flushed_lsn() const { return base_lsn_ + (file_end_ - kHeaderSize); }
  [[nodiscard]] lsn_t checkpoint_lsn() const { return checkpoint_lsn_; }
  /// File size known to have survived an fsync (used by power-loss simulation).
  [[nodiscard]] std::uint64_t synced_file_size() const { return synced_size_; }
  [[nodiscard]] std::uint64_t file_size() const { return file_end_; }
  [[nodiscard]] std::uint64_t bytes_appended() const { return bytes_appended_; }
  [[nodiscard]] std::uint64_t flush_count() const { return flush_count_; }
  [[nodiscard]] const std::string& path() const { return path_; }

  std::optional<LogRecord> read(lsn_t lsn);
  /// Visits records from `from` (0 = start of log) in order; the callback returns false to stop.
  void scan(lsn_t from, const std::function<bool(const LogRecord&)>& fn);

  /// Durably records `lsn` as the place recovery's analysis pass may start.
  void set_checkpoint(lsn_t lsn);
  /// Discards all existing records (caller guarantees they are no longer needed): the new log
  /// starts at the current end LSN. Atomic via write-temp-then-rename.
  void truncate_all();

  // Encoding helpers (public for tests).
  static std::vector<std::byte> encode(const LogRecord& rec);
  static bool decode(const std::byte* payload, std::size_t len, LogRecord& out);

 private:
  void write_header(std::FILE* f) const;
  void write_buffer_to_os();
  void open_file();
  [[nodiscard]] std::uint64_t offset_of(lsn_t lsn) const { return kHeaderSize + (lsn - base_lsn_); }

  std::string path_;
  bool sync_on_flush_;
  FaultInjector* faults_;
  std::FILE* file_ = nullptr;
  lsn_t base_lsn_ = 1;
  lsn_t checkpoint_lsn_ = 0;
  std::uint64_t file_end_ = kHeaderSize;  // bytes in the file (valid prefix)
  std::uint64_t synced_size_ = kHeaderSize;
  std::vector<std::byte> buffer_;
  std::uint64_t bytes_appended_ = 0;
  std::uint64_t flush_count_ = 0;
};

}  // namespace aqo
