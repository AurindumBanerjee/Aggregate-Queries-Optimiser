# ADR 0007: SQL subset, planner and executor

Status: accepted

## Scope
`CREATE TABLE` (first column `INT PRIMARY KEY`), `CREATE INDEX ON t(col)` (an *aggregate index*, ADR 0003),
`INSERT ... VALUES`, `SELECT` with `WHERE`, `GROUP BY`, `ORDER BY`, `LIMIT`, `COUNT/SUM/AVG/MIN/MAX`,
`UPDATE`, `DELETE`, `BEGIN/COMMIT/ROLLBACK`, `EXPLAIN`. Types INT, REAL, TEXT; all columns NOT NULL.
Not supported: joins, subqueries, HAVING, DISTINCT, NULL columns/defaults, secondary (non-aggregate) indexes.

## Pipeline
tokenizer -> recursive-descent parser -> AST -> `bind` (resolve column names) -> planner -> executor.
Expression semantics copy SQLite where it matters (3-valued logic, integer division, `x/0` = NULL, integer overflow
promotes to REAL, INT/REAL compare numerically, numbers sort before text), so SQLite is usable as an oracle.

## Planner (rule based, no cost model)
1. Flatten `WHERE` into AND-ed conjuncts; fold constants (a false/NULL constant makes the query EMPTY).
2. `column <op> numeric-constant` conjuncts become per-column `ValueRange`s; anything else is a residual filter.
3. Aggregate-only query, no residual, ranges on at most one column, and that column (or any column, if no WHERE) has an
   aggregate index => **INDEX AGGREGATE**: O(log n), independent of rows matched.
4. Otherwise **SCAN**, restricted to the primary-key range when the predicate gives one (point lookups are `pk[3, 3]`),
   with residual filter, hash aggregation for GROUP BY (ordered map, so groups come out sorted), and a stable sort for
   ORDER BY unless the order is already primary-key order.
`EXPLAIN` prints the chosen path. `SessionOptions::use_aggregate_index=false` forces scans for differential testing.

## Transactions
Auto-commit per write statement; reads take no transaction. In an explicit transaction any statement error rolls the
whole transaction back (no savepoints), keeping the engine's physical-undo design simple.

## Verification
* Unit tests for lexer/parser/evaluator edge cases and each statement type, including rollback of DDL and indexes.
* **Differential fuzz vs SQLite** (tests/sqlite_diff_test.cpp): 4 seeds x 450 seeded statements (inserts incl. duplicate
  keys, updates, deletes, aggregates, GROUP BY, ORDER BY/LIMIT) run on aqo with and without the aggregate index and on
  SQLite; results must match (REAL within 1e-9 relative). ~0.5 s. Mutation checks confirmed it catches off-by-one range
  bugs in the index path; the generator is biased toward single-column ranges so the index plan is actually hit.
