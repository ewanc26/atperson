#ifndef ATPERSON_RESOURCE_RUNTIME_HPP
#define ATPERSON_RESOURCE_RUNTIME_HPP

#include "atperson/graph.hpp"
#include "resource_budget.hpp"
#include "system_resources.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>

namespace atperson {

struct RuntimeResourceStatus {
    SystemResources system;
    ResourceBudget budget;
};

/* Probe + derive without mutating graph policy. */
RuntimeResourceStatus inspect_runtime_resources(const LanguageGraph &graph,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides);

/* Probe + derive and apply #9 node/edge ceilings to the live graph. */
RuntimeResourceStatus refresh_runtime_resources(LanguageGraph &graph,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides);

/* Refuse new durable writes when the filesystem is inside its safety reserve. */
void require_runtime_write_headroom(const RuntimeResourceStatus &status);

/* Bound one-shot allocations driven by CLI input/result limits. */
void require_runtime_input_headroom(const RuntimeResourceStatus &status, std::uint64_t bytes);
void require_runtime_inspection_limit(const RuntimeResourceStatus &status, std::size_t items);

/* Human-readable diagnostics for `atperson resources`. */
void print_runtime_resources(std::ostream &out, const RuntimeResourceStatus &status);

} // namespace atperson

#endif
