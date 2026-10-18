// Typed values, schemas and the row codec.
//
// Row encoding (all columns NOT NULL): INT = 8 bytes LE, REAL = 8 bytes IEEE-754, TEXT = u16 length + bytes.
// The primary key is column 0 (INT) and is also the B+ tree key, so it is not repeated in the value bytes.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "aqo/page.hpp"

namespace aqo {

enum class ColType : std::uint8_t { kInt = 1, kReal = 2, kText = 3 };

/// SQL value. monostate is NULL, which only appears in query results (e.g. SUM over zero rows).
using Value = std::variant<std::monostate, std::int64_t, double, std::string>;
using Row = std::vector<Value>;

struct SqlError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[nodiscard]] bool is_null(const Value& v);
[[nodiscard]] bool is_numeric(const Value& v);
[[nodiscard]] double as_double(const Value& v);
/// Total order: NULL < numbers (compared numerically across INT/REAL) < text.
[[nodiscard]] int compare(const Value& a, const Value& b);
[[nodiscard]] std::string to_string(const Value& v);
[[nodiscard]] const char* type_name(ColType t);

struct Column {
  std::string name;
  ColType type;
};

struct Schema {
  std::string table;
  std::vector<Column> cols;                // cols[0] is the INT primary key
  std::vector<std::size_t> indexed;        // columns that have an aggregate index
  [[nodiscard]] std::optional<std::size_t> find(const std::string& col) const;
  [[nodiscard]] bool is_indexed(std::size_t col) const;
  /// Numeric columns in order; the position in this list is the column's slot in aggregate payloads.
  [[nodiscard]] std::vector<std::size_t> numeric_columns() const;
  [[nodiscard]] std::optional<std::size_t> slot_of(std::size_t col) const;
};

constexpr std::size_t kMaxNumericColumns = 6;

/// Checks arity/types (INT literals widen to REAL), returns the coerced row. Throws SqlError.
Row coerce_row(const Schema& s, const Row& row);
std::vector<std::byte> encode_row(const Schema& s, const Row& row);
Row decode_row(const Schema& s, std::int64_t pk, std::span<const std::byte> bytes);

std::vector<std::byte> encode_schema(const Schema& s, page_id_t root);
Schema decode_schema(std::span<const std::byte> bytes, page_id_t& root);

}  // namespace aqo
