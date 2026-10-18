// SQL abstract syntax tree.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "aqo/value.hpp"

namespace aqo::sql {

enum class Op { kAdd, kSub, kMul, kDiv, kMod, kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr, kNot, kNeg, kIsNull, kIsNotNull };
enum class AggFn { kCount, kSum, kAvg, kMin, kMax };

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
  enum class Kind { kLiteral, kColumn, kUnary, kBinary, kAggregate } kind = Kind::kLiteral;
  Value literal;              // kLiteral
  std::string name;           // kColumn: column name (lower-case)
  int column = -1;            // kColumn: resolved index (set by bind())
  Op op = Op::kAdd;           // kUnary / kBinary
  ExprPtr lhs, rhs;           // operands (kUnary uses lhs)
  AggFn fn = AggFn::kCount;   // kAggregate
  bool star = false;          // COUNT(*)
  ExprPtr arg;                // kAggregate argument
  std::string text;           // source-like rendering, used for result headers
};

struct SelectItem {
  ExprPtr expr;  // null means '*'
  std::string alias;
};
struct OrderTerm {
  ExprPtr expr;
  std::optional<int> ordinal;  // ORDER BY 2
  bool desc = false;
};

struct CreateTable {
  std::string name;
  std::vector<Column> cols;
};
struct CreateIndex {
  std::string table, column;
};
struct Insert {
  std::string table;
  std::vector<std::string> columns;  // empty = all columns in order
  std::vector<std::vector<ExprPtr>> rows;
};
struct Select {
  std::vector<SelectItem> items;
  std::string table;
  ExprPtr where;
  std::vector<ExprPtr> group_by;
  std::vector<OrderTerm> order_by;
  std::optional<std::int64_t> limit;
};
struct Update {
  std::string table;
  std::vector<std::pair<std::string, ExprPtr>> sets;
  ExprPtr where;
};
struct Delete {
  std::string table;
  ExprPtr where;
};
struct TxnControl {
  enum Kind { kBegin, kCommit, kRollback } kind;
};
struct Explain;

using Statement = std::variant<CreateTable, CreateIndex, Insert, Select, Update, Delete, TxnControl>;

struct ParsedStatement {
  Statement stmt;
  bool explain = false;
};

}  // namespace aqo::sql
