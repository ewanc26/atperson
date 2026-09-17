#ifndef ATPERSON_DAEMON_LOOP_HPP
#define ATPERSON_DAEMON_LOOP_HPP

// Long-running ingestion cycle orchestrator (#21).
//
// Owns scheduling, bounded per-cycle work, retry backoff, snapshot cadence
// and graceful shutdown for the daemon. It is deliberately network- and
// Wolfram-free: fetching, sleeping, clock-independent stopping and durable
// persistence are injected, so the whole control flow is testable offline by
// driving hooks. The CLI daemon supplies the real transport (converting
// transport failures to RetryableError), real sleeps, signals and atomic
// persistence.
//
// Invariants:
//  - deterministic for fixed inputs/hooks; no hidden global state;
//  - only RetryableError is retried; any other exception is fatal and
//    propagates without a final snapshot attempt;
//  - ingestion state is persisted after every cycle and once more on exit;
//  - the graph snapshot is persisted on the configured cadence and on a
//    graceful exit whenever the graph is dirty.
//
// Ownership: run_daemon borrows graph/ledger/state and never outlives them.
// The caller owns loading and saving layout; the daemon only calls the
// injected persistence callbacks.

#include "backoff.hpp"
#include "config.hpp"
#include "failure.hpp"
#include "sync/engine.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

namespace atperson {

/* One bounded sync run. The daemon supplies either the sequential runner or
 * the parallel one (which overlaps fetches on a worker pool); the loop is
 * unchanged either way, so scheduling, backoff, snapshot cadence and graceful
 * shutdown are identical. */
using SyncRunner = std::function<SyncResult(LanguageGraph &, Ledger &, IngestionState &,
                                           const SyncLimits &, const SyncLinker &)>;

/* Durable writes the daemon performs. Both must be atomic/durable; a throw
 * is fatal and stops the daemon. */
struct DaemonPersistence {
    std::function<void()> save_ingestion;
    std::function<void()> save_graph;
};

/* Injectable runtime concerns so the loop stays offline-testable.
 * `stop_requested` (signals/operator shutdown) and `ready_to_run` (operator
 * pause) default to "not stopped" and "ready" when unset. */
struct DaemonHooks {
    std::function<void(std::chrono::milliseconds)> sleep_for;
    std::function<bool()> stop_requested;
    std::function<bool()> ready_to_run;
    std::function<void(const SyncResult &)> on_cycle;
    std::function<void(std::chrono::milliseconds, const RetryableError &)> on_backoff;
};

/* Aggregate accounting for one daemon run. `bounded` is true when the run
 * stopped because `max_cycles` was reached rather than because of shutdown. */
struct DaemonRunReport {
    std::uint64_t cycles{};
    std::uint64_t pages_completed{};
    std::uint64_t observations_seen{};
    std::size_t learned{};
    std::size_t skipped{};
    std::size_t duplicates{};
    std::uint64_t transient_failures{};
    std::uint64_t snapshots_saved{};
    bool stopped{};
    bool exhausted{};
    bool bounded{};
};

/*
 * Run the ingestion loop until shutdown, a fatal error, or the configured
 * cycle bound. `limits` is the per-cycle traversal budget (page size and
 * observation cap come from the resource budget; max_pages from
 * DaemonConfig::pages_per_cycle). `run_sync` is the sync runner; the loop
 * does not know whether it is sequential or parallel. `link` is the #27
 * action-event linker, fired per observation under the same durability
 * invariant as the ledger commit; a null linker disables linkage. Returns
 * the report; throws on fatal (non-retryable) errors.
 */
DaemonRunReport run_daemon(const DaemonConfig &config, LanguageGraph &graph, Ledger &ledger,
                           IngestionState &state, const SyncLimits &limits,
                           const SyncRunner &run_sync, const DaemonPersistence &persistence,
                           const DaemonHooks &hooks, const SyncLinker &link = nullptr);

/*
 * Convenience overload: wrap a `SyncPageFetcher` in a sequential `SyncRunner`
 * and delegate to the runner form. Kept so the daemon test and any other
 * caller that has only a fetcher do not need to build a runner closure.
 */
DaemonRunReport run_daemon(const DaemonConfig &config, LanguageGraph &graph, Ledger &ledger,
                           IngestionState &state, const SyncLimits &limits,
                           const SyncPageFetcher &fetch_page, const DaemonPersistence &persistence,
                           const DaemonHooks &hooks, const SyncLinker &link = nullptr);

} // namespace atperson

#endif
