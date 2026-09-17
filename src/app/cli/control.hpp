#ifndef ATPERSON_CLI_CONTROL_HPP
#define ATPERSON_CLI_CONTROL_HPP

// CLI operator-control command: control <sub>.
//
// Implements the #22 operator surface: pause/resume, the fail-closed write
// gate, dry-run mode, approval binding, shutdown request, and a status view
// over the persisted control state. Mutating subcommands take the StateLock
// writer lock on the data directory and save atomically; `status` is
// read-only. Control state never mutates learned C23 state.

#include "runtime.hpp"

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

int run_control(std::ostream &out, const RuntimeResourceStatus &resource_status,
                const std::filesystem::path &data_dir,
                const std::filesystem::path &control_file, std::string_view sub,
                std::string_view argument);

} // namespace cli
} // namespace atperson

#endif
