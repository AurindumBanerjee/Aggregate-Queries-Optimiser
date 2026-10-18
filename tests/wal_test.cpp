#include "aqo/wal.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "test_util.hpp"

namespace aqo {
namespace {

struct TempWal {
  std::string path;
  explicit TempWal(const std::string& n) : path(aqo::testing::unique_path("wal_" + n, ".wal")) {
    std::filesystem::remove(path);
  }
  ~TempWal() {
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
  }
};

LogRecord update_rec(txn_id_t txn, lsn_t prev, page_id_t page) {
  LogRecord r;
  r.type = LogType::kUpdate;
  r.txn = txn;
  r.prev_lsn = prev;
  r.page = page;
  Segment s;
  s.offset = 100;
  s.before = {std::byte{1}, std::byte{2}};
  s.after = {std::byte{3}, std::byte{4}};
  r.segments.push_back(s);
  return r;
}

TEST(Wal, EncodeDecodeRoundTripsEveryType) {
  LogRecord u = update_rec(7, 5, 42);
  LogRecord clr = u;
  clr.type = LogType::kClr;
  clr.undo_next = 9;
  clr.segments[0].before.clear();
  LogRecord ck;
  ck.type = LogType::kCheckpoint;
  ck.active_txns = {{3, 100}, {4, 200}};
  ck.dirty_pages = {{9, 50}};
  LogRecord b;
  b.type = LogType::kBegin;
  b.txn = 1;
  for (const auto& r : {u, clr, ck, b}) {
    auto bytes = Wal::encode(r);
    LogRecord out;
    ASSERT_TRUE(Wal::decode(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.type, r.type);
    EXPECT_EQ(out.txn, r.txn);
    EXPECT_EQ(out.prev_lsn, r.prev_lsn);
    EXPECT_EQ(out.page, r.page);
    EXPECT_EQ(out.undo_next, r.undo_next);
    EXPECT_EQ(out.active_txns, r.active_txns);
    EXPECT_EQ(out.dirty_pages, r.dirty_pages);
    ASSERT_EQ(out.segments.size(), r.segments.size());
    for (std::size_t i = 0; i < r.segments.size(); ++i) {
      EXPECT_EQ(out.segments[i].offset, r.segments[i].offset);
      EXPECT_EQ(out.segments[i].before, r.segments[i].before);
      EXPECT_EQ(out.segments[i].after, r.segments[i].after);
    }
  }
  std::byte junk[3] = {std::byte{99}, std::byte{0}, std::byte{0}};
  LogRecord out;
  EXPECT_FALSE(Wal::decode(junk, 3, out));
}

TEST(Wal, AppendFlushReadScanAndReopen) {
  TempWal f("basic");
  std::vector<lsn_t> lsns;
  {
    Wal w(f.path, true);
    lsn_t prev = 0;
    for (int i = 0; i < 50; ++i) {
      prev = w.append(update_rec(1, prev, static_cast<page_id_t>(i)));
      lsns.push_back(prev);
    }
    EXPECT_EQ(w.read(lsns[10])->page, 10u) << "records are readable before being flushed";
    w.flush(lsns[49]);
    EXPECT_EQ(w.synced_file_size(), w.file_size());
  }
  Wal w(f.path, true);
  std::size_t n = 0;
  w.scan(0, [&](const LogRecord& r) {
    EXPECT_EQ(r.lsn, lsns[n]);
    EXPECT_EQ(r.page, n);
    ++n;
    return true;
  });
  EXPECT_EQ(n, 50u);
  EXPECT_EQ(w.read(lsns[7])->page, 7u);
  EXPECT_FALSE(w.read(lsns[7] + 1).has_value()) << "LSN not on a record boundary";
  EXPECT_GT(w.end_lsn(), lsns[49]);
}

TEST(Wal, LsnsAreMonotonicAcrossTruncation) {
  TempWal f("trunc");
  Wal w(f.path, true);
  lsn_t last = 0;
  for (int i = 0; i < 10; ++i) last = w.append(update_rec(1, last, 1));
  lsn_t before_end = w.end_lsn();
  w.truncate_all();
  EXPECT_EQ(w.end_lsn(), before_end);
  EXPECT_EQ(w.file_size(), Wal::kHeaderSize);
  lsn_t next = w.append(update_rec(2, 0, 1));
  EXPECT_EQ(next, before_end) << "new records continue the LSN sequence";
  w.flush_all();
  EXPECT_FALSE(w.read(last).has_value()) << "truncated records are gone";
  EXPECT_EQ(w.read(next)->txn, 2u);
}

TEST(Wal, TornTailIsDiscardedOnOpen) {
  TempWal f("torn");
  lsn_t good_end;
  {
    Wal w(f.path, true);
    for (int i = 0; i < 5; ++i) w.append(update_rec(1, 0, 1));
    w.flush_all();
    good_end = w.end_lsn();
    w.append(update_rec(1, 0, 2));
    w.flush_all();
  }
  auto full = std::filesystem::file_size(f.path);
  for (std::uint64_t cut : {full - 1, full - 5, full - 20}) {  // chop into the last record
    std::filesystem::remove(f.path + ".cut");
    std::filesystem::copy_file(f.path, f.path + ".cut");
    std::filesystem::resize_file(f.path + ".cut", cut);
    Wal w(f.path + ".cut", true);
    EXPECT_EQ(w.end_lsn(), good_end) << "cut at " << cut;
    lsn_t l = w.append(update_rec(9, 0, 3));  // appending after the repaired tail works
    w.flush_all();
    EXPECT_EQ(w.read(l)->txn, 9u);
  }
  std::filesystem::remove(f.path + ".cut");
}

TEST(Wal, CorruptedMiddleStopsTheLogThere) {
  TempWal f("corrupt");
  {
    Wal w(f.path, true);
    for (int i = 0; i < 6; ++i) w.append(update_rec(1, 0, static_cast<page_id_t>(i)));
    w.flush_all();
  }
  {  // flip a byte inside the 3rd record's payload
    std::FILE* fp = std::fopen(f.path.c_str(), "r+b");
    long rec_size = static_cast<long>((std::filesystem::file_size(f.path) - Wal::kHeaderSize) / 6);
    std::fseek(fp, static_cast<long>(Wal::kHeaderSize) + 2 * rec_size + 12, SEEK_SET);
    std::fputc(0xEE, fp);
    std::fclose(fp);
  }
  Wal w(f.path, true);
  std::size_t n = 0;
  w.scan(0, [&](const LogRecord&) { return ++n, true; });
  EXPECT_EQ(n, 2u);
}

TEST(Wal, CheckpointLsnIsPersisted) {
  TempWal f("ckpt");
  lsn_t l;
  {
    Wal w(f.path, true);
    l = w.append(update_rec(1, 0, 1));
    w.flush_all();
    w.set_checkpoint(l);
  }
  Wal w(f.path, true);
  EXPECT_EQ(w.checkpoint_lsn(), l);
}

TEST(Wal, FaultInjectorTearsFlushes) {
  TempWal f("fault");
  FaultInjector inj(5);
  Wal w(f.path, true, &inj);
  lsn_t l = w.append(update_rec(1, 0, 1));
  inj.arm_after(0);
  EXPECT_THROW(w.flush(l), SimulatedCrash);
  EXPECT_TRUE(inj.fired());
  EXPECT_EQ(w.synced_file_size(), Wal::kHeaderSize) << "nothing was durable";
}

}  // namespace
}  // namespace aqo
