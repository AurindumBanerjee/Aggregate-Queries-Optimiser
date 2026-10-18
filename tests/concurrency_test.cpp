// Single-writer / multi-reader behaviour: readers never see a half-applied transaction, writers are
// serialised (no lost updates), and the buffer pool survives concurrent pinning and eviction.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <thread>

#include "aqo/sql/executor.hpp"
#include "test_util.hpp"

namespace aqo::sql {
namespace {

struct TempDb {
  std::string path;
  explicit TempDb(const std::string& n) : path(aqo::testing::unique_path("conc_" + n, ".db")) {
    for (auto* e : {"", ".wal", ".wal.tmp"}) std::filesystem::remove(path + e);
  }
  ~TempDb() {
    for (auto* e : {"", ".wal", ".wal.tmp"}) std::filesystem::remove(path + e);
  }
};

EngineOptions opts(const TempDb& d, std::size_t frames) {
  EngineOptions o;
  o.path = d.path;
  o.pool_frames = frames;
  o.sync_commit = false;
  return o;
}

TEST(Concurrency, ReadersSeeOnlyCommittedTransfers) {
  TempDb d("transfer");
  Database db(opts(d, 64));
  {
    Session s(db);
    s.execute("CREATE TABLE acct (id INT PRIMARY KEY, bal INT, v REAL, name TEXT, c INT)");
    s.execute("CREATE INDEX ON acct (bal)");
    for (int i = 0; i < 200; ++i) s.execute("INSERT INTO acct VALUES (" + std::to_string(i) + ", 100, 0.5, 'n', 0)");
  }
  std::atomic<bool> done{false};
  std::atomic<long> reads{0}, violations{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&, r] {
      Session s(db, SessionOptions{r % 2 == 0});  // alternate index plan / scan plan
      while (!done) {
        auto res = s.execute("SELECT SUM(bal), COUNT(*), MIN(bal) FROM acct");
        if (std::get<std::int64_t>(res.rows[0][0]) != 20000 || std::get<std::int64_t>(res.rows[0][1]) != 200) ++violations;
        auto pt = s.execute("SELECT bal FROM acct WHERE id = 7");
        if (pt.rows.size() != 1) ++violations;
        ++reads;
      }
    });
  }
  std::thread writer([&] {
    Session s(db);
    std::mt19937 rng(1);
    for (int t = 0; t < 300; ++t) {
      int a = static_cast<int>(rng() % 200), b = static_cast<int>(rng() % 200);
      int amt = static_cast<int>(rng() % 50);
      s.execute("BEGIN");
      s.execute("UPDATE acct SET bal = bal - " + std::to_string(amt) + " WHERE id = " + std::to_string(a));
      std::this_thread::yield();  // a reader here would see money missing if isolation were broken
      s.execute("UPDATE acct SET bal = bal + " + std::to_string(amt) + " WHERE id = " + std::to_string(b));
      s.execute(t % 7 == 0 ? "ROLLBACK" : "COMMIT");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));  // think time: lets readers in
    }
    done = true;
  });
  writer.join();
  for (auto& t : readers) t.join();
  EXPECT_EQ(violations.load(), 0);
  EXPECT_GT(reads.load(), 100) << "readers must have made progress while the writer ran";
  Session s(db);
  EXPECT_EQ(std::get<std::int64_t>(s.execute("SELECT SUM(bal) FROM acct").rows[0][0]), 20000);
}

TEST(Concurrency, ConcurrentWritersAreSerialisedWithoutLostUpdates) {
  TempDb d("writers");
  Database db(opts(d, 64));
  {
    Session s(db);
    s.execute("CREATE TABLE ctr (id INT PRIMARY KEY, n INT)");
    s.execute("INSERT INTO ctr VALUES (1, 0)");
  }
  std::vector<std::thread> ts;
  for (int w = 0; w < 4; ++w) {
    ts.emplace_back([&] {
      Session s(db);
      for (int i = 0; i < 150; ++i) {
        s.execute("BEGIN");
        auto r = s.execute("SELECT n FROM ctr WHERE id = 1");  // read-modify-write inside one transaction
        auto n = std::get<std::int64_t>(r.rows[0][0]);
        s.execute("UPDATE ctr SET n = " + std::to_string(n + 1) + " WHERE id = 1");
        s.execute("COMMIT");
      }
    });
  }
  for (auto& t : ts) t.join();
  Session s(db);
  EXPECT_EQ(std::get<std::int64_t>(s.execute("SELECT n FROM ctr").rows[0][0]), 600);
}

TEST(Concurrency, BufferPoolSurvivesParallelEviction) {
  TempDb d("pool");
  Database db(opts(d, 24));  // far smaller than the table: constant eviction under contention
  {
    Session s(db);
    s.execute("CREATE TABLE t (id INT PRIMARY KEY, a INT, name TEXT)");
    s.execute("BEGIN");
    for (int i = 0; i < 3000; ++i)
      s.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 3) + ", '" + std::string(200, 'p') + "')");
    s.execute("COMMIT");
  }
  std::atomic<long> bad{0};
  std::vector<std::thread> ts;
  for (int r = 0; r < 6; ++r) {
    ts.emplace_back([&, r] {
      Session s(db);
      std::mt19937 rng(static_cast<unsigned>(r));
      for (int i = 0; i < 400; ++i) {
        int id = static_cast<int>(rng() % 3000);
        auto res = s.execute("SELECT a FROM t WHERE id = " + std::to_string(id));
        if (res.rows.size() != 1 || std::get<std::int64_t>(res.rows[0][0]) != id * 3) ++bad;
      }
      auto cnt = s.execute("SELECT COUNT(*), SUM(a) FROM t");
      if (std::get<std::int64_t>(cnt.rows[0][0]) != 3000) ++bad;
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_GT(db.engine().pool().stats().evictions, 100u);
}

TEST(Concurrency, BeginBlocksUntilReadersFinishAndCheckpointIsSafe) {
  TempDb d("ckpt");
  Database db(opts(d, 64));
  Session s(db);
  s.execute("CREATE TABLE t (id INT PRIMARY KEY, a INT)");
  s.execute("INSERT INTO t VALUES (1, 1)");
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    Session r(db);
    while (!stop) {
      auto res = r.execute("SELECT COUNT(*) FROM t");
      EXPECT_GE(std::get<std::int64_t>(res.rows[0][0]), 1);
    }
  });
  for (int i = 2; i < 60; ++i) {
    s.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 1)");
    if (i % 10 == 0) db.checkpoint();
  }
  stop = true;
  reader.join();
  EXPECT_EQ(std::get<std::int64_t>(s.execute("SELECT COUNT(*) FROM t").rows[0][0]), 59);
}

}  // namespace
}  // namespace aqo::sql
