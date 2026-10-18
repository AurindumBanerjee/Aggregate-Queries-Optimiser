# ADR 0009: Concurrency = many readers XOR one writer

Status: accepted (stretch goal; deliberately minimal)

## Decision
* `Database::rw_mutex()` is a `std::shared_mutex`. A SELECT takes it shared for the statement; an auto-commit write takes
  it exclusively for the statement; `BEGIN` takes it exclusively and the session holds it until COMMIT/ROLLBACK (or
  its destructor). Readers therefore only ever observe committed state: serialisable, with no MVCC machinery.
* `BufferPool` has one internal mutex around its tables, replacer, stats and frame metadata, because concurrent readers
  pin/unpin and evict pages even though they never modify them.
* This keeps ADR 0006's single-writer assumption true, so physical before-image undo and the txn-listener index rollback
  stay correct with no changes.

## Consequences / limits
* A long write transaction blocks all readers (and vice versa). No fairness guarantee: `std::shared_mutex` is
  implementation-defined, so a tight writer loop can starve readers (observed in testing: 300 back-to-back transfers
  finished while readers completed only 40 queries).
* Commit holds the exclusive lock while fsyncing: no group commit.
* One global pool mutex serialises page fetches; this is a throughput limit, not a correctness one. Per-frame latches
  with a sharded page table would be the next step.
* **Tested** (tests/concurrency_test.cpp): money-transfer invariant seen by 4 concurrent readers using both index and scan
  plans, 4 writers doing read-modify-write with no lost updates, 6 readers over a 24-frame pool (constant eviction), and
  checkpoints racing readers. CI also builds with ThreadSanitizer (Linux); I could not run TSan locally (MinGW has none),
  so that job is unverified until it runs on GitHub.
