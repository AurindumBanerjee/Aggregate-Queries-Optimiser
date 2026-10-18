# Interview prep for aqo (SDE / systems)

Every number is from this repo's tests and `bench/results/` (median of 3 runs; one laptop; see ADR 0008).

## Resume bullets

* Built a single-node C++20 storage engine (B+ tree on 4KB slotted pages, LRU/Clock buffer pool, ARIES-style WAL with
  checkpoints and redo/undo recovery); validated by crash injection at every I/O point and 62 hard process kills per test
  run with zero consistency violations, then recovered 100k un-checkpointed inserts in 0.2–0.5 s.
* Designed aggregate-augmented B+ tree indexes (a B-ary segment tree) giving O(log n) filtered SUM/MIN/MAX/COUNT: 15 µs
  vs 1.47 ms for SQLite's covering index at 1% selectivity (~100x) and 31 µs vs 81 ms at 50% (~2,600x) on 1M rows,
  median of 3 runs.
* Wrote a SQL layer (tokenizer, parser, planner, executor, REPL) and verified it with a seeded differential fuzz
  against SQLite, mutation checks and 98 tests at 95% line coverage; found and fixed a Windows file-lock bug in log truncation.

## 60-second pitch

I took a course project, a toy database with a B-tree and segment trees that saved to CSV, and turned it into a real
storage engine. It stores data in 4KB pages through a buffer pool, with a write-ahead log and ARIES-style recovery, so a
crash never loses a committed transaction or exposes a half-done one. I proved that two ways: by simulating a crash at every
single I/O point, and by killing a real process at random times and checking the recovered database against a model.
The part I'm proudest of is aggregates. Instead of a textbook segment tree, which can't follow inserts and deletes,
I cached count/sum/min/max in the nodes of a B+ tree, so a filtered SUM costs O(log n). On a million rows that is about
100x faster than SQLite at 1% selectivity and thousands of times at 50%. I also measured where it loses: SQLite is
faster at sequential loading, point lookups and scans, and I report that. On top sits a small SQL layer I checked
against SQLite with differential fuzzing.

## 10 likely questions

1. **Why a B+ tree rather than a B-tree or LSM?** Values live only in leaves, so internal nodes hold just separators,
   fan-out is higher, and range scans follow a leaf chain. Deleting never needs predecessor swaps, which was the
   unimplemented part of the original. LSM would favour write throughput but complicates range aggregates and recovery;
   not worth it for a read-heavy aggregate workload.
2. **How does recovery work?** ARIES shape. Analysis scans the log from the last checkpoint to rebuild active
   transactions and dirty pages. Redo repeats history: reapply any record whose page LSN is older. Undo rolls back
   uncommitted transactions newest-first, writing compensation records so a crash during recovery is also recoverable.
3. **What is the WAL rule and where is it enforced?** A dirty page may not reach disk before the log up to its LSN is
   durable. It is one hook in the buffer pool's write-back path, which calls `Wal::flush(pageLSN)`.
4. **Why single writer?** It makes physical before-image undo sound: nothing else touched those pages since, so
   restoring images in reverse order is exact. Multi-writer B-tree ARIES needs logical undo and nested top actions.
   The cost is that writers block readers; I'd add MVCC next.
5. **How do you log changes without sprinkling log calls everywhere?** A write guard snapshots the page; on release the
   engine diffs the snapshot against the page and emits one record with changed byte ranges. Splits, merges and the
   free list are all logged automatically.
6. **How did you test crash safety, and what can't it show?** Fault injection at every I/O point (including torn log
   tails and crashes during recovery) plus real process kills. It cannot show torn data pages (I assume atomic 4KB
   writes and only detect corruption via CRC) or OS/disk lying about fsync. The kill test is nondeterministic, so passing
   is evidence, not proof; I saw one unexplained failure that did not reproduce.
7. **Why not a real segment tree for aggregates?** An array segment tree needs a fixed index universe; a merge-sort tree
   is static and a Fenwick tree needs known values. A B+ tree whose nodes cache the monoid fold supports inserts,
   updates and deletes in O(log n) and any range in O(log n). I still implemented classic segment tree and Fenwick as
   baselines and tests.
8. **What happens to the aggregate index on crash or abort?** It is derived data and never logged. It is rebuilt from the
   table at open (5–10 s at 1M rows, the known cost) and rolled back through a per-transaction undo log on abort.
9. **Where does SQLite beat you, and why?** Sequential inserts (2–4x), point lookups (~4x, once its Windows
   read-lock overhead is excluded) and full scans (7–10x). My scan decodes each row into `std::variant` with string
   allocation, and I have no prepared statements, so the SQL path parses every call. Fixes: row-format scans and prepared statements.
10. **How do you trust the benchmark numbers?** Three full runs with the median and min–max reported, because run 2 was
    ~2x slower for both engines (throttling). Aggregate answers are cross-checked against SQLite before timing, SQLite
    gets a covering index and its best-case read transaction, and I only claim ratios that held in every run.
