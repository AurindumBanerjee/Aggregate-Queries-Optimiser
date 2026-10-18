// SQL tokenizer.
#pragma once

#include <string>
#include <vector>

#include "aqo/value.hpp"

namespace aqo::sql {

enum class Tok { kIdent, kInt, kReal, kString, kSymbol, kEnd };

struct Token {
  Tok kind = Tok::kEnd;
  std::string text;   // original spelling (string literals: unescaped contents)
  std::string lower;  // lower-cased text for keyword/identifier matching
  std::size_t pos = 0;  // byte offsets [pos, end) in the source
  std::size_t end = 0;
};

/// Splits `sql` into tokens, ending with a kEnd token. Throws SqlError on bad input.
/// Symbols: ( ) , ; * + - / % = <> != < <= > >= .
std::vector<Token> tokenize(const std::string& sql);

}  // namespace aqo::sql
