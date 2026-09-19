#ifndef ATPERSON_RESOURCE_BUDGET_HPP
#define ATPERSON_RESOURCE_BUDGET_HPP

#include "system.hpp"

#include <atperson/core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace atperson {

/*
 * Versioned runtime capacity recommendation for the variable-shape neural
 * core (issue #63, #64). It is operational metadata, not learned state. It is
 * authoritative only at first model creation (issue #73), where
 * neural_architecture_of translates it into a validated, durable C23
 * architecture. On every later run the persisted architecture is
 * authoritative and this recommendation is only a headroom/expansion signal:
 * a loaded model is never silently reshaped merely because this
 * recommendation changes.
 */
enum class NeuralCapacityClass {
    constrained,
    baseline,
    capable,
    large,
    expansive,
};

struct NeuralCapacityRecommendation {
    std::uint32_t policy_version{1u};
    NeuralCapacityClass capacity_class{NeuralCapacityClass::constrained};
    std::uint64_t memory_budget_bytes{};
    std::size_t embedding_dim{32u};
    std::size_t hidden_layer_count{1u};
    std::array<std::size_t, 3u> hidden_widths{64u, 0u, 0u};
    std::uint64_t shared_parameter_count{};
    std::uint64_t shared_parameter_bytes{};
    std::size_t runtime_batch_observations{1u};
};

/* Optional operator choices. Empty means automatic. */
struct ResourceOverrides {
    std::optional<std::uint64_t> memory_growth_budget_bytes;
    std::optional<std::uint64_t> disk_reserve_bytes;
    std::optional<std::size_t> node_capacity_max;
    std::optional<std::size_t> edge_capacity_max;
    std::optional<int> sync_page_size;
    std::optional<std::uint64_t> sync_max_observations;
    /* Forced neural capacity class (ATPERSON_NEURAL_CAPACITY). */
    std::optional<NeuralCapacityClass> neural_capacity_class;
    /* Explicit first-creation neural shape overrides. All three are optional
     * independently: unset fields keep the automatic class's shape, but the
     * combined effective shape must always pass the runtime's policy-bounds
     * validation (see apply_neural_overrides). A forced capacity class cannot
     * be combined with any explicit shape override. */
    std::optional<std::size_t> neural_embedding_dim;
    std::optional<std::size_t> neural_hidden_layer_count;
    std::optional<std::array<std::size_t, 3u>> neural_hidden_widths;
};

const char *neural_capacity_class_name(NeuralCapacityClass capacity_class) noexcept;

/*
 * Total shared learned parameters for a recommendation (input/layer/output
 * weights and biases), counted exactly as the C core layout allocates them.
 * Pure and deterministic; never mutates state.
 */
std::uint64_t neural_parameter_count(const NeuralCapacityRecommendation &profile) noexcept;

/*
 * Apply parsed operator neural overrides to a recommendation (issue #73).
 * A forced capacity class replaces the automatic class's shape; explicit
 * shape overrides replace only the fields they name and keep the rest from
 * the automatic shape. Class and shape overrides are mutually exclusive, and
 * the combined effective shape must always pass the runtime policy-bounds
 * validation shared with neural_architecture_of. Throws std::runtime_error
 * on conflicts or an out-of-bounds shape.
 */
void apply_neural_overrides(NeuralCapacityRecommendation &profile,
                            const ResourceOverrides &overrides);

/*
 * Translate a capacity recommendation into the validated C23 architecture
 * descriptor used at first model creation (issue #73). The descriptor always
 * passes atp_graph_create_with_architecture: version 1, output scalar-1,
 * input = 2 * embedding, active hidden widths within the C-core limit and
 * inactive widths zero. Throws std::runtime_error if the recommendation
 * cannot be expressed within the runtime policy bounds.
 */
atp_neural_architecture neural_architecture_of(const NeuralCapacityRecommendation &profile);

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

    NeuralCapacityRecommendation neural;

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
