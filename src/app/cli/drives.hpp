#ifndef ATPERSON_CLI_DRIVES_HPP
#define ATPERSON_CLI_DRIVES_HPP

// `atperson drives` (#148): read-only inspection of the experience-derived
// drive signals over the most recent committed ledger contexts. Shows each
// candidate's curiosity and reciprocity plus the evidence that earned them.
// Never mutates state and never requires network access.

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace atperson {
namespace cli {

/* Recompute drives over `max_contexts` recent contexts and print them in
 * scheduler preference order. `journal_path` is <data>/action-journal.jsonl.
 * `now` is injected for deterministic window checks. */
int run_drives_command(std::ostream &out, const LanguageGraph &graph, const Ledger &ledger,
                       const std::filesystem::path &journal_path, std::int64_t now,
                       std::size_t max_contexts);

} // namespace cli
} // namespace atperson

#endif