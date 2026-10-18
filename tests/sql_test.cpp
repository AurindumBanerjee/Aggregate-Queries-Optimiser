#include "aqo/sql/executor.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "aqo/sql/expr.hpp"
#include "test_util.hpp"

namespace aqo::sql {
namespace {

struct TempDb {
  std::string path;
  explicit TempDb(const std::string& n) : path(aqo::testing::unique_path("sql_" + n, ".db")) { cleanup(); }
  ~TempDb() { cleanup(); }
  void cleanup() const {
    for (auto* ext : {"", ".wal", ".wal.tmp"}) std::filesystem::remove(path + ext);
  }
};
EngineOptions opts(const TempDb& d) {
  EngineOptions o;
  o.path = d.path;
  o.pool_frames = 256;
  o.sync_commit = false;
  return o;
}

// ---- lexer / parser ----

TEST(Lexer, TokensLiteralsAndComments) {
  auto t = tokenize("SELECT a, 'it''s', 1.5e2, 42, x<>y, p!=q -- comment\n;");
  std::vector<std::string> texts;
  for (auto& k : t) texts.push_back(k.text);
  EXPECT_EQ(texts, (std::vector<std::string>{"SELECT", "a", ",", "it's", ",", "1.5e2", ",", "42", ",", "x", "<>", "y", ",", "p", "<>", "q", ";", ""}));
  EXPECT_EQ(t[5].kind, Tok::kReal);
  EXPECT_EQ(t[7].kind, Tok::kInt);
  EXPECT_THROW(tokenize("select 'oops"), SqlError);
  EXPECT_THROW(tokenize("select #"), SqlError);
}

TEST(Parser, SelectShapes) {
  auto ps = parse_statement("SELECT a, SUM(b) AS total FROM t WHERE a > 1 AND NOT b < 2 GROUP BY a ORDER BY total DESC, 1 LIMIT 5;");
  auto& s = std::get<Select>(ps.stmt);
  EXPECT_EQ(s.table, "t");
  ASSERT_EQ(s.items.size(), 2u);
  EXPECT_EQ(s.items[1].alias, "total");
  EXPECT_EQ(s.items[1].expr->text, "SUM(b)");
  EXPECT_EQ(s.group_by.size(), 1u);
  ASSERT_EQ(s.order_by.size(), 2u);
  EXPECT_TRUE(s.order_by[0].desc);
  EXPECT_EQ(s.order_by[1].ordinal, 1);
  EXPECT_EQ(s.limit, 5);
  EXPECT_EQ(s.where->text, "a > 1 AND NOT b < 2");
}

TEST(Parser, OperatorPrecedenceAndBetween) {
  auto e = parse_expression("1 + 2 * 3 - 4 / 2");
  EXPECT_EQ(std::get<std::int64_t>(*fold_constant(*e)), 5);
  EXPECT_EQ(std::get<std::int64_t>(*fold_constant(*parse_expression("-(2 + 3) * 2"))), -10);
  EXPECT_TRUE(is_true(*fold_constant(*parse_expression("5 BETWEEN 1 AND 10"))));
  EXPECT_TRUE(is_true(*fold_constant(*parse_expression("5 NOT BETWEEN 6 AND 10"))));
  EXPECT_TRUE(is_true(*fold_constant(*parse_expression("1 = 2 OR 2 = 2 AND 3 = 3"))));
  EXPECT_EQ(std::get<std::int64_t>(*fold_constant(*parse_expression("-9223372036854775808"))), std::numeric_limits<std::int64_t>::min());
}

TEST(Parser, ThreeValuedLogicAndArithmeticEdges) {
  auto f = [](const char* s) { return *fold_constant(*parse_expression(s)); };
  EXPECT_TRUE(is_null(f("NULL = 1")));
  EXPECT_TRUE(is_null(f("NULL AND 1")));
  EXPECT_FALSE(is_true(f("NULL AND 0")));
  EXPECT_TRUE(is_true(f("NULL OR 1")));
  EXPECT_TRUE(is_null(f("1 / 0")));
  EXPECT_EQ(std::get<std::int64_t>(f("7 / 2")), 3);
  EXPECT_EQ(std::get<double>(f("7 / 2.0")), 3.5);
  EXPECT_EQ(std::get<std::int64_t>(f("-7 % 3")), -1);
  EXPECT_TRUE(std::holds_alternative<double>(f("9223372036854775807 + 1"))) << "overflow promotes to REAL";
  EXPECT_TRUE(is_true(f("NULL IS NULL")));
}

TEST(Parser, ErrorsCarryPositions) {
  for (const char* bad : {"SELEC 1", "SELECT FROM t", "SELECT a FROM", "INSERT INTO t VALUES", "CREATE TABLE t ()",
                          "SELECT a FROM t WHERE", "SELECT a FROM t LIMIT x", "UPDATE t SET", "SELECT SUM(*) FROM t"}) {
    try {
      parse_statement(bad);
      FAIL() << "should not parse: " << bad;
    } catch (const SqlError& e) {
      EXPECT_NE(std::string(e.what()).find("syntax error"), std::string::npos) << bad;
    }
  }
}

// ---- executor ----

struct Fx : ::testing::Test {
  TempDb d{::testing::UnitTest::GetInstance()->current_test_info()->name()};
  std::unique_ptr<Database> db = std::make_unique<Database>(opts(d));
  Session s{*db};
  void SetUp() override {
    s.execute("CREATE TABLE t (id INT PRIMARY KEY, a INT, b REAL, name TEXT, c INT)");
    s.execute("CREATE INDEX ON t (a)");
    s.execute("CREATE INDEX ON t (id)");
    s.execute(
        "INSERT INTO t VALUES (1, 10, 1.5, 'x', 100), (2, 20, 2.5, 'y', 200), (3, 30, 3.5, 'x', 300), (4, 20, 4.5, 'z', 400), (5, 50, 5.5, 'y', 500)");
  }
  ResultSet q(const std::string& sql) { return s.execute(sql); }
  static std::string cell(const ResultSet& r, std::size_t row, std::size_t col) { return to_string(r.rows.at(row).at(col)); }
};

TEST_F(Fx, SelectProjectionFilterOrderLimit) {
  auto r = q("SELECT id, a * 2 AS dbl FROM t WHERE a >= 20 AND name <> 'z' ORDER BY dbl DESC, id LIMIT 2");
  ASSERT_EQ(r.rows.size(), 2u);
  EXPECT_EQ(r.columns, (std::vector<std::string>{"id", "dbl"}));
  EXPECT_EQ(cell(r, 0, 0), "5");
  EXPECT_EQ(cell(r, 1, 0), "3");
  EXPECT_EQ(q("SELECT * FROM t WHERE id = 3").rows.at(0).size(), 5u);
  EXPECT_EQ(q("SELECT id FROM t ORDER BY id LIMIT 0").rows.size(), 0u);
}

TEST_F(Fx, AggregatesUseIndexAndMatchScan) {
  auto idx = q("SELECT COUNT(*), SUM(a), MIN(b), MAX(c), AVG(a) FROM t WHERE a > 10 AND a <= 30");
  EXPECT_NE(idx.plan.find("INDEX AGGREGATE"), std::string::npos) << idx.plan;
  EXPECT_EQ(cell(idx, 0, 0), "3");
  EXPECT_EQ(cell(idx, 0, 1), "70");
  EXPECT_EQ(cell(idx, 0, 2), "2.5");
  EXPECT_EQ(cell(idx, 0, 3), "400");
  s.options().use_aggregate_index = false;
  auto scan = q("SELECT COUNT(*), SUM(a), MIN(b), MAX(c), AVG(a) FROM t WHERE a > 10 AND a <= 30");
  EXPECT_NE(scan.plan.find("SCAN"), std::string::npos);
  EXPECT_EQ(scan.rows, idx.rows);
}

TEST_F(Fx, AggregateOverEmptyAndConstantFalse) {
  auto r = q("SELECT COUNT(*), SUM(a), MIN(a), AVG(b) FROM t WHERE a > 1000");
  EXPECT_EQ(cell(r, 0, 0), "0");
  EXPECT_EQ(cell(r, 0, 1), "NULL");
  EXPECT_EQ(cell(r, 0, 3), "NULL");
  auto f = q("SELECT COUNT(*) FROM t WHERE 1 = 2");
  EXPECT_NE(f.plan.find("EMPTY"), std::string::npos);
  EXPECT_EQ(cell(f, 0, 0), "0");
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t WHERE a = NULL"), 0, 0), "0");
}

TEST_F(Fx, AggregateExpressionsAndFractionalLiterals) {
  EXPECT_EQ(cell(q("SELECT SUM(a) / COUNT(*) FROM t"), 0, 0), "26");
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t WHERE a > 19.5 AND a < 30.5"), 0, 0), "3");
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t WHERE 20 < a"), 0, 0), "2") << "constant on the left";
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t WHERE a > 20.0"), 0, 0), "2") << "integral REAL literal, exclusive bound";
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t WHERE a < 30.0"), 0, 0), "3");
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t WHERE a BETWEEN 20 AND 30"), 0, 0), "3");
}

TEST_F(Fx, GroupByHavingStyleOrdering) {
  auto r = q("SELECT name, COUNT(*), SUM(c) FROM t GROUP BY name ORDER BY name");
  ASSERT_EQ(r.rows.size(), 3u);
  EXPECT_NE(r.plan.find("HASH GROUP BY"), std::string::npos);
  EXPECT_EQ(cell(r, 0, 0), "x");
  EXPECT_EQ(cell(r, 0, 2), "400");
  EXPECT_EQ(cell(r, 1, 1), "2");
  auto r2 = q("SELECT a, COUNT(*) FROM t WHERE c > 100 GROUP BY a ORDER BY 2 DESC, 1");
  EXPECT_EQ(cell(r2, 0, 0), "20");
  EXPECT_EQ(cell(r2, 0, 1), "2");
  EXPECT_THROW(q("SELECT name, a FROM t GROUP BY name"), SqlError);
  EXPECT_THROW(q("SELECT name, COUNT(*) FROM t"), SqlError);
}

TEST_F(Fx, UpdateAndDeleteKeepIndexesConsistent) {
  EXPECT_EQ(q("UPDATE t SET a = a + 1, b = b * 2 WHERE id >= 4").affected, 2u);
  EXPECT_EQ(cell(q("SELECT SUM(a) FROM t"), 0, 0), "132");
  EXPECT_EQ(q("DELETE FROM t WHERE a > 25").affected, 2u);
  auto r = q("SELECT COUNT(*), SUM(a) FROM t");
  EXPECT_NE(r.plan.find("INDEX AGGREGATE"), std::string::npos);
  EXPECT_EQ(cell(r, 0, 0), "3");
  EXPECT_EQ(cell(r, 0, 1), "51");
  EXPECT_THROW(q("UPDATE t SET id = 9"), SqlError);
  EXPECT_THROW(q("UPDATE t SET nope = 1"), SqlError);
  EXPECT_EQ(q("DELETE FROM t").affected, 3u);
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t"), 0, 0), "0");
}

TEST_F(Fx, ConstraintsAndTypeErrors) {
  EXPECT_THROW(q("INSERT INTO t VALUES (1, 1, 1.0, 'dup', 1)"), SqlError);
  EXPECT_THROW(q("INSERT INTO t VALUES (9, 1, 1.0, 'short')"), SqlError);
  EXPECT_THROW(q("INSERT INTO t VALUES (9, 'text', 1.0, 'n', 1)"), SqlError);
  EXPECT_THROW(q("INSERT INTO t (id, a) VALUES (9, 1)"), SqlError);
  EXPECT_THROW(q("SELECT nope FROM t"), SqlError);
  EXPECT_THROW(q("SELECT a FROM missing"), SqlError);
  EXPECT_THROW(q("SELECT a FROM t WHERE SUM(a) > 1"), SqlError);
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t"), 0, 0), "5") << "failed statements leave no partial effects";
}

TEST_F(Fx, ExplicitTransactionsCommitAndRollback) {
  q("BEGIN");
  EXPECT_TRUE(s.in_transaction());
  q("INSERT INTO t VALUES (6, 60, 6.5, 'w', 600)");
  EXPECT_EQ(cell(q("SELECT SUM(a) FROM t"), 0, 0), "190");
  q("ROLLBACK");
  EXPECT_EQ(cell(q("SELECT SUM(a) FROM t"), 0, 0), "130") << "rollback restores table and aggregate index";
  q("BEGIN");
  q("DELETE FROM t WHERE id = 1");
  q("COMMIT");
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t"), 0, 0), "4");
  EXPECT_THROW(q("COMMIT"), SqlError);
  q("BEGIN");
  q("DELETE FROM t WHERE id = 2");
  EXPECT_THROW(q("INSERT INTO t VALUES (3, 0, 0.0, 'dup', 0)"), SqlError);
  EXPECT_FALSE(s.in_transaction()) << "an error inside BEGIN rolls the transaction back";
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t"), 0, 0), "4");
}

TEST_F(Fx, DdlInsideTransactionRollsBack) {
  q("BEGIN");
  q("CREATE TABLE u (k INT PRIMARY KEY, v INT)");
  q("INSERT INTO u VALUES (1, 2)");
  q("ROLLBACK");
  EXPECT_THROW(q("SELECT * FROM u"), SqlError);
}

TEST_F(Fx, ExplainDoesNotExecute) {
  auto r = q("EXPLAIN DELETE FROM t WHERE id = 1");
  EXPECT_EQ(r.columns, std::vector<std::string>{"plan"});
  EXPECT_EQ(cell(q("SELECT COUNT(*) FROM t"), 0, 0), "5");
  auto p = q("EXPLAIN SELECT SUM(c) FROM t WHERE id BETWEEN 2 AND 4");
  EXPECT_NE(p.rows.at(0).at(0).index(), 0u);
  EXPECT_NE(to_string(p.rows[0][0]).find("INDEX AGGREGATE"), std::string::npos);
  EXPECT_NE(to_string(q("EXPLAIN SELECT name FROM t WHERE id = 3").rows[0][0]).find("pk[3, 3]"), std::string::npos);
}

TEST_F(Fx, SurvivesRestartAndRebuildsIndexes) {
  q("INSERT INTO t VALUES (6, 7, 0.5, 'q', 1)");
  db->engine().close();
  db.reset();
  db = std::make_unique<Database>(opts(d));
  Session s2(*db);
  auto r = s2.execute("SELECT COUNT(*), SUM(a) FROM t WHERE a < 25");
  EXPECT_NE(r.plan.find("INDEX AGGREGATE"), std::string::npos);
  EXPECT_EQ(to_string(r.rows[0][0]), "4");
  EXPECT_EQ(to_string(r.rows[0][1]), "57");
}

TEST_F(Fx, ScriptsRunInOrderAndStopAtFirstError) {
  auto out = s.execute_script("INSERT INTO t VALUES (7,1,1.0,'s',1); SELECT COUNT(*) FROM t; ");
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(to_string(out[1].rows[0][0]), "6");
  EXPECT_THROW(s.execute_script("SELECT 1 FROM t; SELECT nope FROM t; INSERT INTO t VALUES (8,1,1.0,'s',1)"), SqlError);
  EXPECT_EQ(to_string(q("SELECT COUNT(*) FROM t").rows[0][0]), "6");
}

}  // namespace
}  // namespace aqo::sql
