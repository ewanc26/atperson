#ifndef ATPERSON_CLI_METRICS_HPP
#define ATPERSON_CLI_METRICS_HPP

// Metric command atoms: `metrics`, `selfeval` (#153).
//
// `metrics [limit] [--since <iso>]` lists the longitudinal self-evaluation
// snapshots (newest first, filtered), each with its period span, cadence,
// metric values and the provenance of the chain (the previous snapshot it
// compares against). Deltas are computed on this display path between
// neighbouring snapshots, so the stored records stay single, self-contained
// periods. Output is format-versioned and stable.
//
// `selfeval` runs the deterministic longitudinal pass explicitly, regardless
// of `ATPERSON_SELFEVAL` (that flag governs the daemon hook only), and prints
// the snapshot it wrote (or why none was due).
//
// `metrics` is read-only and lock-free like the thought listing. `selfeval`
// takes the named `.metrics-lock` inside the state directory so a concurrent
// daemon pass and an operator pass never interleave appends. Neither commands
// touch learned state: the pass reads the journal and graph but never observes
// or mutates them, keeping the operator-mediated, non-training contract.

#include "atperson/graph.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace atperson {
namespace cli {

/* truth path to the metric snapshot store within a state directory. */
std::filesystem::path metrics_path(const std::filesystem::path &data_dir);

/**
 * `metrics [limit] [--since <iso>]`: list stored snapshots, newest first.
 * limit defaults to 50 and is clamped to 1000; `--since` filters to
 * snapshots at or after the given RFC 3339 instant. Returns 0 on success,
 * 2 on bad arguments.
 */
int run_metrics_list(std::ostream &out, const std::filesystem::path &data_dir,
                     const std::string_view *arguments, std::size_t argument_count);

/**
 * `selfeval`: run the longitudinal evaluation pass at wall time `now_unix`
 * and print the snapshot written plus the report. Returns 0 on success.
 */
int run_self_eval_command(std::ostream &out, const std::filesystem::path &data_dir,
                          const std::filesystem::path &journal_path,
                          const LanguageGraph &graph, std::int64_t now_unix,
                          std::string_view now_rfc3339);

} // namespace cli
} // namespace atperson

#endif