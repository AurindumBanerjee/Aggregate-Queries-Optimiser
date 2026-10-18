#include "aqo/sql/parser.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <set>

namespace aqo::sql {
namespace {

const std::set<std::string>& reserved() {
  static const std::set<std::string> kw = {"select", "from", "where", "group", "by", "order", "limit", "insert", "into",
                                           "values", "update", "set", "delete", "create", "table", "index", "on", "and",
                                           "or", "not", "between", "is", "null", "as", "asc", "desc", "begin", "commit",
                                           "rollback", "explain", "primary", "key"};
  return kw;
}

class Parser {
 public:
  explicit Parser(const std::string& sql) : sql_(sql), toks_(tokenize(sql)) {}

  std::vector<ParsedStatement> script() {
    std::vector<ParsedStatement> out;
    while (!at_end()) {
      if (accept_sym(";")) continue;
      out.push_back(statement());
      if (!at_end() && !is_sym(";")) fail("expected ';' or end of input");
    }
    return out;
  }

  ParsedStatement one() {
    auto st = statement();
    accept_sym(";");
    if (!at_end()) fail("unexpected trailing input");
    return st;
  }

  ExprPtr expression_only() {
    auto e = expr();
    if (!at_end()) fail("unexpected trailing input");
    return e;
  }

 private:
  // ---- token helpers ----
  const Token& cur() const { return toks_[pos_]; }
  bool at_end() const { return cur().kind == Tok::kEnd; }
  [[noreturn]] void fail(const std::string& msg) const {
    throw SqlError("syntax error at position " + std::to_string(cur().pos) + " near '" + cur().text + "': " + msg);
  }
  bool is_kw(const char* k) const { return cur().kind == Tok::kIdent && cur().lower == k; }
  bool accept_kw(const char* k) {
    if (is_kw(k)) {
      ++pos_;
      return true;
    }
    return false;
  }
  void expect_kw(const char* k) {
    if (!accept_kw(k)) fail(std::string("expected ") + k);
  }
  bool is_sym(const char* s) const { return cur().kind == Tok::kSymbol && cur().text == s; }
  bool accept_sym(const char* s) {
    if (is_sym(s)) {
      ++pos_;
      return true;
    }
    return false;
  }
  void expect_sym(const char* s) {
    if (!accept_sym(s)) fail(std::string("expected '") + s + "'");
  }
  std::string ident() {
    if (cur().kind != Tok::kIdent || reserved().count(cur().lower)) fail("expected an identifier");
    return toks_[pos_++].lower;
  }
  std::string source_since(std::size_t first_tok) const {
    std::size_t b = toks_[first_tok].pos, e = toks_[pos_ - 1].end;
    return sql_.substr(b, e - b);
  }

  // ---- statements ----
  ParsedStatement statement() {
    ParsedStatement ps;
    if (accept_kw("explain")) ps.explain = true;
    if (accept_kw("create")) {
      if (accept_kw("table")) ps.stmt = create_table();
      else if (accept_kw("index")) ps.stmt = create_index();
      else fail("expected TABLE or INDEX");
    } else if (accept_kw("insert")) {
      ps.stmt = insert();
    } else if (accept_kw("select")) {
      ps.stmt = select();
    } else if (accept_kw("update")) {
      ps.stmt = update();
    } else if (accept_kw("delete")) {
      ps.stmt = del();
    } else if (accept_kw("begin")) {
      ps.stmt = TxnControl{TxnControl::kBegin};
    } else if (accept_kw("commit")) {
      ps.stmt = TxnControl{TxnControl::kCommit};
    } else if (accept_kw("rollback")) {
      ps.stmt = TxnControl{TxnControl::kRollback};
    } else {
      fail("expected a statement");
    }
    return ps;
  }

  CreateTable create_table() {
    CreateTable ct;
    ct.name = ident();
    expect_sym("(");
    bool first = true;
    do {
      Column c;
      c.name = ident();
      if (cur().kind != Tok::kIdent) fail("expected a column type");
      std::string t = toks_[pos_++].lower;
      if (t == "int" || t == "integer" || t == "bigint") c.type = ColType::kInt;
      else if (t == "real" || t == "double" || t == "float") c.type = ColType::kReal;
      else if (t == "text" || t == "varchar" || t == "string") c.type = ColType::kText;
      else fail("unknown column type " + t);
      if (accept_kw("primary")) {
        expect_kw("key");
        if (!first) fail("PRIMARY KEY must be on the first column");
      }
      first = false;
      ct.cols.push_back(c);
    } while (accept_sym(","));
    expect_sym(")");
    return ct;
  }

  CreateIndex create_index() {
    CreateIndex ci;
    if (!is_kw("on")) ident();  // optional index name (not stored: indexes are named by table+column)
    expect_kw("on");
    ci.table = ident();
    expect_sym("(");
    ci.column = ident();
    expect_sym(")");
    return ci;
  }

  Insert insert() {
    Insert in;
    expect_kw("into");
    in.table = ident();
    if (accept_sym("(")) {
      do in.columns.push_back(ident());
      while (accept_sym(","));
      expect_sym(")");
    }
    expect_kw("values");
    do {
      expect_sym("(");
      std::vector<ExprPtr> row;
      do row.push_back(expr());
      while (accept_sym(","));
      expect_sym(")");
      in.rows.push_back(std::move(row));
    } while (accept_sym(","));
    return in;
  }

  Select select() {
    Select s;
    do {
      SelectItem it;
      if (is_sym("*")) {
        ++pos_;
      } else {
        std::size_t first = pos_;
        it.expr = expr();
        it.expr->text = source_since(first);
        if (accept_kw("as")) it.alias = ident();
        else if (cur().kind == Tok::kIdent && !reserved().count(cur().lower)) it.alias = ident();
      }
      s.items.push_back(std::move(it));
    } while (accept_sym(","));
    expect_kw("from");
    s.table = ident();
    if (accept_kw("where")) s.where = text_expr();
    if (accept_kw("group")) {
      expect_kw("by");
      do s.group_by.push_back(text_expr());
      while (accept_sym(","));
    }
    if (accept_kw("order")) {
      expect_kw("by");
      do {
        OrderTerm t;
        if (cur().kind == Tok::kInt) t.ordinal = static_cast<int>(std::strtol(toks_[pos_++].text.c_str(), nullptr, 10));
        else t.expr = text_expr();
        if (accept_kw("desc")) t.desc = true;
        else accept_kw("asc");
        s.order_by.push_back(std::move(t));
      } while (accept_sym(","));
    }
    if (accept_kw("limit")) {
      if (cur().kind != Tok::kInt) fail("LIMIT expects an integer");
      s.limit = std::strtoll(toks_[pos_++].text.c_str(), nullptr, 10);
    }
    return s;
  }

  Update update() {
    Update u;
    u.table = ident();
    expect_kw("set");
    do {
      std::string col = ident();
      expect_sym("=");
      u.sets.emplace_back(col, expr());
    } while (accept_sym(","));
    if (accept_kw("where")) u.where = text_expr();
    return u;
  }

  Delete del() {
    Delete d;
    expect_kw("from");
    d.table = ident();
    if (accept_kw("where")) d.where = text_expr();
    return d;
  }

  // ---- expressions ----
  static ExprPtr binary(Op op, ExprPtr l, ExprPtr r) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kBinary;
    e->op = op;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    return e;
  }
  static ExprPtr unary(Op op, ExprPtr x) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kUnary;
    e->op = op;
    e->lhs = std::move(x);
    return e;
  }
  static ExprPtr literal(Value v) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::kLiteral;
    e->literal = std::move(v);
    return e;
  }

  ExprPtr expr() { return or_expr(); }
  // Like expr(), but records the source text (used by EXPLAIN and ORDER BY / GROUP BY matching).
  ExprPtr text_expr() {
    std::size_t first = pos_;
    auto e = expr();
    e->text = source_since(first);
    return e;
  }
  ExprPtr or_expr() {
    auto l = and_expr();
    while (accept_kw("or")) l = binary(Op::kOr, l, and_expr());
    return l;
  }
  ExprPtr and_expr() {
    auto l = not_expr();
    while (accept_kw("and")) l = binary(Op::kAnd, l, not_expr());
    return l;
  }
  ExprPtr not_expr() {
    if (accept_kw("not")) return unary(Op::kNot, not_expr());
    return cmp_expr();
  }
  ExprPtr cmp_expr() {
    auto l = add_expr();
    for (;;) {
      if (cur().kind == Tok::kSymbol) {
        Op op;
        const std::string& s = cur().text;
        if (s == "=") op = Op::kEq;
        else if (s == "<>") op = Op::kNe;
        else if (s == "<") op = Op::kLt;
        else if (s == "<=") op = Op::kLe;
        else if (s == ">") op = Op::kGt;
        else if (s == ">=") op = Op::kGe;
        else return l;
        ++pos_;
        l = binary(op, l, add_expr());
        continue;
      }
      bool negate = false;
      std::size_t save = pos_;
      if (accept_kw("not")) negate = true;
      if (accept_kw("between")) {
        auto lo = add_expr();
        expect_kw("and");
        auto hi = add_expr();
        ExprPtr e = binary(Op::kAnd, binary(Op::kGe, l, lo), binary(Op::kLe, l, hi));
        l = negate ? unary(Op::kNot, e) : e;
        continue;
      }
      if (negate) {
        pos_ = save;
        return l;
      }
      if (accept_kw("is")) {
        bool neg = accept_kw("not");
        expect_kw("null");
        l = unary(neg ? Op::kIsNotNull : Op::kIsNull, l);
        continue;
      }
      return l;
    }
  }
  ExprPtr add_expr() {
    auto l = mul_expr();
    for (;;) {
      if (accept_sym("+")) l = binary(Op::kAdd, l, mul_expr());
      else if (accept_sym("-")) l = binary(Op::kSub, l, mul_expr());
      else return l;
    }
  }
  ExprPtr mul_expr() {
    auto l = unary_expr();
    for (;;) {
      if (accept_sym("*")) l = binary(Op::kMul, l, unary_expr());
      else if (accept_sym("/")) l = binary(Op::kDiv, l, unary_expr());
      else if (accept_sym("%")) l = binary(Op::kMod, l, unary_expr());
      else return l;
    }
  }
  ExprPtr unary_expr() {
    if (accept_sym("-")) {
      if (cur().kind == Tok::kInt && cur().text == "9223372036854775808") {
        ++pos_;
        return literal(std::numeric_limits<std::int64_t>::min());
      }
      return unary(Op::kNeg, unary_expr());
    }
    if (accept_sym("+")) return unary_expr();
    return primary();
  }
  ExprPtr primary() {
    const Token& t = cur();
    if (t.kind == Tok::kInt) {
      ++pos_;
      errno = 0;
      long long v = std::strtoll(t.text.c_str(), nullptr, 10);
      if (errno == ERANGE) return literal(std::strtod(t.text.c_str(), nullptr));
      return literal(static_cast<std::int64_t>(v));
    }
    if (t.kind == Tok::kReal) {
      ++pos_;
      return literal(std::strtod(t.text.c_str(), nullptr));
    }
    if (t.kind == Tok::kString) {
      ++pos_;
      return literal(t.text);
    }
    if (accept_sym("(")) {
      auto e = expr();
      expect_sym(")");
      return e;
    }
    if (t.kind == Tok::kIdent) {
      if (t.lower == "null") {
        ++pos_;
        return literal(Value{});
      }
      static const std::pair<const char*, AggFn> fns[] = {{"count", AggFn::kCount}, {"sum", AggFn::kSum}, {"avg", AggFn::kAvg},
                                                          {"min", AggFn::kMin},     {"max", AggFn::kMax}};
      if (pos_ + 1 < toks_.size() && toks_[pos_ + 1].kind == Tok::kSymbol && toks_[pos_ + 1].text == "(") {
        for (auto& [name, fn] : fns) {
          if (t.lower == name) {
            std::size_t first = pos_;
            pos_ += 2;
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::kAggregate;
            e->fn = fn;
            if (accept_sym("*")) {
              if (fn != AggFn::kCount) fail("only COUNT accepts *");
              e->star = true;
            } else {
              e->arg = expr();
            }
            expect_sym(")");
            e->text = source_since(first);
            return e;
          }
        }
        fail("unknown function " + t.text);
      }
      auto e = std::make_shared<Expr>();
      e->kind = Expr::Kind::kColumn;
      e->name = ident();
      return e;
    }
    fail("expected an expression");
  }

  const std::string& sql_;
  std::vector<Token> toks_;
  std::size_t pos_ = 0;
};

}  // namespace

std::vector<ParsedStatement> parse_script(const std::string& sql) { return Parser(sql).script(); }
ParsedStatement parse_statement(const std::string& sql) { return Parser(sql).one(); }
ExprPtr parse_expression(const std::string& sql) { return Parser(sql).expression_only(); }

}  // namespace aqo::sql
