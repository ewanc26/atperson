#ifndef ATPERSON_SYNC_PARALLEL_HPP
#define ATPERSON_SYNC_PARALLEL_HPP

#include "engine.hpp"
#include "worker/pool.hpp"

#include <functional>
#include <optional>

namespace atperson {

/*
 * Parallel bounded catch-up traversal with a one-stage fetch pipeline.
 *
 * The C23 core is single-threaded by contract, so this does not split the
 * learning work: every observation is still processed on the owner thread in
 * the same order as the sequential path, and the ledger commit order is
 * unchanged. What changes is that the network fetch for page N+1 overlaps
 * with the owner processing page N's observations, instead of blocking the
 * owner while the service round-trips.
 *
 * The pipeline is deterministic for fixed inputs: the same fetch results and
 * the same graph/ledger state produce the same SyncResult, because the owner
 * always drains results in cursor order and the ledger's deduplication is the
 * authority for what was already committed.
 *
 * The pool is fail-closed: a fetch failure is stored and re-thrown by the
 * owner, leaving the cursor and the ledger untouched. The owner never sees a
 * partial page.
 *
 * `pool` must outlive this call. If `pool` has one worker the behaviour is
 * identical to the sequential path, just through the queue.
 */
SyncResult run_sync_parallel(LanguageGraph &graph, Ledger &ledger,
                            IngestionState &state, const SyncPageFetcher &fetch_page,
                            const SyncLimits &limits, WorkerPool &pool,
                            const SyncLinker &link = nullptr);

} // namespace atperson

#endif