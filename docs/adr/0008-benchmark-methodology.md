# ADR 0008: Benchmark methodology and what the numbers do (and do not) show

Status: accepted. Raw data: `bench/results/runs/run{1,2,3}.json (run1 = before pool mutex)` (+ logs), merged by `python bench/plot.py` into
`bench/results/results.json`, `bench/results/table.md` and `docs/bench_*.png`. Machine: `bench/results/hardware.json`.

## Setup
1,000,000 rows, schema `(id INT PK, a INT, b REAL, name TEXT, c INT)` (~100 B/row), seeded data identical for both
engines. Intel i5-1240P (12C/16T) laptop, 63.7 GiB RAM, WD SN770 NVMe, Windows 11, MinGW-w64 GCC 13.1 `-O3`,
SQLite 3.42.0. Both engines: 64 MiB cache, WAL, fsync-on-commit (`journal_mode=WAL`, `synchronous=FULL`), one commit per
10,000 rows; aqo checkpoints every 100k rows. Aggregate answers are cross-checked against SQLite before timing and
point-lookup checksums must match.

**Three full runs; the tables report the median and (min-max).** Run 1 was taken before the buffer-pool mutex (ADR 0009),
runs 2 and 3 after. Run 2 was slow for *both* engines (SQLite sequential insert 745k -> 305k rows/s), so the laptop was
throttled or busy; run 3 matches run 1. Lesson: absolute numbers on this machine move by up to ~2.5x between runs, so
only ratios that hold in all three runs are claimed, and differences under ~2x between absolute numbers are noise.
The mutex's cost is not separable from that noise, but run 3 (with mutex) is within the run 1 (without) spread.

## Fairness decisions
* SQLite gets a **covering index (a,b,c)** for the aggregate queries (otherwise it would scan the table), and reads are
  reported both in autocommit and inside one open read transaction. On Windows the WAL read-transaction lock dominates
  autocommit lookups; the open-transaction number is SQLite's best case and is the one to compare against.
* aqo has no prepared statements: the "SQL" rows include tokenizing, parsing and planning on every call. The
  "direct API" rows isolate the data structure.

## Findings (median of 3 runs, 1M rows)
| | aqo | SQLite | holds in all 3 runs? |
|---|---|---|---|
| Sequential-key bulk insert | 194k rows/s | **745k rows/s** | SQLite 2.1-4.2x faster |
| Random-key insert | **80k rows/s** | 41k rows/s | aqo 1.8-2.7x faster |
| Insert + indexes (aqo: 2 aggregate; SQLite: 2 secondary) | **42k rows/s** | 18k rows/s | aqo 1.6-2.5x faster |
| Point lookup, best case each | 132k ops/s (p50 2.0us, p99 37-66us) | **541k ops/s** (p50 1.5us) | SQLite 3.7-5.0x faster |
| Range SUM/COUNT/MAX at 0.01% / 1% / 50% of rows | **4 / 15 / 31 us** | 22 / 1,473 / 81,486 us | aqo 5-7x / 98-112x / 2,340-2,650x |
| Full-table-scan aggregate (1%-range query, no usable index) | 520 ms | **71 ms** | SQLite 7.5-10x faster |
| Reopen incl. aggregate-index rebuild | 5.4-10 s | n/a | |
| Crash recovery of 100k un-checkpointed inserts | 198-487 ms | n/a | |

* **The headline result** is range aggregates: aqo's latency is nearly flat in selectivity (O(log n)); SQLite's grows
  linearly with rows matched. At 1% of rows that is ~100x and at 50% ~2,300-2,650x (direct-API p50), and the ratios held in every run.
* **Honest negatives**: SQLite is faster at sequential bulk load, point reads and scans. aqo's scan decodes each row
  into `std::variant` values and allocates strings; there is no vectorisation or row-format shortcut. aqo's win on
  random-key inserts is real in all runs but I did not investigate why (candidate: SQLite's per-commit WAL
  checkpointing and 64 MiB cache pressure at this size). The aggregate index is memory-resident and rebuilt on open
  (ADR 0003): that is the price of the derived-data design.
* LRU vs Clock, skewed 80/20 reads, 4 MiB pool: hit rate 75.8% vs 75.2%; throughput differs by less than the
  run-to-run noise (LRU won one run, Clock another). The policy choice does not matter at this scale.
* Single machine, single table shape, uniform random keys, warm cache for reads. Nothing here says how either engine
  behaves on other hardware, schemas, concurrent load, or datasets larger than RAM.
