#ifndef ATPERSON_CLI_THOUGHTS_HPP
#define ATPERSON_CLI_THOUGHTS_HPP

// Thought store command atoms: `thought`, `thoughts`, `reflect` (#151).
//
// `thought <text...>` records one hand-written reflection into the durable
// thought store. `thoughts [limit] [--since <iso>] [--kind <k>]` lists the
// store's entries (newest first, filtered). `reflect` runs the deterministic
// reflection pass (#151) over the action journal, the learned graph and the
// existing thought surface, writing any bounded consolidation and movement
// thoughts the thresholds select. `reflect` is an explicit operator request
// and runs regardless of `ATPERSON_REFLECTION`; that flag governs the daemon
// hook only.
//
// Record and reflect take the named `.thoughts-lock` inside the state
// directory so a concurrent `atperson daemon` reflect step and an operator
// record never interleave appends. Listing is lock-free (read-only).
//
// None of these commands touch learned state: `thought` and `thoughts`
// operate on the thought store alone; `reflect` reads the journal and graph
// but never observes or mutates them, keeping the non-training contract.

#include "atperson/graph.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace atperson {
namespace cli {

/* truth path to the thought store within a state directory. */
std::filesystem::path thoughts_path(const std::filesystem::path &data_dir);

/**
 * `thought <text...>`: record one hand-written reflection. Joins the text
 * parts with spaces and refuses empty text. Returns 0 on success.
 */
int run_thought_record(std::ostream &out, const std::filesystem::path &data_dir,
                       const std::vector<std::string> &text_parts,
                       std::string_view now_rfc3339);

/**
 * `thoughts [limit] [--since <iso>] [--kind <kind>]`: list stored thoughts,
 * newest first. limit defaults to 50 and is clamped to 1000; `--since`
 * filters to entries at or after the given RFC 3339 instant; `--kind`
 * filters to one kind. Returns 0 on success, 2 on bad arguments.
 */
int run_thoughts_list(std::ostream &out, const std::filesystem::path &data_dir,
                      const std::string_view *arguments, std::size_t argument_count);

/**
 * `reflect`: run the deterministic reflection pass at wall time `now_unix`
 * (`now_rfc3339` stamps the written entries) and print a report of what was
 * written and why. Returns 0 on success.
 */
int run_reflect_command(std::ostream &out, const std::filesystem::path &data_dir,
                        const std::filesystem::path &journal_path, const LanguageGraph &graph,
                        std::int64_t now_unix, std::string_view now_rfc3339);

} // namespace cli
} // namespace atperson

#endif