#include "aqo/database.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <random>

#include "test_util.hpp"

namespace aqo {
namespace {

struct TempDb {
  std::string path;
  explicit TempDb(const std::string& n) : path(aqo::testing::unique_path("tbl_" + n, ".db")) { cleanup(); }
  ~TempDb() { cleanup(); }
  void cleanup() const {
    for (auto* ext : {"", ".wal", ".wal.tmp"}) std::filesystem::remove(path + ext);
  }
};
EngineOptions opts(const TempDb& d, std::size_t frames = 256) {
  EngineOptions o;
  o.path = d.path;
  o.pool_frames = frames;
  o.sync_commit = false;
  return o;
}

std::vector<Column> test_cols() {
  return {{"id", ColType::kInt}, {"a", ColType::kInt}, {"b", ColType::kReal}, {"name", ColType::kText}, {"c", ColType::kInt}};
}
constexpr std::size_t kId = 0, kA = 1, kB = 2, kC = 4;

Row make_row(std::int64_t id, std::int64_t a, double b, std::string name, std::int64_t c) {
  return {id, a, b, std::move(name), c};
}

// ---- brute-force oracle for aggregates ----
struct Agg {
  std::int64_t count = 0;
  double sum = 0;
  Value min, max;  // monostate when empty
};

bool in_range(const Value& v, const ValueRange& r) {
  if (r.empty) return false;
  if (r.lo) {
    int c = compare(v, *r.lo);
    if (c < 0 || (c == 0 && !r.lo_incl)) return false;
  }
  if (r.hi) {
    int c = compare(v, *r.hi);
    if (c > 0 || (c == 0 && !r.hi_incl)) return false;
  }
  return true;
}

Agg brute(const std::map<std::int64_t, Row>& rows, std::size_t filter, const ValueRange& r, std::size_t target) {
  Agg a;
  for (auto& [pk, row] : rows) {
    if (!in_range(row[filter], r)) continue;
    ++a.count;
    a.sum += as_double(row[target]);
    if (is_null(a.min) || compare(row[target], a.min) < 0) a.min = row[target];
    if (is_null(a.max) || compare(row[target], a.max) > 0) a.max = row[target];
  }
  return a;
}

void expect_matches(const Table& t, const std::map<std::int64_t, Row>& rows, std::size_t filter, const ValueRange& r,
                    const std::string& ctx) {
  const AggIndex* idx = t.index(filter);
  ASSERT_NE(idx, nullptr);
  RowStats st = idx->query(r);
  for (std::size_t target : {kId, kA, kB, kC}) {
    Agg want = brute(rows, filter, r, target);
    ASSERT_EQ(st.count, want.count) << ctx;
    if (want.count == 0) continue;
    auto slot = *t.schema().slot_of(target);
    const SlotStats& s = st.slots[slot];
    if (t.schema().cols[target].type == ColType::kInt) {
      ASSERT_EQ(static_cast<double>(s.isum), want.sum) << ctx << " col " << target;
      ASSERT_EQ(s.imin, std::get<std::int64_t>(want.min)) << ctx;
      ASSERT_EQ(s.imax, std::get<std::int64_t>(want.max)) << ctx;
    } else {
      ASSERT_NEAR(s.dsum, want.sum, 1e-6 * (1 + std::fabs(want.sum))) << ctx;
      ASSERT_EQ(s.dmin, std::get<double>(want.min)) << ctx;
      ASSERT_EQ(s.dmax, std::get<double>(want.max)) << ctx;
    }
  }
}

ValueRange random_range(std::mt19937_64& rng, ColType type, std::int64_t span) {
  ValueRange r;
  auto lit = [&]() -> Value {
    std::int64_t base = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(2 * span)) - span;
    if (type == ColType::kReal) return static_cast<double>(base) / 4.0;
    if (rng() % 3 == 0) return static_cast<double>(base) + 0.5;  // fractional literal against an INT column
    return base;
  };
  if (rng() % 4 != 0) r.restrict_lo(lit(), rng() & 1);
  if (rng() % 4 != 0) r.restrict_hi(lit(), rng() & 1);
  return r;
}

TEST(Table, CrudAndTypeChecks) {
  TempDb d("crud");
  Database db(opts(d));
  Txn tx = db.engine().begin();
  Table& t = db.create_table("t", test_cols());
  EXPECT_TRUE(t.insert(make_row(1, 10, 1.5, "x", 7)));
  EXPECT_FALSE(t.insert(make_row(1, 11, 2.5, "y", 8))) << "duplicate pk";
  EXPECT_THROW(t.insert({std::int64_t{2}, std::int64_t{1}}), SqlError) << "arity";
  Row bad = make_row(2, 1, 1.0, "x", 1);
  bad[kA] = std::string("not an int");
  EXPECT_THROW(t.insert(bad), SqlError);
  Row widened = make_row(3, 1, 0.0, "w", 1);
  widened[kB] = std::int64_t{4};  // INT literal into REAL column widens
  EXPECT_TRUE(t.insert(widened));
  EXPECT_EQ(std::get<double>(t.get(3)->at(kB)), 4.0);
  EXPECT_THROW(t.insert(make_row(4, 1, std::nan(""), "n", 1)), SqlError);
  EXPECT_TRUE(t.update(1, make_row(1, 99, 9.5, "upd", 7)));
  EXPECT_EQ(std::get<std::int64_t>(t.get(1)->at(kA)), 99);
  EXPECT_THROW(t.update(1, make_row(5, 99, 9.5, "upd", 7)), SqlError) << "pk change";
  EXPECT_FALSE(t.update(77, make_row(77, 1, 1.0, "z", 1)));
  EXPECT_TRUE(t.erase(1));
  EXPECT_FALSE(t.erase(1));
  EXPECT_EQ(t.count(), 1u);
  tx.commit();
}

TEST(Table, DdlRequiresTransactionAndRollsBack) {
  TempDb d("ddl");
  Database db(opts(d));
  EXPECT_THROW(db.create_table("t", test_cols()), StorageError);
  {
    Txn tx = db.engine().begin();
    db.create_table("t", test_cols());
    EXPECT_NE(db.find_table("t"), nullptr);
    tx.abort();
  }
  EXPECT_EQ(db.find_table("t"), nullptr) << "aborted CREATE TABLE leaves no trace";
  {
    Txn tx = db.engine().begin();
    db.create_table("t", test_cols());
    tx.commit();
  }
  {
    Txn tx = db.engine().begin();
    EXPECT_THROW(db.create_table("t", test_cols()), SqlError);
    db.create_index("t", "a");
    EXPECT_NE(db.find_table("t")->index(kA), nullptr);
    tx.abort();
  }
  EXPECT_EQ(db.find_table("t")->index(kA), nullptr) << "aborted CREATE INDEX is undone";
  Txn tx = db.engine().begin();
  EXPECT_THROW(db.create_index("t", "name"), SqlError) << "TEXT columns cannot be indexed";
  EXPECT_THROW(db.create_index("t", "nope"), SqlError);
  EXPECT_THROW(db.create_index("missing", "a"), SqlError);
  tx.commit();
}

TEST(AggIndex, RangeEdgeCases) {
  Schema s;
  s.table = "e";
  s.cols = {{"id", ColType::kInt}, {"v", ColType::kInt}, {"r", ColType::kReal}};
  AggIndex iv(s, 1), ir(s, 2);
  std::vector<std::int64_t> vals = {std::numeric_limits<std::int64_t>::min(), -5, 0, 3, 3, 7, std::numeric_limits<std::int64_t>::max()};
  std::vector<double> reals = {-INFINITY, -2.5, -0.0, 0.0, 1.5, 1.5, INFINITY};
  for (std::size_t i = 0; i < vals.size(); ++i) {
    Row r = {static_cast<std::int64_t>(i), vals[i], reals[i]};
    iv.add(r);
    ir.add(r);
  }
  auto cnt = [](const AggIndex& i, std::optional<Value> lo, bool li, std::optional<Value> hi, bool hii) {
    ValueRange r;
    if (lo) r.restrict_lo(*lo, li);
    if (hi) r.restrict_hi(*hi, hii);
    return i.query(r).count;
  };
  EXPECT_EQ(cnt(iv, {}, true, {}, true), 7);
  EXPECT_EQ(cnt(iv, Value{std::int64_t{3}}, true, Value{std::int64_t{3}}, true), 2);
  EXPECT_EQ(cnt(iv, Value{std::int64_t{3}}, false, {}, true), 2) << "> 3 : 7 and INT64_MAX";
  EXPECT_EQ(cnt(iv, Value{2.5}, true, Value{7.0}, false), 2) << "[2.5, 7) -> 3,3";
  EXPECT_EQ(cnt(iv, Value{2.5}, false, Value{3.5}, true), 2);
  EXPECT_EQ(cnt(iv, Value{3.0}, false, Value{3.0}, false), 0);
  EXPECT_EQ(cnt(iv, {}, true, Value{std::numeric_limits<std::int64_t>::min()}, false), 0);
  EXPECT_EQ(cnt(iv, Value{std::numeric_limits<std::int64_t>::max()}, false, {}, true), 0);
  EXPECT_EQ(cnt(iv, Value{1e30}, true, {}, true), 0);
  EXPECT_EQ(cnt(iv, Value{-1e30}, true, {}, true), 7);
  EXPECT_EQ(cnt(iv, Value{std::string("text")}, true, {}, true), 0) << "non-numeric literal never matches";
  EXPECT_EQ(cnt(ir, Value{0.0}, true, Value{0.0}, true), 2) << "-0.0 and 0.0 are equal";
  EXPECT_EQ(cnt(ir, Value{-1e308}, true, {}, true), 6) << "excludes -inf";
  EXPECT_EQ(cnt(ir, {}, true, Value{std::int64_t{2}}, true), 6) << "<= 2 includes -inf";
  EXPECT_EQ(cnt(ir, Value{1.5}, false, {}, true), 1) << "> 1.5 : +inf only";
  EXPECT_EQ(cnt(ir, Value{1.5}, true, Value{1.5}, true), 2);
  ValueRange e;
  e.empty = true;
  EXPECT_EQ(ir.query(e).count, 0);
  EXPECT_EQ(iv.validate(), "");
}

TEST(AggIndex, DuplicateAndMissingEntriesAreErrors) {
  Schema s;
  s.table = "e";
  s.cols = {{"id", ColType::kInt}, {"v", ColType::kInt}};
  AggIndex idx(s, 1);
  Row r = {std::int64_t{1}, std::int64_t{5}};
  idx.add(r);
  EXPECT_THROW(idx.add(r), StorageError);
  idx.remove(r);
  EXPECT_THROW(idx.remove(r), StorageError);
}

// Random single-row writes, range aggregates, aborts, clean reopen and crash recovery — all vs. a model.
void run_table_property(std::uint32_t seed, bool crash_instead_of_close) {
  TempDb d("prop" + std::to_string(seed) + (crash_instead_of_close ? "c" : "k"));
  std::mt19937_64 rng(seed);
  std::map<std::int64_t, Row> model;
  auto check_all = [&](Database& db, const std::string& ctx) {
    Table* t = db.find_table("t");
    ASSERT_NE(t, nullptr) << ctx;
    ASSERT_EQ(t->validate(), "") << ctx;
    ASSERT_EQ(t->count(), model.size()) << ctx;
    std::size_t n = 0;
    t->scan({}, [&](const Row& r) {
      auto it = model.find(std::get<std::int64_t>(r[0]));
      EXPECT_TRUE(it != model.end());
      if (it != model.end()) {
        EXPECT_EQ(r, it->second);
      }
      ++n;
      return true;
    });
    ASSERT_EQ(n, model.size()) << ctx;
    for (std::size_t col : {kId, kA, kB}) {
      for (int q = 0; q < 12; ++q) {
        ValueRange r = random_range(rng, t->schema().cols[col].type, col == kId ? 400 : 60);
        expect_matches(*t, model, col, r, ctx + " col " + std::to_string(col));
        if (::testing::Test::HasFatalFailure()) return;
      }
    }
  };

  auto db = std::make_unique<Database>(opts(d, 24));
  {
    Txn tx = db->engine().begin();
    db->create_table("t", test_cols());
    db->create_index("t", "id");
    db->create_index("t", "a");
    db->create_index("t", "b");
    tx.commit();
  }
  for (int round = 0; round < 6; ++round) {
    for (int batch = 0; batch < 10; ++batch) {
      bool will_abort = rng() % 4 == 0;
      auto before = model;
      Txn tx = db->engine().begin();
      Table& t = *db->find_table("t");
      for (int i = 0; i < 40; ++i) {
        auto pk = static_cast<std::int64_t>(rng() % 300);
        int op = static_cast<int>(rng() % 10);
        Row row = make_row(pk, static_cast<std::int64_t>(rng() % 100) - 50, static_cast<double>(rng() % 200) / 4.0 - 25.0,
                           std::string(rng() % 20, 'n'), static_cast<std::int64_t>(rng() % 1000));
        if (op < 5) {
          bool ok = t.insert(row);
          ASSERT_EQ(ok, model.count(pk) == 0);
          if (ok) model[pk] = row;
        } else if (op < 8) {
          bool ok = t.update(pk, row);
          ASSERT_EQ(ok, model.count(pk) == 1);
          if (ok) model[pk] = row;
        } else {
          bool ok = t.erase(pk);
          ASSERT_EQ(ok, model.erase(pk) == 1);
        }
      }
      if (will_abort) {
        tx.abort();
        model = before;
      } else {
        tx.commit();
      }
    }
    check_all(*db, "round " + std::to_string(round));
    if (::testing::Test::HasFatalFailure()) return;
    if (crash_instead_of_close) {
      db.reset();  // drop everything unflushed: pages come back via WAL redo
    } else {
      db->engine().close();
      db.reset();
    }
    db = std::make_unique<Database>(opts(d, 24));  // reopen: recovery + aggregate index rebuild
    check_all(*db, "after reopen " + std::to_string(round));
    if (::testing::Test::HasFatalFailure()) return;
  }
}

TEST(TableProperty, AggregatesMatchBruteForce_CleanReopen) {
  for (std::uint32_t seed : {1u, 2u, 3u}) {
    run_table_property(seed, false);
    if (::testing::Test::HasFatalFailure()) return;
  }
}
TEST(TableProperty, AggregatesMatchBruteForce_CrashReopen) {
  for (std::uint32_t seed : {11u, 12u, 13u}) {
    run_table_property(seed, true);
    if (::testing::Test::HasFatalFailure()) return;
  }
}

TEST(Table, IndexRebuildOnOpenIsSameAsIncremental) {
  TempDb d("rebuild");
  RowStats before;
  {
    Database db(opts(d));
    Txn tx = db.engine().begin();
    Table& t = db.create_table("t", test_cols());
    db.create_index("t", "a");
    for (int i = 0; i < 2000; ++i) t.insert(make_row(i, i % 37, i * 0.25, "r", i % 11));
    for (int i = 0; i < 2000; i += 3) t.erase(i);
    tx.commit();
    before = t.index(kA)->query_all();
    db.engine().close();
  }
  Database db(opts(d));
  const AggIndex* idx = db.find_table("t")->index(kA);
  ASSERT_NE(idx, nullptr);
  RowStats after = idx->query_all();
  EXPECT_EQ(after.count, before.count);
  for (std::size_t s = 0; s < 4; ++s) {
    EXPECT_EQ(after.slots[s].isum, before.slots[s].isum);
    EXPECT_NEAR(after.slots[s].dsum, before.slots[s].dsum, 1e-6 * (1 + std::fabs(before.slots[s].dsum)));
  }
  EXPECT_GT(db.index_rebuild_ms(), 0.0);
}

}  // namespace
}  // namespace aqo
