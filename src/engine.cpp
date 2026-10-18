#include "aqo/engine.hpp"

#include <algorithm>

namespace aqo {
namespace {

// Changed byte ranges between two page images, ignoring the LSN/CRC bytes. Ranges closer than
// `kGap` bytes are merged so a record stays small but never needs more than a few segments.
std::vector<Segment> diff_page(const std::byte* before, const std::byte* after) {
  constexpr std::size_t kGap = 24;
  std::vector<Segment> segs;
  std::size_t i = kTypeOff;
  while (i < kPageSize) {
    while (i < kPageSize && before[i] == after[i]) ++i;
    if (i >= kPageSize) break;
    std::size_t start = i, last = i;
    ++i;
    while (i < kPageSize && i - last <= kGap) {
      if (before[i] != after[i]) last = i;
      ++i;
    }
    Segment s;
    s.offset = static_cast<std::uint16_t>(start);
    s.before.assign(before + start, before + last + 1);
    s.after.assign(after + start, after + last + 1);
    segs.push_back(std::move(s));
    i = last + 1;
  }
  return segs;
}

}  // namespace

Txn& Txn::operator=(Txn&& o) noexcept {
  if (this != &o) {
    if (engine_ != nullptr && !engine_->crashed()) {
      try {
        abort();
      } catch (...) {
      }
    }
    engine_ = o.engine_;
    id_ = o.id_;
    o.engine_ = nullptr;
  }
  return *this;
}
Txn::~Txn() {
  if (engine_ != nullptr && !engine_->crashed()) {
    try {
      abort();
    } catch (...) {
    }
  }
}
void Txn::commit() {
  if (engine_ == nullptr) throw StorageError("transaction is not active");
  auto* e = engine_;
  engine_ = nullptr;
  e->commit_active();
}
void Txn::abort() {
  if (engine_ == nullptr) return;
  auto* e = engine_;
  engine_ = nullptr;
  e->abort_active();
}

StorageEngine::StorageEngine(EngineOptions opts)
    : opts_(std::move(opts)), disk_(opts_.path, opts_.faults), pool_(disk_, opts_.pool_frames, opts_.policy) {
  if (opts_.enable_wal) {
    wal_ = std::make_unique<Wal>(opts_.path + ".wal", opts_.sync_commit, opts_.faults);
    recovery_ = recover(*wal_, pool_);
  }
  pager_ = std::make_unique<Pager>(pool_);
  if (pager_->was_formatted()) {  // make the formatted meta page durable before anything is logged against it
    pool_.flush_all();
    disk_.sync();
  }
  install_hooks();
  if (recovery_.ran) checkpoint();  // flush recovered pages and shorten the log
  if (pager_->catalog_root() == kInvalidPage) {
    Txn t = begin();
    pager_->set_catalog_root(DiskBTree::create(*pager_));
    t.commit();
  }
}

StorageEngine::~StorageEngine() = default;

void StorageEngine::install_hooks() {
  pool_.set_write_gate([this] {
    if (!active_) throw StorageError("page modification outside a transaction");
  });
  if (!wal_) return;
  pool_.set_write_hook([this](page_id_t pid, const std::byte* before, std::byte* after) {
    if (!active_) return;
    auto segs = diff_page(before, after);
    if (segs.empty()) return;
    LogRecord r;
    r.type = LogType::kUpdate;
    r.txn = active_->id;
    r.prev_lsn = active_->last_lsn;
    r.page = pid;
    r.segments = std::move(segs);
    lsn_t lsn = wal_->append(r);
    active_->last_lsn = lsn;
    set_page_lsn(after, lsn);
    dirty_table_.emplace(pid, lsn);
  });
  pool_.set_log_flush_hook([this](lsn_t lsn) { wal_->flush(lsn); });
  pool_.set_page_written_hook([this](page_id_t pid) { dirty_table_.erase(pid); });
}

Txn StorageEngine::begin() {
  if (active_) throw StorageError("a write transaction is already active (single-writer engine)");
  txn_id_t id = next_txn_++;
  active_ = ActiveTxn{id, 0};
  if (wal_) {
    LogRecord r;
    r.type = LogType::kBegin;
    r.txn = id;
    active_->last_lsn = wal_->append(r);
  }
  return Txn(this, id);
}

std::size_t StorageEngine::add_txn_listener(std::function<void(bool)> fn) {
  listeners_.emplace_back(next_listener_, std::move(fn));
  return next_listener_++;
}
void StorageEngine::remove_txn_listener(std::size_t token) {
  std::erase_if(listeners_, [&](auto& p) { return p.first == token; });
}
void StorageEngine::notify(bool committed) {
  // Listeners may remove themselves (or others) while running, so snapshot the tokens and re-look each up.
  std::vector<std::size_t> tokens;
  for (auto& [id, fn] : listeners_) tokens.push_back(id);
  for (auto t : tokens) {
    auto it = std::find_if(listeners_.begin(), listeners_.end(), [&](auto& p) { return p.first == t; });
    if (it == listeners_.end()) continue;
    auto fn = it->second;
    fn(committed);
  }
}

void StorageEngine::commit_active() {
  if (wal_) {
    LogRecord r;
    r.type = LogType::kCommit;
    r.txn = active_->id;
    r.prev_lsn = active_->last_lsn;
    lsn_t lsn = wal_->append(r);
    active_.reset();  // from here on the transaction is logically committed; durability follows
    wal_->flush(lsn);
  } else {
    active_.reset();
  }
  ++stats_.commits;
  notify(true);
}

void StorageEngine::abort_active() {
  if (!wal_) {
    active_.reset();
    throw StorageError("cannot roll back without a WAL");
  }
  lsn_t cur = active_->last_lsn, chain = active_->last_lsn;
  while (cur != 0) {
    auto rec = wal_->read(cur);
    if (!rec) throw StorageError("rollback: unreadable log record");
    if (rec->type == LogType::kUpdate) {
      chain = undo_update(*wal_, pool_, *rec, chain, [this](page_id_t p, lsn_t l) { dirty_table_.emplace(p, l); });
      cur = rec->prev_lsn;
    } else if (rec->type == LogType::kClr) {
      cur = rec->undo_next;
    } else {
      break;  // kBegin
    }
  }
  LogRecord end;
  end.type = LogType::kAbort;
  end.txn = active_->id;
  end.prev_lsn = chain;
  wal_->append(end);
  active_.reset();
  ++stats_.aborts;
  notify(false);
}

void StorageEngine::checkpoint() {
  pool_.flush_all();
  disk_.sync();
  if (!wal_) return;
  LogRecord c;
  c.type = LogType::kCheckpoint;
  if (active_) c.active_txns.emplace_back(active_->id, active_->last_lsn);
  for (auto& [p, l] : dirty_table_) c.dirty_pages.emplace_back(p, l);
  lsn_t lsn = wal_->append(c);
  wal_->flush(lsn);
  wal_->set_checkpoint(lsn);
  if (!active_) wal_->truncate_all();
  ++stats_.checkpoints;
}

void StorageEngine::close() {
  if (active_) abort_active();
  checkpoint();
}

}  // namespace aqo
