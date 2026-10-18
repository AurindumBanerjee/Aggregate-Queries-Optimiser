# ADR 0005: Buffer pool

Status: accepted

* Frames hold `pin_count`, `dirty`, `page_id`. `PageGuard` is RAII/move-only, so pins cannot leak (tested: every page
  has pin count 0 after a mixed workload). Fetching with all frames pinned throws instead of deadlocking.
* Replacement is behind a `Replacer` interface with **LRU** (list of unpinned frames, O(1)) and **Clock** (reference bit,
  two-sweep) so the policies can be benchmarked against each other (bench/).
* Write-back happens only on eviction or explicit flush. Before any dirty page is written the pool calls a
  **log-flush hook** with the page LSN: this is the WAL rule, enforced in one place (ADR 0006).
* Single-threaded by design in this milestone; ADR 0009 covers concurrency.
