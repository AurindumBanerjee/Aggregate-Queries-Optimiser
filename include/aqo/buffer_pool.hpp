// Buffer pool: caches pages in a fixed number of frames with pin counts, dirty tracking and
// pluggable eviction (LRU or Clock).
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "aqo/disk_manager.hpp"
#include "aqo/page.hpp"

namespace aqo {

enum class EvictionPolicy { kLru, kClock };

/// Chooses which unpinned frame to evict.
class Replacer {
 public:
  virtual ~Replacer() = default;
  /// Frame became unpinned (pin count reached 0): it is now a candidate.
  virtual void unpinned(std::size_t frame) = 0;
  /// Frame was pinned: it is no longer a candidate.
  virtual void pinned(std::size_t frame) = 0;
  /// Picks and removes a victim; nullopt if every frame is pinned.
  virtual std::optional<std::size_t> victim() = 0;
};

std::unique_ptr<Replacer> make_replacer(EvictionPolicy policy, std::size_t frames);

struct BufferStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::uint64_t dirty_writebacks = 0;
  [[nodiscard]] double hit_rate() const {
    auto total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
  }
};

class BufferPool;

/// RAII pin on a page. Move-only; unpins on destruction.
class PageGuard {
 public:
  PageGuard() = default;
  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;
  PageGuard(PageGuard&& o) noexcept { *this = std::move(o); }
  PageGuard& operator=(PageGuard&& o) noexcept;
  ~PageGuard() { release(); }

  [[nodiscard]] std::byte* data() const { return data_; }
  /// True for guards obtained from fetch_write (the page is expected to change).
  [[nodiscard]] bool is_writer() const { return writer_; }
  [[nodiscard]] page_id_t id() const { return pid_; }
  [[nodiscard]] explicit operator bool() const { return pool_ != nullptr; }
  /// Records that the page content was modified so it is written back on eviction/flush.
  void mark_dirty() { dirty_ = true; }
  void release();

 private:
  friend class BufferPool;
  PageGuard(BufferPool* pool, std::size_t frame, page_id_t pid, std::byte* data)
      : pool_(pool), frame_(frame), pid_(pid), data_(data) {}
  BufferPool* pool_ = nullptr;
  std::size_t frame_ = 0;
  page_id_t pid_ = kInvalidPage;
  std::byte* data_ = nullptr;
  bool dirty_ = false;
  bool writer_ = false;
  std::unique_ptr<std::byte[]> before_;  // snapshot taken by fetch_write when a write hook is installed
};

class BufferPool {
 public:
  BufferPool(DiskManager& disk, std::size_t capacity, EvictionPolicy policy = EvictionPolicy::kLru);
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  ~BufferPool();  // does not flush: callers decide (see flush_all)

  /// Pins page `pid`, reading it from disk on a miss. Throws StorageError if every frame is pinned.
  PageGuard fetch(page_id_t pid);
  /// Like fetch(), but the caller intends to modify the page: it is marked dirty up front and
  /// (once the WAL is attached) the modification is logged when the guard is released.
  PageGuard fetch_write(page_id_t pid);
  /// Writes one page back if dirty (honouring the WAL rule via the log-flush hook).
  void flush_page(page_id_t pid);
  void flush_all();

  /// Installed by the WAL: must make the log durable up to `lsn` before a page with that LSN is written.
  void set_log_flush_hook(std::function<void(lsn_t)> hook) { log_flush_ = std::move(hook); }
  /// Called when a write guard is released: (page, before-image, live page). The hook may stamp a new
  /// page LSN into the live page. It must not throw or perform I/O (it runs inside a destructor).
  using WriteHook = std::function<void(page_id_t, const std::byte*, std::byte*)>;
  void set_write_hook(WriteHook hook) { write_hook_ = std::move(hook); }
  /// Called on every fetch_write before anything changes; may throw to veto (e.g. no open transaction).
  void set_write_gate(std::function<void()> gate) { write_gate_ = std::move(gate); }
  /// Called after a dirty page has been written (used for the checkpoint dirty-page table).
  void set_page_written_hook(std::function<void(page_id_t)> hook) { page_written_ = std::move(hook); }

  [[nodiscard]] std::size_t capacity() const { return frames_.size(); }
  [[nodiscard]] std::size_t pin_count(page_id_t pid) const;
  [[nodiscard]] bool is_cached(page_id_t pid) const;
  [[nodiscard]] bool is_dirty(page_id_t pid) const;
  [[nodiscard]] std::size_t dirty_count() const;
  [[nodiscard]] std::vector<page_id_t> dirty_pages() const;
  [[nodiscard]] const BufferStats& stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }
  DiskManager& disk() { return disk_; }

  /// Drops every cached page without writing (simulates losing memory in a crash). Test hook.
  void discard_all();

 private:
  friend class PageGuard;
  struct Frame {
    alignas(8) std::byte data[kPageSize];
    page_id_t pid = kInvalidPage;
    std::size_t pins = 0;
    bool dirty = false;
    bool writer = false;  // a write guard is outstanding (at most one per page)
  };
  void unpin(std::size_t frame, bool dirty, bool was_writer, const std::byte* before);
  std::size_t acquire_frame();
  void write_back(Frame& f);

  DiskManager& disk_;
  std::vector<std::unique_ptr<Frame>> frames_;
  std::unordered_map<page_id_t, std::size_t> table_;
  std::vector<std::size_t> free_;
  std::unique_ptr<Replacer> replacer_;
  BufferStats stats_;
  std::function<void(lsn_t)> log_flush_;
  std::function<void(page_id_t)> page_written_;
  WriteHook write_hook_;
  std::function<void()> write_gate_;
};

}  // namespace aqo
