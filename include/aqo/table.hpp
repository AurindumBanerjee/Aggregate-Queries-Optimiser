// A table = clustered DiskBTree (pk -> encoded row) + optional in-memory aggregate indexes.
//
// Durable state lives only in the B+ tree (and is protected by the WAL). Aggregate indexes are derived
// data: they are rebuilt from the tree when the database opens, updated on every write, and rolled back
// with an undo log when the surrounding transaction aborts.
#pragma once

#include <functional>
#include <map>
#include <memory>

#include "aqo/agg.hpp"
#include "aqo/engine.hpp"

namespace aqo {

class Table {
 public:
  Table(StorageEngine& engine, Schema schema, page_id_t root);
  ~Table();
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  [[nodiscard]] const Schema& schema() const { return schema_; }
  [[nodiscard]] page_id_t root() const { return tree_.root(); }

  /// Returns false if the primary key already exists.
  bool insert(const Row& row);
  bool erase(std::int64_t pk);
  /// Replaces the row with primary key `pk` (row[0] must equal pk). False if absent.
  bool update(std::int64_t pk, const Row& new_row);
  [[nodiscard]] std::optional<Row> get(std::int64_t pk) const;
  void scan(const KeyRange<std::int64_t>& pk_range, const std::function<bool(const Row&)>& fn) const;
  [[nodiscard]] std::size_t count() const { return tree_.count(); }

  [[nodiscard]] const AggIndex* index(std::size_t col) const;
  /// Builds an aggregate index over an existing column from the current table contents.
  void add_index(std::size_t col);
  void drop_index(std::size_t col);
  [[nodiscard]] std::string validate() const;
  [[nodiscard]] DiskBTree& tree() { return tree_; }

 private:
  void index_add(const Row& row);
  void index_remove(const Row& row);

  StorageEngine& engine_;
  Schema schema_;
  DiskBTree tree_;
  std::map<std::size_t, std::unique_ptr<AggIndex>> indexes_;
  std::vector<std::function<void()>> undo_;  // inverse index operations for the open transaction
  std::size_t listener_ = 0;
};

}  // namespace aqo
