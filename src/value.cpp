#include "aqo/value.hpp"

#include <cmath>
#include <cstdio>
#include <span>

namespace aqo {

bool is_null(const Value& v) { return std::holds_alternative<std::monostate>(v); }
bool is_numeric(const Value& v) { return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v); }
double as_double(const Value& v) {
  if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
  if (auto* d = std::get_if<double>(&v)) return *d;
  throw SqlError("value is not numeric");
}

int compare(const Value& a, const Value& b) {
  auto rank = [](const Value& v) { return is_null(v) ? 0 : (is_numeric(v) ? 1 : 2); };
  int ra = rank(a), rb = rank(b);
  if (ra != rb) return ra < rb ? -1 : 1;
  if (ra == 0) return 0;
  if (ra == 2) {
    int c = std::get<std::string>(a).compare(std::get<std::string>(b));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  auto* ia = std::get_if<std::int64_t>(&a);
  auto* ib = std::get_if<std::int64_t>(&b);
  if (ia != nullptr && ib != nullptr) return *ia < *ib ? -1 : (*ia > *ib ? 1 : 0);
  double x = as_double(a), y = as_double(b);
  return x < y ? -1 : (x > y ? 1 : 0);
}

std::string to_string(const Value& v) {
  if (is_null(v)) return "NULL";
  if (auto* i = std::get_if<std::int64_t>(&v)) return std::to_string(*i);
  if (auto* d = std::get_if<double>(&v)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", *d);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // keep REAL visibly real
    return s;
  }
  return std::get<std::string>(v);
}

const char* type_name(ColType t) {
  switch (t) {
    case ColType::kInt: return "INT";
    case ColType::kReal: return "REAL";
    case ColType::kText: return "TEXT";
  }
  return "?";
}

std::optional<std::size_t> Schema::find(const std::string& col) const {
  for (std::size_t i = 0; i < cols.size(); ++i)
    if (cols[i].name == col) return i;
  return std::nullopt;
}
bool Schema::is_indexed(std::size_t col) const {
  for (auto c : indexed)
    if (c == col) return true;
  return false;
}
std::vector<std::size_t> Schema::numeric_columns() const {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < cols.size(); ++i)
    if (cols[i].type != ColType::kText) out.push_back(i);
  return out;
}
std::optional<std::size_t> Schema::slot_of(std::size_t col) const {
  std::size_t slot = 0;
  for (std::size_t i = 0; i < cols.size(); ++i) {
    if (cols[i].type == ColType::kText) continue;
    if (i == col) return slot;
    ++slot;
  }
  return std::nullopt;
}

Row coerce_row(const Schema& s, const Row& row) {
  if (row.size() != s.cols.size())
    throw SqlError("table " + s.table + " has " + std::to_string(s.cols.size()) + " columns but " +
                   std::to_string(row.size()) + " values were supplied");
  Row out = row;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto& c = s.cols[i];
    Value& v = out[i];
    if (is_null(v)) throw SqlError("column " + c.name + " is NOT NULL");
    switch (c.type) {
      case ColType::kInt:
        if (!std::holds_alternative<std::int64_t>(v)) throw SqlError("column " + c.name + " expects INT");
        break;
      case ColType::kReal:
        if (auto* iv = std::get_if<std::int64_t>(&v)) v = static_cast<double>(*iv);
        if (!std::holds_alternative<double>(v)) throw SqlError("column " + c.name + " expects REAL");
        if (std::isnan(std::get<double>(v))) throw SqlError("NaN is not storable");
        if (std::get<double>(v) == 0.0) v = 0.0;  // normalise -0.0
        break;
      case ColType::kText:
        if (!std::holds_alternative<std::string>(v)) throw SqlError("column " + c.name + " expects TEXT");
        if (std::get<std::string>(v).size() > 900) throw SqlError("TEXT value too long (max 900 bytes)");
        break;
    }
  }
  return out;
}

std::vector<std::byte> encode_row(const Schema& s, const Row& row) {
  std::vector<std::byte> out;
  auto put = [&](const void* p, std::size_t n) {
    auto* b = static_cast<const std::byte*>(p);
    out.insert(out.end(), b, b + n);
  };
  for (std::size_t i = 1; i < s.cols.size(); ++i) {  // column 0 (the key) lives in the tree key
    switch (s.cols[i].type) {
      case ColType::kInt: {
        std::int64_t v = std::get<std::int64_t>(row[i]);
        put(&v, 8);
        break;
      }
      case ColType::kReal: {
        double v = std::get<double>(row[i]);
        put(&v, 8);
        break;
      }
      case ColType::kText: {
        const auto& str = std::get<std::string>(row[i]);
        auto len = static_cast<std::uint16_t>(str.size());
        put(&len, 2);
        put(str.data(), str.size());
        break;
      }
    }
  }
  if (out.size() > 992) throw SqlError("row too large (max 992 bytes encoded)");
  return out;
}

Row decode_row(const Schema& s, std::int64_t pk, std::span<const std::byte> bytes) {
  Row row;
  row.reserve(s.cols.size());
  row.emplace_back(pk);
  std::size_t pos = 0;
  for (std::size_t i = 1; i < s.cols.size(); ++i) {
    switch (s.cols[i].type) {
      case ColType::kInt:
        row.emplace_back(load<std::int64_t>(bytes.data() + pos));
        pos += 8;
        break;
      case ColType::kReal:
        row.emplace_back(load<double>(bytes.data() + pos));
        pos += 8;
        break;
      case ColType::kText: {
        auto len = load<std::uint16_t>(bytes.data() + pos);
        pos += 2;
        row.emplace_back(std::string(reinterpret_cast<const char*>(bytes.data() + pos), len));
        pos += len;
        break;
      }
    }
  }
  return row;
}

std::vector<std::byte> encode_schema(const Schema& s, page_id_t root) {
  std::vector<std::byte> out;
  auto put = [&](const void* p, std::size_t n) {
    auto* b = static_cast<const std::byte*>(p);
    out.insert(out.end(), b, b + n);
  };
  auto put_str = [&](const std::string& str) {
    auto len = static_cast<std::uint16_t>(str.size());
    put(&len, 2);
    put(str.data(), str.size());
  };
  put(&root, 4);
  put_str(s.table);
  auto n = static_cast<std::uint16_t>(s.cols.size());
  put(&n, 2);
  for (auto& c : s.cols) {
    auto t = static_cast<std::uint8_t>(c.type);
    put(&t, 1);
    put_str(c.name);
  }
  auto ni = static_cast<std::uint16_t>(s.indexed.size());
  put(&ni, 2);
  for (auto c : s.indexed) {
    auto v = static_cast<std::uint16_t>(c);
    put(&v, 2);
  }
  if (out.size() > 992) throw SqlError("schema too large for the catalog");
  return out;
}

Schema decode_schema(std::span<const std::byte> b, page_id_t& root) {
  Schema s;
  std::size_t pos = 0;
  auto get_str = [&] {
    auto len = load<std::uint16_t>(b.data() + pos);
    pos += 2;
    std::string str(reinterpret_cast<const char*>(b.data() + pos), len);
    pos += len;
    return str;
  };
  root = load<std::uint32_t>(b.data());
  pos = 4;
  s.table = get_str();
  auto n = load<std::uint16_t>(b.data() + pos);
  pos += 2;
  for (std::uint16_t i = 0; i < n; ++i) {
    auto t = static_cast<ColType>(load<std::uint8_t>(b.data() + pos));
    pos += 1;
    s.cols.push_back({get_str(), t});
  }
  auto ni = load<std::uint16_t>(b.data() + pos);
  pos += 2;
  for (std::uint16_t i = 0; i < ni; ++i) {
    s.indexed.push_back(load<std::uint16_t>(b.data() + pos));
    pos += 2;
  }
  return s;
}

}  // namespace aqo
