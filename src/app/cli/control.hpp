#ifndef ATPERSON_CLI_CONTROL_HPP
#define ATPERSON_CLI_CONTROL_HPP

// CLI operator-control command: control <sub>.
//
// Implements the #22 operator surface: pause/resume, the fail-closed write
// gate, dry-run mode, approval binding, shutdown request, and a status view
// over the persisted control state. Mutating subcommands save atomically;
// `status` is read-only. Control state never mutates learned C23 state.
//
// Control state is runtime metadata outside the durable state set (snapshot,
// ledger, commit marker, ingestion cursor), so subcommands do NOT take the
// StateLock writer lock. That is deliberate: an operator must be able to
// pause or request shutdown while a long-running daemon owns the writer
// lock. Atomic save/load keeps the file self-consistent under concurrent
// operator use.

#include "runtime.hpp"

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

int run_control(std::ostream &out, const RuntimeResourceStatus &resource_status,
                const std::filesystem::path &control_file, std::string_view sub,
                std::string_view argument);

} // namespace cli
} // namespace atperson

#endif
