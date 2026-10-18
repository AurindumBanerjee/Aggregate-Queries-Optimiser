#include "aqo/database.hpp"

#include <chrono>

namespace aqo {

Database::Database(EngineOptions opts) : engine_(std::move(opts)) {
  auto t0 = std::chrono::steady_clock::now();
  auto cat = engine_.catalog();
  cat.scan({}, [&](std::int64_t id, std::span<const std::byte> bytes) {
    page_id_t root;
    Schema s = decode_schema(bytes, root);
    std::string name = s.table;
    tables_[name] = Entry{id, std::make_unique<Table>(engine_, std::move(s), root)};
    next_id_ = std::max(next_id_, id + 1);
    return true;
  });
  rebuild_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  listener_ = engine_.add_txn_listener([this](bool committed) {
    if (!committed) {
      for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) (*it)();
    }
    undo_.clear();
  });
}

Database::~Database() { engine_.remove_txn_listener(listener_); }

Table* Database::find_table(const std::string& name) {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : it->second.table.get();
}

std::vector<std::string> Database::table_names() const {
  std::vector<std::string> out;
  for (auto& [n, e] : tables_) out.push_back(n);
  return out;
}

void Database::persist_schema(Entry& e) {
  auto cat = engine_.catalog();
  cat.upsert(e.id, encode_schema(e.table->schema(), e.table->root()));
}

Table& Database::create_table(const std::string& name, std::vector<Column> cols) {
  if (!engine_.in_txn()) throw StorageError("DDL requires a transaction");
  if (tables_.count(name)) throw SqlError("table " + name + " already exists");
  if (cols.empty() || cols[0].type != ColType::kInt) throw SqlError("the first column must be an INT primary key");
  for (std::size_t i = 0; i < cols.size(); ++i)
    for (std::size_t j = i + 1; j < cols.size(); ++j)
      if (cols[i].name == cols[j].name) throw SqlError("duplicate column name " + cols[i].name);
  Schema s;
  s.table = name;
  s.cols = std::move(cols);
  page_id_t root = DiskBTree::create(engine_.pager());
  Entry e{next_id_++, std::make_unique<Table>(engine_, s, root)};
  persist_schema(e);
  Table& ref = *e.table;
  tables_[name] = std::move(e);
  undo_.emplace_back([this, name] {
    tables_.erase(name);
    --next_id_;
  });
  return ref;
}

void Database::create_index(const std::string& table, const std::string& column) {
  if (!engine_.in_txn()) throw StorageError("DDL requires a transaction");
  auto it = tables_.find(table);
  if (it == tables_.end()) throw SqlError("no such table: " + table);
  Table& t = *it->second.table;
  auto col = t.schema().find(column);
  if (!col) throw SqlError("no such column: " + column);
  if (t.index(*col) != nullptr) throw SqlError("column " + column + " is already indexed");
  t.add_index(*col);
  persist_schema(it->second);
  undo_.emplace_back([this, table, c = *col] {
    if (auto* tp = find_table(table)) tp->drop_index(c);
  });
}

}  // namespace aqo
