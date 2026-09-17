#ifndef ATPERSON_RESOURCE_BUDGET_HPP
#define ATPERSON_RESOURCE_BUDGET_HPP

#include "system.hpp"

#include <atperson/core.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace atperson {

/* Optional operator choices. Empty means automatic. */
struct ResourceOverrides {
    std::optional<std::uint64_t> memory_growth_budget_bytes;
    std::optional<std::uint64_t> disk_reserve_bytes;
    std::optional<std::size_t> node_capacity_max;
    std::optional<std::size_t> edge_capacity_max;
    std::optional<int> sync_page_size;
    std::optional<std::uint64_t> sync_max_observations;
};

/*
 * Runtime-only resource policy derived from a system snapshot. None of these
 * values are learned state or persisted in model snapshots.
 */
struct ResourceBudget {
    std::uint64_t memory_reserve_bytes{};
    std::uint64_t memory_growth_budget_bytes{};

    std::filesystem::path limiting_disk_path;
    std::uint64_t limiting_disk_capacity_bytes{};
    std::uint64_t limiting_disk_available_bytes{};
    std::uint64_t disk_reserve_bytes{};
    std::uint64_t disk_write_budget_bytes{};

    std::size_t node_capacity_max{};
    std::size_t edge_capacity_max{};

    std::uint64_t max_input_bytes{};
    std::size_t inspection_item_limit{};
    int sync_page_size{1};
    std::uint64_t sync_max_observations{1u};

    bool memory_pressure{};
    bool disk_pressure{};
};

/* Read and validate ATPERSON_* resource overrides from the environment. */
ResourceOverrides resource_overrides_from_environment();

/*
 * Pure deterministic derivation. Fixed system/stats/override inputs always
 * produce the same result. Graph ceilings never fall below current counts;
 * no eviction is implied by a lower machine budget.
 */
ResourceBudget derive_resource_budget(const SystemResources &system,
                                      const atp_graph_stats &graph,
                                      const ResourceOverrides &overrides = {});

} // namespace atperson

#endif
