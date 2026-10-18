#include "aqo/disk_btree.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace aqo {
namespace {

using Bytes = std::vector<std::byte>;
constexpr std::size_t kIR = 8 + 4;  // internal record size

std::int64_t key_of(std::span<const std::byte> r) { return load<std::int64_t>(r.data()); }
page_id_t child_of(std::span<const std::byte> r) { return load<std::uint32_t>(r.data() + 8); }

Bytes make_leaf_rec(std::int64_t key, std::span<const std::byte> value) {
  Bytes r(8 + value.size());
  store<std::int64_t>(r.data(), key);
  if (!value.empty()) std::memcpy(r.data() + 8, value.data(), value.size());
  return r;
}
Bytes make_int_rec(std::int64_t key, page_id_t child) {
  Bytes r(kIR);
  store<std::int64_t>(r.data(), key);
  store<std::uint32_t>(r.data() + 8, child);
  return r;
}

// First slot whose key is >= key (strict=false) or > key (strict=true).
std::size_t bound_idx(const SlottedPage& sp, std::int64_t key, bool strict) {
  std::size_t lo = 0, hi = sp.count();
  while (lo < hi) {
    std::size_t mid = (lo + hi) / 2;
    std::int64_t k = key_of(sp.get(mid));
    if (strict ? k <= key : k < key) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
std::size_t lower_idx(const SlottedPage& sp, std::int64_t key) { return bound_idx(sp, key, false); }
// Index of the child to descend into: number of separators <= key.
std::size_t child_index(const SlottedPage& sp, std::int64_t key) { return bound_idx(sp, key, true); }
page_id_t child_at(const SlottedPage& sp, std::size_t c) { return c == 0 ? sp.aux1() : child_of(sp.get(c - 1)); }

std::vector<Bytes> gather(const SlottedPage& sp) {
  std::vector<Bytes> out;
  out.reserve(sp.count());
  for (std::size_t i = 0; i < sp.count(); ++i) {
    auto r = sp.get(i);
    out.emplace_back(r.begin(), r.end());
  }
  return out;
}
void fill(SlottedPage& sp, const std::vector<Bytes>& recs, std::size_t begin, std::size_t end) {
  sp.clear();
  for (std::size_t i = begin; i < end; ++i)
    if (!sp.insert_at(i - begin, recs[i])) throw StorageError("internal error: records do not fit page");
}
std::size_t total_bytes(const std::vector<Bytes>& recs) {
  std::size_t t = 0;
  for (auto& r : recs) t += r.size() + SlottedPage::kSlotSize;
  return t;
}
// Left side takes recs[0, k): the smallest prefix holding at least half of the bytes, with 1 <= k <= n-1.
std::size_t split_point(const std::vector<Bytes>& recs) {
  std::size_t total = total_bytes(recs), acc = 0;
  for (std::size_t i = 0; i < recs.size(); ++i) {
    acc += recs[i].size() + SlottedPage::kSlotSize;
    if (acc * 2 >= total) return std::clamp<std::size_t>(i + 1, 1, recs.size() - 1);
  }
  return recs.size() - 1;
}

}  // namespace

page_id_t DiskBTree::create(Pager& pager) {
  page_id_t root = pager.allocate();
  auto g = pager.pool().fetch_write(root);
  SlottedPage(g.data()).init(PageType::kLeaf);
  return root;
}

// ---------------------------------------------------------------- reads

page_id_t DiskBTree::find_leaf(std::int64_t key) const {
  page_id_t cur = root_;
  for (;;) {
    auto g = pool_.fetch(cur);
    if (page_type(g.data()) == PageType::kLeaf) return cur;
    SlottedPage sp(g.data());
    cur = child_at(sp, child_index(sp, key));
  }
}

page_id_t DiskBTree::leftmost_leaf() const {
  page_id_t cur = root_;
  for (;;) {
    auto g = pool_.fetch(cur);
    if (page_type(g.data()) == PageType::kLeaf) return cur;
    cur = SlottedPage(g.data()).aux1();
  }
}

std::optional<Bytes> DiskBTree::get(std::int64_t key) const {
  auto g = pool_.fetch(find_leaf(key));
  SlottedPage sp(g.data());
  std::size_t i = lower_idx(sp, key);
  if (i >= sp.count() || key_of(sp.get(i)) != key) return std::nullopt;
  auto r = sp.get(i);
  return Bytes(r.begin() + 8, r.end());
}

void DiskBTree::scan(const KeyRange<std::int64_t>& range,
                     const std::function<bool(std::int64_t, std::span<const std::byte>)>& fn) const {
  page_id_t pid = range.lo ? find_leaf(*range.lo) : leftmost_leaf();
  bool first = true;
  while (pid != kInvalidPage) {
    auto g = pool_.fetch(pid);
    SlottedPage sp(g.data());
    std::size_t i = (first && range.lo) ? bound_idx(sp, *range.lo, !range.lo_inclusive) : 0;
    first = false;
    for (; i < sp.count(); ++i) {
      auto r = sp.get(i);
      std::int64_t k = key_of(r);
      if (range.hi && (range.hi_inclusive ? k > *range.hi : k >= *range.hi)) return;
      if (!fn(k, r.subspan(8))) return;
    }
    pid = sp.aux1();
  }
}

std::size_t DiskBTree::count() const {
  std::size_t n = 0;
  scan({}, [&](auto, auto) {
    ++n;
    return true;
  });
  return n;
}

std::size_t DiskBTree::height() const {
  std::size_t h = 1;
  page_id_t cur = root_;
  for (;;) {
    auto g = pool_.fetch(cur);
    if (page_type(g.data()) == PageType::kLeaf) return h;
    cur = SlottedPage(g.data()).aux1();
    ++h;
  }
}

// ---------------------------------------------------------------- insert

bool DiskBTree::insert(std::int64_t key, std::span<const std::byte> value) { return put(key, value, false); }
bool DiskBTree::upsert(std::int64_t key, std::span<const std::byte> value) { return put(key, value, true); }

void DiskBTree::grow_root() {
  page_id_t child = pager_.allocate();
  auto rg = pool_.fetch_write(root_);
  auto cg = pool_.fetch_write(child);
  std::memcpy(cg.data() + kCommonHeader, rg.data() + kCommonHeader, kPageSize - kCommonHeader);
  set_page_type(cg.data(), page_type(rg.data()));
  SlottedPage root(rg.data());
  root.init(PageType::kInternal);
  root.set_aux1(child);
}

bool DiskBTree::put(std::int64_t key, std::span<const std::byte> value, bool overwrite) {
  if (value.size() > kMaxValueSize) throw StorageError("value too large for a page (max 992 bytes)");
  Bytes rec = make_leaf_rec(key, value);
  {
    auto g = pool_.fetch(root_);
    SlottedPage sp(g.data());
    bool internal = page_type(g.data()) == PageType::kInternal;
    bool need_grow = internal ? !sp.fits(kIR) : !sp.fits(rec.size());
    g.release();
    if (need_grow) {
      grow_root();
      if (internal) split_internal_child(root_, 0);
    }
  }
  page_id_t cur = root_;
  for (;;) {
    auto g = pool_.fetch(cur);
    if (page_type(g.data()) == PageType::kLeaf) {  // root leaf with guaranteed room
      g.release();
      return leaf_put(kInvalidPage, 0, cur, key, rec, overwrite);
    }
    SlottedPage sp(g.data());
    std::size_t c = child_index(sp, key);
    page_id_t child = child_at(sp, c);
    auto cg = pool_.fetch(child);
    if (page_type(cg.data()) == PageType::kInternal) {
      if (!SlottedPage(cg.data()).fits(kIR)) {
        cg.release();
        g.release();
        split_internal_child(cur, c);
        continue;
      }
      cur = child;
      continue;
    }
    cg.release();
    g.release();
    return leaf_put(cur, c, child, key, rec, overwrite);
  }
}

bool DiskBTree::leaf_put(page_id_t parent, std::size_t c, page_id_t leaf, std::int64_t key, const Bytes& rec,
                         bool overwrite) {
  auto lg = pool_.fetch(leaf);
  SlottedPage sp(lg.data());
  std::size_t idx = lower_idx(sp, key);
  bool exists = idx < sp.count() && key_of(sp.get(idx)) == key;
  if (exists) {
    if (!overwrite) return false;
    if (sp.replace_at(idx, rec)) {
      lg.mark_dirty();
      return false;
    }
    sp.remove_at(idx);
    lg.mark_dirty();
  }
  if (sp.insert_at(idx, rec)) {
    lg.mark_dirty();
    return !exists;
  }
  lg.release();
  if (parent == kInvalidPage) throw std::logic_error("root leaf unexpectedly full");
  split_leaf(parent, c, leaf, idx, rec);
  return !exists;
}

void DiskBTree::split_leaf(page_id_t parent, std::size_t c, page_id_t leaf, std::size_t idx, const Bytes& rec) {
  auto lg = pool_.fetch_write(leaf);
  SlottedPage ls(lg.data());
  auto recs = gather(ls);
  recs.insert(recs.begin() + static_cast<std::ptrdiff_t>(idx), rec);
  std::size_t k = split_point(recs);
  page_id_t rp = pager_.allocate();
  auto rg = pool_.fetch_write(rp);
  SlottedPage rs(rg.data());
  rs.init(PageType::kLeaf);
  page_id_t old_next = ls.aux1();
  fill(ls, recs, 0, k);
  fill(rs, recs, k, recs.size());
  rs.set_aux1(old_next);
  rs.set_aux2(leaf);
  ls.set_aux1(rp);
  if (old_next != kInvalidPage) {
    auto ng = pool_.fetch_write(old_next);
    SlottedPage(ng.data()).set_aux2(rp);
  }
  auto pg = pool_.fetch_write(parent);
  if (!SlottedPage(pg.data()).insert_at(c, make_int_rec(key_of(recs[k]), rp)))
    throw std::logic_error("parent had no room for separator");
}

void DiskBTree::split_internal_child(page_id_t parent, std::size_t c) {
  page_id_t pid;
  {
    auto pg = pool_.fetch(parent);
    pid = child_at(SlottedPage(pg.data()), c);
  }
  auto cg = pool_.fetch_write(pid);
  SlottedPage cs(cg.data());
  auto recs = gather(cs);
  std::size_t mid = recs.size() / 2;
  std::int64_t sep = key_of(recs[mid]);
  page_id_t right_child0 = child_of(recs[mid]);
  page_id_t rp = pager_.allocate();
  auto rg = pool_.fetch_write(rp);
  SlottedPage rs(rg.data());
  rs.init(PageType::kInternal);
  rs.set_aux1(right_child0);
  fill(rs, recs, mid + 1, recs.size());
  fill(cs, recs, 0, mid);
  auto pg = pool_.fetch_write(parent);
  if (!SlottedPage(pg.data()).insert_at(c, make_int_rec(sep, rp))) throw std::logic_error("parent had no room");
}

// ---------------------------------------------------------------- erase

bool DiskBTree::erase(std::int64_t key) {
  std::vector<PathEl> path;
  page_id_t cur = root_;
  for (;;) {
    auto g = pool_.fetch(cur);
    if (page_type(g.data()) == PageType::kLeaf) break;
    SlottedPage sp(g.data());
    std::size_t c = child_index(sp, key);
    path.push_back({cur, c});
    cur = child_at(sp, c);
  }
  {
    auto g = pool_.fetch(cur);
    SlottedPage sp(g.data());
    std::size_t i = lower_idx(sp, key);
    if (i >= sp.count() || key_of(sp.get(i)) != key) return false;
    sp.remove_at(i);
    g.mark_dirty();
  }
  rebalance(path, cur);
  return true;
}

void DiskBTree::rebalance(std::vector<PathEl>& path, page_id_t node) {
  while (!path.empty()) {
    auto [parent, c] = path.back();
    {
      auto g = pool_.fetch(node);
      if (SlottedPage(g.data()).used_space() >= SlottedPage::kCapacity / 4) break;
    }
    path.pop_back();
    std::size_t left_idx = c > 0 ? c - 1 : c;
    if (!merge_or_redistribute(parent, left_idx)) break;
    node = parent;
  }
  collapse_root_if_needed();
}

bool DiskBTree::merge_or_redistribute(page_id_t parent, std::size_t li) {
  auto pg = pool_.fetch_write(parent);
  SlottedPage ps(pg.data());
  page_id_t lp = child_at(ps, li), rp = child_at(ps, li + 1);
  std::int64_t sep_key = key_of(ps.get(li));
  auto lg = pool_.fetch_write(lp);
  auto rg = pool_.fetch_write(rp);
  SlottedPage ls(lg.data()), rs(rg.data());
  bool leaf = page_type(lg.data()) == PageType::kLeaf;
  auto recs = gather(ls);
  if (!leaf) recs.push_back(make_int_rec(sep_key, rs.aux1()));
  for (auto& r : gather(rs)) recs.push_back(std::move(r));
  std::size_t n = recs.size();

  if (total_bytes(recs) <= SlottedPage::kCapacity) {  // merge right into left
    fill(ls, recs, 0, n);
    if (leaf) {
      page_id_t next = rs.aux1();
      ls.set_aux1(next);
      if (next != kInvalidPage) {
        auto ng = pool_.fetch_write(next);
        SlottedPage(ng.data()).set_aux2(lp);
      }
    }
    rg.release();
    pager_.free(rp);
    ps.remove_at(li);
    return true;
  }
  std::int64_t new_sep;
  if (leaf) {
    std::size_t k = split_point(recs);
    fill(ls, recs, 0, k);
    fill(rs, recs, k, n);
    new_sep = key_of(recs[k]);
  } else {
    std::size_t mid = std::clamp<std::size_t>(split_point(recs), 1, n - 2);
    new_sep = key_of(recs[mid]);
    rs.set_aux1(child_of(recs[mid]));
    fill(ls, recs, 0, mid);
    fill(rs, recs, mid + 1, n);
  }
  if (!ps.replace_at(li, make_int_rec(new_sep, rp))) throw std::logic_error("separator replace failed");
  return false;
}

void DiskBTree::collapse_root_if_needed() {
  for (;;) {
    page_id_t child;
    {
      auto g = pool_.fetch(root_);
      SlottedPage sp(g.data());
      if (page_type(g.data()) != PageType::kInternal || sp.count() != 0) return;
      child = sp.aux1();
    }
    {
      auto rg = pool_.fetch_write(root_);
      auto cg = pool_.fetch(child);
      std::memcpy(rg.data() + kCommonHeader, cg.data() + kCommonHeader, kPageSize - kCommonHeader);
      set_page_type(rg.data(), page_type(cg.data()));
    }
    pager_.free(child);
  }
}

void DiskBTree::free_subtree(page_id_t pid) {
  std::vector<page_id_t> kids;
  {
    auto g = pool_.fetch(pid);
    if (page_type(g.data()) == PageType::kInternal) {
      SlottedPage sp(g.data());
      for (std::size_t c = 0; c <= sp.count(); ++c) kids.push_back(child_at(sp, c));
    }
  }
  for (auto k : kids) free_subtree(k);
  pager_.free(pid);
}

void DiskBTree::clear() {
  std::vector<page_id_t> kids;
  {
    auto g = pool_.fetch(root_);
    if (page_type(g.data()) == PageType::kInternal) {
      SlottedPage sp(g.data());
      for (std::size_t c = 0; c <= sp.count(); ++c) kids.push_back(child_at(sp, c));
    }
  }
  for (auto k : kids) free_subtree(k);
  auto g = pool_.fetch_write(root_);
  SlottedPage(g.data()).init(PageType::kLeaf);
}

// ---------------------------------------------------------------- validation

namespace {
struct Checker {
  BufferPool& pool;
  std::ostringstream err;
  std::optional<std::size_t> leaf_depth;
  page_id_t prev_leaf = kInvalidPage;
  page_id_t expected_next_of_prev = kInvalidPage;
  bool saw_leaf = false;

  void walk(page_id_t pid, std::optional<std::int64_t> lo, std::optional<std::int64_t> hi, std::size_t depth,
            bool is_root) {
    if (!err.str().empty()) return;
    std::vector<page_id_t> kids;
    std::vector<std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>> bounds;
    {
      auto g = pool.fetch(pid);
      SlottedPage sp(g.data());
      if (auto e = sp.validate(); !e.empty()) {
        err << "page " << pid << ": " << e;
        return;
      }
      PageType t = page_type(g.data());
      if (t == PageType::kLeaf) {
        if (!leaf_depth) leaf_depth = depth;
        if (*leaf_depth != depth) {
          err << "leaves at different depths";
          return;
        }
        if (!is_root && sp.count() == 0) {
          err << "empty non-root leaf " << pid;
          return;
        }
        for (std::size_t i = 0; i < sp.count(); ++i) {
          auto k = key_of(sp.get(i));
          if (i > 0 && k <= key_of(sp.get(i - 1))) err << "leaf " << pid << " keys unsorted";
          if (lo && k < *lo) err << "leaf " << pid << " key below bound";
          if (hi && k >= *hi) err << "leaf " << pid << " key above bound";
        }
        if (sp.aux2() != prev_leaf) err << "leaf " << pid << " bad prev link";
        if (saw_leaf && expected_next_of_prev != pid) err << "leaf chain broken before " << pid;
        saw_leaf = true;
        prev_leaf = pid;
        expected_next_of_prev = sp.aux1();
        return;
      }
      if (t != PageType::kInternal) {
        err << "page " << pid << " has unexpected type";
        return;
      }
      if (sp.count() == 0) {
        err << "internal page " << pid << " has no separators";
        return;
      }
      for (std::size_t i = 0; i < sp.count(); ++i) {
        auto k = key_of(sp.get(i));
        if (i > 0 && k <= key_of(sp.get(i - 1))) err << "internal " << pid << " keys unsorted";
      }
      for (std::size_t c = 0; c <= sp.count(); ++c) {
        kids.push_back(child_at(sp, c));
        bounds.push_back({c == 0 ? lo : std::optional<std::int64_t>(key_of(sp.get(c - 1))),
                          c == sp.count() ? hi : std::optional<std::int64_t>(key_of(sp.get(c)))});
      }
    }
    for (std::size_t i = 0; i < kids.size(); ++i) walk(kids[i], bounds[i].first, bounds[i].second, depth + 1, false);
  }
};
}  // namespace

std::string DiskBTree::validate() const {
  Checker ck{pool_, {}, std::nullopt, kInvalidPage, kInvalidPage, false};
  ck.walk(root_, std::nullopt, std::nullopt, 0, true);
  if (ck.err.str().empty() && ck.expected_next_of_prev != kInvalidPage) ck.err << "last leaf has a next link";
  return ck.err.str();
}

}  // namespace aqo
