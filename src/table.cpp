#include "aqo/table.hpp"

#include <algorithm>

namespace aqo {

Table::Table(StorageEngine& engine, Schema schema, page_id_t root)
    : engine_(engine), schema_(std::move(schema)), tree_(engine.pager(), root) {
  if (schema_.cols.empty() || schema_.cols[0].type != ColType::kInt)
    throw SqlError("the first column must be an INT primary key");
  for (auto col : schema_.indexed) indexes_[col] = std::make_unique<AggIndex>(schema_, col);
  if (!indexes_.empty()) {
    scan({}, [&](const Row& r) {
      for (auto& [c, idx] : indexes_) idx->add(r);
      return true;
    });
  }
  listener_ = engine_.add_txn_listener([this](bool committed) {
    if (!committed) {
      for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) (*it)();
    }
    undo_.clear();
  });
}

Table::~Table() { engine_.remove_txn_listener(listener_); }

void Table::index_add(const Row& row) {
  for (auto& [c, idx] : indexes_) idx->add(row);
  if (!indexes_.empty()) {
    undo_.emplace_back([this, row] {
      for (auto& [c, idx] : indexes_) idx->remove(row);
    });
  }
}

void Table::index_remove(const Row& row) {
  for (auto& [c, idx] : indexes_) idx->remove(row);
  if (!indexes_.empty()) {
    undo_.emplace_back([this, row] {
      for (auto& [c, idx] : indexes_) idx->add(row);
    });
  }
}

bool Table::insert(const Row& raw) {
  Row row = coerce_row(schema_, raw);
  auto bytes = encode_row(schema_, row);
  if (!tree_.insert(std::get<std::int64_t>(row[0]), bytes)) return false;
  index_add(row);
  return true;
}

bool Table::erase(std::int64_t pk) {
  auto old = get(pk);
  if (!old) return false;
  tree_.erase(pk);
  index_remove(*old);
  return true;
}

bool Table::update(std::int64_t pk, const Row& raw) {
  Row row = coerce_row(schema_, raw);
  if (std::get<std::int64_t>(row[0]) != pk) throw SqlError("UPDATE cannot change the primary key");
  auto old = get(pk);
  if (!old) return false;
  tree_.upsert(pk, encode_row(schema_, row));
  index_remove(*old);
  index_add(row);
  return true;
}

std::optional<Row> Table::get(std::int64_t pk) const {
  auto v = tree_.get(pk);
  if (!v) return std::nullopt;
  return decode_row(schema_, pk, *v);
}

void Table::scan(const KeyRange<std::int64_t>& range, const std::function<bool(const Row&)>& fn) const {
  tree_.scan(range, [&](std::int64_t pk, std::span<const std::byte> v) { return fn(decode_row(schema_, pk, v)); });
}

const AggIndex* Table::index(std::size_t col) const {
  auto it = indexes_.find(col);
  return it == indexes_.end() ? nullptr : it->second.get();
}

void Table::add_index(std::size_t col) {
  if (indexes_.count(col)) return;
  auto idx = std::make_unique<AggIndex>(schema_, col);
  scan({}, [&](const Row& r) {
    idx->add(r);
    return true;
  });
  indexes_[col] = std::move(idx);
  if (!schema_.is_indexed(col)) schema_.indexed.push_back(col);
}

void Table::drop_index(std::size_t col) {
  indexes_.erase(col);
  std::erase(schema_.indexed, col);
}

std::string Table::validate() const {
  if (auto e = tree_.validate(); !e.empty()) return "tree: " + e;
  for (auto& [c, idx] : indexes_) {
    if (auto e = idx->validate(); !e.empty()) return "index on " + schema_.cols[c].name + ": " + e;
    if (idx->size() != tree_.count()) return "index on " + schema_.cols[c].name + " has the wrong size";
  }
  return "";
}

}  // namespace aqo
