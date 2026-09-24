#ifndef ATPERSON_CLI_RECONSTRUCT_HPP
#define ATPERSON_CLI_RECONSTRUCT_HPP

// CLI reconstruct command (#142): `atperson reconstruct --into <dir>`.
// Rebuilds fresh entity state from the entity's published AT Protocol
// records via a Wolfram-backed RecordSource, then reports what replayed.
// Network build only.

#include <filesystem>
#include <iosfwd>

namespace atperson {
namespace cli {

int run_reconstruct(std::ostream &out, std::ostream &err,
                    const std::filesystem::path &into_dir);

} // namespace cli
} // namespace atperson

#endif
