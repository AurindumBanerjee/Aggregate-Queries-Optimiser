// Deterministic transactional workload shared by the fault-injection tests and the crash-worker
// process: transaction i of seed s always performs the same operations and either commits or
// deliberately aborts, so a model can be replayed independently of the database.
#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "aqo/engine.hpp"

namespace aqo::testing {

struct Op {
  enum Kind { kInsert, kUpsert, kErase } kind;
  std::int64_t key;
  std::string value;
};

inline std::mt19937_64 txn_rng(std::uint64_t seed, std::uint64_t idx) { return std::mt19937_64(seed * 0x9E3779B97F4A7C15ULL + idx * 7919 + 1); }

inline std::vector<Op> txn_ops(std::uint64_t seed, std::uint64_t idx, std::int64_t key_space = 800) {
  auto rng = txn_rng(seed, idx);
  std::size_t n = 1 + rng() % 10;
  if (rng() % 12 == 0) n = 60 + rng() % 200;  // an occasional big transaction forces splits/merges
  std::vector<Op> ops;
  for (std::size_t i = 0; i < n; ++i) {
    Op op;
    int k = static_cast<int>(rng() % 10);
    op.kind = k < 4 ? Op::kInsert : (k < 6 ? Op::kUpsert : Op::kErase);
    op.key = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(key_space));
    std::size_t len = rng() % 5 == 0 ? rng() % 900 : rng() % 120;
    op.value = std::string(len, static_cast<char>('a' + rng() % 26));
    ops.push_back(std::move(op));
  }
  return ops;
}

inline bool txn_aborts(std::uint64_t seed, std::uint64_t idx) {
  auto rng = txn_rng(seed ^ 0xABCDEF, idx);
  return rng() % 6 == 0;
}

using Model = std::map<std::int64_t, std::string>;

inline void apply_model(Model& m, const std::vector<Op>& ops) {
  for (auto& op : ops) {
    if (op.kind == Op::kInsert) m.emplace(op.key, op.value);
    else if (op.kind == Op::kUpsert) m[op.key] = op.value;
    else m.erase(op.key);
  }
}

inline std::span<const std::byte> as_bytes(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

inline void apply_tree(DiskBTree& t, const std::vector<Op>& ops) {
  for (auto& op : ops) {
    if (op.kind == Op::kInsert) t.insert(op.key, as_bytes(op.value));
    else if (op.kind == Op::kUpsert) t.upsert(op.key, as_bytes(op.value));
    else t.erase(op.key);
  }
}

/// Model state after txns [0, committed_through) of the workload.
inline Model model_after(std::uint64_t seed, std::uint64_t txns) {
  Model m;
  for (std::uint64_t i = 0; i < txns; ++i)
    if (!txn_aborts(seed, i)) apply_model(m, txn_ops(seed, i));
  return m;
}

constexpr std::int64_t kTableKey = 1;  // catalog key holding the data tree's root page id

/// Returns the workload tree, creating it (in its own transaction) on first use.
inline std::optional<DiskBTree> open_table(StorageEngine& e, bool create) {
  auto cat = e.catalog();
  if (auto v = cat.get(kTableKey)) return DiskBTree(e.pager(), load<std::uint32_t>(v->data()));
  if (!create) return std::nullopt;
  Txn t = e.begin();
  page_id_t root = DiskBTree::create(e.pager());
  std::byte b[4];
  store<std::uint32_t>(b, root);
  cat.insert(kTableKey, b);
  t.commit();
  return DiskBTree(e.pager(), root);
}

inline Model dump(const DiskBTree& t) {
  Model m;
  t.scan({}, [&](std::int64_t k, std::span<const std::byte> v) {
    m[k] = std::string(reinterpret_cast<const char*>(v.data()), v.size());
    return true;
  });
  return m;
}

}  // namespace aqo::testing
