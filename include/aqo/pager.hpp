// Page allocation on top of the buffer pool. Page 0 is the meta page:
//   16 u64 magic | 24 u32 format version | 28 u32 next never-used page id
//   32 u32 head of the free-page list | 36 u32 catalog root page
// A free page stores the id of the next free page at offset 16.
#pragma once

#include "aqo/buffer_pool.hpp"

namespace aqo {

class Pager {
 public:
  static constexpr std::uint64_t kMagic = 0x31304244'4f514121ULL;  // "!AQODB01"-ish tag

  /// Opens the meta page, formatting a brand-new file. Throws on a foreign/corrupt file.
  explicit Pager(BufferPool& pool);

  /// True if this constructor created the meta page (a brand-new database).
  [[nodiscard]] bool was_formatted() const { return formatted_; }
  page_id_t allocate();
  void free(page_id_t pid);

  [[nodiscard]] page_id_t catalog_root() const;
  void set_catalog_root(page_id_t pid);
  /// High-water mark of allocated page ids.
  [[nodiscard]] page_id_t next_page() const;
  [[nodiscard]] std::size_t free_list_length() const;
  BufferPool& pool() { return pool_; }

 private:
  BufferPool& pool_;
  bool formatted_ = false;
};

}  // namespace aqo
