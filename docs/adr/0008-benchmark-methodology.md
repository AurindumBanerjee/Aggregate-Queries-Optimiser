# ADR 0008: Benchmark methodology and what the numbers do (and do not) show

Status: accepted. Raw results: `bench/results/results.json` (+ `run.log`, `hardware.json`); regenerate with
`aqo_bench --rows 1000000` then `python bench/plot.py`.

## Setup
1,000,000 rows, schema `(id INT PK, a INT, b REAL, name TEXT, c INT)` (~100 B/row), seeded data identical for both
engines. Machine: Intel i5-1240P (12C/16T), 63.7 GiB RAM, WD SN770 NVMe, Windows 11, MinGW-w64 GCC 13.1 `-O3`,
SQLite 3.42.0. Both engines: 64 MiB cache, WAL, fsync-on-commit (`journal_mode=WAL`, `synchronous=FULL`), one commit per
10,000 rows. aqo checkpoints every 100k rows. Single run per number (no variance estimate), so treat differences under
~20% as noise. Aggregate answers are cross-checked against SQLite before timing; lookup checksums must match.

## Fairness decisions
* SQLite gets a **covering index (a,b,c)** for the aggregate queries (otherwise it would be forced into table scans),
  and read benchmarks are reported with SQLite both in autocommit and inside one open read transaction. On Windows the
  WAL read-transaction lock dominates autocommit lookups; the open-transaction number is SQLite's best case and is
  the one to quote.
* aqo has no prepared statements: the "SQL" rows include tokenizing, parsing and planning every call. The "direct API"
  rows isolate the data structure.

## Findings (1M rows)
| | aqo | SQLite |
|---|---|---|
| Sequential-key bulk insert | 194k rows/s | **745k rows/s (3.8x faster)** |
| Random-key insert | **80k rows/s** | 46k rows/s |
| Insert + indexes (aqo: 2 aggregate indexes; SQLite: 2 secondary) | **63k rows/s** | 29k rows/s |
| Point lookup (best case for each) | 132k ops/s, p50 2.0us, p99 40us | **541k ops/s**, p50 1.5us, p99 4us |
| Range SUM/COUNT/MAX, 0.01% / 1% / 50% of rows | **2.7 / 15 / 30 us** | 17.7 / 1,470 / 70,700 us |
| Full-table-scan aggregate | 520 ms | **67 ms (8x faster)** |
| Reopen incl. aggregate-index rebuild | 6.1 s | n/a |
| Crash recovery, 100k inserts un-checkpointed | 198 ms | n/a |

* **The headline result** is range aggregates: aqo's latency is nearly flat in selectivity (O(log n)), SQLite's grows
  linearly with rows matched, giving 6x at 0.01% and ~2,300x at 50%. This is the point of the project.
* **Honest negatives**: SQLite is faster at sequential bulk load, point reads, and scans. aqo's scan path decodes every row
  into `std::variant` values and allocates strings; there is no vectorisation and no row-format shortcut.
  aqo is faster on random-key inserts probably because its 4KB-page tree with in-place slot shifts degrades less than
  SQLite's under cache pressure at this size; I did not investigate further.
* LRU vs Clock on a skewed 80/20 workload with a 4 MiB pool: hit rates 75.8% vs 75.2%, throughput 45.6k vs 50.2k ops/s
  (10%, within noise). Clock's cheaper bookkeeping is the plausible reason; the policy choice does not matter much here.
* The aggregate index is in-memory and rebuilt on open (6 s at 1M rows, ~6 us/row), a real cost of the derived-data design
  (ADR 0003); bulk-loading the tree would shrink it.
