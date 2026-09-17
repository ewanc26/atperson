#ifndef ATPERSON_DAEMON_CONFIG_HPP
#define ATPERSON_DAEMON_CONFIG_HPP

// Daemon scheduling and retry configuration (#21).
//
// Owns the operator-tunable knobs for a long-running ingestion daemon: how
// much work each sync cycle may traverse, how long to wait between cycles,
// how often to snapshot, and the retry backoff shape. Per-fetch work limits
// (page size, observation budget) stay owned by the resource budget so the
// daemon behaves like the one-shot `sync` command; this scope owns only
// daemon-specific scheduling.
//
// Parsing is a pure app concern — no network, no core mutation — and every
// value is validated so an impossible configuration fails at startup rather
// than mid-run.
//
// Environment overrides (all optional):
//   ATPERSON_DAEMON_PAGES_PER_CYCLE
//   ATPERSON_DAEMON_POLL_MS
//   ATPERSON_DAEMON_CATCHUP_MS
//   ATPERSON_DAEMON_MAX_CYCLES
//   ATPERSON_DAEMON_SNAPSHOT_EVERY
//   ATPERSON_DAEMON_BACKOFF_INITIAL_MS
//   ATPERSON_DAEMON_BACKOFF_MAX_MS
//   ATPERSON_DAEMON_BACKOFF_FACTOR
//   ATPERSON_DAEMON_BACKOFF_JITTER

#include "backoff.hpp"

#include <chrono>
#include <cstdint>

namespace atperson {

/* Tuning for one daemon run. `max_cycles` 0 means run until stopped. */
struct DaemonConfig {
    int pages_per_cycle{1};

    /* Wait after a cycle that exhausted the timeline (nothing new to fetch)
     * versus after a cycle that still has catch-up pending. A zero
     * catch-up interval means "continue immediately". */
    std::chrono::milliseconds poll_interval{300000};
    std::chrono::milliseconds catchup_interval{0};

    std::uint64_t max_cycles{0};
    std::uint64_t snapshot_every_cycles{1};

    BackoffConfig backoff;
};

/* Read overrides from the environment on top of the defaults. Malformed
 * values throw std::runtime_error naming the variable. */
[[nodiscard]] DaemonConfig daemon_config_from_environment();

/* Reject impossible values (non-positive cycle bounds, zero snapshot
 * cadence, a backoff maximum below its initial delay, out-of-range factor or
 * jitter). Throws std::runtime_error. */
void validate_daemon_config(const DaemonConfig &config);

} // namespace atperson

#endif
