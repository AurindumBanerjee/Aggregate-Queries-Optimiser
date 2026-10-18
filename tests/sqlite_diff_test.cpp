// Differential fuzz: the same seeded statement stream runs against aqo (with and without the aggregate
// index) and SQLite; every SELECT result must agree. Small and seeded so it runs in a few seconds.
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <random>

#include "aqo/sql/executor.hpp"
#include "test_util.hpp"

namespace aqo::sql {
namespace {

using Cell = std::variant<std::monostate, std::int64_t, double, std::string>;
using Rows = std::vector<std::vector<Cell>>;

struct Lite {
  sqlite3* db = nullptr;
  Lite() { sqlite3_open(":memory:", &db); }
  ~Lite() { sqlite3_close(db); }
  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string m = err ? err : "?";
      sqlite3_free(err);
      throw std::runtime_error("sqlite: " + m + " in " + sql);
    }
  }
  Rows query(const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db) + " in " + sql);
    Rows out;
    while (sqlite3_step(st) == SQLITE_ROW) {
      std::vector<Cell> row;
      for (int i = 0; i < sqlite3_column_count(st); ++i) {
        switch (sqlite3_column_type(st, i)) {
          case SQLITE_INTEGER: row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(st, i))); break;
          case SQLITE_FLOAT: row.emplace_back(sqlite3_column_double(st, i)); break;
          case SQLITE_TEXT: row.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, i)))); break;
          default: row.emplace_back(std::monostate{}); break;
        }
      }
      out.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return out;
  }
};

bool cell_eq(const Cell& a, const Cell& b) {
  if (a.index() == 0 || b.index() == 0) return a.index() == b.index();
  if (a.index() == 3 || b.index() == 3) return a == b;
  double x = a.index() == 1 ? static_cast<double>(std::get<1>(a)) : std::get<2>(a);
  double y = b.index() == 1 ? static_cast<double>(std::get<1>(b)) : std::get<2>(b);
  return std::fabs(x - y) <= 1e-9 * (1 + std::max(std::fabs(x), std::fabs(y)));
}

std::string render(const Rows& r) {
  std::string s;
  for (auto& row : r) {
    for (auto& c : row) {
      if (c.index() == 0) s += "NULL";
      else if (c.index() == 1) s += std::to_string(std::get<1>(c));
      else if (c.index() == 2) s += std::to_string(std::get<2>(c));
      else s += "'" + std::get<3>(c) + "'";
      s += ' ';
    }
    s += "| ";
  }
  return s;
}

Rows to_cells(const ResultSet& rs) {
  Rows out;
  for (auto& r : rs.rows) {
    std::vector<Cell> row;
    for (auto& v : r) row.push_back(v);
    out.push_back(std::move(row));
  }
  return out;
}

bool less_row(const std::vector<Cell>& a, const std::vector<Cell>& b) {
  for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
    Value x = a[i], y = b[i];
    int c = compare(x, y);
    if (c != 0) return c < 0;
  }
  return false;
}

struct Gen {
  std::mt19937_64 rng;
  explicit Gen(std::uint64_t seed) : rng(seed) {}
  std::int64_t n(std::int64_t lo, std::int64_t hi) { return lo + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(hi - lo + 1)); }
  std::string pick(std::initializer_list<const char*> xs) { return *(xs.begin() + rng() % xs.size()); }

  std::string num_col() { return pick({"id", "a", "b", "c"}); }
  std::string literal(const std::string& col) {
    if (col == "b") return std::to_string(static_cast<double>(n(-40, 40)) / 4.0).substr(0, 6);
    if (rng() % 5 == 0) return std::to_string(n(-40, 40)) + ".5";
    return std::to_string(n(col == "id" ? 0 : -30, col == "id" ? 300 : 30));
  }
  std::string term() {
    int k = static_cast<int>(rng() % 8);
    std::string col = num_col();
    if (k == 0) return "name = '" + pick({"x", "y", "z"}) + "'";
    if (k == 1) return col + " BETWEEN " + literal(col) + " AND " + literal(col);
    if (k == 2) return "a + c > " + std::to_string(n(-30, 60));
    if (k == 3) return "(" + col + " < " + literal(col) + " OR " + col + " > " + literal(col) + ")";
    static const char* ops[] = {"=", "<", "<=", ">", ">=", "<>"};
    return col + " " + ops[rng() % 6] + " " + literal(col);
  }
  // One or two comparisons on a single indexed column: eligible for the aggregate-index plan.
  std::string range_where() {
    std::string col = pick({"a", "b", "id"});
    static const char* ops[] = {"=", "<", "<=", ">", ">="};
    std::string w = " WHERE " + col + " " + ops[rng() % 5] + " " + literal(col);
    if (rng() % 2) w += " AND " + col + " " + ops[1 + rng() % 4] + " " + literal(col);
    return w;
  }
  std::string where() {
    if (rng() % 2 == 0) return range_where();
    if (rng() % 6 == 0) return "";
    std::string w = " WHERE " + term();
    for (int i = static_cast<int>(rng() % 3); i > 0; --i) w += " AND " + term();
    return w;
  }
};

TEST(SqliteDifferential, RandomWorkloadAgreesWithSqlite) {
  for (std::uint64_t seed : {1, 2, 3, 4}) {
    std::string path = aqo::testing::unique_path("diff" + std::to_string(seed), ".db");
    for (auto* ext : {"", ".wal"}) std::filesystem::remove(path + ext);
    {
    EngineOptions o;
    o.path = path;
    o.sync_commit = false;
    Database db(o);
    Session fast(db), slow(db, SessionOptions{false});
    Lite lite;
    const char* ddl = "CREATE TABLE t (id INT PRIMARY KEY, a INT, b REAL, name TEXT, c INT)";
    fast.execute(ddl);
    fast.execute("CREATE INDEX ON t (a)");
    fast.execute("CREATE INDEX ON t (b)");
    fast.execute("CREATE INDEX ON t (id)");
    lite.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b REAL, name TEXT, c INTEGER)");
    Gen g(seed);
    std::size_t selects = 0, index_plans = 0;
    for (int step = 0; step < 450; ++step) {
      std::string sql;
      int k = static_cast<int>(g.rng() % 20);
      bool is_select = false;
      if (k < 7) {
        sql = "INSERT INTO t VALUES (" + std::to_string(g.n(0, 300)) + ", " + std::to_string(g.n(-30, 30)) + ", " +
              std::to_string(static_cast<double>(g.n(-40, 40)) / 4.0) + ", '" + g.pick({"x", "y", "z"}) + "', " + std::to_string(g.n(-30, 30)) + ")";
      } else if (k < 9) {
        sql = "UPDATE t SET a = a + " + std::to_string(g.n(-5, 5)) + ", b = b * 0.5 + 1" + g.where();
      } else if (k < 10) {
        sql = "DELETE FROM t" + g.where();
      } else {
        is_select = true;
        int kind = static_cast<int>(g.rng() % 4);
        if (kind == 0) {
          sql = "SELECT COUNT(*), SUM(a), MIN(b), MAX(c), AVG(a), SUM(b), COUNT(a) FROM t" + g.where();
        } else if (kind == 1) {
          sql = "SELECT SUM(c), MAX(a), MIN(id) FROM t" + g.where();
        } else if (kind == 2) {
          sql = "SELECT name, COUNT(*), SUM(a), MAX(b) FROM t" + g.where() + " GROUP BY name ORDER BY name";
        } else {
          sql = "SELECT id, a, b FROM t" + g.where() + " ORDER BY id" + (g.rng() % 2 ? " LIMIT 7" : "");
        }
      }
      if (!is_select) {
        // Duplicate-key inserts must fail in both engines and leave both unchanged.
        bool lite_ok = true, aqo_ok = true;
        try { lite.exec(sql); } catch (const std::runtime_error&) { lite_ok = false; }
        try { fast.execute(sql); } catch (const SqlError&) { aqo_ok = false; }
        ASSERT_EQ(lite_ok, aqo_ok) << sql;
        continue;
      }
      ++selects;
      Rows want = lite.query(sql);
      auto r1 = fast.execute(sql);
      auto r2 = slow.execute(sql);
      if (r1.plan.find("INDEX AGGREGATE") != std::string::npos) ++index_plans;
      for (auto* r : {&r1, &r2}) {
        Rows got = to_cells(*r);
        bool ordered = sql.find("ORDER BY") != std::string::npos;
        if (!ordered) {
          std::sort(got.begin(), got.end(), less_row);
          std::sort(want.begin(), want.end(), less_row);
        }
        ASSERT_EQ(got.size(), want.size()) << sql << "\n aqo: " << render(got) << "\n sqlite: " << render(want) << "\n plan " << r->plan;
        for (std::size_t i = 0; i < got.size(); ++i) {
          ASSERT_EQ(got[i].size(), want[i].size());
          for (std::size_t j = 0; j < got[i].size(); ++j)
            ASSERT_TRUE(cell_eq(got[i][j], want[i][j])) << sql << "\n aqo: " << render(got) << "\n sqlite: " << render(want) << "\n plan " << r->plan;
        }
      }
    }
    EXPECT_GT(selects, 100u);
    EXPECT_GT(index_plans, 20u) << "fuzz must actually exercise the aggregate-index path";
    }
    for (auto* ext : {"", ".wal"}) std::filesystem::remove(path + ext);
  }
}

}  // namespace
}  // namespace aqo::sql
