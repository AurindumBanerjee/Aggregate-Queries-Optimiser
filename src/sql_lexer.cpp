#include "aqo/sql/lexer.hpp"

#include <cctype>

namespace aqo::sql {
namespace {
std::string lowered(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

std::vector<Token> tokenize(const std::string& s) {
  std::vector<Token> out;
  std::size_t i = 0, n = s.size();
  auto push = [&](Tok k, std::string text, std::size_t pos) {
    Token t;
    t.kind = k;
    t.lower = lowered(text);
    t.text = std::move(text);
    t.pos = pos;
    out.push_back(std::move(t));
  };
  std::size_t counted = 0;
  while (i < n) {
    if (out.size() > counted) {
      out.back().end = i;
      counted = out.size();
    }
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c)) {
      ++i;
    } else if (c == '-' && i + 1 < n && s[i + 1] == '-') {  // line comment
      while (i < n && s[i] != '\n') ++i;
    } else if (std::isalpha(c) || c == '_') {
      std::size_t st = i;
      while (i < n && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
      push(Tok::kIdent, s.substr(st, i - st), st);
    } else if (std::isdigit(c) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      std::size_t st = i;
      bool real = false;
      while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
      if (i < n && s[i] == '.') {
        real = true;
        ++i;
        while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        std::size_t save = i;
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        if (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
          real = true;
          while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        } else {
          i = save;
        }
      }
      push(real ? Tok::kReal : Tok::kInt, s.substr(st, i - st), st);
    } else if (c == '\'') {
      std::size_t st = i++;
      std::string lit;
      bool closed = false;
      while (i < n) {
        if (s[i] == '\'') {
          if (i + 1 < n && s[i + 1] == '\'') {
            lit += '\'';
            i += 2;
            continue;
          }
          closed = true;
          ++i;
          break;
        }
        lit += s[i++];
      }
      if (!closed) throw SqlError("unterminated string literal at position " + std::to_string(st));
      push(Tok::kString, lit, st);
    } else {
      std::size_t st = i;
      auto two = (i + 1 < n) ? s.substr(i, 2) : std::string();
      if (two == "<>" || two == "!=" || two == "<=" || two == ">=") {
        push(Tok::kSymbol, two == "!=" ? "<>" : two, st);
        i += 2;
      } else if (std::string("(),;*+-/%=<>.").find(static_cast<char>(c)) != std::string::npos) {
        push(Tok::kSymbol, std::string(1, static_cast<char>(c)), st);
        ++i;
      } else {
        throw SqlError(std::string("unexpected character '") + static_cast<char>(c) + "' at position " + std::to_string(i));
      }
    }
  }
  if (out.size() > counted) out.back().end = i;
  push(Tok::kEnd, "", n);
  out.back().end = n;
  return out;
}

}  // namespace aqo::sql
