#ifndef ATPERSON_TESTS_LIFECYCLE_HARNESS_HPP
#define ATPERSON_TESTS_LIFECYCLE_HARNESS_HPP

// Shared fixtures for the deterministic end-to-end lifecycle harness.
//
// The harness stands in for a host: a scratch data directory holding a
// real ledger, graph snapshot and ingestion state, plus a scripted page
// fetcher that replaces the network client. Scenarios are grouped by
// concern into their own translation units (see the run_* entry points)
// so no single file owns more than one story; they share these types
// rather than each growing a private copy.
//
// Everything here is offline and deterministic. Timestamps in the feed
// fixture are fixed strings, so no wall-clock value can leak into learned
// state and two equivalent runs must compare byte-equal.

#include "engine.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson::e2e {

/* A fresh, empty scratch directory for one scenario. Removes any previous
 * run's contents, so a scenario is never polluted by an earlier failure. */
[[nodiscard]] std::filesystem::path scratch_dir(std::string_view tag);

/* ---------------------------------------------------------------- */
/* Scripted feed fixture                                             */
/* ---------------------------------------------------------------- */

/* One scripted page: items plus the next cursor. `failure` makes the
 * fetcher throw instead of returning the page (retryable transport
 * failure). */
struct ScriptedPage {
    std::vector<SyncObservation> items;
    std::optional<std::string> next_cursor;
    bool failure{false};
};

/* Convenience for a single observation with the fixture's fixed default
 * author and timestamp. */
[[nodiscard]] SyncObservation obs(std::string_view uri, std::string_view text,
                                  std::string_view author = "did:plc:author",
                                  std::string_view created_at = "2026-09-16T00:00:00Z");

/* A feed is a function from the incoming cursor to the scripted page.
 * The cursor is the page index: null -> page 0 -> page 1 ... until a
 * scripted page has no next_cursor (exhausted). */
class ScriptedFeed {
  public:
    explicit ScriptedFeed(std::vector<ScriptedPage> pages);

    SyncPage operator()(const std::optional<std::string> &cursor);

  private:
    std::vector<ScriptedPage> pages_;
};

/* ---------------------------------------------------------------- */
/* Durable scenario runner                                           */
/* ---------------------------------------------------------------- */

/* Paths and run() mirror the CLI command composition (sync.cpp run order):
 * load state -> run_sync -> save snapshot -> save state. The runner is the
 * fixture equivalent of `atperson sync N` with the scripted feed standing in
 * for the network client.
 *
 * A scenario holds the paths of one host's data directory. Recovery
 * scenarios destroy and recreate the directory; they must construct a new
 * Scenario rather than reuse one, because the paths belong to the host,
 * not to the run. */
struct Scenario {
    std::filesystem::path dir;
    std::filesystem::path ledger_file;
    std::filesystem::path model_file;
    std::filesystem::path state_file;

    explicit Scenario(std::string_view tag);

    /* One process run: open the durable files, run the sync, persist.
     * `crash_after_sync` simulates an unclean exit after the given
     * run_sync return but before the snapshot/state saves (crash point
     * between the ledger commits and the snapshot write). */
    SyncResult run(const SyncPageFetcher &feed, int max_pages, bool crash_after_sync = false);

    /* One daemon cycle, pause gate included: the operator gate in the
     * daemon (src/app/cli/daemon.cpp) is a durable read of
     * `load_control_state(control_file).paused`, so a paused host opens
     * nothing, fetches nothing and writes nothing for that cycle. This
     * harness reads the same file through the same production function, so
     * a scenario asserting on a pause is asserting on the real gate rather
     * than on a fixture's copy of it.
     *
     * `paused` is true when the gate stopped the cycle, in which case
     * `result` is the zero value and no durable file was touched. */
    struct Cycle {
        SyncResult result;
        bool paused{false};
    };
    Cycle run_cycle(const SyncPageFetcher &feed, int max_pages, bool crash_after_sync = false);

    /* The additional durable files the recovery scenarios need, derived
     * from this host's directory so a scenario never hard-codes a layout
     * the harness and the CLI disagree about. */
    [[nodiscard]] std::filesystem::path journal_file() const;
    [[nodiscard]] std::filesystem::path control_file() const;
};

/* A canonical digest of the final learned state: snapshot bytes plus the
 * durable ledger contents. Equivalent scenario runs must produce equal
 * digests, and a host that loses everything and rebuilds from the network
 * must produce the same digest as the host that lost it. */
[[nodiscard]] std::string final_state_digest(const Scenario &scenario);

/* ---------------------------------------------------------------- */
/* Scenario groups, one per concern                                 */
/* ---------------------------------------------------------------- */

/* Ingestion and catch-up: multi-run traversal, crash recovery, retry,
 * dedup, withdrawal, planning abstention, cross-run determinism. */
void run_ingestion_scenarios();

/* Derived and self-authored state through the durable pipeline:
 * conversation context and journal valence. */
void run_derived_state_scenarios();

/* Issue #143 host loss: publish to the network, destroy the host,
 * reconstruct from the network alone, and require the learned state back. */
void run_host_loss_scenarios();

/* Issue #143 remote operator control: pause, resume and approve applied
 * through the AT Protocol operator channel across a restart. */
void run_remote_control_scenarios();

} // namespace atperson::e2e

#endif // ATPERSON_TESTS_LIFECYCLE_HARNESS_HPP
