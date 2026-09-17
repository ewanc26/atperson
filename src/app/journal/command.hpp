#ifndef ATPERSON_JOURNAL_COMMAND_HPP
#define ATPERSON_JOURNAL_COMMAND_HPP

// Action/outcome journal: command orchestration (#27).
//
// Owns the `journal` CLI flow. Read-only listing subcommands (actions,
// events, valence) render the journal's append order and never touch
// learned state. The mutating `apply` subcommand is the only surface
// besides rebuild that turns journal experience into valence state:
// it applies one explicit valence event through the C23 API and then
// records that it happened, so a rebuild can replay it after the ledger.
//
// `apply` is opt-in by construction: the operator names the token, the
// valence kind, the signal and the source (a journal action id or an
// AT URI). Nothing here derives valence from the journal automatically —
// the journal records experience; the operator decides what it means.
//
// Locking: `apply` takes the data-directory writer lock (same lock as
// ingest/rebuild) across the graph mutation, the journal append and the
// model save, so the three durable effects commit as one unit or not at
// all. The listing subcommands run lock-free like other read-only
// commands.
//
// Failure semantics: throws std::runtime_error for I/O and unknown
// tokens; returns 2 for bad arguments (the caller prints usage).

#include "atperson/graph.hpp"
#include "journal/store.hpp"

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace journal {

/**
 * Run one `journal` command:
 *   journal actions [limit]        list attempted actions (newest last)
 *   journal events [limit]         list linked outcome events
 *   journal valence [limit]        list applied valence updates
 *   journal apply <token> <kind> <signal> <source-id>
 *                                  apply one explicit valence event and
 *                                  journal it (writer lock; saves the model)
 * Returns 0 on success, 2 on bad arguments. `now_unix` stamps the journal
 * entry; `now_rfc3339` stamps the event log.
 */
int run_journal_command(std::ostream &out, LanguageGraph &graph,
                        const std::filesystem::path &journal_path,
                        const std::filesystem::path &data_dir,
                        const std::filesystem::path &model_path,
                        std::string_view subcommand,
                        const std::string_view *arguments, std::size_t argument_count,
                        std::int64_t now_unix, std::string_view now_rfc3339);

} // namespace journal
} // namespace atperson

#endif
