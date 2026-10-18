// ARIES-style crash recovery (analysis -> redo -> undo) and the shared physical-undo primitive.
#pragma once

#include <functional>

#include "aqo/buffer_pool.hpp"
#include "aqo/wal.hpp"

namespace aqo {

struct RecoveryStats {
  bool ran = false;
  lsn_t analysis_start = 0;
  lsn_t redo_start = 0;
  std::size_t records_scanned = 0;
  std::size_t redone = 0;        // updates actually re-applied
  std::size_t redo_skipped = 0;  // page already contained the update (pageLSN >= lsn)
  std::size_t losers = 0;        // transactions rolled back
  std::size_t clrs_written = 0;
  double millis = 0;
};

/// Brings the data file to a transaction-consistent state using the log:
///  1. analysis from the last checkpoint rebuilds the active-transaction and dirty-page tables;
///  2. redo repeats history for every logged change whose page LSN is older than the record;
///  3. undo rolls back every transaction that has no COMMIT/ABORT, writing compensation records.
/// Pages are modified through the pool but not flushed; the caller checkpoints afterwards.
RecoveryStats recover(Wal& wal, BufferPool& pool);

/// Undoes one kUpdate record: restores the before-images, logs a CLR chained after `txn_last_lsn`,
/// stamps the page with the CLR's LSN and returns it.
lsn_t undo_update(Wal& wal, BufferPool& pool, const LogRecord& update, lsn_t txn_last_lsn,
                  const std::function<void(page_id_t, lsn_t)>& on_dirty = {});

}  // namespace aqo
