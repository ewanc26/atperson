#ifndef ATPERSON_CLI_LEDGER_MAINTENANCE_HPP
#define ATPERSON_CLI_LEDGER_MAINTENANCE_HPP

// CLI ledger maintenance commands: rebuild, compact, withdraw.
//
// Owns the side-effectful ledger/runtime maintenance bodies of the `atperson`
// CLI, moved byte-faithfully from the former single-file dispatch in
// src/app/main.cpp. Each atom receives the concrete runtime pieces it
// operates on (resource status, data/ledger/model paths, resource paths and
// overrides) plus the text-report callbacks `print_stats` and `usage` that
// main still owns. No Wolfram, AtprotoClient or network access.
//
// Failure modes: mutating commands call require_runtime_write_headroom before
// locking; StateLock serialises writers; a missing ledger path throws
// std::runtime_error. Returns 0 on success, 2 on argument/usage errors.

#include "atperson/graph.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace atperson {
namespace cli {

int run_rebuild(std::ostream &out, const RuntimeResourceStatus &resource_status,
                const std::filesystem::path &data_dir,
                const std::filesystem::path &ledger_file,
                const std::filesystem::path &model_path,
                const std::vector<std::filesystem::path> &resource_paths,
                const ResourceOverrides &resource_overrides,
                const std::function<void(const LanguageGraph &)> &print_stats);

int run_compact(std::ostream &out, const RuntimeResourceStatus &resource_status,
                const std::filesystem::path &data_dir,
                const std::filesystem::path &ledger_file);

int run_withdraw(std::ostream &out, const RuntimeResourceStatus &resource_status,
                 const std::filesystem::path &data_dir,
                 const std::filesystem::path &ledger_file, std::string_view scope,
                 std::string_view target,
                 const std::function<void(std::ostream &)> &usage);

} // namespace cli
} // namespace atperson

#endif
