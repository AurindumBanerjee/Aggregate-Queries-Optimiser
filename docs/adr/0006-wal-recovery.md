# ADR 0006: Write-ahead logging and ARIES-style recovery

Status: accepted

## Decisions
* **Physiological logging with before/after images.** A `fetch_write` guard snapshots the 4KB page; when the guard is
  released the engine diffs snapshot vs. page and appends one `UPDATE` record holding the changed byte ranges (ranges
  closer than 24 bytes are merged). B+ tree code needs no logging calls at all, and *every* structural change (splits,
  merges, free-list, allocator counters) is logged for free. The LSN/CRC bytes (0..11) are excluded from diffs.
* **Single writer** (one open `Txn`). This is what makes *physical* undo sound: when a transaction rolls back, no other
  transaction has touched its pages since, so restoring before-images in reverse LSN order is exact. It also removes
  the need for logical undo and nested top actions that multi-writer B-tree ARIES requires. Revisit with ADR 0009.
* **Steal / no-force.** Dirty pages of uncommitted transactions may be evicted (WAL rule enforced in
  `BufferPool::write_back` -> `Wal::flush(pageLSN)`); commit forces only the log (one fsync), never data pages.
* **Recovery** = analysis (from the checkpoint's active-txn and dirty-page tables) -> redo (repeat history: reapply a
  record iff `pageLSN < record.lsn`) -> undo (roll back losers newest-first, writing CLRs whose `undo_next` skips
  already-undone work, so a crash *during* recovery is also recoverable).
* **Checkpoint** = flush all dirty pages, fsync data file, log a checkpoint record, then (if no txn is active)
  atomically replace the log with an empty one. LSNs continue from the old end (`base_lsn` in the header) so page LSNs
  stay comparable.
* **Log framing**: `len | crc32 | payload`. The first bad frame ends the log, so a torn tail is simply discarded.
* **Writes outside a transaction are rejected** (`write gate`), so no half-finished multi-page operation can ever be
  logged as "system" activity.

## Not handled (documented limitations)
* **Torn data-page writes**: pages carry a CRC so corruption is *detected* (open fails), but there is no double-write
  buffer or full-page image, so a page torn by power loss mid-write is not repaired. The fault model assumes atomic
  4KB page writes.
* The log buffer lives in memory until commit/eviction/checkpoint, so one very large transaction costs O(log) RAM.
* Files > 2GB on platforms where `long` is 32 bits (stdio `fseek`).

## Verification (see tests/)
* `CrashSweep`: for *every* I/O point (page write, log write, fsync) of a 40-transaction workload with a 14-frame pool, crash,
  simulate power loss (sometimes dropping the unsynced log tail, sometimes leaving a torn record), optionally crash again
  during recovery, recover, and require the database to equal the model after exactly the acknowledged commits (plus
  the in-flight one if the crash was inside `commit()`); then keep using the database.
* `KillNine`: a real worker process is hard-killed at random times (TerminateProcess/SIGKILL) over many rounds, with and
  without fsync and around checkpoints; the parent recovers and checks the same invariant.
