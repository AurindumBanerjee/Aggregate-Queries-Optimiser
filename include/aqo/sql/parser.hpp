// Recursive-descent SQL parser.
//
//   stmt   := [EXPLAIN] ( CREATE TABLE | CREATE INDEX | INSERT | SELECT | UPDATE | DELETE | BEGIN | COMMIT | ROLLBACK )
//   expr   := OR-chain of AND-chains of NOT / comparison / BETWEEN / IS [NOT] NULL over + - * / % and unary -
#pragma once

#include "aqo/sql/ast.hpp"
#include "aqo/sql/lexer.hpp"

namespace aqo::sql {

/// Parses one or more ';'-separated statements. Throws SqlError with a position on syntax errors.
std::vector<ParsedStatement> parse_script(const std::string& sql);
/// Parses exactly one statement (a trailing ';' is allowed).
ParsedStatement parse_statement(const std::string& sql);
/// Parses a standalone expression (used by tests and the planner's constant folding).
ExprPtr parse_expression(const std::string& sql);

}  // namespace aqo::sql
