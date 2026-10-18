# ADR 0002: In-memory B+ tree design

Status: accepted

## Context
The legacy B-tree stored records in every node and left deletion from internal nodes unimplemented.
Range scans required an in-order walk of the whole tree.

## Decision
* **B+ tree**: values only in leaves; internal nodes hold separator keys. Leaves are doubly linked, so a range scan
  is one root-to-leaf descent plus a sequential walk. Deleting a key never needs "replace with predecessor": the
  gap in the separator set is harmless because separators are only *bounds*, which makes the internal-node-deletion
  gap in the legacy tree disappear by construction.
* **Order** = max keys per node (min keys = `Order/2`). Leaf overflow splits keep `floor((Order+1)/2)` left.
  Merge is always legal because `2 * floor(Order/2) <= Order`.
* **Delete**: borrow from the left sibling, else the right sibling, else merge (into the left when one exists).
  The root collapses when it is left with one child; an emptied tree keeps a single empty leaf.
* **No parent pointers; recursive descent.** Depth is O(log n), and parent pointers would have to be kept
  consistent across splits/merges. Cost: iterators are invalidated by writes (documented).
* **Verification**: `validate()` checks every structural invariant (depth, occupancy, ordering, bounds, links,
  cached aggregates). Property tests run random insert/assign/erase/find/scan sequences against `std::map` for
  orders 3, 4, 5, 8, 64 over many seeds and call `validate()` periodically. Order 3 gives very tall trees so
  borrow/merge at every level is exercised constantly.

## Consequences
* The in-memory tree is the algorithmic reference and the aggregate index (ADR 0003). The persistent, page-based
  tree (ADR 0004) re-implements the same algorithm over pages and is tested against the same `std::map` model.
