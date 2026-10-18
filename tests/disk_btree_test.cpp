#include "aqo/disk_btree.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>

#include "test_util.hpp"

namespace aqo {
namespace {

struct TempFile {
  std::string path;
  explicit TempFile(const std::string& name) : path(aqo::testing::unique_path("bt_" + name, ".db")) {
    std::remove(path.c_str());
  }
  ~TempFile() { std::remove(path.c_str()); }
};

std::vector<std::byte> val(const std::string& s) {
  std::vector<std::byte> v(s.size());
  if (!s.empty()) std::memcpy(v.data(), s.data(), s.size());
  return v;
}
std::string str(const std::vector<std::byte>& v) { return {reinterpret_cast<const char*>(v.data()), v.size()}; }

struct Env {
  TempFile file;
  DiskManager disk;
  BufferPool pool;
  Pager pager;
  page_id_t root;
  DiskBTree tree;
  Env(const std::string& name, std::size_t frames, EvictionPolicy p = EvictionPolicy::kLru)
      : file(name), disk(file.path), pool(disk, frames, p), pager(pool), root(DiskBTree::create(pager)), tree(pager, root) {}
};

TEST(DiskBTree, BasicCrud) {
  Env e("crud", 16);
  EXPECT_TRUE(e.tree.insert(5, val("five")));
  EXPECT_FALSE(e.tree.insert(5, val("dup")));
  EXPECT_EQ(str(*e.tree.get(5)), "five");
  EXPECT_FALSE(e.tree.upsert(5, val("FIVE")));
  EXPECT_EQ(str(*e.tree.get(5)), "FIVE");
  EXPECT_TRUE(e.tree.upsert(-7, val("neg")));
  EXPECT_FALSE(e.tree.get(6).has_value());
  EXPECT_TRUE(e.tree.erase(5));
  EXPECT_FALSE(e.tree.erase(5));
  EXPECT_EQ(e.tree.count(), 1u);
  EXPECT_TRUE(e.tree.insert(1, {}));  // empty value is legal
  EXPECT_EQ(e.tree.get(1)->size(), 0u);
  EXPECT_EQ(e.tree.validate(), "");
  EXPECT_THROW(e.tree.insert(9, val(std::string(DiskBTree::kMaxValueSize + 1, 'x'))), StorageError);
}

TEST(DiskBTree, SplitsGrowHeightAndScanIsOrdered) {
  Env e("grow", 32);
  const int n = 5000;
  for (int i = 0; i < n; ++i) ASSERT_TRUE(e.tree.insert((i * 7919) % n, val(std::string(900, static_cast<char>('a' + i % 26)))));
  EXPECT_EQ(e.tree.validate(), "");
  EXPECT_GE(e.tree.height(), 3u);
  EXPECT_EQ(e.tree.count(), static_cast<std::size_t>(n));
  std::int64_t expect = 0;
  e.tree.scan({}, [&](auto k, auto) {
    EXPECT_EQ(k, expect++);
    return true;
  });
  EXPECT_EQ(expect, n);
}

TEST(DiskBTree, RangeScanAndEarlyStop) {
  Env e("scan", 16);
  for (int i = 0; i < 1000; i += 2) e.tree.insert(i, val("v"));
  std::vector<std::int64_t> got;
  e.tree.scan({101, true, 109, false}, [&](auto k, auto) {
    got.push_back(k);
    return true;
  });
  EXPECT_EQ(got, (std::vector<std::int64_t>{102, 104, 106, 108}));
  got.clear();
  e.tree.scan({100, false, 106, true}, [&](auto k, auto) {
    got.push_back(k);
    return true;
  });
  EXPECT_EQ(got, (std::vector<std::int64_t>{102, 104, 106}));
  int seen = 0;
  e.tree.scan({}, [&](auto, auto) { return ++seen < 3; });
  EXPECT_EQ(seen, 3);
}

TEST(DiskBTree, MaxSizeValuesForceFrequentSplitsAndMerges) {
  Env e("big", 16);
  std::string big(DiskBTree::kMaxValueSize, 'z');
  for (int i = 0; i < 300; ++i) ASSERT_TRUE(e.tree.insert(i, val(big)));
  ASSERT_EQ(e.tree.validate(), "");
  for (int i = 0; i < 300; ++i) ASSERT_TRUE(e.tree.erase(i)) << i;
  ASSERT_EQ(e.tree.validate(), "");
  EXPECT_EQ(e.tree.count(), 0u);
  EXPECT_EQ(e.tree.height(), 1u);
}

TEST(DiskBTree, DeleteEverythingReturnsPagesToFreeList) {
  Env e("free", 32);
  for (int i = 0; i < 4000; ++i) e.tree.insert(i, val(std::string(100, 'q')));
  page_id_t high = e.pager.next_page();
  for (int i = 0; i < 4000; ++i) ASSERT_TRUE(e.tree.erase(i));
  EXPECT_EQ(e.tree.validate(), "");
  EXPECT_GT(e.pager.free_list_length(), 10u);
  for (int i = 0; i < 4000; ++i) e.tree.insert(i, val(std::string(100, 'q')));
  EXPECT_EQ(e.pager.next_page(), high) << "reinserting must reuse freed pages rather than growing the file";
}

TEST(DiskBTree, ClearFreesAllButRoot) {
  Env e("clear", 32);
  for (int i = 0; i < 3000; ++i) e.tree.insert(i, val(std::string(80, 'c')));
  e.tree.clear();
  EXPECT_EQ(e.tree.count(), 0u);
  EXPECT_EQ(e.tree.validate(), "");
  EXPECT_TRUE(e.tree.insert(1, val("x")));
}

TEST(DiskBTree, PersistsAcrossReopen) {
  TempFile f("persist");
  page_id_t root;
  {
    DiskManager disk(f.path);
    BufferPool pool(disk, 16);
    Pager pager(pool);
    root = DiskBTree::create(pager);
    DiskBTree t(pager, root);
    for (int i = 0; i < 2000; ++i) t.insert(i, val("row" + std::to_string(i)));
    pool.flush_all();
    disk.sync();
  }
  DiskManager disk(f.path);
  BufferPool pool(disk, 8);
  Pager pager(pool);
  DiskBTree t(pager, root);
  EXPECT_EQ(t.validate(), "");
  EXPECT_EQ(t.count(), 2000u);
  EXPECT_EQ(str(*t.get(1234)), "row1234");
}

TEST(DiskBTree, NoPinsLeakedAfterOperations) {
  Env e("pins", 8);
  for (int i = 0; i < 2000; ++i) e.tree.insert(i, val(std::string(50, 'p')));
  for (int i = 0; i < 2000; i += 2) e.tree.erase(i);
  EXPECT_GT(e.tree.count(), 0u);
  EXPECT_EQ(e.tree.validate(), "");
  for (page_id_t p = 0; p < e.pager.next_page(); ++p) ASSERT_EQ(e.pool.pin_count(p), 0u) << "page " << p;
}

class PropertyTest : public ::testing::TestWithParam<EvictionPolicy> {};

TEST_P(PropertyTest, MatchesStdMapUnderTinyBufferPool) {
  for (std::uint32_t seed = 1; seed <= 4; ++seed) {
    Env e("prop" + std::to_string(seed), 10, GetParam());  // tiny pool: constant eviction
    std::mt19937_64 rng(seed);
    std::map<std::int64_t, std::string> model;
    const int ops = 12000;
    for (int i = 0; i < ops; ++i) {
      auto k = static_cast<std::int64_t>(rng() % 1500) - 700;
      int op = static_cast<int>(rng() % 10);
      if (op < 3) {
        std::string v(rng() % 250, static_cast<char>('a' + rng() % 26));
        ASSERT_EQ(e.tree.insert(k, val(v)), model.emplace(k, v).second);
      } else if (op < 5) {
        std::string v(rng() % (seed % 2 ? 900 : 120), static_cast<char>('A' + rng() % 26));
        ASSERT_EQ(e.tree.upsert(k, val(v)), model.find(k) == model.end());
        model[k] = v;
      } else if (op < 8) {
        ASSERT_EQ(e.tree.erase(k), model.erase(k) == 1);
      } else if (op < 9) {
        auto got = e.tree.get(k);
        auto it = model.find(k);
        ASSERT_EQ(got.has_value(), it != model.end());
        if (got) {
          ASSERT_EQ(str(*got), it->second);
        }
      } else {
        std::int64_t hi = k + static_cast<std::int64_t>(rng() % 60);
        std::vector<std::int64_t> g, w;
        e.tree.scan({k, true, hi, true}, [&](auto key, auto) {
          g.push_back(key);
          return true;
        });
        for (auto it = model.lower_bound(k); it != model.end() && it->first <= hi; ++it) w.push_back(it->first);
        ASSERT_EQ(g, w);
      }
      if (i % 500 == 0) {
        ASSERT_EQ(e.tree.validate(), "") << "seed " << seed << " op " << i;
      }
    }
    ASSERT_EQ(e.tree.validate(), "");
    ASSERT_EQ(e.tree.count(), model.size());
    auto it = model.begin();
    e.tree.scan({}, [&](auto k, auto v) {
      EXPECT_EQ(k, it->first);
      EXPECT_EQ(std::string(reinterpret_cast<const char*>(v.data()), v.size()), it->second);
      ++it;
      return true;
    });
    // drain: exercises merges and root collapse at every level
    std::vector<std::int64_t> keys;
    for (auto& [k, v] : model) keys.push_back(k);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (std::size_t i = 0; i < keys.size(); ++i) {
      ASSERT_TRUE(e.tree.erase(keys[i]));
      if (i % 400 == 0) {
        ASSERT_EQ(e.tree.validate(), "") << "drain " << i;
      }
    }
    EXPECT_EQ(e.tree.count(), 0u);
    EXPECT_EQ(e.tree.height(), 1u);
  }
}

INSTANTIATE_TEST_SUITE_P(Policies, PropertyTest, ::testing::Values(EvictionPolicy::kLru, EvictionPolicy::kClock));

}  // namespace
}  // namespace aqo
