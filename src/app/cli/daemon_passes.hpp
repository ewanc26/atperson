#ifndef ATPERSON_CLI_DAEMON_PASSES_HPP
#define ATPERSON_CLI_DAEMON_PASSES_HPP

// The daemon's per-cycle post-perception passes, and the operator-facing
// report each one prints.
//
// One cycle is: perceive (sync), then run the passes that need the freshly
// committed state. They are separate subsystems, each off by default, so they
// live here rather than inside the cycle body — where one growing file would
// own all of them. Nothing in this file is stateful and nothing decides
// policy: each pass reads its own configuration and durable inputs, runs its
// own gates, and reports what it did.
//
// Order in a cycle is significant and is owned by the caller: the remote
// control poll runs first, so a pause the operator issued since the last
// cycle is in force for this one; the scheduler follows, because it proposes
// from what this cycle learned; state publication follows the scheduler, so
// the actions it just attempted are published alongside the observations
// that produced them.
//
// Network boundary: the scheduler and the control poll open a Wolfram
// session lazily, so a cycle with nothing to do reads no credentials and
// touches no network. This file is compiled only into the network runtime.

#include "reflect/config.hpp"
#include "reflect/pass.hpp"
#include "replicate/publication.hpp"
#include "resource/runtime.hpp"
#include "scheduler/cycle.hpp"

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

namespace atperson {

class LanguageGraph;
class Ledger;

} // namespace atperson

namespace atperson::cli {

/* One autonomous scheduler cycle (#140) after a successful perception cycle.
 * Off unless ATPERSON_SCHEDULER=1; the writer is established lazily inside
 * the attempt atom, so a cycle with no approved proposal never reads
 * credentials or touches the network. */
[[nodiscard]] SchedulerCycleReport run_scheduler_after_cycle(
    const std::filesystem::path &data_dir, const SchedulerConfig &config,
    const LanguageGraph &graph, const Ledger &ledger, std::int64_t now);

/* One remote operator poll (#143) per cycle. Off unless
 * ATPERSON_OPERATOR_DID names a trusted DID; with no DID this is a no-op and
 * opens no session. A refusal or an unavailability is reported on `out` and
 * never stops the daemon: local control keeps working either way. */
void run_remote_control_poll(std::ostream &out, const RuntimeResourceStatus &resource_status,
                             const std::string &account_did,
                             const std::filesystem::path &control_file,
                             const std::filesystem::path &cursor_file);

/* One bounded state-publication pass (#142, #143) after the scheduler, so
 * the actions this cycle attempted are published with the observations that
 * produced them. Off unless ATPERSON_STATEPUB=1, and then only on the
 * configured cadence — durable experience reaching the network is a rate
 * decision, not something to do every cycle by surprise.
 *
 * A pass that published something is reported. A pass that was refused is
 * reported only when the reason changed since the last refusal, so a
 * misconfigured channel is diagnosable without a line per cycle.
 *
 * `last_refusal` is the caller's memory of the previous reason; it is
 * updated here and read nowhere else. */
void run_state_publication(std::ostream &out, const RuntimeResourceStatus &resource_status,
                           const PublicationConfig &config, std::uint64_t cycle,
                           Ledger &ledger, const std::filesystem::path &data_dir,
                           std::string &last_refusal);

void print_scheduler_report(std::ostream &out, const SchedulerCycleReport &report);

/* Printed only when the pass actually wrote something, so an idle bound
 * (cadence not yet elapsed, no triggers) stays silent. */
void print_reflection_report(std::ostream &out, const ReflectionReport &report,
                             const ReflectionConfig &config, std::string_view now_rfc3339);

} // namespace atperson::cli

#endif // ATPERSON_CLI_DAEMON_PASSES_HPP
