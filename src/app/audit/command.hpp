#ifndef ATPERSON_AUDIT_COMMAND_HPP
#define ATPERSON_AUDIT_COMMAND_HPP

// Advisory audit: command orchestration.
//
// Owns the `audit` CLI flow: build the decision trace, marshal it (with the
// fixed question rubric) into the TypeSafe `state`, POST it, parse and render
// the report. Read-only with respect to learned state, planning, and network
// policy; sending the bounded trace to a third party requires the caller to
// bind an explicit API key, so the outbound call is opt-in by construction.
// Network-only: this atom is compiled only in the ATPERSON_BUILD_NETWORK
// configuration.
//
// Environment:
//   ATPERSON_TYPESAFE_API_KEY    required to opt in (Bearer credential)
//   ATPERSON_TYPESAFE_ENDPOINT   override endpoint (default v1/systemone)
//
// Failure semantics: throws std::runtime_error when the key is missing, the
// request is rejected, or the response cannot be parsed; the trace and report
// are still rendered up to the failing step.

#include "atperson/graph.hpp"
#include "atperson/action.h"

#include <iosfwd>
#include <string_view>
#include <vector>

namespace atperson {
namespace audit {

/**
 * Run one advisory audit: `audit <context> [max-tokens] [beam-width]`.
 * Prints the report to `out` and returns 0 on success, 2 on bad arguments.
 * `api_key` comes from ATPERSON_TYPESAFE_API_KEY; empty refuses to send.
 */
int run_audit_command(std::ostream &out, const LanguageGraph &graph,
                      std::string_view command,
                      const std::vector<std::string_view> &arguments);

} // namespace audit
} // namespace atperson

#endif