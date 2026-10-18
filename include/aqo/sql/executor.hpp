// SQL session: parse -> bind -> plan -> execute, with auto-commit and explicit transactions.
//
// Planner rules (see docs/adr/0007-sql-layer.md):
//   * WHERE is split into AND-ed conjuncts; `column <op> constant` conjuncts on numeric columns become
//     per-column ValueRanges, everything else is a residual filter evaluated per row.
//   * Aggregate-only SELECTs whose conjuncts are all ranges on ONE indexed column (or no WHERE) are
//     answered from the aggregate index in O(log n): "INDEX AGGREGATE".
//   * Otherwise the table is scanned (restricted to the primary-key range when one is present),
//     filtered, and projected / hash-aggregated.
#pragma once

#include <optional>

#include "aqo/database.hpp"
#include "aqo/sql/parser.hpp"

namespace aqo::sql {

struct ResultSet {
  std::vector<std::string> columns;
  std::vector<Row> rows;
  std::size_t affected = 0;  // rows changed by INSERT / UPDATE / DELETE
  std::string plan;          // human-readable access path
  std::string message;       // status for DDL / transaction statements
  double millis = 0;
};

struct SessionOptions {
  bool use_aggregate_index = true;  // false forces scans (differential testing, benchmarks)
};

class Session {
 public:
  explicit Session(Database& db, SessionOptions opts = {});
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  /// Executes exactly one statement.
  ResultSet execute(const std::string& sql);
  /// Executes every statement in the script, stopping at the first error.
  std::vector<ResultSet> execute_script(const std::string& sql);

  [[nodiscard]] bool in_transaction() const { return txn_.has_value(); }
  SessionOptions& options() { return opts_; }
  Database& database() { return db_; }

 private:
  ResultSet run(ParsedStatement& ps);
  ResultSet dispatch(ParsedStatement& ps);
  ResultSet do_select(Select& s, bool explain_only);
  ResultSet do_insert(Insert& s);
  ResultSet do_update(Update& s, bool explain_only);
  ResultSet do_delete(Delete& s, bool explain_only);

  Database& db_;
  SessionOptions opts_;
  std::optional<Txn> txn_;
};

}  // namespace aqo::sql
