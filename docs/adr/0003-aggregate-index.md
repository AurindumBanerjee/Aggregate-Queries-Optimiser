# ADR 0003: Aggregate index = B-ary segment tree (aggregate-augmented B+ tree)

Status: accepted. **Deviates from the literal brief** ("segment tree + merge-sort/Fenwick tree, O(log^2 n)"), see below.

## Problem
The legacy `rangeSumGreaterThan` was O(n log n) (it sorted/scanned to answer `SUM(x) WHERE x > t`), and the segment
tree was an array over a fixed snapshot, so it could not follow inserts/updates/deletes.

## Decision
Each aggregate index is an in-memory B+ tree keyed by `(filter-column value, primary key)`. Every internal node caches
the monoid fold `(count, per-numeric-column sum/min/max)` of each child subtree. A query for any range of the filter
column visits at most two root-to-leaf paths and reads cached folds for everything in between:

* `SELECT SUM|MIN|MAX|COUNT|AVG(any numeric col) WHERE filter_col <op> literal` -> **O(log n)** node visits
  (times the fan-out of 32 for the two boundary nodes), independent of the number of matching rows.
* Insert / update / delete refresh one root-to-leaf path: **O(log n)**.
* One index serves every numeric target column (payload holds up to 6 numeric columns), so the cost of the "filter on x,
  aggregate y" case is paid once per filter column, not per pair.

This is a segment tree whose fan-out is 32 instead of 2 and whose leaves are the sorted rows themselves. It is *better*
than the requested O(log^2 n): the merge-sort tree and Fenwick-over-compressed-values need a **static value universe**
(a merge-sort tree cannot be updated without rebuilding O(n) lists; a Fenwick tree needs the set of distinct values known
up front, so inserting an unseen value forces a rebuild).

Classic `SegmentTree` and `Fenwick` (include/aqo/range_structs.hpp) are still implemented, tested, and benchmarked as
baselines (bench/), so the claim is measured rather than asserted.

## Consistency with the durable state
The index is **derived data**, never logged:
* rebuilt from the table scan when the database opens (so crash recovery needs no index redo);
* mutated together with the tree on every write;
* rolled back via a per-transaction undo log when the transaction aborts (`StorageEngine` txn listeners).

## Limits
* SUM over INT columns uses 128-bit accumulators (exact; the executor errors if the result does not fit int64).
  SUM over REAL is a floating-point sum whose association order differs from a sequential scan, so results can
  differ in the last bits (tests use a relative tolerance; SQLite uses Kahan-Babuska summation).
* At most 6 numeric columns per indexed table; memory is ~72 bytes per row per index; rebuild on open is O(n log n).
* NaN is rejected on insert so that REAL ordering is total.
