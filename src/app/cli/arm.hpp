#ifndef ATPERSON_CLI_ARM_HPP
#define ATPERSON_CLI_ARM_HPP

// CLI for unattended operation: autonomy arm | disarm | preflight.
//
//   autonomy arm --kinds <kind:count/window,...> [options]
//       Plan the setup that lets the entity act on its own within the stated
//       ceilings. Without --apply it only prints the plan. With --apply it
//       writes the outbound policy, a standing authorization envelope and the
//       control state (writes on last), then runs the preflight.
//         --kinds post:3/1d,reply:5/1d,like:20/1h   kind:count/window
//                (window = seconds, or a number with s/m/h/d)
//         --scope moon,wolf        action text must contain one of these terms
//         --min-plan <0..1>        decision-score floor
//         --min-support <0..1>     support-score floor
//         --expires <RFC3339|never>   default: never
//         --id <envelope-id>       default: autonomy
//         --drives --intents --graduated-likes --valence-guard <-1..0>
//                only add the matching environment line to the printed setup
//   autonomy disarm [--id <envelope-id>]
//       Writes off and the envelope revoked. Immediate.
//   autonomy preflight
//       Read-only end-to-end readiness report; exit 0 ready, 1 not ready.
//
// Environment (the scheduler switch, the publishing master switch,
// credentials) is per process and is never edited by this command: it prints
// the exact lines to put in the .env file, as the publishing prompt does.
// Policy, envelope and control state are runtime metadata outside the durable
// learned set, so no StateLock is taken and an operator can disarm while the
// daemon owns the lock.

#include <cstdint>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace atperson {
namespace cli {

int run_autonomy_arming(std::ostream &out, std::string_view sub,
                        const std::vector<std::string_view> &arguments, std::int64_t now_unix);

} // namespace cli
} // namespace atperson

#endif
