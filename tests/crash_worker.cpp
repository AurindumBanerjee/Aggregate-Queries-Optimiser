// Child process for the kill-9 crash test. Runs the deterministic workload from txn `start`,
// appending a line to the ack file *after* each commit/abort returns, until it is killed.
//
//   crash_worker <db> <seed> <start_txn> <ack_file> <pool_frames> <sync_commit 0|1> <checkpoint_every>
#include <cstdio>
#include <cstdlib>

#include "workload.hpp"

int main(int argc, char** argv) {
  if (argc != 8) {
    std::fprintf(stderr, "usage: crash_worker db seed start ack frames sync ckpt_every\n");
    return 2;
  }
  using namespace aqo;
  using namespace aqo::testing;
  EngineOptions o;
  o.path = argv[1];
  std::uint64_t seed = std::strtoull(argv[2], nullptr, 10);
  std::uint64_t start = std::strtoull(argv[3], nullptr, 10);
  const char* ack_path = argv[4];
  o.pool_frames = std::strtoull(argv[5], nullptr, 10);
  o.sync_commit = std::atoi(argv[6]) != 0;
  std::uint64_t ckpt_every = std::strtoull(argv[7], nullptr, 10);

  StorageEngine e(o);
  auto tree = *open_table(e, true);
  std::FILE* ack = std::fopen(ack_path, "ab");
  for (std::uint64_t i = start;; ++i) {
    Txn t = e.begin();
    apply_tree(tree, txn_ops(seed, i));
    if (txn_aborts(seed, i)) t.abort();
    else t.commit();
    std::fprintf(ack, "%llu\n", static_cast<unsigned long long>(i + 1));
    std::fflush(ack);
    if (ckpt_every != 0 && (i + 1) % ckpt_every == 0) e.checkpoint();
  }
}
