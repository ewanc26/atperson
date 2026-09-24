#ifndef ATPERSON_CLI_ENVELOPE_HPP
#define ATPERSON_CLI_ENVELOPE_HPP

// CLI standing-authorization surface (#141): control envelope <sub>.
//
// Subcommands:
//   list                      — every envelope id, one per line
//   show <id>                 — the full envelope document
//   grant <id> [key=value...] — write a new envelope (fails if it exists)
//   revoke <id>               — immediate, durable revocation (delete)
//   dry-run <action-file>     — which envelope (if any) would cover the
//                               action in the file, evaluated now
//
// Envelope state is runtime metadata outside the durable learned set, so
// subcommands do not take the StateLock writer lock: an operator must be
// able to revoke while a daemon owns the lock. Atomic save/load keeps each
// file self-consistent.

#include "runtime.hpp"

#include <filesystem>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace atperson {
namespace cli {

int run_envelope_command(std::ostream &out, const RuntimeResourceStatus &resource_status,
                         const std::filesystem::path &envelopes_dir,
                         const std::filesystem::path &policy_file,
                         std::string_view sub,
                         const std::vector<std::string_view> &arguments,
                         std::int64_t now);

} // namespace cli
} // namespace atperson

#endif
