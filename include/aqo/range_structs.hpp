// Classic array-based range structures. They need a fixed index universe, so the engine uses the
// aggregate-augmented B+ tree (agg.hpp) for live data; these are kept as the textbook baselines the
// benchmarks compare against and as independent oracles in tests.
#pragma once

#include <cstddef>
#include <vector>

namespace aqo {

/// Iterative bottom-up segment tree over a monoid. Point update and half-open range query in O(log n).
template <class Monoid>
class SegmentTree {
 public:
  using T = typename Monoid::Value;
  explicit SegmentTree(std::size_t n) : n_(n), t_(2 * n, Monoid::identity()) {}
  explicit SegmentTree(const std::vector<T>& init) : n_(init.size()), t_(2 * init.size(), Monoid::identity()) {
    for (std::size_t i = 0; i < n_; ++i) t_[n_ + i] = init[i];
    for (std::size_t i = n_; i-- > 1;) t_[i] = Monoid::combine(t_[2 * i], t_[2 * i + 1]);
  }
  void set(std::size_t i, const T& v) {
    for (t_[i += n_] = v; i > 1; i >>= 1) t_[i >> 1] = Monoid::combine(t_[i & ~std::size_t{1}], t_[i | 1]);
  }
  [[nodiscard]] T query(std::size_t l, std::size_t r) const {  // [l, r)
    T left = Monoid::identity(), right = Monoid::identity();
    for (l += n_, r += n_; l < r; l >>= 1, r >>= 1) {
      if (l & 1) left = Monoid::combine(left, t_[l++]);
      if (r & 1) right = Monoid::combine(t_[--r], right);
    }
    return Monoid::combine(left, right);
  }
  [[nodiscard]] std::size_t size() const { return n_; }

 private:
  std::size_t n_;
  std::vector<T> t_;
};

/// Fenwick (binary indexed) tree for prefix sums with point updates, O(log n).
template <class T>
class Fenwick {
 public:
  explicit Fenwick(std::size_t n) : t_(n + 1, T{}) {}
  void add(std::size_t i, T delta) {
    for (++i; i < t_.size(); i += i & (~i + 1)) t_[i] += delta;
  }
  /// Sum of elements [0, i).
  [[nodiscard]] T prefix(std::size_t i) const {
    T s{};
    for (; i > 0; i -= i & (~i + 1)) s += t_[i];
    return s;
  }
  [[nodiscard]] T range(std::size_t l, std::size_t r) const { return prefix(r) - prefix(l); }

 private:
  std::vector<T> t_;
};

}  // namespace aqo
