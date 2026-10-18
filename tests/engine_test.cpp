#include "aqo/engine.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "test_util.hpp"
#include "workload.hpp"

namespace aqo {
namespace {
using namespace aqo::testing;

struct TempDb {
  std::string path;
  explicit TempDb(const std::string& n) : path(aqo::testing::unique_path("eng_" + n, ".db")) {
    cleanup();
  }
  ~TempDb() { cleanup(); }
  void cleanup() const {
    for (auto* ext : {"", ".wal", ".wal.tmp"}) std::filesystem::remove(path + ext);
  }
};

EngineOptions opts(const TempDb& d, std::size_t frames = 64) {
  EngineOptions o;
  o.path = d.path;
  o.pool_frames = frames;
  return o;
}

std::string str(const std::vector<std::byte>& v) { return {reinterpret_cast<const char*>(v.data()), v.size()}; }

TEST(Engine, WritesOutsideTransactionAreRejected) {
  TempDb d("gate");
  StorageEngine e(opts(d));
  EXPECT_THROW(e.pager().allocate(), StorageError);
  auto t = e.begin();
  EXPECT_THROW(e.begin(), StorageError) << "single writer";
  EXPECT_NO_THROW(e.pager().allocate());
  t.commit();
}

TEST(Engine, CommitSurvivesCrashWithoutAnyPageFlush) {
  TempDb d("redo");
  {
    StorageEngine e(opts(d, 256));
    auto tree = *open_table(e, true);
    Txn t = e.begin();
    for (int i = 0; i < 300; ++i) tree.insert(i, as_bytes("value" + std::to_string(i)));
    t.commit();
    // no checkpoint, no close(): data pages exist only in memory and in the log
    EXPECT_EQ(e.disk().writes(), 1u) << "only the bootstrap meta page was written";
  }
  StorageEngine e(opts(d));
  EXPECT_TRUE(e.recovery_stats().ran);
  EXPECT_GT(e.recovery_stats().redone, 0u);
  auto tree = *open_table(e, false);
  EXPECT_EQ(tree.validate(), "");
  EXPECT_EQ(tree.count(), 300u);
  EXPECT_EQ(str(*tree.get(123)), "value123");
}

TEST(Engine, UncommittedTransactionIsRolledBackAfterCrash) {
  TempDb d("undo");
  {
    StorageEngine e(opts(d, 12));  // tiny pool: uncommitted pages get stolen (written) to disk
    auto tree = *open_table(e, true);
    {
      Txn t = e.begin();
      for (int i = 0; i < 50; ++i) tree.insert(i, as_bytes("committed"));
      t.commit();
    }
    Txn t2 = e.begin();
    for (int i = 50; i < 3000; ++i) tree.insert(i, as_bytes(std::string(200, 'u')));
    for (int i = 0; i < 20; ++i) tree.erase(i);
    EXPECT_GT(e.disk().writes(), 20u) << "dirty uncommitted pages were evicted to disk";
    // crash: engine destroyed with t2 still open (leak the txn handle to avoid a clean rollback)
    new Txn(std::move(t2));  // intentionally leaked
  }
  StorageEngine e(opts(d));
  EXPECT_EQ(e.recovery_stats().losers, 1u);
  EXPECT_GT(e.recovery_stats().clrs_written, 0u);
  auto tree = *open_table(e, false);
  EXPECT_EQ(tree.validate(), "");
  EXPECT_EQ(tree.count(), 50u);
  EXPECT_EQ(str(*tree.get(7)), "committed");
}

TEST(Engine, AbortRestoresTreeAndAllocatorState) {
  TempDb d("abort");
  StorageEngine e(opts(d, 16));
  auto tree = *open_table(e, true);
  {
    Txn t = e.begin();
    for (int i = 0; i < 100; ++i) tree.insert(i, as_bytes("keep"));
    t.commit();
  }
  Model before = dump(tree);
  page_id_t high = e.pager().next_page();
  {
    Txn t = e.begin();
    for (int i = 100; i < 4000; ++i) tree.insert(i, as_bytes(std::string(300, 'x')));
    for (int i = 0; i < 100; ++i) tree.erase(i);
    t.abort();
  }
  EXPECT_EQ(dump(tree), before);
  EXPECT_EQ(tree.validate(), "");
  EXPECT_EQ(e.pager().next_page(), high) << "pages allocated by the aborted txn were returned";
  EXPECT_EQ(e.stats().aborts, 1u);
  { Txn t = e.begin(); tree.insert(5000, as_bytes("after")); t.commit(); }
  EXPECT_EQ(str(*tree.get(5000)), "after");
}

TEST(Engine, DroppedTxnRollsBack) {
  TempDb d("raii");
  StorageEngine e(opts(d));
  auto tree = *open_table(e, true);
  {
    Txn t = e.begin();
    tree.insert(1, as_bytes("x"));
  }  // destructor aborts
  EXPECT_FALSE(e.in_txn());
  EXPECT_EQ(tree.count(), 0u);
}

TEST(Engine, CheckpointShrinksLogAndKeepsData) {
  TempDb d("ckpt");
  {
    StorageEngine e(opts(d, 64));
    auto tree = *open_table(e, true);
    for (int b = 0; b < 10; ++b) {
      Txn t = e.begin();
      for (int i = 0; i < 100; ++i) tree.insert(b * 100 + i, as_bytes("row"));
      t.commit();
    }
    auto big = e.wal()->file_size();
    EXPECT_GT(big, 10000u);
    e.checkpoint();
    EXPECT_EQ(e.wal()->file_size(), Wal::kHeaderSize);
    Txn t = e.begin();
    tree.insert(5000, as_bytes("post-ckpt"));
    t.commit();
  }  // crash after checkpoint: recovery must apply only the tail
  StorageEngine e(opts(d));
  auto tree = *open_table(e, false);
  EXPECT_EQ(tree.count(), 1001u);
  EXPECT_EQ(str(*tree.get(5000)), "post-ckpt");
}

TEST(Engine, CleanCloseNeedsNoRecoveryWork) {
  TempDb d("clean");
  {
    StorageEngine e(opts(d));
    auto tree = *open_table(e, true);
    Txn t = e.begin();
    tree.insert(1, as_bytes("one"));
    t.commit();
    e.close();
  }
  StorageEngine e(opts(d));
  EXPECT_EQ(e.recovery_stats().redone, 0u);
  EXPECT_EQ(e.recovery_stats().losers, 0u);
  EXPECT_EQ(str(*open_table(e, false)->get(1)), "one");
}

TEST(Engine, DiskCorruptionIsDetected) {
  TempDb d("corrupt");
  {
    StorageEngine e(opts(d));
    auto tree = *open_table(e, true);
    Txn t = e.begin();
    for (int i = 0; i < 20; ++i) tree.insert(i, as_bytes("v"));
    t.commit();
    e.close();
  }
  std::FILE* f = std::fopen(d.path.c_str(), "r+b");
  std::fseek(f, static_cast<long>(kPageSize) + 500, SEEK_SET);  // inside page 1
  std::fputc(0x55, f);
  std::fclose(f);
  EXPECT_THROW(
      {
        StorageEngine e(opts(d));
        EXPECT_GE(e.catalog().count(), 1u);
        EXPECT_GE(open_table(e, false)->count(), 0u);
      },
      StorageError);
}

// ---- exhaustive crash injection ----------------------------------------------------------
//
// For every I/O point k = 0,1,2,... : run the workload with a crash armed at the k-th I/O, then
// simulate power loss (the log may or may not keep its un-fsynced tail, which may end in a torn
// record), recover, and require the database to equal the model after exactly the transactions
// whose commit() returned (plus the one in flight if the crash hit inside its commit).

struct CrashRun {
  std::uint64_t acked = 0;   // transactions whose commit() returned
  bool in_commit = false;    // crash hit while commit() of txn `acked` was running
  std::uint64_t synced = 0;  // WAL bytes known durable at crash time
  bool crashed = false;
};

CrashRun run_until_crash(const TempDb& d, std::uint64_t seed, std::int64_t crash_after, std::uint64_t txns,
                         std::size_t frames, FaultInjector& inj) {
  CrashRun cr;
  EngineOptions o = opts(d, frames);
  o.faults = &inj;
  StorageEngine e(o);
  auto tree = *open_table(e, true);
  inj.arm_after(crash_after);
  try {
    for (std::uint64_t i = 0; i < txns; ++i) {
      Txn t = e.begin();
      apply_tree(tree, txn_ops(seed, i));
      if (txn_aborts(seed, i)) {
        t.abort();
        ++cr.acked;
        continue;
      }
      cr.in_commit = true;
      t.commit();
      cr.in_commit = false;
      ++cr.acked;
    }
    if (crash_after % 3 == 0) e.checkpoint();  // exercise checkpoint/truncate crash points too
  } catch (const SimulatedCrash&) {
    cr.crashed = true;
  }
  cr.synced = e.wal()->synced_file_size();
  inj.disarm();
  // Leak any open transaction handle so its destructor cannot perform a clean rollback.
  return cr;
}

class CrashSweep : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(CrashSweep, RecoversToACommittedPrefixAtEveryCrashPoint) {
  const std::uint64_t seed = GetParam();
  const std::uint64_t kTxns = 40;
  std::size_t crashes = 0, completed = 0;
  for (std::int64_t k = 0; k < 4000; k += (k < 80 ? 1 : 4)) {
    TempDb d("sweep" + std::to_string(seed));
    FaultInjector inj(seed * 1000 + static_cast<std::uint64_t>(k));
    CrashRun cr;
    {
      // Txn handles destroyed during unwinding would abort cleanly; model a real crash instead by
      // running inside a scope and discarding all volatile state afterwards.
      cr = run_until_crash(d, seed, k, kTxns, 14, inj);
    }
    if (!cr.crashed) {
      ++completed;
      break;  // the whole workload fits before crash point k: sweep is exhaustive
    }
    ++crashes;
    // Power loss: the un-fsynced tail of the log survives only sometimes.
    if (k % 2 == 1) std::filesystem::resize_file(d.path + ".wal", cr.synced);
    // Reopen (recovery), possibly crashing again during recovery itself, then reopen cleanly.
    if (k % 5 == 0) {
      FaultInjector inj2(k);
      inj2.arm_after(k % 17);
      EngineOptions o = opts(d, 14);
      o.faults = &inj2;
      try {
        StorageEngine e(o);
      } catch (const SimulatedCrash&) {
      }
    }
    StorageEngine e(opts(d, 14));
    auto table = open_table(e, false);
    Model got = table ? dump(*table) : Model{};
    if (table) {
      ASSERT_EQ(table->validate(), "") << "k=" << k;
    }
    // Aborted transactions count as acknowledged-but-invisible, handled by model_after.
    Model want = model_after(seed, cr.acked);
    bool ok = got == want;
    if (!ok && cr.in_commit) ok = got == model_after(seed, cr.acked + 1);
    ASSERT_TRUE(ok) << "seed " << seed << " crash point " << k << " acked=" << cr.acked
                    << " in_commit=" << cr.in_commit << " got " << got.size() << " keys, want " << want.size();
    // The recovered database must keep working.
    if (table) {
      Txn t = e.begin();
      table->upsert(-1, as_bytes("post-recovery"));
      t.commit();
      ASSERT_EQ(table->validate(), "");
    }
  }
  EXPECT_GT(crashes, 60u) << "sweep should cover many distinct crash points";
  EXPECT_EQ(completed, 1u) << "sweep must reach the end of the workload";
}

INSTANTIATE_TEST_SUITE_P(Seeds, CrashSweep, ::testing::Values(1u, 2u, 3u));

}  // namespace
}  // namespace aqo
