#include "aqo/bptree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace aqo {
namespace {

struct Stats {
  std::int64_t count = 0;
  std::int64_t sum = 0;
  std::int64_t min = std::numeric_limits<std::int64_t>::max();
  std::int64_t max = std::numeric_limits<std::int64_t>::min();
  bool operator==(const Stats&) const = default;
};

struct StatsAgg {
  static constexpr bool kEnabled = true;
  using Value = Stats;
  static Value identity() { return {}; }
  static Value lift(const std::int64_t&, const std::int64_t& v) { return {1, v, v, v}; }
  static Value combine(const Value& a, const Value& b) {
    return {a.count + b.count, a.sum + b.sum, std::min(a.min, b.min), std::max(a.max, b.max)};
  }
};

template <std::size_t Order>
using Tree = BPlusTree<std::int64_t, std::int64_t, NoAggregate<std::int64_t, std::int64_t>, Order>;
template <std::size_t Order>
using AggTree = BPlusTree<std::int64_t, std::int64_t, StatsAgg, Order>;

TEST(BPlusTree, EmptyTree) {
  Tree<4> t;
  EXPECT_TRUE(t.empty());
  EXPECT_EQ(t.find(1), nullptr);
  EXPECT_FALSE(t.erase(1));
  EXPECT_EQ(t.begin(), t.end());
  EXPECT_EQ(t.height(), 1u);
  EXPECT_EQ(t.validate(), "");
}

TEST(BPlusTree, InsertFindDuplicateAndAssign) {
  Tree<4> t;
  EXPECT_TRUE(t.insert(5, 50));
  EXPECT_FALSE(t.insert(5, 99)) << "insert must not overwrite";
  EXPECT_EQ(*t.find(5), 50);
  EXPECT_FALSE(t.insert_or_assign(5, 99));
  EXPECT_EQ(*t.find(5), 99);
  EXPECT_EQ(t.size(), 1u);
}

TEST(BPlusTree, StringValuesAndCustomCompare) {
  BPlusTree<std::string, std::string, NoAggregate<std::string, std::string>, 4, std::greater<>> t;
  for (const char* k : {"b", "d", "a", "c", "e", "f", "g"}) t.insert(k, std::string(k) + "!");
  std::string order;
  for (auto it = t.begin(); it != t.end(); ++it) order += it.key();
  EXPECT_EQ(order, "gfedcba");
  EXPECT_EQ(t.validate(), "");
}

TEST(BPlusTree, SequentialAscendingAndDescendingGrowHeight) {
  Tree<3> asc, desc;
  for (std::int64_t i = 0; i < 2000; ++i) {
    asc.insert(i, i);
    desc.insert(2000 - i, i);
  }
  EXPECT_EQ(asc.validate(), "");
  EXPECT_EQ(desc.validate(), "");
  EXPECT_GE(asc.height(), 6u);
  std::int64_t expect = 0;
  for (auto it = asc.begin(); it != asc.end(); ++it) EXPECT_EQ(it.key(), expect++);
  EXPECT_EQ(expect, 2000);
}

TEST(BPlusTree, EraseAllInManyOrdersKeepsInvariants) {
  for (int mode = 0; mode < 4; ++mode) {
    Tree<4> t;
    std::vector<std::int64_t> keys(500);
    std::iota(keys.begin(), keys.end(), 0);
    for (auto k : keys) t.insert(k, k);
    if (mode == 1) std::reverse(keys.begin(), keys.end());
    if (mode == 2) std::shuffle(keys.begin(), keys.end(), std::mt19937(7));
    if (mode == 3) {  // middle out: stresses internal-node separator removal
      std::vector<std::int64_t> mid;
      for (std::size_t i = 0; i < keys.size(); ++i) mid.push_back(i % 2 == 0 ? keys[250 + i / 2] : keys[249 - i / 2]);
      keys = mid;
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
      ASSERT_TRUE(t.erase(keys[i])) << "mode " << mode << " key " << keys[i];
      ASSERT_EQ(t.validate(), "") << "mode " << mode << " after " << i;
    }
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.height(), 1u);
    t.insert(1, 1);  // tree is reusable after emptying
    EXPECT_EQ(t.validate(), "");
  }
}

TEST(BPlusTree, RangeScanBoundsAndInclusivity) {
  Tree<4> t;
  for (std::int64_t i = 0; i < 100; i += 2) t.insert(i, i);  // evens
  auto collect = [&](KeyRange<std::int64_t> r) {
    std::vector<std::int64_t> out;
    t.scan(r, [&](auto k, auto) { out.push_back(k); });
    return out;
  };
  EXPECT_EQ(collect({10, true, 16, true}), (std::vector<std::int64_t>{10, 12, 14, 16}));
  EXPECT_EQ(collect({10, false, 16, false}), (std::vector<std::int64_t>{12, 14}));
  EXPECT_EQ(collect({11, true, 15, true}), (std::vector<std::int64_t>{12, 14}));
  EXPECT_EQ(collect({std::nullopt, true, 4, true}), (std::vector<std::int64_t>{0, 2, 4}));
  EXPECT_EQ(collect({96, false, std::nullopt, true}), (std::vector<std::int64_t>{98}));
  EXPECT_TRUE(collect({200, true, std::nullopt, true}).empty());
  EXPECT_TRUE(collect({20, true, 10, true}).empty());
  EXPECT_EQ(t.lower_bound(7).key(), 8);
  EXPECT_EQ(t.upper_bound(8).key(), 10);
  EXPECT_EQ(t.upper_bound(98), t.end());
}

// ---- property tests against std::map --------------------------------------------------

template <class T>
void compare_to_model(const T& t, const std::map<std::int64_t, std::int64_t>& m) {
  ASSERT_EQ(t.size(), m.size());
  auto it = t.begin();
  for (const auto& [k, v] : m) {
    ASSERT_NE(it, t.end());
    ASSERT_EQ(it.key(), k);
    ASSERT_EQ(it.value(), v);
    ++it;
  }
  ASSERT_EQ(it, t.end());
}

template <std::size_t Order>
void run_property(std::uint32_t seed, int ops, std::int64_t key_space) {
  std::mt19937_64 rng(seed);
  Tree<Order> t;
  std::map<std::int64_t, std::int64_t> model;
  for (int i = 0; i < ops; ++i) {
    std::int64_t k = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(key_space));
    int op = static_cast<int>(rng() % 10);
    if (op < 4) {
      ASSERT_EQ(t.insert(k, i), model.emplace(k, i).second);
    } else if (op < 5) {
      ASSERT_EQ(t.insert_or_assign(k, i), model.insert_or_assign(k, i).second);
    } else if (op < 8) {
      ASSERT_EQ(t.erase(k), model.erase(k) == 1);
    } else if (op < 9) {
      auto* v = t.find(k);
      auto mit = model.find(k);
      ASSERT_EQ(v != nullptr, mit != model.end());
      if (v != nullptr) {
        ASSERT_EQ(*v, mit->second);
      }
    } else {
      std::int64_t lo = k, hi = k + static_cast<std::int64_t>(rng() % 50);
      bool li = rng() & 1, hiinc = rng() & 1;
      std::vector<std::int64_t> got, want;
      t.scan({lo, li, hi, hiinc}, [&](auto key, auto) { got.push_back(key); });
      for (auto it = li ? model.lower_bound(lo) : model.upper_bound(lo);
           it != model.end() && (hiinc ? it->first <= hi : it->first < hi); ++it)
        want.push_back(it->first);
      ASSERT_EQ(got, want);
    }
    if (i % 97 == 0) {
      ASSERT_EQ(t.validate(), "") << "seed " << seed << " op " << i;
    }
  }
  ASSERT_EQ(t.validate(), "");
  compare_to_model(t, model);
}

TEST(BPlusTreeProperty, MatchesStdMapAcrossOrdersAndSeeds) {
  for (std::uint32_t seed = 1; seed <= 12; ++seed) {
    run_property<3>(seed, 6000, 400);
    run_property<4>(seed, 6000, 400);
    run_property<5>(seed, 6000, 1500);
    run_property<8>(seed, 6000, 1500);
    run_property<64>(seed, 12000, 6000);
    if (HasFatalFailure()) return;
  }
}

TEST(BPlusTreeProperty, GrowThenShrinkLarge) {
  std::mt19937_64 rng(99);
  Tree<16> t;
  std::map<std::int64_t, std::int64_t> model;
  for (int i = 0; i < 50000; ++i) {
    auto k = static_cast<std::int64_t>(rng() % 1000000);
    t.insert_or_assign(k, i);
    model[k] = i;
  }
  ASSERT_EQ(t.validate(), "");
  compare_to_model(t, model);
  std::vector<std::int64_t> keys;
  for (auto& [k, v] : model) keys.push_back(k);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    ASSERT_TRUE(t.erase(keys[i]));
    model.erase(keys[i]);
    if (i % 5000 == 0) {
      ASSERT_EQ(t.validate(), "");
      compare_to_model(t, model);
    }
  }
  EXPECT_TRUE(t.empty());
}

// ---- aggregates ---------------------------------------------------------------------

Stats brute(const std::map<std::int64_t, std::int64_t>& m, const KeyRange<std::int64_t>& r) {
  Stats s;
  for (const auto& [k, v] : m) {
    if (r.lo && (r.lo_inclusive ? k < *r.lo : k <= *r.lo)) continue;
    if (r.hi && (r.hi_inclusive ? k > *r.hi : k >= *r.hi)) continue;
    s = StatsAgg::combine(s, StatsAgg::lift(k, v));
  }
  return s;
}

template <std::size_t Order>
void run_agg_property(std::uint32_t seed) {
  std::mt19937_64 rng(seed);
  AggTree<Order> t;
  std::map<std::int64_t, std::int64_t> model;
  for (int i = 0; i < 4000; ++i) {
    auto k = static_cast<std::int64_t>(rng() % 700);
    auto v = static_cast<std::int64_t>(rng() % 2001) - 1000;
    int op = static_cast<int>(rng() % 10);
    if (op < 5) {
      t.insert_or_assign(k, v);
      model[k] = v;
    } else if (op < 8) {
      t.erase(k);
      model.erase(k);
    } else {
      KeyRange<std::int64_t> r;
      if (rng() % 5 != 0) r.lo = static_cast<std::int64_t>(rng() % 700);
      if (rng() % 5 != 0) r.hi = static_cast<std::int64_t>(rng() % 700);
      r.lo_inclusive = rng() & 1;
      r.hi_inclusive = rng() & 1;
      ASSERT_EQ(t.aggregate(r), brute(model, r)) << "seed " << seed << " op " << i;
    }
    if (i % 50 == 0) {
      ASSERT_EQ(t.validate(), "") << "seed " << seed << " op " << i;
    }
  }
  ASSERT_EQ(t.aggregate_all(), brute(model, {}));
}

TEST(BPlusTreeAggregate, RangeAggregatesMatchBruteForceUnderUpdates) {
  for (std::uint32_t seed = 1; seed <= 10; ++seed) {
    run_agg_property<3>(seed);
    run_agg_property<4>(seed);
    run_agg_property<7>(seed);
    run_agg_property<32>(seed);
    if (HasFatalFailure()) return;
  }
}

}  // namespace
}  // namespace aqo
