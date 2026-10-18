// Fixed-size page I/O on a single file, with per-page CRC32 verification.
#pragma once

#include <cstdio>
#include <string>

#include "aqo/fault_injector.hpp"
#include "aqo/page.hpp"

namespace aqo {

class DiskManager {
 public:
  /// Opens (creating if needed) the file at `path`.
  explicit DiskManager(const std::string& path, FaultInjector* faults = nullptr);
  ~DiskManager();
  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  /// Reads page `pid` into `buf` (kPageSize bytes). Throws StorageError on short read or CRC mismatch.
  /// A page that was never written (beyond EOF, or all zeroes) reads back as zeroes.
  void read_page(page_id_t pid, std::byte* buf);
  /// Stamps the CRC and writes the page. Extends the file when `pid` is past the end.
  void write_page(page_id_t pid, const std::byte* buf);
  /// Flushes OS buffers to stable storage.
  void sync();

  [[nodiscard]] page_id_t page_count() const { return page_count_; }
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::uint64_t reads() const { return reads_; }
  [[nodiscard]] std::uint64_t writes() const { return writes_; }

 private:
  std::string path_;
  FaultInjector* faults_ = nullptr;
  std::FILE* file_ = nullptr;
  page_id_t page_count_ = 0;
  std::uint64_t reads_ = 0;
  std::uint64_t writes_ = 0;
};

/// fsync a stdio stream (flush + OS sync). Shared with the WAL.
void sync_file(std::FILE* f);

}  // namespace aqo
