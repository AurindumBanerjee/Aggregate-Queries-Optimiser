#include "aqo/sql/executor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <sstream>

#include "aqo/sql/expr.hpp"

namespace aqo::sql {
namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ------------------------------------------------------------------------------------------
// WHERE analysis
// ------------------------------------------------------------------------------------------

struct WhereInfo {
  bool always_false = false;
  std::map<std::size_t, ValueRange> ranges;  // column -> interval implied by the range conjuncts
  std::size_t residual = 0;                  // conjuncts that are not simple column/constant comparisons
};

void flatten_and(const ExprPtr& e, std::vector<ExprPtr>& out) {
  if (e->kind == Expr::Kind::kBinary && e->op == Op::kAnd) {
    flatten_and(e->lhs, out);
    flatten_and(e->rhs, out);
  } else {
    out.push_back(e);
  }
}

bool is_cmp(Op op) { return op == Op::kEq || op == Op::kLt || op == Op::kLe || op == Op::kGt || op == Op::kGe; }
Op flip(Op op) {
  switch (op) {
    case Op::kLt: return Op::kGt;
    case Op::kLe: return Op::kGe;
    case Op::kGt: return Op::kLt;
    case Op::kGe: return Op::kLe;
    default: return op;
  }
}

WhereInfo analyze_where(const ExprPtr& where, const Schema& schema) {
  WhereInfo w;
  if (!where) return w;
  std::vector<ExprPtr> conj;
  flatten_and(where, conj);
  for (auto& c : conj) {
    if (auto folded = fold_constant(*c)) {
      if (!is_true(*folded)) w.always_false = true;
      continue;
    }
    if (c->kind == Expr::Kind::kBinary && is_cmp(c->op)) {
      const Expr* col = nullptr;
      std::optional<Value> lit;
      Op op = c->op;
      if (c->lhs->kind == Expr::Kind::kColumn && (lit = fold_constant(*c->rhs))) {
        col = c->lhs.get();
      } else if (c->rhs->kind == Expr::Kind::kColumn && (lit = fold_constant(*c->lhs))) {
        col = c->rhs.get();
        op = flip(op);
      }
      if (col != nullptr) {
        if (is_null(*lit)) {
          w.always_false = true;  // comparison with NULL is never true
          continue;
        }
        auto idx = static_cast<std::size_t>(col->column);
        if (schema.cols[idx].type != ColType::kText && is_numeric(*lit)) {
          ValueRange& r = w.ranges[idx];
          switch (op) {
            case Op::kEq: r.restrict_lo(*lit, true); r.restrict_hi(*lit, true); break;
            case Op::kLt: r.restrict_hi(*lit, false); break;
            case Op::kLe: r.restrict_hi(*lit, true); break;
            case Op::kGt: r.restrict_lo(*lit, false); break;
            default: r.restrict_lo(*lit, true); break;
          }
          continue;
        }
      }
    }
    ++w.residual;
  }
  return w;
}

std::string range_text(const ValueRange& r) {
  std::ostringstream o;
  o << (r.lo && r.lo_incl ? '[' : '(') << (r.lo ? to_string(*r.lo) : "-inf") << ", " << (r.hi ? to_string(*r.hi) : "+inf")
    << (r.hi && r.hi_incl ? ']' : ')');
  return o.str();
}

KeyRange<std::int64_t> pk_range(const WhereInfo& w, bool& empty) {
  KeyRange<std::int64_t> kr;
  empty = false;
  auto it = w.ranges.find(0);
  if (it == w.ranges.end()) return kr;
  auto iv = int_interval(it->second);
  if (!iv) {
    empty = true;
    return kr;
  }
  if (it->second.lo) kr.lo = iv->first;
  if (it->second.hi) kr.hi = iv->second;
  return kr;
}

// ------------------------------------------------------------------------------------------
// Aggregation state
// ------------------------------------------------------------------------------------------

struct AggAcc {
  AggFn fn = AggFn::kCount;
  bool star = false;
  std::int64_t count = 0;
  int128 isum = 0;
  double dsum = 0;
  bool saw_real = false;
  Value best;  // MIN / MAX

  void add(const Value& v) {
    if (star) {
      ++count;
      return;
    }
    if (is_null(v)) return;
    ++count;
    switch (fn) {
      case AggFn::kCount: break;
      case AggFn::kSum:
      case AggFn::kAvg:
        if (auto* i = std::get_if<std::int64_t>(&v)) isum += *i;
        else if (auto* d = std::get_if<double>(&v)) { dsum += *d; saw_real = true; }
        else throw SqlError("SUM/AVG of TEXT is not supported");
        break;
      case AggFn::kMin:
        if (is_null(best) || compare(v, best) < 0) best = v;
        break;
      case AggFn::kMax:
        if (is_null(best) || compare(v, best) > 0) best = v;
        break;
    }
  }

  static Value int_result(int128 v) {
    if (v > std::numeric_limits<std::int64_t>::max() || v < std::numeric_limits<std::int64_t>::min())
      throw SqlError("integer overflow in SUM");
    return static_cast<std::int64_t>(v);
  }

  Value finish() const {
    switch (fn) {
      case AggFn::kCount: return count;
      case AggFn::kSum:
        if (count == 0) return Value{};
        return saw_real ? Value{static_cast<double>(isum) + dsum} : int_result(isum);
      case AggFn::kAvg:
        if (count == 0) return Value{};
        return (static_cast<double>(isum) + dsum) / static_cast<double>(count);
      default: return best;
    }
  }
};

bool row_less(const Row& a, const Row& b) {
  for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
    int c = compare(a[i], b[i]);
    if (c != 0) return c < 0;
  }
  return a.size() < b.size();
}
struct RowLess {
  bool operator()(const Row& a, const Row& b) const { return row_less(a, b); }
};

// Columns referenced outside of aggregate arguments.
void bare_columns(const Expr& e, std::vector<int>& out) {
  switch (e.kind) {
    case Expr::Kind::kColumn: out.push_back(e.column); break;
    case Expr::Kind::kUnary: bare_columns(*e.lhs, out); break;
    case Expr::Kind::kBinary:
      bare_columns(*e.lhs, out);
      bare_columns(*e.rhs, out);
      break;
    default: break;
  }
}

struct OutItem {
  ExprPtr expr;
  std::string header;
};

}  // namespace

// ------------------------------------------------------------------------------------------
// Session
// ------------------------------------------------------------------------------------------

Session::Session(Database& db, SessionOptions opts) : db_(db), opts_(opts) {}

Session::~Session() {
  if (txn_) {
    try {
      txn_->abort();
    } catch (...) {
    }
    txn_.reset();
  }
  if (txn_lock_.owns_lock()) txn_lock_.unlock();
}

ResultSet Session::execute(const std::string& sql) {
  auto ps = parse_statement(sql);
  return run(ps);
}

std::vector<ResultSet> Session::execute_script(const std::string& sql) {
  std::vector<ResultSet> out;
  for (auto& ps : parse_script(sql)) out.push_back(run(ps));
  return out;
}

ResultSet Session::run(ParsedStatement& ps) {
  auto t0 = std::chrono::steady_clock::now();
  ResultSet rs;
  if (auto* tc = std::get_if<TxnControl>(&ps.stmt)) {
    switch (tc->kind) {
      case TxnControl::kBegin: {
        if (txn_) throw SqlError("a transaction is already open");
        std::unique_lock<std::shared_mutex> lk(db_.rw_mutex());  // blocks until all readers/writers finish
        txn_ = db_.engine().begin();
        txn_lock_ = std::move(lk);
        rs.message = "BEGIN";
        break;
      }
      case TxnControl::kCommit:
        if (!txn_) throw SqlError("no transaction is open");
        try {
          txn_->commit();
        } catch (...) {
          txn_.reset();
          txn_lock_.unlock();
          throw;
        }
        txn_.reset();
        txn_lock_.unlock();
        rs.message = "COMMIT";
        break;
      case TxnControl::kRollback:
        if (!txn_) throw SqlError("no transaction is open");
        txn_->abort();
        txn_.reset();
        txn_lock_.unlock();
        rs.message = "ROLLBACK";
        break;
    }
  } else if (std::holds_alternative<Select>(ps.stmt) || ps.explain) {
    // Reads need no write transaction; inside our own transaction we already hold the exclusive lock.
    std::shared_lock<std::shared_mutex> rd;
    if (!txn_) rd = std::shared_lock<std::shared_mutex>(db_.rw_mutex());
    rs = dispatch(ps);
  } else if (txn_) {
    try {
      rs = dispatch(ps);
    } catch (const std::exception& e) {
      txn_->abort();
      txn_.reset();
      txn_lock_.unlock();
      throw SqlError(std::string(e.what()) + " (transaction rolled back)");
    }
  } else {
    std::unique_lock<std::shared_mutex> wr(db_.rw_mutex());
    Txn tx = db_.engine().begin();
    try {
      rs = dispatch(ps);
      tx.commit();
    } catch (...) {
      tx.abort();
      throw;
    }
  }
  rs.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return rs;
}

ResultSet Session::dispatch(ParsedStatement& ps) {
  ResultSet rs;
  return std::visit(
      [&](auto& st) -> ResultSet {
        using T = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<T, CreateTable>) {
          if (ps.explain) {
            rs.message = "CREATE TABLE " + st.name;
            return rs;
          }
          db_.create_table(st.name, st.cols);
          rs.message = "CREATE TABLE";
          return rs;
        } else if constexpr (std::is_same_v<T, CreateIndex>) {
          if (!ps.explain) db_.create_index(st.table, st.column);
          rs.message = "CREATE INDEX";
          return rs;
        } else if constexpr (std::is_same_v<T, Insert>) {
          if (ps.explain) {
            rs.message = "INSERT INTO " + st.table;
            return rs;
          }
          return do_insert(st);
        } else if constexpr (std::is_same_v<T, Select>) {
          return do_select(st, ps.explain);
        } else if constexpr (std::is_same_v<T, Update>) {
          return do_update(st, ps.explain);
        } else if constexpr (std::is_same_v<T, Delete>) {
          return do_delete(st, ps.explain);
        } else {
          return rs;
        }
      },
      ps.stmt);
}

// ------------------------------------------------------------------------------------------
// INSERT
// ------------------------------------------------------------------------------------------

ResultSet Session::do_insert(Insert& s) {
  Table* t = db_.find_table(s.table);
  if (t == nullptr) throw SqlError("no such table: " + s.table);
  const Schema& schema = t->schema();
  std::vector<std::size_t> target;
  if (s.columns.empty()) {
    for (std::size_t i = 0; i < schema.cols.size(); ++i) target.push_back(i);
  } else {
    for (auto& c : s.columns) {
      auto idx = schema.find(c);
      if (!idx) throw SqlError("no such column: " + c);
      target.push_back(*idx);
    }
    if (target.size() != schema.cols.size())
      throw SqlError("INSERT must supply every column (columns are NOT NULL and have no defaults)");
  }
  ResultSet rs;
  for (auto& vals : s.rows) {
    if (vals.size() != target.size()) throw SqlError("INSERT has " + std::to_string(vals.size()) + " values for " + std::to_string(target.size()) + " columns");
    Row row(schema.cols.size());
    for (std::size_t i = 0; i < vals.size(); ++i) {
      auto v = fold_constant(*vals[i]);
      if (!v) throw SqlError("INSERT values must be constants");
      row[target[i]] = *v;
    }
    if (!t->insert(row)) throw SqlError("UNIQUE constraint failed: " + schema.table + "." + schema.cols[0].name);
    ++rs.affected;
  }
  rs.message = "INSERT " + std::to_string(rs.affected);
  return rs;
}

// ------------------------------------------------------------------------------------------
// UPDATE / DELETE
// ------------------------------------------------------------------------------------------

ResultSet Session::do_update(Update& s, bool explain_only) {
  Table* t = db_.find_table(s.table);
  if (t == nullptr) throw SqlError("no such table: " + s.table);
  const Schema& schema = t->schema();
  std::vector<std::pair<std::size_t, ExprPtr>> sets;
  for (auto& [name, e] : s.sets) {
    auto idx = schema.find(name);
    if (!idx) throw SqlError("no such column: " + name);
    if (*idx == 0) throw SqlError("UPDATE cannot change the primary key");
    if (contains_aggregate(*e)) throw SqlError("aggregates are not allowed in UPDATE");
    bind(*e, schema);
    sets.emplace_back(*idx, e);
  }
  if (s.where) {
    bind(*s.where, schema);
    if (contains_aggregate(*s.where)) throw SqlError("aggregates are not allowed in WHERE");
  }
  WhereInfo w = analyze_where(s.where, schema);
  bool empty = false;
  auto kr = pk_range(w, empty);
  ResultSet rs;
  rs.plan = w.always_false || empty ? "EMPTY" : "SCAN " + s.table + " pk" + (w.ranges.count(0) ? range_text(w.ranges.at(0)) : "[all]") +
                                                    (s.where ? " FILTER " + s.where->text : "");
  if (explain_only) {
    rs.columns = {"plan"};
    rs.rows.push_back({rs.plan});
    return rs;
  }
  if (w.always_false || empty) return rs;
  std::vector<Row> updated;
  t->scan(kr, [&](const Row& old) {
    if (s.where && !is_true(eval(*s.where, &old))) return true;
    Row nr = old;
    for (auto& [col, e] : sets) nr[col] = eval(*e, &old);
    updated.push_back(std::move(nr));
    return true;
  });
  for (auto& r : updated) {
    t->update(std::get<std::int64_t>(r[0]), r);
    ++rs.affected;
  }
  rs.message = "UPDATE " + std::to_string(rs.affected);
  return rs;
}

ResultSet Session::do_delete(Delete& s, bool explain_only) {
  Table* t = db_.find_table(s.table);
  if (t == nullptr) throw SqlError("no such table: " + s.table);
  const Schema& schema = t->schema();
  if (s.where) {
    bind(*s.where, schema);
    if (contains_aggregate(*s.where)) throw SqlError("aggregates are not allowed in WHERE");
  }
  WhereInfo w = analyze_where(s.where, schema);
  bool empty = false;
  auto kr = pk_range(w, empty);
  ResultSet rs;
  rs.plan = w.always_false || empty ? "EMPTY" : "SCAN " + s.table + " pk" + (w.ranges.count(0) ? range_text(w.ranges.at(0)) : "[all]") +
                                                    (s.where ? " FILTER " + s.where->text : "");
  if (explain_only) {
    rs.columns = {"plan"};
    rs.rows.push_back({rs.plan});
    return rs;
  }
  if (w.always_false || empty) return rs;
  std::vector<std::int64_t> victims;
  t->scan(kr, [&](const Row& r) {
    if (!s.where || is_true(eval(*s.where, &r))) victims.push_back(std::get<std::int64_t>(r[0]));
    return true;
  });
  for (auto pk : victims) {
    t->erase(pk);
    ++rs.affected;
  }
  rs.message = "DELETE " + std::to_string(rs.affected);
  return rs;
}

// ------------------------------------------------------------------------------------------
// SELECT
// ------------------------------------------------------------------------------------------

ResultSet Session::do_select(Select& s, bool explain_only) {
  Table* t = db_.find_table(s.table);
  if (t == nullptr) throw SqlError("no such table: " + s.table);
  const Schema& schema = t->schema();

  // ---- bind ----
  std::vector<OutItem> items;
  for (auto& it : s.items) {
    if (!it.expr) {
      for (std::size_t i = 0; i < schema.cols.size(); ++i) {
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Kind::kColumn;
        e->name = schema.cols[i].name;
        e->column = static_cast<int>(i);
        e->text = e->name;
        items.push_back({e, e->name});
      }
    } else {
      bind(*it.expr, schema);
      std::string header = !it.alias.empty() ? it.alias : (it.expr->kind == Expr::Kind::kColumn ? it.expr->name : it.expr->text);
      items.push_back({it.expr, header});
    }
  }
  if (s.where) {
    bind(*s.where, schema);
    if (contains_aggregate(*s.where)) throw SqlError("aggregates are not allowed in WHERE");
  }
  for (auto& g : s.group_by) {
    bind(*g, schema);
    if (contains_aggregate(*g)) throw SqlError("aggregates are not allowed in GROUP BY");
  }
  for (auto& o : s.order_by) {
    if (!o.expr) continue;
    try {
      bind(*o.expr, schema);
    } catch (const SqlError&) {
      // A bare name may refer to a SELECT alias instead of a column; resolved against the output below.
      bool alias = false;
      for (auto& it : items) alias = alias || (o.expr->kind == Expr::Kind::kColumn && lower(it.header) == o.expr->name);
      if (!alias) throw;
    }
  }

  bool grouped = !s.group_by.empty();
  bool is_agg = grouped;
  for (auto& it : items) is_agg = is_agg || contains_aggregate(*it.expr);

  // Non-aggregated expressions in an aggregate query must be built from GROUP BY columns.
  if (is_agg) {
    std::vector<int> group_cols;
    for (auto& g : s.group_by)
      if (g->kind == Expr::Kind::kColumn) group_cols.push_back(g->column);
    auto allowed = [&](const Expr& e) {
      for (auto& g : s.group_by)
        if (lower(g->text) == lower(e.text) && !e.text.empty()) return true;
      std::vector<int> cols;
      bare_columns(e, cols);
      for (int c : cols)
        if (std::find(group_cols.begin(), group_cols.end(), c) == group_cols.end()) return false;
      return true;
    };
    for (auto& it : items)
      if (!allowed(*it.expr)) throw SqlError("column in SELECT must appear in GROUP BY or inside an aggregate: " + it.header);
  }

  WhereInfo w = analyze_where(s.where, schema);
  bool pk_empty = false;
  KeyRange<std::int64_t> kr = pk_range(w, pk_empty);

  ResultSet rs;
  for (auto& it : items) rs.columns.push_back(it.header);

  // ---- ORDER BY resolution: either an output column or an extra expression ----
  struct SortKey {
    int out_col = -1;
    ExprPtr expr;
    bool desc = false;
  };
  std::vector<SortKey> sort_keys;
  for (auto& o : s.order_by) {
    SortKey k;
    k.desc = o.desc;
    if (o.ordinal) {
      if (*o.ordinal < 1 || static_cast<std::size_t>(*o.ordinal) > items.size()) throw SqlError("ORDER BY position out of range");
      k.out_col = *o.ordinal - 1;
    } else {
      for (std::size_t i = 0; i < items.size() && k.out_col < 0; ++i) {
        if (o.expr->kind == Expr::Kind::kColumn && lower(items[i].header) == o.expr->name) k.out_col = static_cast<int>(i);
        else if (lower(items[i].expr->text) == lower(o.expr->text)) k.out_col = static_cast<int>(i);
      }
      if (k.out_col < 0) k.expr = o.expr;
    }
    sort_keys.push_back(std::move(k));
  }
  // Rows already come out in primary-key order, so ORDER BY pk ASC needs no sort.
  bool natural_order = sort_keys.empty();
  if (sort_keys.size() == 1 && !sort_keys[0].desc && !is_agg) {
    const Expr* e = sort_keys[0].expr ? sort_keys[0].expr.get() : (sort_keys[0].out_col >= 0 ? items[static_cast<std::size_t>(sort_keys[0].out_col)].expr.get() : nullptr);
    natural_order = e != nullptr && e->kind == Expr::Kind::kColumn && e->column == 0;
  }

  // ---- access-path choice for aggregate-only queries ----
  bool agg_only = is_agg && !grouped;
  std::vector<const Expr*> agg_nodes;
  for (auto& it : items) collect_aggregates(*it.expr, agg_nodes);
  if (agg_only) {
    for (auto& it : items) {
      std::vector<int> cols;
      bare_columns(*it.expr, cols);
      if (!cols.empty()) throw SqlError("column in SELECT must appear in GROUP BY or inside an aggregate: " + it.header);
    }
  }
  const AggIndex* use_index = nullptr;
  std::optional<ValueRange> index_range;
  if (agg_only && opts_.use_aggregate_index && !w.always_false && w.residual == 0 && w.ranges.size() <= 1) {
    bool shapes_ok = true;
    for (auto* a : agg_nodes) {
      if (a->star) continue;
      if (a->arg->kind != Expr::Kind::kColumn) shapes_ok = false;
      else if (schema.cols[static_cast<std::size_t>(a->arg->column)].type == ColType::kText && a->fn != AggFn::kCount) shapes_ok = false;
    }
    if (shapes_ok) {
      if (w.ranges.size() == 1) {
        use_index = t->index(w.ranges.begin()->first);
        if (use_index != nullptr) index_range = w.ranges.begin()->second;
      } else if (!schema.indexed.empty()) {
        use_index = t->index(schema.indexed.front());
      }
    }
  }

  // ---- describe the plan ----
  std::string where_text = s.where ? s.where->text : "";
  if (w.always_false || pk_empty) {
    rs.plan = "EMPTY (WHERE is unsatisfiable)";
  } else if (use_index != nullptr) {
    rs.plan = "INDEX AGGREGATE " + s.table + " USING index(" + schema.cols[use_index->filter_column()].name + ")" +
              (index_range ? " RANGE " + range_text(*index_range) : " ALL ROWS") + " [O(log n)]";
  } else {
    rs.plan = "SCAN " + s.table + " pk" + (w.ranges.count(0) ? range_text(w.ranges.at(0)) : "[all]");
    if (!where_text.empty()) rs.plan += " FILTER " + where_text;
    if (grouped) rs.plan += " -> HASH GROUP BY";
    else if (is_agg) rs.plan += " -> AGGREGATE";
    if (!natural_order) rs.plan += " -> SORT";
  }
  if (explain_only) {
    ResultSet ex;
    ex.columns = {"plan"};
    ex.rows.push_back({rs.plan});
    ex.plan = rs.plan;
    return ex;
  }

  // ---- shared pieces ----
  auto make_acc = [](const Expr* a) {
    AggAcc acc;
    acc.fn = a->fn;
    acc.star = a->star;
    return acc;
  };

  // Output rows paired with their sort keys.
  std::vector<std::pair<Row, Row>> produced;
  auto emit = [&](const Row* src, const AggValues* aggs) {
    Row out;
    out.reserve(items.size());
    for (auto& it : items) out.push_back(eval(*it.expr, src, aggs));
    Row keys;
    for (auto& k : sort_keys) keys.push_back(k.out_col >= 0 ? out[static_cast<std::size_t>(k.out_col)] : eval(*k.expr, src, aggs));
    produced.emplace_back(std::move(out), std::move(keys));
  };

  if (w.always_false || pk_empty) {
    if (agg_only) {
      AggValues av;
      for (auto* a : agg_nodes) av[a] = make_acc(a).finish();
      emit(nullptr, &av);
    }
  } else if (use_index != nullptr) {
    RowStats st = index_range ? use_index->query(*index_range) : use_index->query_all();
    AggValues av;
    for (auto* a : agg_nodes) {
      Value v;
      if (a->fn == AggFn::kCount) {
        v = st.count;
      } else if (st.count == 0) {
        v = Value{};
      } else {
        std::size_t col = static_cast<std::size_t>(a->arg->column);
        const SlotStats& ss = st.slots[*schema.slot_of(col)];
        bool real = schema.cols[col].type == ColType::kReal;
        switch (a->fn) {
          case AggFn::kSum: v = real ? Value{ss.dsum} : AggAcc::int_result(ss.isum); break;
          case AggFn::kAvg: v = (real ? ss.dsum : static_cast<double>(ss.isum)) / static_cast<double>(st.count); break;
          case AggFn::kMin: v = real ? Value{ss.dmin} : Value{ss.imin}; break;
          default: v = real ? Value{ss.dmax} : Value{ss.imax}; break;
        }
      }
      av[a] = v;
    }
    emit(nullptr, &av);
  } else if (!is_agg) {
    std::int64_t limit = s.limit.value_or(-1);
    bool stream_limit = natural_order && limit >= 0;
    if (limit == 0) {
      // nothing to do
    } else {
      t->scan(kr, [&](const Row& r) {
        if (s.where && !is_true(eval(*s.where, &r))) return true;
        emit(&r, nullptr);
        return !(stream_limit && static_cast<std::int64_t>(produced.size()) >= limit);
      });
    }
  } else if (agg_only) {
    std::vector<AggAcc> accs;
    for (auto* a : agg_nodes) accs.push_back(make_acc(a));
    t->scan(kr, [&](const Row& r) {
      if (s.where && !is_true(eval(*s.where, &r))) return true;
      for (std::size_t i = 0; i < accs.size(); ++i)
        accs[i].add(agg_nodes[i]->star ? Value{} : eval(*agg_nodes[i]->arg, &r));
      return true;
    });
    AggValues av;
    for (std::size_t i = 0; i < accs.size(); ++i) av[agg_nodes[i]] = accs[i].finish();
    emit(nullptr, &av);
  } else {  // GROUP BY
    struct Group {
      Row rep;
      std::vector<AggAcc> accs;
    };
    std::map<Row, Group, RowLess> groups;
    t->scan(kr, [&](const Row& r) {
      if (s.where && !is_true(eval(*s.where, &r))) return true;
      Row key;
      for (auto& g : s.group_by) key.push_back(eval(*g, &r));
      auto [it, fresh] = groups.try_emplace(std::move(key));
      if (fresh) {
        it->second.rep = r;
        for (auto* a : agg_nodes) it->second.accs.push_back(make_acc(a));
      }
      for (std::size_t i = 0; i < agg_nodes.size(); ++i)
        it->second.accs[i].add(agg_nodes[i]->star ? Value{} : eval(*agg_nodes[i]->arg, &r));
      return true;
    });
    for (auto& [key, g] : groups) {
      AggValues av;
      for (std::size_t i = 0; i < agg_nodes.size(); ++i) av[agg_nodes[i]] = g.accs[i].finish();
      emit(&g.rep, &av);
    }
  }

  // ---- ORDER BY / LIMIT ----
  if (!natural_order) {
    std::stable_sort(produced.begin(), produced.end(), [&](const auto& a, const auto& b) {
      for (std::size_t i = 0; i < sort_keys.size(); ++i) {
        int c = compare(a.second[i], b.second[i]);
        if (c != 0) return sort_keys[i].desc ? c > 0 : c < 0;
      }
      return false;
    });
  }
  std::size_t n = produced.size();
  if (s.limit && *s.limit >= 0) n = std::min<std::size_t>(n, static_cast<std::size_t>(*s.limit));
  rs.rows.reserve(n);
  for (std::size_t i = 0; i < n; ++i) rs.rows.push_back(std::move(produced[i].first));
  return rs;
}

}  // namespace aqo::sql
