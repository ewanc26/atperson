#ifndef ATPERSON_CLI_CONTROL_REMOTE_HPP
#define ATPERSON_CLI_CONTROL_REMOTE_HPP

// CLI for the remote operator channel (#143): `control remote <sub>`.
//
// Three subcommands, matching the three things an operator needs:
//
//   status   read-only. Whether the channel is enabled, which DID is
//            trusted, and how far the replay watermark has advanced.
//   poll     run one bounded fetch-and-apply pass now, instead of waiting
//            for the next daemon cycle. Needs a service.
//   emit     publish a request record into the operator's own repo, so a
//            remote command can be issued from this host. Needs a service.
//
// `status` is offline and safe at any time. `poll` and `emit` need
// credentials, so they are only wired in the network build.
//
// The daemon never calls `emit`: writing a control record is an operator
// act, not something the runtime may do to itself. See control/remote.hpp
// for what that means when the operator DID is the entity's own account.

#include "runtime.hpp"

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

/* `sub` is status | poll | emit. `argument` is the op name (and digest,
 * space-separated) for emit, and unused otherwise. */
int run_control_remote(std::ostream &out, const RuntimeResourceStatus &resource_status,
                       std::string_view sub, std::string_view argument);

} // namespace cli
} // namespace atperson

#endif
