// Aggregate indexes: an in-memory B+ tree keyed by (filter-column value, primary key) whose nodes cache
// (count, per-column sum/min/max) for their whole subtree. One index answers
//     SELECT SUM|MIN|MAX|COUNT|AVG(any numeric column) FROM t WHERE filter_col <op> literal
// in O(log n) node visits regardless of how many rows match, and stays correct under inserts, updates
// and deletes because every write refreshes the aggregates along one root-to-leaf path.
// (A B-ary segment tree: see docs/adr/0003-aggregate-index.md.)
#pragma once

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "aqo/bptree.hpp"
#include "aqo/value.hpp"

namespace aqo {

__extension__ using int128 = __int128;

/// Numeric payload of one row. raw[i] holds numeric column i either as an int64 or, if bit i of
/// real_mask is set, as the bit pattern of a double.
struct RowNums {
  std::array<std::int64_t, kMaxNumericColumns> raw{};
  std::uint8_t real_mask = 0;
};

struct SlotStats {
  int128 isum = 0;
  double dsum = 0;
  std::int64_t imin = std::numeric_limits<std::int64_t>::max();
  std::int64_t imax = std::numeric_limits<std::int64_t>::min();
  double dmin = std::numeric_limits<double>::infinity();
  double dmax = -std::numeric_limits<double>::infinity();
  bool operator==(const SlotStats&) const = default;
};

struct RowStats {
  std::int64_t count = 0;
  std::array<SlotStats, kMaxNumericColumns> slots{};
  bool operator==(const RowStats&) const = default;
};

struct RowStatsAgg;

/// Composite index key: ordered column value, then primary key to make it unique.
struct IdxKey {
  std::int64_t v;
  std::int64_t pk;
  auto operator<=>(const IdxKey&) const = default;
};

struct RowStatsAgg {
  static constexpr bool kEnabled = true;
  using Value = RowStats;
  static Value identity() { return {}; }
  static Value lift(const IdxKey&, const RowNums& r) {
    RowStats s;
    s.count = 1;
    for (std::size_t i = 0; i < kMaxNumericColumns; ++i) {
      SlotStats& t = s.slots[i];
      if ((r.real_mask >> i) & 1) {
        double d;
        std::memcpy(&d, &r.raw[i], 8);
        t.dsum = d;
        t.dmin = t.dmax = d;
      } else {
        t.isum = r.raw[i];
        t.imin = t.imax = r.raw[i];
      }
    }
    return s;
  }
  static Value combine(const Value& a, const Value& b) {
    RowStats s;
    s.count = a.count + b.count;
    for (std::size_t i = 0; i < kMaxNumericColumns; ++i) {
      const SlotStats &x = a.slots[i], &y = b.slots[i];
      SlotStats& t = s.slots[i];
      t.isum = x.isum + y.isum;
      t.dsum = x.dsum + y.dsum;
      t.imin = x.imin < y.imin ? x.imin : y.imin;
      t.imax = x.imax > y.imax ? x.imax : y.imax;
      t.dmin = x.dmin < y.dmin ? x.dmin : y.dmin;
      t.dmax = x.dmax > y.dmax ? x.dmax : y.dmax;
    }
    return s;
  }
};

/// Order-preserving map double -> int64 (so REAL columns can use integer keys). -0.0 == 0.0.
inline std::int64_t order_bits(double d) {
  if (d == 0.0) d = 0.0;
  std::int64_t b;
  std::memcpy(&b, &d, 8);
  return b >= 0 ? b : b ^ std::numeric_limits<std::int64_t>::max();
}

/// Interval on a column's values; `empty` short-circuits provably unsatisfiable predicates.
struct ValueRange {
  std::optional<Value> lo;
  bool lo_incl = true;
  std::optional<Value> hi;
  bool hi_incl = true;
  bool empty = false;
  void restrict_lo(const Value& v, bool incl);
  void restrict_hi(const Value& v, bool incl);
};

/// Inclusive int64 interval equivalent to `r` over an INT column (fractional/out-of-range literals are
/// rounded inward); nullopt if no integer can satisfy it.
std::optional<std::pair<std::int64_t, std::int64_t>> int_interval(const ValueRange& r);

class AggIndex {
 public:
  using Tree = BPlusTree<IdxKey, RowNums, RowStatsAgg, 32>;

  AggIndex(const Schema& schema, std::size_t filter_col);

  void add(const Row& row);
  /// Removes the entry for `row` (which must equal the row that was added).
  void remove(const Row& row);
  [[nodiscard]] std::size_t size() const { return tree_.size(); }
  [[nodiscard]] std::size_t filter_column() const { return col_; }

  /// Aggregates over rows whose filter column lies in `range`. O(log n).
  [[nodiscard]] RowStats query(const ValueRange& range) const;
  [[nodiscard]] RowStats query_all() const { return tree_.aggregate_all(); }
  [[nodiscard]] std::string validate() const { return tree_.validate(); }
  [[nodiscard]] const Tree& tree() const { return tree_; }

  /// Converts a value-domain range into index-key bounds; nullopt if no row can match.
  [[nodiscard]] std::optional<KeyRange<IdxKey>> to_key_range(const ValueRange& r) const;

 private:
  [[nodiscard]] IdxKey key_of(const Row& row) const;
  [[nodiscard]] RowNums nums_of(const Row& row) const;

  const Schema& schema_;
  std::size_t col_;
  ColType type_;
  std::vector<std::size_t> numeric_;  // schema column of each payload slot
  Tree tree_;
};

}  // namespace aqo
