#include "aqo/agg.hpp"

#include <cmath>

namespace aqo {
namespace {

constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

// Tightens a lower/upper bound in place. Bounds are compared as numbers (INT/REAL mixed) or text.
bool tighter_lo(const Value& cand, bool cand_incl, const Value& cur, bool cur_incl) {
  int c = compare(cand, cur);
  return c > 0 || (c == 0 && !cand_incl && cur_incl);
}
bool tighter_hi(const Value& cand, bool cand_incl, const Value& cur, bool cur_incl) {
  int c = compare(cand, cur);
  return c < 0 || (c == 0 && !cand_incl && cur_incl);
}

}  // namespace

void ValueRange::restrict_lo(const Value& v, bool incl) {
  if (!lo || tighter_lo(v, incl, *lo, lo_incl)) {
    lo = v;
    lo_incl = incl;
  }
}
void ValueRange::restrict_hi(const Value& v, bool incl) {
  if (!hi || tighter_hi(v, incl, *hi, hi_incl)) {
    hi = v;
    hi_incl = incl;
  }
}

AggIndex::AggIndex(const Schema& schema, std::size_t filter_col)
    : schema_(schema), col_(filter_col), type_(schema.cols.at(filter_col).type), numeric_(schema.numeric_columns()) {
  if (type_ == ColType::kText) throw SqlError("cannot index TEXT column " + schema.cols[filter_col].name);
  if (numeric_.size() > kMaxNumericColumns)
    throw SqlError("aggregate indexes support at most " + std::to_string(kMaxNumericColumns) + " numeric columns");
}

IdxKey AggIndex::key_of(const Row& row) const {
  std::int64_t pk = std::get<std::int64_t>(row[0]);
  if (type_ == ColType::kInt) return {std::get<std::int64_t>(row[col_]), pk};
  return {order_bits(std::get<double>(row[col_])), pk};
}

RowNums AggIndex::nums_of(const Row& row) const {
  RowNums n;
  for (std::size_t i = 0; i < numeric_.size(); ++i) {
    const Value& v = row[numeric_[i]];
    if (schema_.cols[numeric_[i]].type == ColType::kReal) {
      n.real_mask = static_cast<std::uint8_t>(n.real_mask | (1u << i));
      double d = std::get<double>(v);
      std::memcpy(&n.raw[i], &d, 8);
    } else {
      n.raw[i] = std::get<std::int64_t>(v);
    }
  }
  // Unused slots stay INT zero: they contribute 0 to sums and the identity-ish min/max never queried.
  return n;
}

void AggIndex::add(const Row& row) {
  if (!tree_.insert(key_of(row), nums_of(row))) throw StorageError("aggregate index: duplicate key");
}

void AggIndex::remove(const Row& row) {
  if (!tree_.erase(key_of(row))) throw StorageError("aggregate index out of sync with table");
}

std::optional<KeyRange<IdxKey>> AggIndex::to_key_range(const ValueRange& r) const {
  if (r.empty) return std::nullopt;
  KeyRange<IdxKey> out;
  out.lo_inclusive = out.hi_inclusive = true;
  if (type_ == ColType::kInt) {
    std::int64_t lo = kMin, hi = kMax;
    if (r.lo) {
      if (!is_numeric(*r.lo)) return std::nullopt;
      if (auto* i = std::get_if<std::int64_t>(&*r.lo)) {
        if (!r.lo_incl) {
          if (*i == kMax) return std::nullopt;
          lo = *i + 1;
        } else {
          lo = *i;
        }
      } else {
        double d = std::get<double>(*r.lo);
        if (std::isnan(d)) return std::nullopt;
        double c = r.lo_incl ? std::ceil(d) : std::floor(d) + 1.0;
        if (c >= 9223372036854775808.0) return std::nullopt;
        if (c > -9223372036854775808.0) lo = static_cast<std::int64_t>(c);
      }
    }
    if (r.hi) {
      if (!is_numeric(*r.hi)) return std::nullopt;
      if (auto* i = std::get_if<std::int64_t>(&*r.hi)) {
        if (!r.hi_incl) {
          if (*i == kMin) return std::nullopt;
          hi = *i - 1;
        } else {
          hi = *i;
        }
      } else {
        double d = std::get<double>(*r.hi);
        if (std::isnan(d)) return std::nullopt;
        double f = r.hi_incl ? std::floor(d) : std::ceil(d) - 1.0;
        if (f < -9223372036854775808.0) return std::nullopt;
        if (f < 9223372036854775808.0) hi = static_cast<std::int64_t>(f);
      }
    }
    if (lo > hi) return std::nullopt;
    if (r.lo) out.lo = IdxKey{lo, kMin};
    if (r.hi) out.hi = IdxKey{hi, kMax};
    return out;
  }
  // REAL column
  if (r.lo) {
    if (!is_numeric(*r.lo)) return std::nullopt;
    double d = as_double(*r.lo);
    if (std::isnan(d)) return std::nullopt;
    out.lo = IdxKey{order_bits(d), r.lo_incl ? kMin : kMax};
    out.lo_inclusive = r.lo_incl;
  }
  if (r.hi) {
    if (!is_numeric(*r.hi)) return std::nullopt;
    double d = as_double(*r.hi);
    if (std::isnan(d)) return std::nullopt;
    out.hi = IdxKey{order_bits(d), r.hi_incl ? kMax : kMin};
    out.hi_inclusive = r.hi_incl;
  }
  if (out.lo && out.hi && (*out.hi < *out.lo)) return std::nullopt;
  return out;
}

RowStats AggIndex::query(const ValueRange& range) const {
  auto kr = to_key_range(range);
  if (!kr) return RowStats{};
  return tree_.aggregate(*kr);
}

}  // namespace aqo
