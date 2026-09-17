#ifndef ATPERSON_CLI_PUBLISH_HPP
#define ATPERSON_CLI_PUBLISH_HPP

// CLI publish command: publish <action-file>.
//
// Executes one exact outbound post or reply through the full #25 gate chain —
// operator pause, #23 policy and rate budget, dry-run, #22 control approval —
// and, only if every gate passes, performs the write through Wolfram. The
// action text is never regenerated: it is submitted exactly as the approved
// document carries it.
//
// Every attempt is appended to the credential-free outbound audit log and to
// the action/outcome journal (#27), which is the durable experience record
// the later event linkage and valence replay build on. Refusals and dry runs
// are reported, not errors; a confirmed write returns 0, an execution failure
// returns 1.
//
// The credentials (`ATPERSON_IDENTIFIER`, `ATPERSON_APP_PASSWORD`) are read
// only when a write is actually reached, so a dry run or a refusal never
// establishes a session. This command takes a dedicated outbound lock, never
// the daemon's writer lock.

#include <cstdint>
#include <filesystem>
#include <iosfwd>

namespace atperson {
namespace cli {

int run_publish(std::ostream &out, const std::filesystem::path &data_dir,
                const std::filesystem::path &policy_file, const std::filesystem::path &budget_file,
                const std::filesystem::path &control_file, const std::filesystem::path &audit_file,
                const std::filesystem::path &journal_file,
                const std::filesystem::path &action_file, std::int64_t now);

} // namespace cli
} // namespace atperson

#endif
