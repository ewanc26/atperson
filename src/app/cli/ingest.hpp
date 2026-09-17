#ifndef ATPERSON_CLI_INGEST_HPP
#define ATPERSON_CLI_INGEST_HPP

// CLI local ingestion commands: ingest, ingest-file.
//
// Owns the mutating local-observation bodies of the `atperson` CLI, moved
// byte-faithfully from the former single-file dispatch in src/app/main.cpp.
// The atom takes the StateLock writer lock on the data directory, observes
// the text through the C23 core, saves the snapshot, and reports stats via
// the caller-owned `print_stats` callback. No network access.
//
// Failure modes: require_runtime_write_headroom and
// require_runtime_input_headroom refuse unsafe writes before locking; a
// missing/unreadable input file throws std::runtime_error. Returns 0 on
// success, 2 on argument errors after printing usage.

#include "atperson/graph.hpp"
#include "runtime.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

int run_ingest(std::ostream &out, const RuntimeResourceStatus &resource_status,
               const std::filesystem::path &data_dir, LanguageGraph &graph,
               const std::filesystem::path &model_path, std::string_view text,
               const char *source_value,
               const std::function<void(const LanguageGraph &)> &print_stats);

int run_ingest_file(std::ostream &out, const RuntimeResourceStatus &resource_status,
                    const std::filesystem::path &data_dir, LanguageGraph &graph,
                    const std::filesystem::path &model_path,
                    const std::filesystem::path &input_path, const char *source_value,
                    const std::function<void(const LanguageGraph &)> &print_stats);

} // namespace cli
} // namespace atperson

#endif
