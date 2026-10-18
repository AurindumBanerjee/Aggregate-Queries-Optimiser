// Disk-resident clustered B+ tree: int64 key -> variable-length value, stored in slotted pages
// through the buffer pool.
//
//   * Leaf record     = key(8) | value bytes ; leaves are chained through aux1 (next) / aux2 (prev).
//   * Internal record = key(8) | child(4)    ; child i+1 holds keys >= key_i ; child 0 lives in aux1.
//   * The root page id never changes (a full root is copied down into a new child), so catalogs can
//     store it once.
//   * Insert splits full internal nodes on the way down (so a parent always has room for a separator);
//     delete rebalances bottom-up by merging siblings or redistributing evenly.
// See docs/adr/0004-paged-btree.md.
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "aqo/bptree.hpp"
#include "aqo/pager.hpp"
#include "aqo/slotted_page.hpp"

namespace aqo {

class DiskBTree {
 public:
  /// Largest value that can be stored (no overflow pages; see ADR 0004).
  static constexpr std::size_t kMaxValueSize = 992;

  DiskBTree(Pager& pager, page_id_t root) : pager_(pager), pool_(pager.pool()), root_(root) {}

  /// Allocates and formats an empty tree; returns its (permanent) root page id.
  static page_id_t create(Pager& pager);

  /// Inserts; returns false (leaving the tree untouched) if the key already exists.
  bool insert(std::int64_t key, std::span<const std::byte> value);
  /// Inserts or overwrites; returns true if the key was new.
  bool upsert(std::int64_t key, std::span<const std::byte> value);
  /// Removes; returns true if the key existed.
  bool erase(std::int64_t key);
  [[nodiscard]] std::optional<std::vector<std::byte>> get(std::int64_t key) const;

  /// Visits entries in key order within `range`; the callback returns false to stop early.
  void scan(const KeyRange<std::int64_t>& range,
            const std::function<bool(std::int64_t, std::span<const std::byte>)>& fn) const;

  [[nodiscard]] std::size_t count() const;
  [[nodiscard]] std::size_t height() const;
  [[nodiscard]] page_id_t root() const { return root_; }
  /// Walks the whole tree checking every structural invariant; "" if healthy.
  [[nodiscard]] std::string validate() const;
  /// Frees every page of the tree except the root, which is reset to an empty leaf.
  void clear();

 private:
  struct PathEl {
    page_id_t pid;
    std::size_t child_idx;
  };
  bool put(std::int64_t key, std::span<const std::byte> value, bool overwrite);
  bool leaf_put(page_id_t parent, std::size_t c, page_id_t leaf, std::int64_t key,
                const std::vector<std::byte>& rec, bool overwrite);
  void split_leaf(page_id_t parent, std::size_t c, page_id_t leaf, std::size_t idx, const std::vector<std::byte>& rec);
  void split_internal_child(page_id_t parent, std::size_t c);
  void grow_root();
  void rebalance(std::vector<PathEl>& path, page_id_t node);
  bool merge_or_redistribute(page_id_t parent, std::size_t left_idx);
  void collapse_root_if_needed();
  page_id_t leftmost_leaf() const;
  page_id_t find_leaf(std::int64_t key) const;
  void free_subtree(page_id_t pid);

  Pager& pager_;
  BufferPool& pool_;
  page_id_t root_;
};

}  // namespace aqo
