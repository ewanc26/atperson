#ifndef ATPERSON_CLI_OUTBOUND_HPP
#define ATPERSON_CLI_OUTBOUND_HPP

// CLI outbound-policy command: outbound <sub>.
//
// Inspects the #23 outbound policy and rate budgets, and evaluates an action
// proposal through the policy without any network access:
//
//   outbound status [kind]                 - enabled kinds and current usage
//   outbound rules                         - effective policy document
//   outbound evaluate <kind> [target] [digest]
//                                          - read-only decision (no bookkeeping)
//   outbound admit <kind> [target] [digest]
//                                          - decision plus budget bookkeeping
//
// Both evaluate and admit are dry runs with respect to the network: they
// never perform an outbound action. `admit` additionally consumes budget, so
// it exercises the same admission bookkeeping a real write path will use.
// The #22 control gate is reported alongside the policy decision; this command
// never calls the gate's throwing form and never writes to the network.
//
// Policy and budget are runtime metadata outside the durable state set, so
// this command does not take the StateLock (like `control`).

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace atperson {
namespace cli {

int run_outbound_command(std::ostream &out, const std::filesystem::path &policy_file,
                         const std::filesystem::path &budget_file,
                         const std::filesystem::path &control_file, std::string_view sub,
                         const std::vector<std::string_view> &arguments, std::int64_t now);

} // namespace cli
} // namespace atperson

#endif
