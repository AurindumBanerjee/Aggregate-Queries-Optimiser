// Expression binding and evaluation (SQLite-compatible NULL / integer / division semantics).
#pragma once

#include <unordered_map>

#include "aqo/sql/ast.hpp"

namespace aqo::sql {

/// Resolves column names to indexes in `schema`. Throws SqlError for unknown columns.
void bind(Expr& e, const Schema& schema);
[[nodiscard]] bool contains_aggregate(const Expr& e);
[[nodiscard]] bool references_columns(const Expr& e);

/// Values of aggregate sub-expressions, keyed by node identity, used while evaluating post-aggregation expressions.
using AggValues = std::unordered_map<const Expr*, Value>;

/// Evaluates `e` against one row. Aggregate nodes are looked up in `aggs` (must be non-null if any exist).
Value eval(const Expr& e, const Row* row, const AggValues* aggs = nullptr);
/// WHERE semantics: only a non-NULL, non-zero value is true.
[[nodiscard]] bool is_true(const Value& v);
/// Evaluates an expression that references no columns/aggregates, or nullopt if it does.
std::optional<Value> fold_constant(const Expr& e);
/// Collects every aggregate node reachable from `e`.
void collect_aggregates(const Expr& e, std::vector<const Expr*>& out);

}  // namespace aqo::sql
