#include "aqo/range_structs.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <random>

namespace aqo {
namespace {

struct SumMin {
  struct Value {
    long long sum = 0;
    long long min = LLONG_MAX;
    bool operator==(const Value&) const = default;
  };
  static Value identity() { return {}; }
  static Value combine(const Value& a, const Value& b) { return {a.sum + b.sum, std::min(a.min, b.min)}; }
};

TEST(SegmentTree, MatchesBruteForceUnderUpdates) {
  std::mt19937_64 rng(5);
  for (std::size_t n : {1u, 2u, 7u, 64u, 1000u}) {
    std::vector<SumMin::Value> init(n);
    std::vector<long long> v(n);
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = static_cast<long long>(rng() % 2000) - 1000;
      init[i] = {v[i], v[i]};
    }
    SegmentTree<SumMin> st(init);
    for (int it = 0; it < 3000; ++it) {
      if (rng() % 2) {
        std::size_t i = rng() % n;
        v[i] = static_cast<long long>(rng() % 2000) - 1000;
        st.set(i, {v[i], v[i]});
      } else {
        std::size_t l = rng() % (n + 1), r = rng() % (n + 1);
        if (l > r) std::swap(l, r);
        SumMin::Value want;
        for (std::size_t i = l; i < r; ++i) want = SumMin::combine(want, {v[i], v[i]});
        ASSERT_EQ(st.query(l, r), want) << n << " [" << l << "," << r << ")";
      }
    }
  }
}

TEST(Fenwick, PrefixAndRangeMatchBruteForce) {
  std::mt19937_64 rng(6);
  const std::size_t n = 500;
  Fenwick<long long> f(n);
  std::vector<long long> v(n, 0);
  for (int it = 0; it < 5000; ++it) {
    std::size_t i = rng() % n;
    long long d = static_cast<long long>(rng() % 100) - 50;
    f.add(i, d);
    v[i] += d;
    std::size_t l = rng() % (n + 1), r = rng() % (n + 1);
    if (l > r) std::swap(l, r);
    long long want = 0;
    for (std::size_t k = l; k < r; ++k) want += v[k];
    ASSERT_EQ(f.range(l, r), want);
  }
}

}  // namespace
}  // namespace aqo
