#ifndef ATPERSON_CLI_STATEPUB_HPP
#define ATPERSON_CLI_STATEPUB_HPP

// CLI statepub commands (#142): `atperson statepub status` and
// `atperson statepub drain`.
//
// status — report publication progress: cursor position vs ledger and
//          journal counts, no network access.
// drain  — one bounded publisher pass: committed ledger entries and
//          journal entries become AT Protocol records under the
//          entity's DID. Gated on control writes_enabled (fail-closed:
//          no control state, no drain). Network-backed: establishes a
//          Wolfram session lazily, only when there is a backlog.

#include "resource/runtime.hpp"

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

int run_statepub_status(std::ostream &out, std::ostream &err,
                        const RuntimeResourceStatus &resource_status,
                        const std::filesystem::path &data_dir,
                        const std::filesystem::path &ledger_file,
                        const std::filesystem::path &journal_file,
                        const std::filesystem::path &thoughts_file);

int run_statepub_drain(std::ostream &out, std::ostream &err,
                       const RuntimeResourceStatus &resource_status,
                       const std::filesystem::path &data_dir,
                       const std::filesystem::path &ledger_file,
                       const std::filesystem::path &journal_file,
                       const std::filesystem::path &thoughts_file,
                       const std::filesystem::path &control_file,
                       bool offline = false);

} // namespace cli
} // namespace atperson

#endif
