#include "aqo/sql/expr.hpp"

#include <cmath>
#include <limits>

namespace aqo::sql {
namespace {

Value from_bool(bool b) { return Value{static_cast<std::int64_t>(b ? 1 : 0)}; }

bool truthy_non_null(const Value& v) {
  if (auto* i = std::get_if<std::int64_t>(&v)) return *i != 0;
  if (auto* d = std::get_if<double>(&v)) return *d != 0.0;
  if (auto* s = std::get_if<std::string>(&v)) return !s->empty() && std::strtod(s->c_str(), nullptr) != 0.0;
  return false;
}

Value arith(Op op, const Value& a, const Value& b) {
  if (is_null(a) || is_null(b)) return Value{};
  if (!is_numeric(a) || !is_numeric(b)) throw SqlError("arithmetic on TEXT is not supported");
  auto* ia = std::get_if<std::int64_t>(&a);
  auto* ib = std::get_if<std::int64_t>(&b);
  if (ia != nullptr && ib != nullptr) {
    std::int64_t r;
    switch (op) {
      case Op::kAdd:
        if (!__builtin_add_overflow(*ia, *ib, &r)) return r;
        return static_cast<double>(*ia) + static_cast<double>(*ib);  // SQLite promotes to REAL on overflow
      case Op::kSub:
        if (!__builtin_sub_overflow(*ia, *ib, &r)) return r;
        return static_cast<double>(*ia) - static_cast<double>(*ib);
      case Op::kMul:
        if (!__builtin_mul_overflow(*ia, *ib, &r)) return r;
        return static_cast<double>(*ia) * static_cast<double>(*ib);
      case Op::kDiv:
        if (*ib == 0) return Value{};
        if (*ia == std::numeric_limits<std::int64_t>::min() && *ib == -1) return -static_cast<double>(*ia);
        return *ia / *ib;
      case Op::kMod:
        if (*ib == 0) return Value{};
        if (*ib == -1) return std::int64_t{0};
        return *ia % *ib;
      default:
        break;
    }
  }
  double x = as_double(a), y = as_double(b);
  switch (op) {
    case Op::kAdd: return x + y;
    case Op::kSub: return x - y;
    case Op::kMul: return x * y;
    case Op::kDiv: return y == 0.0 ? Value{} : Value{x / y};
    case Op::kMod: {
      auto xi = static_cast<std::int64_t>(x), yi = static_cast<std::int64_t>(y);
      if (yi == 0) return Value{};
      if (yi == -1) return static_cast<double>(0);
      return static_cast<double>(xi % yi);
    }
    default: break;
  }
  throw SqlError("internal: bad arithmetic operator");
}

Value cmp(Op op, const Value& a, const Value& b) {
  if (is_null(a) || is_null(b)) return Value{};
  int c = compare(a, b);
  switch (op) {
    case Op::kEq: return from_bool(c == 0);
    case Op::kNe: return from_bool(c != 0);
    case Op::kLt: return from_bool(c < 0);
    case Op::kLe: return from_bool(c <= 0);
    case Op::kGt: return from_bool(c > 0);
    case Op::kGe: return from_bool(c >= 0);
    default: break;
  }
  throw SqlError("internal: bad comparison operator");
}

}  // namespace

void bind(Expr& e, const Schema& schema) {
  switch (e.kind) {
    case Expr::Kind::kColumn: {
      auto c = schema.find(e.name);
      if (!c) throw SqlError("no such column: " + e.name);
      e.column = static_cast<int>(*c);
      break;
    }
    case Expr::Kind::kUnary:
      bind(*e.lhs, schema);
      break;
    case Expr::Kind::kBinary:
      bind(*e.lhs, schema);
      bind(*e.rhs, schema);
      break;
    case Expr::Kind::kAggregate:
      if (e.arg) {
        if (contains_aggregate(*e.arg)) throw SqlError("aggregate functions cannot be nested");
        bind(*e.arg, schema);
      }
      break;
    case Expr::Kind::kLiteral:
      break;
  }
}

bool contains_aggregate(const Expr& e) {
  switch (e.kind) {
    case Expr::Kind::kAggregate: return true;
    case Expr::Kind::kUnary: return contains_aggregate(*e.lhs);
    case Expr::Kind::kBinary: return contains_aggregate(*e.lhs) || contains_aggregate(*e.rhs);
    default: return false;
  }
}

bool references_columns(const Expr& e) {
  switch (e.kind) {
    case Expr::Kind::kColumn: return true;
    case Expr::Kind::kUnary: return references_columns(*e.lhs);
    case Expr::Kind::kBinary: return references_columns(*e.lhs) || references_columns(*e.rhs);
    case Expr::Kind::kAggregate: return true;
    default: return false;
  }
}

void collect_aggregates(const Expr& e, std::vector<const Expr*>& out) {
  switch (e.kind) {
    case Expr::Kind::kAggregate: out.push_back(&e); break;
    case Expr::Kind::kUnary: collect_aggregates(*e.lhs, out); break;
    case Expr::Kind::kBinary:
      collect_aggregates(*e.lhs, out);
      collect_aggregates(*e.rhs, out);
      break;
    default: break;
  }
}

bool is_true(const Value& v) { return !is_null(v) && truthy_non_null(v); }

Value eval(const Expr& e, const Row* row, const AggValues* aggs) {
  switch (e.kind) {
    case Expr::Kind::kLiteral:
      return e.literal;
    case Expr::Kind::kColumn:
      if (row == nullptr) throw SqlError("column " + e.name + " is not allowed here");
      return (*row)[static_cast<std::size_t>(e.column)];
    case Expr::Kind::kAggregate: {
      if (aggs == nullptr) throw SqlError("aggregate " + e.text + " is not allowed here");
      return aggs->at(&e);
    }
    case Expr::Kind::kUnary: {
      Value v = eval(*e.lhs, row, aggs);
      switch (e.op) {
        case Op::kNeg:
          if (is_null(v)) return v;
          if (auto* i = std::get_if<std::int64_t>(&v)) {
            if (*i == std::numeric_limits<std::int64_t>::min()) return -static_cast<double>(*i);
            return -*i;
          }
          if (auto* d = std::get_if<double>(&v)) return -*d;
          throw SqlError("cannot negate TEXT");
        case Op::kNot:
          if (is_null(v)) return v;
          return from_bool(!truthy_non_null(v));
        case Op::kIsNull: return from_bool(is_null(v));
        case Op::kIsNotNull: return from_bool(!is_null(v));
        default: throw SqlError("internal: bad unary operator");
      }
    }
    case Expr::Kind::kBinary: {
      if (e.op == Op::kAnd || e.op == Op::kOr) {
        // Three-valued logic with short-circuit on the dominating value.
        Value l = eval(*e.lhs, row, aggs);
        bool l_null = is_null(l);
        bool l_true = !l_null && truthy_non_null(l);
        if (e.op == Op::kAnd && !l_null && !l_true) return from_bool(false);
        if (e.op == Op::kOr && l_true) return from_bool(true);
        Value r = eval(*e.rhs, row, aggs);
        bool r_null = is_null(r);
        bool r_true = !r_null && truthy_non_null(r);
        if (e.op == Op::kAnd) {
          if (!r_null && !r_true) return from_bool(false);
          return (l_null || r_null) ? Value{} : from_bool(true);
        }
        if (r_true) return from_bool(true);
        return (l_null || r_null) ? Value{} : from_bool(false);
      }
      Value l = eval(*e.lhs, row, aggs);
      Value r = eval(*e.rhs, row, aggs);
      switch (e.op) {
        case Op::kAdd: case Op::kSub: case Op::kMul: case Op::kDiv: case Op::kMod:
          return arith(e.op, l, r);
        default:
          return cmp(e.op, l, r);
      }
    }
  }
  throw SqlError("internal: bad expression");
}

std::optional<Value> fold_constant(const Expr& e) {
  if (references_columns(e)) return std::nullopt;
  return eval(e, nullptr, nullptr);
}

}  // namespace aqo::sql
