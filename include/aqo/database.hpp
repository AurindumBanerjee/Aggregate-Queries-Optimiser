// Database = storage engine + catalog + tables. DDL must run inside a transaction; the in-memory
// table registry is rolled back together with the catalog pages if that transaction aborts.
#pragma once

#include <map>
#include <memory>

#include "aqo/table.hpp"

namespace aqo {

class Database {
 public:
  explicit Database(EngineOptions opts);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  StorageEngine& engine() { return engine_; }

  Table& create_table(const std::string& name, std::vector<Column> cols);
  /// Builds an aggregate index on a numeric column and records it in the catalog.
  void create_index(const std::string& table, const std::string& column);
  [[nodiscard]] Table* find_table(const std::string& name);
  [[nodiscard]] std::vector<std::string> table_names() const;
  /// Milliseconds spent rebuilding aggregate indexes when the database was opened.
  [[nodiscard]] double index_rebuild_ms() const { return rebuild_ms_; }

 private:
  struct Entry {
    std::int64_t id;
    std::unique_ptr<Table> table;
  };
  void persist_schema(Entry& e);

  StorageEngine engine_;
  std::map<std::string, Entry> tables_;
  std::vector<std::function<void()>> undo_;
  std::int64_t next_id_ = 1;
  std::size_t listener_ = 0;
  double rebuild_ms_ = 0;
};

}  // namespace aqo
