#ifndef ATPERSON_ACTION_INSPECTION_HPP
#define ATPERSON_ACTION_INSPECTION_HPP

#include "atperson/graph.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

/** Whether `command` is one of the read-only action inspection commands. */
[[nodiscard]] bool is_action_inspection_command(std::string_view command) noexcept;

/**
 * Stable digest binding an approval to the exact inspected decision (#22).
 *
 * The digest covers the context text and the accepted plan (tokens, step
 * count, score, stop reason). A regenerated or altered plan produces a
 * different digest, so an approval can never authorise a replacement action.
 * Returns 16 lowercase hex characters (64-bit atp_ledger_digest).
 */
[[nodiscard]] std::string decision_digest(std::string_view context,
                                          const atp_action_decision &decision);

/**
 * Render one read-only action inspection command.
 *
 * Supported commands:
 * - plans <context> [max-tokens] [beam-width]
 * - decide <context> [max-tokens] [beam-width]
 * - context <text> [source-id] [author-did]
 *
 * This module is presentation only. All scores, selections, plans and stop
 * decisions come from the C23 core through LanguageGraph's thin wrappers.
 */
int run_action_inspection_command(std::ostream &out, const LanguageGraph &graph,
                                  std::string_view command,
                                  const std::vector<std::string_view> &arguments,
                                  std::uint64_t at_epoch);

} // namespace atperson

#endif
