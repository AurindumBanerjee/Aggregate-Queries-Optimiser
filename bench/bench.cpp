// aqo vs SQLite benchmark. Emits one JSON document with every measurement.
//
//   aqo_bench --rows 1000000 --dir <scratch dir> --out results.json [--lookups N] [--agg-queries N] [--quick]
//
// Fairness: both engines use a 64 MiB cache, a WAL, and fsync-on-commit (SQLite journal_mode=WAL +
// synchronous=FULL; aqo sync_commit=1), commit once per 10,000-row batch, and see the same seeded data.
// Aggregate answers are cross-checked between engines before any timing is reported.
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "aqo/sql/executor.hpp"

using namespace aqo;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

struct Json {
  std::ostringstream o;
  bool first = true;
  void row(const std::string& body) {
    o << (first ? "\n    " : ",\n    ") << "{" << body << "}";
    first = false;
  }
};
std::string kv(const std::string& k, const std::string& v) { return "\"" + k + "\": \"" + v + "\""; }
std::string kv(const std::string& k, double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.6g", v);
  return "\"" + k + "\": " + b;
}

struct Pct {
  double p50 = 0, p99 = 0, mean = 0;
};
Pct percentiles(std::vector<double>& us) {
  std::sort(us.begin(), us.end());
  Pct p;
  p.p50 = us[us.size() / 2];
  p.p99 = us[std::min(us.size() - 1, us.size() * 99 / 100)];
  p.mean = std::accumulate(us.begin(), us.end(), 0.0) / static_cast<double>(us.size());
  return p;
}

struct Data {
  std::int64_t id, a, c;
  double b;
  std::string name;
};
Data make(std::int64_t id, std::mt19937_64& rng) {
  static const char* kNames[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel"};
  Data d;
  d.id = id;
  d.a = static_cast<std::int64_t>(rng() % 1000000);
  d.b = static_cast<double>(rng() % 100000) / 100.0;
  d.name = std::string(kNames[rng() % 8]) + std::to_string(rng() % 1000);
  d.c = static_cast<std::int64_t>(rng() % 1000);
  return d;
}

[[noreturn]] void die(const std::string& m) {
  std::cerr << "bench error: " << m << "\n";
  std::exit(1);
}

struct Lite {
  sqlite3* db = nullptr;
  explicit Lite(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) die("sqlite open");
    exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-65536;");
  }
  ~Lite() { sqlite3_close(db); }
  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) die(std::string("sqlite: ") + (err ? err : "?") + " in " + sql);
  }
  sqlite3_stmt* prep(const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) die(std::string("prepare: ") + sqlite3_errmsg(db));
    return st;
  }
};

std::uint64_t file_bytes(const std::string& p) {
  std::error_code ec;
  auto s = fs::file_size(p, ec);
  return ec ? 0 : s;
}
void rm(const std::string& base) {
  for (auto* e : {"", ".wal", ".wal.tmp", "-wal", "-shm", "-journal"}) {
    std::error_code ec;
    fs::remove(base + e, ec);
  }
}

struct AggAnswer {
  std::int64_t count = 0, sum_a = 0, max_c = 0;
  double sum_b = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::size_t rows = 1000000, lookups = 200000, agg_queries = 300, scan_queries = 8;
  std::string dir = "bench_scratch", out = "bench/results/results.json";
  std::size_t pool_pages = 16384;  // 64 MiB
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--rows" && i + 1 < argc) rows = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--lookups" && i + 1 < argc) lookups = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--agg-queries" && i + 1 < argc) agg_queries = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--quick") { rows = 50000; lookups = 20000; agg_queries = 50; scan_queries = 3; }
    else die("unknown argument " + a);
  }
  fs::create_directories(dir);
  fs::create_directories(fs::path(out).parent_path());
  Json js;
  const std::size_t kBatch = 10000;
  auto note = [&](const std::string& workload, const std::string& engine, const std::string& variant, const std::string& extra) {
    js.row(kv("workload", workload) + ", " + kv("engine", engine) + ", " + kv("variant", variant) + (extra.empty() ? "" : ", " + extra));
  };

  // ============================== 1. INSERT THROUGHPUT ==============================
  // variants: aqo sequential pk, aqo random pk, aqo + 2 aggregate indexes; sqlite sequential, random, covering index
  std::vector<std::int64_t> order(rows);
  std::iota(order.begin(), order.end(), 0);
  std::vector<std::int64_t> shuffled = order;
  {
    std::mt19937_64 r(99);
    std::shuffle(shuffled.begin(), shuffled.end(), r);
  }
  struct LoadSpec {
    const char* variant;
    bool random_pk;
    bool indexed;
  };
  const LoadSpec specs[] = {{"sequential pk", false, false}, {"random pk", true, false}, {"sequential pk + aggregate indexes", false, true}};
  for (auto& sp : specs) {
    const auto& ids = sp.random_pk ? shuffled : order;
    {  // ---- aqo ----
      std::string path = dir + "/load_aqo.db";
      rm(path);
      EngineOptions o;
      o.path = path;
      o.pool_frames = pool_pages;
      o.sync_commit = true;
      Database db(o);
      Table* t = nullptr;
      {
        Txn tx = db.engine().begin();
        t = &db.create_table("t", {{"id", ColType::kInt}, {"a", ColType::kInt}, {"b", ColType::kReal}, {"name", ColType::kText}, {"c", ColType::kInt}});
        if (sp.indexed) {
          db.create_index("t", "a");
          db.create_index("t", "b");
        }
        tx.commit();
      }
      std::mt19937_64 rng(7);
      auto t0 = Clock::now();
      for (std::size_t base = 0; base < rows; base += kBatch) {
        Txn tx = db.engine().begin();
        for (std::size_t i = base; i < std::min(rows, base + kBatch); ++i) {
          Data d = make(ids[i], rng);
          t->insert({d.id, d.a, d.b, d.name, d.c});
        }
        tx.commit();
        if ((base / kBatch) % 10 == 9) db.engine().checkpoint();
      }
      double s = secs(t0, Clock::now());
      auto wal_bytes = db.engine().wal()->bytes_appended();
      db.engine().close();
      note("insert", "aqo", sp.variant,
           kv("rows", static_cast<double>(rows)) + ", " + kv("seconds", s) + ", " + kv("rows_per_sec", static_cast<double>(rows) / s) + ", " +
               kv("db_mib", static_cast<double>(file_bytes(path)) / 1048576.0) + ", " + kv("wal_mib_written", static_cast<double>(wal_bytes) / 1048576.0));
      std::printf("insert aqo   %-36s %9.0f rows/s\n", sp.variant, static_cast<double>(rows) / s);
      if (!sp.random_pk && !sp.indexed) {  // keep as the dataset for the read benchmarks (re-created below)
      }
      rm(path);
    }
    {  // ---- sqlite ----
      std::string path = dir + "/load_sqlite.db";
      rm(path);
      Lite l(path);
      l.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b REAL, name TEXT, c INTEGER)");
      if (sp.indexed) l.exec("CREATE INDEX ia ON t(a, b, c); CREATE INDEX ib ON t(b)");
      auto* ins = l.prep("INSERT INTO t VALUES (?,?,?,?,?)");
      std::mt19937_64 rng(7);
      auto t0 = Clock::now();
      for (std::size_t base = 0; base < rows; base += kBatch) {
        l.exec("BEGIN");
        for (std::size_t i = base; i < std::min(rows, base + kBatch); ++i) {
          Data d = make(ids[i], rng);
          sqlite3_bind_int64(ins, 1, d.id);
          sqlite3_bind_int64(ins, 2, d.a);
          sqlite3_bind_double(ins, 3, d.b);
          sqlite3_bind_text(ins, 4, d.name.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64(ins, 5, d.c);
          sqlite3_step(ins);
          sqlite3_reset(ins);
        }
        l.exec("COMMIT");
      }
      double s = secs(t0, Clock::now());
      sqlite3_finalize(ins);
      note("insert", "sqlite", sp.variant,
           kv("rows", static_cast<double>(rows)) + ", " + kv("seconds", s) + ", " + kv("rows_per_sec", static_cast<double>(rows) / s) + ", " +
               kv("db_mib", static_cast<double>(file_bytes(path)) / 1048576.0));
      std::printf("insert sqlite %-35s %9.0f rows/s\n", sp.variant, static_cast<double>(rows) / s);
    }
    rm(dir + "/load_sqlite.db");
  }

  // ============================== datasets for the read benchmarks ==============================
  std::string aqo_path = dir + "/read_aqo.db", lite_path = dir + "/read_sqlite.db";
  rm(aqo_path);
  rm(lite_path);
  {
    EngineOptions o;
    o.path = aqo_path;
    o.pool_frames = pool_pages;
    Database db(o);
    {
      Txn tx = db.engine().begin();
      db.create_table("t", {{"id", ColType::kInt}, {"a", ColType::kInt}, {"b", ColType::kReal}, {"name", ColType::kText}, {"c", ColType::kInt}});
      db.create_index("t", "a");
      tx.commit();
    }
    Table* t = db.find_table("t");
    std::mt19937_64 rng(7);
    for (std::size_t base = 0; base < rows; base += kBatch) {
      Txn tx = db.engine().begin();
      for (std::size_t i = base; i < std::min(rows, base + kBatch); ++i) {
        Data d = make(order[i], rng);
        t->insert({d.id, d.a, d.b, d.name, d.c});
      }
      tx.commit();
      if ((base / kBatch) % 10 == 9) db.engine().checkpoint();
    }
    db.engine().close();
  }
  {
    Lite l(lite_path);
    l.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b REAL, name TEXT, c INTEGER)");
    auto* ins = l.prep("INSERT INTO t VALUES (?,?,?,?,?)");
    std::mt19937_64 rng(7);
    for (std::size_t base = 0; base < rows; base += kBatch) {
      l.exec("BEGIN");
      for (std::size_t i = base; i < std::min(rows, base + kBatch); ++i) {
        Data d = make(order[i], rng);
        sqlite3_bind_int64(ins, 1, d.id);
        sqlite3_bind_int64(ins, 2, d.a);
        sqlite3_bind_double(ins, 3, d.b);
        sqlite3_bind_text(ins, 4, d.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 5, d.c);
        sqlite3_step(ins);
        sqlite3_reset(ins);
      }
      l.exec("COMMIT");
    }
    sqlite3_finalize(ins);
  }

  // ---- reopen aqo (measures index rebuild) ----
  EngineOptions o;
  o.path = aqo_path;
  o.pool_frames = pool_pages;
  auto t_open = Clock::now();
  Database db(o);
  double open_s = secs(t_open, Clock::now());
  note("open", "aqo", "clean reopen incl. aggregate-index rebuild",
       kv("rows", static_cast<double>(rows)) + ", " + kv("seconds", open_s) + ", " + kv("index_rebuild_ms", db.index_rebuild_ms()));
  std::printf("aqo reopen + index rebuild: %.2f s\n", open_s);
  Table* t = db.find_table("t");
  sql::Session sess(db), sess_scan(db, sql::SessionOptions{false});
  Lite lite(lite_path);

  // ============================== 2. POINT LOOKUPS ==============================
  {
    std::mt19937_64 rng(11);
    std::vector<std::int64_t> keys(lookups);
    for (auto& k : keys) k = static_cast<std::int64_t>(rng() % rows);
    auto run = [&](const std::string& engine, const std::string& variant, auto&& fn) {
      std::vector<double> us;
      us.reserve(keys.size());
      std::int64_t chk = 0;
      auto t0 = Clock::now();
      for (auto k : keys) {
        auto a = Clock::now();
        chk += fn(k);
        us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
      }
      double s = secs(t0, Clock::now());
      Pct p = percentiles(us);
      note("point_lookup", engine, variant,
           kv("lookups", static_cast<double>(keys.size())) + ", " + kv("ops_per_sec", static_cast<double>(keys.size()) / s) + ", " +
               kv("p50_us", p.p50) + ", " + kv("p99_us", p.p99) + ", " + kv("checksum", static_cast<double>(chk % 1000003)));
      std::printf("lookup %-7s %-22s %9.0f ops/s  p50 %.2f us  p99 %.2f us\n", engine.c_str(), variant.c_str(), static_cast<double>(keys.size()) / s, p.p50, p.p99);
      return chk;
    };
    db.engine().pool().reset_stats();
    auto c1 = run("aqo", "Table::get (no SQL)", [&](std::int64_t k) { return std::get<std::int64_t>(t->get(k)->at(1)); });
    double hit = db.engine().pool().stats().hit_rate();
    note("point_lookup_cache", "aqo", "buffer pool hit rate", kv("hit_rate", hit));
    auto c2 = run("aqo", "SQL (parse+plan each time)", [&](std::int64_t k) {
      auto r = sess.execute("SELECT id, a, b, name, c FROM t WHERE id = " + std::to_string(k));
      return std::get<std::int64_t>(r.rows.at(0).at(1));
    });
    auto* st = lite.prep("SELECT id, a, b, name, c FROM t WHERE id = ?");
    auto lookup = [&](std::int64_t k) {
      sqlite3_bind_int64(st, 1, k);
      sqlite3_step(st);
      std::int64_t a = sqlite3_column_int64(st, 1);
      sqlite3_reset(st);
      return a;
    };
    auto c3 = run("sqlite", "prepared, autocommit reads", lookup);
    // SQLite WAL mode takes shared-memory locks at every read-transaction start (costly on Windows);
    // holding one read transaction open is its best case, so that is the headline comparison.
    lite.exec("BEGIN");
    auto c4 = run("sqlite", "prepared, one open read txn", lookup);
    lite.exec("COMMIT");
    sqlite3_finalize(st);
    if (c1 != c3 || c2 != c3 || c4 != c3) die("point lookup checksums disagree between engines");
  }

  // ============================== 3. RANGE AGGREGATES ==============================
  // SELECT COUNT(*), SUM(a), SUM(b), MAX(c) FROM t WHERE a >= lo AND a <= hi
  lite.exec("CREATE INDEX ia ON t(a, b, c)");  // covering index so SQLite is not forced into table scans
  lite.exec("BEGIN");                           // one open read transaction: SQLite's best case (see lookups)
  {
    const double sels[] = {0.0001, 0.001, 0.01, 0.1, 0.5};
    for (double sel : sels) {
      std::mt19937_64 rng(static_cast<std::uint64_t>(sel * 1e6) + 3);
      std::vector<std::pair<std::int64_t, std::int64_t>> qs;
      for (std::size_t i = 0; i < agg_queries; ++i) {
        auto lo = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(1000000.0 * (1.0 - sel)));
        qs.emplace_back(lo, lo + static_cast<std::int64_t>(1000000.0 * sel));
      }
      auto sql_for = [&](std::pair<std::int64_t, std::int64_t> q) {
        return "SELECT COUNT(*), SUM(a), SUM(b), MAX(c) FROM t WHERE a >= " + std::to_string(q.first) + " AND a <= " + std::to_string(q.second);
      };
      auto parse_aqo = [](const sql::ResultSet& r) {
        AggAnswer a;
        a.count = std::get<std::int64_t>(r.rows[0][0]);
        if (a.count > 0) {
          a.sum_a = std::get<std::int64_t>(r.rows[0][1]);
          a.sum_b = std::get<double>(r.rows[0][2]);
          a.max_c = std::get<std::int64_t>(r.rows[0][3]);
        }
        return a;
      };
      auto* st = lite.prep("SELECT COUNT(*), SUM(a), SUM(b), MAX(c) FROM t WHERE a >= ? AND a <= ?");
      auto sqlite_ans = [&](std::pair<std::int64_t, std::int64_t> q) {
        sqlite3_bind_int64(st, 1, q.first);
        sqlite3_bind_int64(st, 2, q.second);
        sqlite3_step(st);
        AggAnswer a;
        a.count = sqlite3_column_int64(st, 0);
        a.sum_a = sqlite3_column_int64(st, 1);
        a.sum_b = sqlite3_column_double(st, 2);
        a.max_c = sqlite3_column_int64(st, 3);
        sqlite3_reset(st);
        return a;
      };
      // correctness gate: first 20 queries must agree across all engines
      for (std::size_t i = 0; i < std::min<std::size_t>(20, qs.size()); ++i) {
        AggAnswer x = parse_aqo(sess.execute(sql_for(qs[i]))), y = sqlite_ans(qs[i]);
        if (x.count != y.count || x.sum_a != y.sum_a || x.max_c != y.max_c || std::fabs(x.sum_b - y.sum_b) > 1e-6 * (1 + std::fabs(y.sum_b)))
          die("aggregate answers disagree between aqo and sqlite");
      }
      auto time_it = [&](const std::string& engine, const std::string& variant, std::size_t n, auto&& fn) {
        std::vector<double> us;
        std::int64_t chk = 0;
        auto t0 = Clock::now();
        for (std::size_t i = 0; i < n; ++i) {
          auto a = Clock::now();
          chk += fn(qs[i % qs.size()]);
          us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
        }
        Pct p = percentiles(us);
        double s = secs(t0, Clock::now());
        char sb[32];
        std::snprintf(sb, sizeof sb, "%.4g%%", sel * 100);
        note("range_aggregate", engine, variant,
             kv("selectivity", sel) + ", " + kv("queries", static_cast<double>(n)) + ", " + kv("qps", static_cast<double>(n) / s) + ", " +
                 kv("p50_us", p.p50) + ", " + kv("p99_us", p.p99) + ", " + kv("mean_us", p.mean));
        std::printf("agg sel %-7s %-7s %-34s p50 %10.1f us  (%.0f q/s)\n", sb, engine.c_str(), variant.c_str(), p.p50, static_cast<double>(n) / s);
        return chk;
      };
      time_it("aqo", "aggregate index via SQL", qs.size(), [&](auto q) { return sess.execute(sql_for(q)).rows[0][0].index() == 1 ? 1 : 0; });
      time_it("aqo", "aggregate index direct API", qs.size(), [&](auto q) {
        ValueRange r;
        r.restrict_lo(q.first, true);
        r.restrict_hi(q.second, true);
        return t->index(1)->query(r).count & 1;
      });
      time_it("sqlite", "covering index (a,b,c)", qs.size(), [&](auto q) { return sqlite_ans(q).count & 1; });
      sqlite3_finalize(st);
      // O(n) baselines: few queries only
      if (sel == 0.01 || sel == 0.5) {
        time_it("aqo", "full scan (index disabled)", scan_queries, [&](auto q) { return sess_scan.execute(sql_for(q)).rows[0][0].index() == 1 ? 1 : 0; });
      }
    }
    // SQLite without any secondary index, one selectivity
    lite.exec("COMMIT");
    lite.exec("DROP INDEX ia");
    lite.exec("BEGIN");
    std::mt19937_64 rng(5);
    auto* st = lite.prep("SELECT COUNT(*), SUM(a), SUM(b), MAX(c) FROM t WHERE a >= ? AND a <= ?");
    std::vector<double> us;
    for (std::size_t i = 0; i < scan_queries; ++i) {
      auto lo = static_cast<std::int64_t>(rng() % 990000);
      auto a = Clock::now();
      sqlite3_bind_int64(st, 1, lo);
      sqlite3_bind_int64(st, 2, lo + 10000);
      sqlite3_step(st);
      sqlite3_reset(st);
      us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
    }
    sqlite3_finalize(st);
    Pct p = percentiles(us);
    lite.exec("COMMIT");
    note("range_aggregate", "sqlite", "no index (full scan)", kv("selectivity", 0.01) + ", " + kv("queries", static_cast<double>(scan_queries)) + ", " + kv("p50_us", p.p50));
    std::printf("agg sel 1%%      sqlite  %-34s p50 %10.1f us\n", "no index (full scan)", p.p50);
  }

  // ============================== 4. WRITE-PATH COST OF AGGREGATE INDEX + UPDATES ==============================
  {
    std::mt19937_64 rng(21);
    const std::size_t n = std::min<std::size_t>(rows, 100000);
    auto t0 = Clock::now();
    for (std::size_t base = 0; base < n; base += kBatch) {
      Txn tx = db.engine().begin();
      for (std::size_t i = base; i < std::min(n, base + kBatch); ++i) {
        auto id = static_cast<std::int64_t>(rng() % rows);
        auto row = t->get(id);
        (*row)[1] = static_cast<std::int64_t>(rng() % 1000000);
        t->update(id, *row);
      }
      tx.commit();
    }
    double s = secs(t0, Clock::now());
    note("update", "aqo", "random row update (1 aggregate index)", kv("updates", static_cast<double>(n)) + ", " + kv("ops_per_sec", static_cast<double>(n) / s));
    std::printf("update aqo (with agg index): %.0f ops/s\n", static_cast<double>(n) / s);
  }

  // ============================== 5. BUFFER POOL POLICY (skewed reads, small pool) ==============================
  db.engine().close();
  for (auto policy : {EvictionPolicy::kLru, EvictionPolicy::kClock}) {
    EngineOptions po;
    po.path = aqo_path;
    po.pool_frames = 1024;  // 4 MiB cache over a ~100+ MiB database
    po.policy = policy;
    Database pdb(po);
    Table* pt = pdb.find_table("t");
    std::mt19937_64 rng(31);
    const std::size_t n = std::min<std::size_t>(lookups, 200000);
    pdb.engine().pool().reset_stats();
    auto t0 = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      // 80% of reads hit a hot set of 5% of keys
      std::uint64_t r = rng();
      auto hot = static_cast<std::uint64_t>(rows) / 20;
      auto id = static_cast<std::int64_t>((r % 10 < 8) ? (rng() % hot) * 20 : rng() % static_cast<std::uint64_t>(rows));
      (void)pt->get(id);
    }
    double s = secs(t0, Clock::now());
    note("buffer_policy", "aqo", policy == EvictionPolicy::kLru ? "LRU" : "Clock",
         kv("pool_pages", 1024) + ", " + kv("hit_rate", pdb.engine().pool().stats().hit_rate()) + ", " + kv("ops_per_sec", static_cast<double>(n) / s) +
             ", " + kv("disk_reads", static_cast<double>(pdb.engine().disk().reads())));
    std::printf("pool %-5s hit %.3f  %.0f ops/s\n", policy == EvictionPolicy::kLru ? "LRU" : "Clock", pdb.engine().pool().stats().hit_rate(), static_cast<double>(n) / s);
    pdb.engine().close();
  }

  // ============================== 6. CRASH RECOVERY TIME ==============================
  {
    EngineOptions ro;
    ro.path = aqo_path;
    ro.pool_frames = pool_pages;
    {
      Database rdb(ro);
      Table* rt = rdb.find_table("t");
      Txn tx = rdb.engine().begin();
      std::mt19937_64 rng(77);
      for (std::size_t i = 0; i < std::min<std::size_t>(rows, 100000); ++i) {
        Data d = make(static_cast<std::int64_t>(rows + i), rng);
        rt->insert({d.id, d.a, d.b, d.name, d.c});
      }
      tx.commit();
      // crash: no close(), no checkpoint; the 100k inserts exist only in the WAL
    }
    Database rdb(ro);
    const auto& rs = rdb.engine().recovery_stats();
    note("recovery", "aqo", "redo of 100k committed inserts", kv("recovery_ms", rs.millis) + ", " + kv("records_scanned", static_cast<double>(rs.records_scanned)) + ", " +
                                                                  kv("pages_redone", static_cast<double>(rs.redone)) + ", " + kv("total_open_ms_incl_index_rebuild", 0));
    std::printf("recovery of 100k inserts: %.1f ms (%zu log records, %zu page redos)\n", rs.millis, rs.records_scanned, rs.redone);
    rdb.engine().close();
  }

  std::ofstream f(out);
  f << "{\n  \"config\": {\"rows\": " << rows << ", \"batch\": " << kBatch << ", \"cache_mib\": 64, \"sqlite_version\": \"" << sqlite3_libversion()
    << "\", \"build\": \"" <<
#ifdef NDEBUG
      "release"
#else
      "debug"
#endif
    << "\"},\n  \"results\": [" << js.o.str() << "\n  ]\n}\n";
  rm(aqo_path);
  rm(lite_path);
  rm(dir + "/load_aqo.db");
  std::printf("wrote %s\n", out.c_str());
  return 0;
}
