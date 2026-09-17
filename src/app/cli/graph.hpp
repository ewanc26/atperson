#ifndef ATPERSON_CLI_GRAPH_INSPECTION_HPP
#define ATPERSON_CLI_GRAPH_INSPECTION_HPP

// CLI read-only graph inspection commands: assoc, candidates, familiarity,
// recall.
//
// Owns the read-only inspection bodies of the `atperson` CLI, moved
// byte-faithfully from the former single-file dispatch in src/app/main.cpp.
// Each atom renders C23-core results through the thin LanguageGraph wrapper
// without recomputing scores outside the core. `recall` bumps episodic recall
// counters inside the core (documented core behaviour), so it is read-only
// with respect to durable state but not a pure query.
//
// Failure modes: limits are bounded by require_runtime_inspection_limit;
// std::runtime_error propagates from the wrapper on core errors. Returns 0 on
// success, 2 on argument errors after printing usage.

#include "atperson/graph.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

int run_assoc(std::ostream &out, const RuntimeResourceStatus &resource_status,
              const LanguageGraph &graph, std::string_view token, const char *limit_value);

int run_candidates(std::ostream &out, const RuntimeResourceStatus &resource_status,
                   const LanguageGraph &graph, std::string_view context,
                   const char *limit_value);

int run_familiarity(std::ostream &out, const LanguageGraph &graph, std::string_view token);

int run_recall(std::ostream &out, const RuntimeResourceStatus &resource_status,
               LanguageGraph &graph, std::string_view query, const char *limit_value,
               std::uint64_t at_epoch);

} // namespace cli
} // namespace atperson

#endif
