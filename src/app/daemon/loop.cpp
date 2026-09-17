#include "loop.hpp"

#include <chrono>

namespace atperson {

DaemonRunReport run_daemon(const DaemonConfig &config, LanguageGraph &graph, Ledger &ledger,
                           IngestionState &state, const SyncLimits &limits,
                           const SyncPageFetcher &fetch_page, const DaemonPersistence &persistence,
                           const DaemonHooks &hooks) {
    validate_daemon_config(config);

    DaemonRunReport report;
    Backoff backoff(config.backoff);
    bool dirty = false;

    SyncLimits cycle_limits = limits;
    cycle_limits.max_pages = config.pages_per_cycle;

    const auto should_stop = [&hooks]() { return hooks.stop_requested && hooks.stop_requested(); };
    const auto may_run = [&hooks]() { return !hooks.ready_to_run || hooks.ready_to_run(); };
    const auto wait = [&hooks](std::chrono::milliseconds duration) {
        if (hooks.sleep_for) {
            hooks.sleep_for(duration);
        }
    };

    while (true) {
        if (should_stop()) {
            break;
        }
        if (config.max_cycles != 0u && report.cycles >= config.max_cycles) {
            report.bounded = true;
            break;
        }
        if (!may_run()) {
            wait(config.poll_interval);
            continue;
        }

        try {
            const SyncResult result = run_sync(graph, ledger, state, fetch_page, cycle_limits);
            ++report.cycles;
            report.pages_completed += result.pages_completed;
            report.observations_seen += result.observations_seen;
            report.learned += result.learned;
            report.skipped += result.skipped;
            report.duplicates += result.duplicates;
            report.exhausted = result.exhausted;

            dirty = dirty || result.learned > 0u || result.skipped > 0u;
            backoff.reset();

            if (persistence.save_ingestion) {
                persistence.save_ingestion();
            }
            if (dirty && (report.cycles % config.snapshot_every_cycles) == 0u) {
                if (persistence.save_graph) {
                    persistence.save_graph();
                }
                ++report.snapshots_saved;
                dirty = false;
            }
            if (hooks.on_cycle) {
                hooks.on_cycle(result);
            }
        } catch (const RetryableError &error) {
            ++report.transient_failures;
            const std::chrono::milliseconds delay = backoff.next();
            if (hooks.on_backoff) {
                hooks.on_backoff(delay, error);
            }
            wait(delay);
            continue;
        }

        if (should_stop()) {
            break;
        }
        /* Do not sleep after the final bounded cycle: a short run should exit
         * promptly rather than waiting a poll interval it will never use. */
        if (config.max_cycles != 0u && report.cycles >= config.max_cycles) {
            report.bounded = true;
            break;
        }
        wait(report.exhausted ? config.poll_interval : config.catchup_interval);
    }

    report.stopped = should_stop();

    /* Graceful flush: always persist cursor progress, snapshot the graph if
     * it changed since the last snapshot. */
    if (persistence.save_ingestion) {
        persistence.save_ingestion();
    }
    if (dirty) {
        if (persistence.save_graph) {
            persistence.save_graph();
        }
        ++report.snapshots_saved;
    }

    return report;
}

} // namespace atperson
