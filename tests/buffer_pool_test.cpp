#include "aqo/buffer_pool.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "test_util.hpp"

namespace aqo {
namespace {

struct TempFile {
  std::string path;
  explicit TempFile(const std::string& name) : path(aqo::testing::unique_path(name, ".db")) {
    std::remove(path.c_str());
  }
  ~TempFile() { std::remove(path.c_str()); }
};

void write_tag(BufferPool& pool, page_id_t pid, std::uint32_t tag) {
  auto g = pool.fetch(pid);
  store<std::uint32_t>(g.data() + 100, tag);
  g.mark_dirty();
}
std::uint32_t read_tag(BufferPool& pool, page_id_t pid) { return load<std::uint32_t>(pool.fetch(pid).data() + 100); }

class PoolTest : public ::testing::TestWithParam<EvictionPolicy> {};

TEST_P(PoolTest, PinCountsAndGuards) {
  TempFile f("pins");
  DiskManager dm(f.path);
  BufferPool pool(dm, 3, GetParam());
  {
    auto a = pool.fetch(1);
    EXPECT_EQ(pool.pin_count(1), 1u);
    auto b = pool.fetch(1);
    EXPECT_EQ(pool.pin_count(1), 2u);
    auto c = std::move(b);
    EXPECT_EQ(pool.pin_count(1), 2u);
    c.release();
    EXPECT_EQ(pool.pin_count(1), 1u);
  }
  EXPECT_EQ(pool.pin_count(1), 0u);
  EXPECT_TRUE(pool.is_cached(1));
}

TEST_P(PoolTest, DirtyTrackingAndWriteBackOnEviction) {
  TempFile f("dirty");
  DiskManager dm(f.path);
  BufferPool pool(dm, 2, GetParam());
  write_tag(pool, 1, 111);
  EXPECT_TRUE(pool.is_dirty(1));
  EXPECT_EQ(pool.dirty_count(), 1u);
  EXPECT_EQ(dm.writes(), 0u) << "no write before eviction or flush";
  write_tag(pool, 2, 222);
  write_tag(pool, 3, 333);  // forces eviction of an earlier dirty page
  EXPECT_GE(pool.stats().evictions, 1u);
  EXPECT_GE(pool.stats().dirty_writebacks, 1u);
  EXPECT_EQ(read_tag(pool, 1), 111u) << "evicted dirty page must be re-read intact from disk";
  pool.flush_all();
  EXPECT_EQ(pool.dirty_count(), 0u);
}

TEST_P(PoolTest, AllPinnedThrows) {
  TempFile f("full");
  DiskManager dm(f.path);
  BufferPool pool(dm, 2, GetParam());
  auto a = pool.fetch(1);
  auto b = pool.fetch(2);
  EXPECT_THROW(pool.fetch(3), StorageError);
  a.release();
  EXPECT_NO_THROW(pool.fetch(3));
}

TEST_P(PoolTest, PinnedPageIsNeverEvicted) {
  TempFile f("nevict");
  DiskManager dm(f.path);
  BufferPool pool(dm, 3, GetParam());
  auto keep = pool.fetch(1);
  store<std::uint32_t>(keep.data() + 100, 7u);
  keep.mark_dirty();
  for (page_id_t p = 2; p < 40; ++p) pool.fetch(p);
  EXPECT_TRUE(pool.is_cached(1));
  EXPECT_EQ(load<std::uint32_t>(keep.data() + 100), 7u);
}

TEST_P(PoolTest, DataSurvivesReopen) {
  TempFile f("reopen");
  {
    DiskManager dm(f.path);
    BufferPool pool(dm, 4, GetParam());
    for (page_id_t p = 0; p < 20; ++p) write_tag(pool, p, 1000 + p);
    pool.flush_all();
    dm.sync();
  }
  DiskManager dm(f.path);
  EXPECT_EQ(dm.page_count(), 20u);
  BufferPool pool(dm, 4, GetParam());
  for (page_id_t p = 0; p < 20; ++p) EXPECT_EQ(read_tag(pool, p), 1000 + p);
}

TEST_P(PoolTest, WalRuleHookRunsBeforeWriteWithPageLsn) {
  TempFile f("wal");
  DiskManager dm(f.path);
  BufferPool pool(dm, 1, GetParam());
  std::vector<lsn_t> flushed;
  pool.set_log_flush_hook([&](lsn_t l) { flushed.push_back(l); });
  {
    auto g = pool.fetch(0);
    set_page_lsn(g.data(), 77);
    g.mark_dirty();
  }
  pool.fetch(1);  // evicts page 0
  ASSERT_EQ(flushed.size(), 1u);
  EXPECT_EQ(flushed[0], 77u);
}

TEST_P(PoolTest, HitMissStatsAndDiscard) {
  TempFile f("stats");
  DiskManager dm(f.path);
  BufferPool pool(dm, 4, GetParam());
  pool.fetch(1);
  pool.fetch(1);
  pool.fetch(2);
  EXPECT_EQ(pool.stats().hits, 1u);
  EXPECT_EQ(pool.stats().misses, 2u);
  EXPECT_NEAR(pool.stats().hit_rate(), 1.0 / 3.0, 1e-9);
  write_tag(pool, 5, 9);
  pool.discard_all();
  EXPECT_EQ(pool.dirty_count(), 0u);
  EXPECT_EQ(read_tag(pool, 5), 0u) << "discarded dirty page was never written";
}

INSTANTIATE_TEST_SUITE_P(Policies, PoolTest, ::testing::Values(EvictionPolicy::kLru, EvictionPolicy::kClock));

TEST(Replacer, LruEvictsLeastRecentlyUnpinned) {
  TempFile f("lru");
  DiskManager dm(f.path);
  BufferPool pool(dm, 3, EvictionPolicy::kLru);
  pool.fetch(1);
  pool.fetch(2);
  pool.fetch(3);
  pool.fetch(1);  // 1 becomes most recent; LRU order is now 2,3,1
  pool.fetch(4);  // evicts 2
  EXPECT_FALSE(pool.is_cached(2));
  EXPECT_TRUE(pool.is_cached(1));
  EXPECT_TRUE(pool.is_cached(3));
}

TEST(Replacer, ClockGivesSecondChance) {
  TempFile f("clock");
  DiskManager dm(f.path);
  BufferPool pool(dm, 3, EvictionPolicy::kClock);
  pool.fetch(1);
  pool.fetch(2);
  pool.fetch(3);
  pool.fetch(4);  // all ref bits set: first sweep clears them, then evicts the frame under the hand
  EXPECT_EQ(pool.stats().evictions, 1u);
  EXPECT_TRUE(pool.is_cached(4));
}

TEST(DiskManager, DetectsCorruption) {
  TempFile f("crc");
  {
    DiskManager dm(f.path);
    std::byte page[kPageSize] = {};
    page[200] = std::byte{5};
    dm.write_page(0, page);
    dm.sync();
  }
  {  // flip a byte on disk
    std::FILE* fp = std::fopen(f.path.c_str(), "r+b");
    std::fseek(fp, 300, SEEK_SET);
    std::fputc(0x7F, fp);
    std::fclose(fp);
  }
  DiskManager dm(f.path);
  std::byte out[kPageSize];
  EXPECT_THROW(dm.read_page(0, out), StorageError);
}

TEST(DiskManager, ReadBeyondEofIsZeroPage) {
  TempFile f("eof");
  DiskManager dm(f.path);
  std::byte out[kPageSize];
  std::memset(out, 1, sizeof out);
  dm.read_page(10, out);
  for (auto b : out) ASSERT_EQ(b, std::byte{0});
}

TEST(Crc32, KnownVector) {
  const char* s = "123456789";
  EXPECT_EQ(crc32(reinterpret_cast<const std::byte*>(s), 9), 0xCBF43926u);
}

}  // namespace
}  // namespace aqo
