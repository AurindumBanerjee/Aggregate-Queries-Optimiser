#include "aqo/recovery.hpp"

#include <chrono>
#include <map>

namespace aqo {
namespace {

template <class SegVec>
void apply_after(std::byte* page, const SegVec& segs) {
  for (const auto& s : segs) {
    if (s.offset < kTypeOff || static_cast<std::size_t>(s.offset) + s.after.size() > kPageSize)
      throw StorageError("corrupt log record: segment out of page bounds");
    std::memcpy(page + s.offset, s.after.data(), s.after.size());
  }
}

}  // namespace

lsn_t undo_update(Wal& wal, BufferPool& pool, const LogRecord& upd, lsn_t txn_last_lsn,
                  const std::function<void(page_id_t, lsn_t)>& on_dirty) {
  LogRecord clr;
  clr.type = LogType::kClr;
  clr.txn = upd.txn;
  clr.prev_lsn = txn_last_lsn;
  clr.page = upd.page;
  clr.undo_next = upd.prev_lsn;
  for (const auto& s : upd.segments) {
    Segment c;
    c.offset = s.offset;
    c.after = s.before;  // a CLR's redo image is the original's undo image
    clr.segments.push_back(std::move(c));
  }
  lsn_t lsn = wal.append(clr);
  auto g = pool.fetch(upd.page);
  apply_after(g.data(), clr.segments);
  set_page_lsn(g.data(), lsn);
  g.mark_dirty();
  if (on_dirty) on_dirty(upd.page, lsn);
  return lsn;
}

RecoveryStats recover(Wal& wal, BufferPool& pool) {
  auto t0 = std::chrono::steady_clock::now();
  RecoveryStats st;
  st.ran = true;

  // ---- 1. analysis ----
  std::map<txn_id_t, lsn_t> att;      // active transaction table: txn -> last LSN
  std::map<page_id_t, lsn_t> dpt;     // dirty page table: page -> rec_lsn
  st.analysis_start = wal.checkpoint_lsn();
  if (st.analysis_start != 0) {
    if (auto ck = wal.read(st.analysis_start); ck && ck->type == LogType::kCheckpoint) {
      for (auto& [t, l] : ck->active_txns) att[t] = l;
      for (auto& [p, l] : ck->dirty_pages) dpt[p] = l;
    } else {
      st.analysis_start = 0;  // checkpoint record unreadable: fall back to scanning the whole log
    }
  }
  wal.scan(st.analysis_start, [&](const LogRecord& r) {
    ++st.records_scanned;
    switch (r.type) {
      case LogType::kBegin:
        att[r.txn] = r.lsn;
        break;
      case LogType::kUpdate:
      case LogType::kClr:
        att[r.txn] = r.lsn;
        dpt.emplace(r.page, r.lsn);
        break;
      case LogType::kCommit:
      case LogType::kAbort:
        att.erase(r.txn);
        break;
      case LogType::kCheckpoint:
        break;
    }
    return true;
  });

  // ---- 2. redo: repeat history ----
  if (!dpt.empty()) {
    lsn_t start = dpt.begin()->second;
    for (auto& [p, l] : dpt) start = std::min(start, l);
    st.redo_start = start;
    wal.scan(start, [&](const LogRecord& r) {
      if (r.type != LogType::kUpdate && r.type != LogType::kClr) return true;
      auto it = dpt.find(r.page);
      if (it == dpt.end() || r.lsn < it->second) return true;
      auto g = pool.fetch(r.page);
      if (page_lsn(g.data()) >= r.lsn) {
        ++st.redo_skipped;
        return true;
      }
      apply_after(g.data(), r.segments);
      set_page_lsn(g.data(), r.lsn);
      g.mark_dirty();
      ++st.redone;
      return true;
    });
  }

  // ---- 3. undo losers, newest record first ----
  std::map<txn_id_t, lsn_t> next_undo = att;   // next record to process per loser
  std::map<txn_id_t, lsn_t> chain_last = att;  // tail of each loser's log chain (for CLR prev_lsn)
  st.losers = att.size();
  while (!next_undo.empty()) {
    auto pick = next_undo.begin();
    for (auto it = next_undo.begin(); it != next_undo.end(); ++it)
      if (it->second > pick->second) pick = it;
    txn_id_t txn = pick->first;
    lsn_t at = pick->second;
    auto rec = wal.read(at);
    if (!rec) throw StorageError("recovery: unreadable log record " + std::to_string(at));
    switch (rec->type) {
      case LogType::kUpdate:
        chain_last[txn] = undo_update(wal, pool, *rec, chain_last[txn]);
        ++st.clrs_written;
        next_undo[txn] = rec->prev_lsn;
        break;
      case LogType::kClr:
        next_undo[txn] = rec->undo_next;
        break;
      default:  // kBegin (or anything else): nothing left to undo for this transaction
        next_undo[txn] = 0;
        break;
    }
    if (next_undo[txn] == 0) {
      LogRecord end;
      end.type = LogType::kAbort;
      end.txn = txn;
      end.prev_lsn = chain_last[txn];
      wal.append(end);
      next_undo.erase(txn);
    }
  }
  wal.flush_all();
  st.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return st;
}

}  // namespace aqo
