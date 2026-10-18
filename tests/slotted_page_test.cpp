#include "aqo/slotted_page.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

namespace aqo {
namespace {

std::vector<std::byte> bytes(const std::string& s) {
  std::vector<std::byte> v(s.size());
  std::memcpy(v.data(), s.data(), s.size());
  return v;
}
std::string str(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

struct Fixture : ::testing::Test {
  alignas(8) std::byte buf[kPageSize];
  SlottedPage page{buf};
  void SetUp() override {
    std::memset(buf, 0, sizeof buf);
    page.init(PageType::kLeaf);
  }
};

TEST_F(Fixture, InitState) {
  EXPECT_EQ(page.count(), 0u);
  EXPECT_EQ(page.free_space(), SlottedPage::kCapacity);
  EXPECT_EQ(page_type(buf), PageType::kLeaf);
  EXPECT_EQ(page.aux1(), kInvalidPage);
  EXPECT_EQ(page.validate(), "");
}

TEST_F(Fixture, InsertAtKeepsLogicalOrder) {
  ASSERT_TRUE(page.insert_at(0, bytes("bb")));
  ASSERT_TRUE(page.insert_at(0, bytes("a")));
  ASSERT_TRUE(page.insert_at(2, bytes("ccc")));
  ASSERT_TRUE(page.insert_at(1, bytes("mid")));
  ASSERT_EQ(page.count(), 4u);
  EXPECT_EQ(str(page.get(0)), "a");
  EXPECT_EQ(str(page.get(1)), "mid");
  EXPECT_EQ(str(page.get(2)), "bb");
  EXPECT_EQ(str(page.get(3)), "ccc");
  EXPECT_EQ(page.validate(), "");
  EXPECT_THROW(page.insert_at(9, bytes("x")), StorageError);
}

TEST_F(Fixture, RemoveAndReplace) {
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(page.insert_at(static_cast<std::size_t>(i), bytes(std::string(10, static_cast<char>('a' + i)))));
  page.remove_at(2);
  EXPECT_EQ(page.count(), 4u);
  EXPECT_EQ(str(page.get(2)), std::string(10, 'd'));
  ASSERT_TRUE(page.replace_at(0, bytes("short")));  // shrink in place
  EXPECT_EQ(str(page.get(0)), "short");
  ASSERT_TRUE(page.replace_at(1, bytes(std::string(50, 'z'))));  // grow: relocates
  EXPECT_EQ(str(page.get(1)), std::string(50, 'z'));
  EXPECT_EQ(page.validate(), "");
  EXPECT_THROW(page.remove_at(99), StorageError);
}

TEST_F(Fixture, FullPageRejectsThenCompactionReclaimsGarbage) {
  std::string rec(100, 'x');
  std::size_t n = 0;
  while (page.insert_at(n, bytes(rec))) ++n;
  EXPECT_EQ(n, SlottedPage::kCapacity / (100 + SlottedPage::kSlotSize));
  EXPECT_FALSE(page.fits(100));
  // Delete from the middle: space is garbage (not contiguous) but insert must still succeed.
  page.remove_at(3);
  page.remove_at(3);
  ASSERT_TRUE(page.fits(150));
  EXPECT_TRUE(page.insert_at(3, bytes(std::string(150, 'y'))));
  EXPECT_EQ(page.validate(), "");
  EXPECT_EQ(str(page.get(3)), std::string(150, 'y'));
}

TEST_F(Fixture, ReplaceFailsWhenNoRoomAndLeavesPageIntact) {
  while (page.insert_at(page.count(), bytes(std::string(200, 'q')))) {
  }
  auto before = str(page.get(0));
  EXPECT_FALSE(page.replace_at(0, bytes(std::string(900, 'w'))));
  EXPECT_EQ(str(page.get(0)), before);
  EXPECT_EQ(page.validate(), "");
}

TEST_F(Fixture, InitPreservesLsn) {
  set_page_lsn(buf, 42);
  page.init(PageType::kInternal);
  EXPECT_EQ(page_lsn(buf), 42u);
  EXPECT_EQ(page_type(buf), PageType::kInternal);
}

TEST_F(Fixture, ClearEmptiesPage) {
  page.insert_at(0, bytes("abc"));
  page.clear();
  EXPECT_EQ(page.count(), 0u);
  EXPECT_EQ(page.free_space(), SlottedPage::kCapacity);
}

TEST_F(Fixture, RandomOpsMatchVectorModel) {
  std::mt19937 rng(123);
  std::vector<std::string> model;
  for (int step = 0; step < 20000; ++step) {
    int op = static_cast<int>(rng() % 10);
    if (op < 5) {
      std::string rec(1 + rng() % 120, static_cast<char>('a' + rng() % 26));
      std::size_t idx = model.empty() ? 0 : rng() % (model.size() + 1);
      bool fit = page.fits(rec.size());
      ASSERT_EQ(page.insert_at(idx, bytes(rec)), fit);
      if (fit) model.insert(model.begin() + static_cast<std::ptrdiff_t>(idx), rec);
    } else if (op < 8 && !model.empty()) {
      std::size_t idx = rng() % model.size();
      page.remove_at(idx);
      model.erase(model.begin() + static_cast<std::ptrdiff_t>(idx));
    } else if (!model.empty()) {
      std::size_t idx = rng() % model.size();
      std::string rec(1 + rng() % 120, static_cast<char>('A' + rng() % 26));
      if (page.replace_at(idx, bytes(rec))) model[idx] = rec;
    }
    if (step % 50 == 0) {
      ASSERT_EQ(page.validate(), "") << "step " << step;
      ASSERT_EQ(page.count(), model.size());
      for (std::size_t i = 0; i < model.size(); ++i) ASSERT_EQ(str(page.get(i)), model[i]);
    }
  }
}

}  // namespace
}  // namespace aqo
