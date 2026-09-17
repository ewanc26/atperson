#ifndef ATPERSON_CLI_DAEMON_HPP
#define ATPERSON_CLI_DAEMON_HPP

// CLI long-running ingestion daemon: daemon [max-cycles].
//
// Owns the network-bearing body of the `daemon` command. It holds the
// StateLock writer lock for the daemon's whole lifetime (so no other process
// mutates the state set concurrently), constructs the Wolfram-backed
// AtprotoClient, and hands an offline-testable loop the real transport,
// signals, sleeps and atomic persistence.
//
// Operator control stays usable while the daemon runs: `atperson control
// pause|shutdown` writes runtime metadata, not the state set, so it does not
// take the writer lock. The loop re-reads it every cycle.
//
// Failure modes: require_runtime_write_headroom refuses an unsafe start
// before locking; std::runtime_error propagates from the environment,
// AtprotoClient construction and fatal (non-retryable) persistence errors.
// Transport failures are classified as RetryableError and backed off.
// Network-only atom.

#include "atperson/graph.hpp"
#include "runtime.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>

namespace atperson {
namespace cli {

/* `max_cycles_override` > 0 bounds this run and overrides the environment
 * configuration; 0 uses DaemonConfig as parsed. */
int run_daemon_command(std::ostream &out, std::ostream &err,
                       const RuntimeResourceStatus &resource_status,
                       const std::filesystem::path &data_dir, LanguageGraph &graph,
                       const std::filesystem::path &model_path,
                       const std::filesystem::path &ledger_file,
                       const std::filesystem::path &state_file, int max_cycles_override,
                       const std::function<void(const LanguageGraph &)> &print_stats);

} // namespace cli
} // namespace atperson

#endif
