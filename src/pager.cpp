#include "aqo/pager.hpp"

namespace aqo {
namespace {
constexpr std::size_t kMagicOff = 16, kVersionOff = 24, kNextOff = 28, kFreeOff = 32, kCatalogOff = 36;
constexpr std::size_t kFreeNextOff = 16;
}  // namespace

Pager::Pager(BufferPool& pool) : pool_(pool) {
  auto g = pool_.fetch(kMetaPage);
  bool blank = pool_.disk().page_count() == 0 || load<std::uint64_t>(g.data() + kMagicOff) == 0;
  if (blank) {
    formatted_ = true;
    std::memset(g.data() + kCommonHeader, 0, kPageSize - kCommonHeader);
    set_page_type(g.data(), PageType::kMeta);
    store<std::uint64_t>(g.data() + kMagicOff, kMagic);
    store<std::uint32_t>(g.data() + kVersionOff, 1);
    store<std::uint32_t>(g.data() + kNextOff, 1);
    store<std::uint32_t>(g.data() + kFreeOff, kInvalidPage);
    store<std::uint32_t>(g.data() + kCatalogOff, kInvalidPage);
    g.mark_dirty();
  } else if (load<std::uint64_t>(g.data() + kMagicOff) != kMagic) {
    throw StorageError("not an aqo database file (bad magic)");
  }
}

page_id_t Pager::allocate() {
  auto m = pool_.fetch_write(kMetaPage);
  page_id_t head = load<std::uint32_t>(m.data() + kFreeOff);
  if (head != kInvalidPage) {
    auto f = pool_.fetch(head);
    store<std::uint32_t>(m.data() + kFreeOff, load<std::uint32_t>(f.data() + kFreeNextOff));
    return head;
  }
  page_id_t pid = load<std::uint32_t>(m.data() + kNextOff);
  store<std::uint32_t>(m.data() + kNextOff, pid + 1);
  return pid;
}

void Pager::free(page_id_t pid) {
  auto m = pool_.fetch_write(kMetaPage);
  auto f = pool_.fetch_write(pid);
  std::memset(f.data() + kCommonHeader, 0, kPageSize - kCommonHeader);
  set_page_type(f.data(), PageType::kFree);
  store<std::uint32_t>(f.data() + kFreeNextOff, load<std::uint32_t>(m.data() + kFreeOff));
  store<std::uint32_t>(m.data() + kFreeOff, pid);
}

page_id_t Pager::catalog_root() const { return load<std::uint32_t>(const_cast<BufferPool&>(pool_).fetch(kMetaPage).data() + kCatalogOff); }

void Pager::set_catalog_root(page_id_t pid) {
  auto m = pool_.fetch_write(kMetaPage);
  store<std::uint32_t>(m.data() + kCatalogOff, pid);
}

page_id_t Pager::next_page() const { return load<std::uint32_t>(const_cast<BufferPool&>(pool_).fetch(kMetaPage).data() + kNextOff); }

std::size_t Pager::free_list_length() const {
  auto& pool = const_cast<BufferPool&>(pool_);
  page_id_t cur = load<std::uint32_t>(pool.fetch(kMetaPage).data() + kFreeOff);
  std::size_t n = 0;
  while (cur != kInvalidPage) {
    cur = load<std::uint32_t>(pool.fetch(cur).data() + kFreeNextOff);
    ++n;
  }
  return n;
}

}  // namespace aqo
