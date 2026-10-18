# ADR 0004: Disk B+ tree on slotted pages

Status: accepted

## Decisions
* **Clustered tree, int64 key -> bytes.** Values live in the leaf, so a primary-key lookup is one traversal and there is
  no separate heap file or RID indirection to keep consistent under splits.
* **Slotted pages with the slot directory in key order.** Insert/delete shift 4-byte slot entries, never record bytes;
  free space is reclaimed lazily by `compact()` (ADR-free: simple and measured cheap, 4KB memmove).
* **Fixed root page id.** When the root is full its contents are copied into a new child and the root is rewritten as
  an internal node. Catalog entries therefore never change, which removes a whole class of "update the pointer to the
  root atomically" recovery bugs.
* **Preemptive splits on insert** (split any full internal node while descending) so a split never has to propagate
  upward and a parent always has room for a separator: no path stack, one downward pass.
* **Delete rebalances bottom-up** with the rule: a node below 25% of page capacity is merged with a sibling if the
  union fits one page, otherwise the union is re-split evenly (this subsumes "borrow"). Merging frees the page to a
  free list stored in the meta page; reinserting reuses freed pages (tested).
* **No overflow pages**: values are limited to 992 bytes so every leaf holds >= 4 records. Documented limitation.
* **Per-page CRC32** is stamped on write and verified on read to detect torn/corrupt pages.

## Alternatives rejected
* Heap file + separate index: two structures to log and recover.
* Copy-on-write B-tree: great for crash safety but doubles write amplification and conflicts with the WAL milestone.

## Verification
`validate()` walks the tree (ordering, key bounds, uniform depth, leaf chain both directions, slotted-page accounting).
The property test runs 12,000 random insert/upsert/erase/get/scan operations vs `std::map` with a **10-frame** buffer
pool (constant eviction), for LRU and Clock, then drains the tree in random order.
