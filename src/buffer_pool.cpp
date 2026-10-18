#include "aqo/buffer_pool.hpp"

namespace aqo {
namespace {

class LruReplacer final : public Replacer {
 public:
  explicit LruReplacer(std::size_t n) : pos_(n) {}
  void unpinned(std::size_t f) override {
    pinned(f);
    order_.push_back(f);
    pos_[f] = std::prev(order_.end());
  }
  void pinned(std::size_t f) override {
    if (pos_[f]) {
      order_.erase(*pos_[f]);
      pos_[f].reset();
    }
  }
  std::optional<std::size_t> victim() override {
    if (order_.empty()) return std::nullopt;
    std::size_t f = order_.front();
    order_.pop_front();
    pos_[f].reset();
    return f;
  }

 private:
  std::list<std::size_t> order_;  // front = least recently unpinned
  std::vector<std::optional<std::list<std::size_t>::iterator>> pos_;
};

class ClockReplacer final : public Replacer {
 public:
  explicit ClockReplacer(std::size_t n) : evictable_(n, false), ref_(n, false) {}
  void unpinned(std::size_t f) override {
    evictable_[f] = true;
    ref_[f] = true;
  }
  void pinned(std::size_t f) override { evictable_[f] = false; }
  std::optional<std::size_t> victim() override {
    std::size_t n = evictable_.size();
    for (std::size_t step = 0; step < 2 * n; ++step) {  // at most two sweeps: one to clear ref bits
      std::size_t f = hand_;
      hand_ = (hand_ + 1) % n;
      if (!evictable_[f]) continue;
      if (ref_[f]) {
        ref_[f] = false;
        continue;
      }
      evictable_[f] = false;
      return f;
    }
    return std::nullopt;
  }

 private:
  std::vector<bool> evictable_;
  std::vector<bool> ref_;
  std::size_t hand_ = 0;
};

}  // namespace

std::unique_ptr<Replacer> make_replacer(EvictionPolicy policy, std::size_t frames) {
  if (policy == EvictionPolicy::kLru) return std::make_unique<LruReplacer>(frames);
  return std::make_unique<ClockReplacer>(frames);
}

PageGuard& PageGuard::operator=(PageGuard&& o) noexcept {
  if (this != &o) {
    release();
    pool_ = o.pool_;
    frame_ = o.frame_;
    pid_ = o.pid_;
    data_ = o.data_;
    dirty_ = o.dirty_;
    o.pool_ = nullptr;
    o.data_ = nullptr;
  }
  return *this;
}

void PageGuard::release() {
  if (pool_ != nullptr) {
    pool_->unpin(frame_, dirty_);
    pool_ = nullptr;
    data_ = nullptr;
  }
}

BufferPool::BufferPool(DiskManager& disk, std::size_t capacity, EvictionPolicy policy)
    : disk_(disk), replacer_(make_replacer(policy, capacity)) {
  if (capacity == 0) throw StorageError("buffer pool needs at least one frame");
  frames_.reserve(capacity);
  for (std::size_t i = 0; i < capacity; ++i) {
    frames_.push_back(std::make_unique<Frame>());
    free_.push_back(capacity - 1 - i);
  }
}

BufferPool::~BufferPool() = default;

void BufferPool::write_back(Frame& f) {
  if (log_flush_) log_flush_(page_lsn(f.data));  // WAL rule: log first
  disk_.write_page(f.pid, f.data);
  f.dirty = false;
  ++stats_.dirty_writebacks;
  if (page_written_) page_written_(f.pid);
}

std::size_t BufferPool::acquire_frame() {
  if (!free_.empty()) {
    std::size_t f = free_.back();
    free_.pop_back();
    return f;
  }
  auto v = replacer_->victim();
  if (!v) throw StorageError("buffer pool exhausted: all pages are pinned");
  Frame& fr = *frames_[*v];
  if (fr.dirty) write_back(fr);
  table_.erase(fr.pid);
  fr.pid = kInvalidPage;
  ++stats_.evictions;
  return *v;
}

PageGuard BufferPool::fetch(page_id_t pid) {
  if (auto it = table_.find(pid); it != table_.end()) {
    ++stats_.hits;
    Frame& f = *frames_[it->second];
    if (f.pins++ == 0) replacer_->pinned(it->second);
    return PageGuard(this, it->second, pid, f.data);
  }
  ++stats_.misses;
  std::size_t idx = acquire_frame();
  Frame& f = *frames_[idx];
  try {
    disk_.read_page(pid, f.data);
  } catch (...) {
    free_.push_back(idx);
    throw;
  }
  f.pid = pid;
  f.pins = 1;
  f.dirty = false;
  table_[pid] = idx;
  return PageGuard(this, idx, pid, f.data);
}

void BufferPool::unpin(std::size_t frame, bool dirty) {
  Frame& f = *frames_[frame];
  if (f.pins == 0) throw StorageError("unpin of an unpinned page");
  if (dirty) f.dirty = true;
  if (--f.pins == 0) replacer_->unpinned(frame);
}

void BufferPool::flush_page(page_id_t pid) {
  auto it = table_.find(pid);
  if (it == table_.end()) return;
  Frame& f = *frames_[it->second];
  if (f.dirty) write_back(f);
}

void BufferPool::flush_all() {
  for (auto& f : frames_)
    if (f->pid != kInvalidPage && f->dirty) write_back(*f);
}

std::size_t BufferPool::pin_count(page_id_t pid) const {
  auto it = table_.find(pid);
  return it == table_.end() ? 0 : frames_[it->second]->pins;
}
bool BufferPool::is_cached(page_id_t pid) const { return table_.count(pid) != 0; }
bool BufferPool::is_dirty(page_id_t pid) const {
  auto it = table_.find(pid);
  return it != table_.end() && frames_[it->second]->dirty;
}
std::size_t BufferPool::dirty_count() const {
  std::size_t n = 0;
  for (auto& f : frames_) n += (f->pid != kInvalidPage && f->dirty) ? 1 : 0;
  return n;
}
std::vector<page_id_t> BufferPool::dirty_pages() const {
  std::vector<page_id_t> out;
  for (auto& f : frames_)
    if (f->pid != kInvalidPage && f->dirty) out.push_back(f->pid);
  return out;
}

void BufferPool::discard_all() {
  for (auto& f : frames_) {
    if (f->pins != 0) throw StorageError("discard_all with pinned pages");
  }
  table_.clear();
  free_.clear();
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    frames_[i]->pid = kInvalidPage;
    frames_[i]->dirty = false;
    replacer_->pinned(i);
    free_.push_back(i);
  }
}

}  // namespace aqo
