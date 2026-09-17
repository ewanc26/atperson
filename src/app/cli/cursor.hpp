#ifndef ATPERSON_CLI_CURSOR_HPP
#define ATPERSON_CLI_CURSOR_HPP

// CLI ingestion-cursor command: cursor [status|reset].
//
// Owns the ingestion-cursor body of the `atperson` CLI, moved byte-faithfully
// from the former single-file dispatch in src/app/main.cpp. `status` renders
// the persisted catch-up position; `reset` clears it under the StateLock
// writer lock and bumps the checkpoint generation so the next sync starts at
// the timeline head. Constructs the Wolfram-backed AtprotoClient from
// ATPERSON_SERVICE/ATPERSON_IDENTIFIER/ATPERSON_APP_PASSWORD to resolve the
// account DID; network-only atom.
//
// Failure modes: std::runtime_error propagates from required_env/AtprotoClient
// construction and from state load/save. Returns 0 on success.

#include "resource_runtime.hpp"

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

int run_cursor(std::ostream &out, const RuntimeResourceStatus &resource_status,
               const std::filesystem::path &data_dir,
               const std::filesystem::path &state_file, std::string_view sub);

} // namespace cli
} // namespace atperson

#endif
