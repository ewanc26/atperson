#ifndef ATPERSON_CLI_SYNC_HPP
#define ATPERSON_CLI_SYNC_HPP

// CLI network sync command: sync [max-pages].
//
// Owns the bounded timeline-sync body of the `atperson` CLI, moved
// byte-faithfully from the former single-file dispatch in src/app/main.cpp.
// Takes the StateLock writer lock, constructs the Wolfram-backed
// AtprotoClient from ATPERSON_SERVICE/ATPERSON_IDENTIFIER/
// ATPERSON_APP_PASSWORD, opens the ledger and ingestion state, refreshes
// per-page resource budgets, and delegates page consumption to the sync
// engine. A saved cursor rejected by the service is reported, discarded, and
// retried from the timeline head. Network-only atom.
//
// Failure modes: require_runtime_write_headroom refuses unsafe writes before
// locking; std::runtime_error propagates from required_env/AtprotoClient
// construction and the sync engine. Returns 0 on success.

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "runtime.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>

namespace atperson {
namespace cli {

int run_sync(std::ostream &out, std::ostream &err, const RuntimeResourceStatus &resource_status,
             const std::filesystem::path &data_dir, LanguageGraph &graph,
             const std::filesystem::path &model_path,
             const std::filesystem::path &ledger_file,
             const std::filesystem::path &state_file, int max_pages,
             const std::function<void(const LanguageGraph &)> &print_stats);

} // namespace cli
} // namespace atperson

#endif
