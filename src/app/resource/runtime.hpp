#ifndef ATPERSON_RESOURCE_RUNTIME_HPP
#define ATPERSON_RESOURCE_RUNTIME_HPP

#include "atperson/graph.hpp"
#include "budget.hpp"
#include "system.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace atperson {

struct RuntimeResourceStatus {
    SystemResources system;
    ResourceBudget budget;
};

/*
 * Read-only preflight for issue #66. Migration version 1 defines only the
 * monotonic-shape eligibility contract; it does not mutate learned state.
 */
enum class NeuralExpansionStatus {
    at_recommendation,
    available,
    incompatible,
};

struct NeuralExpansionPlan {
    static constexpr std::uint32_t migration_version = 1u;

    NeuralExpansionStatus status{NeuralExpansionStatus::at_recommendation};
    atp_neural_architecture active{};
    atp_neural_architecture proposed{};
    std::uint64_t active_parameter_count{};
    std::uint64_t proposed_parameter_count{};
    std::uint64_t active_footprint_bytes{};
    std::uint64_t proposed_footprint_bytes{};
    std::uint64_t additional_footprint_bytes{};
    std::uint64_t available_after_reserve_bytes{};
    bool memory_headroom_known{};
    bool fits_current_headroom{true};
    std::string reason;
};

const char *neural_expansion_status_name(NeuralExpansionStatus status) noexcept;

/*
 * Compare the persisted active topology with the current recommendation.
 * A migration is eligible only when embedding width, hidden-layer count and
 * every pre-existing hidden-layer width are non-decreasing, with at least one
 * strict increase. This is inspection only; no graph state is modified.
 */
NeuralExpansionPlan plan_neural_expansion(const LanguageGraph &graph,
                                          const RuntimeResourceStatus &status);

void print_neural_expansion_plan(std::ostream &out, const NeuralExpansionPlan &plan);

/* Probe + derive without mutating graph policy. */
RuntimeResourceStatus inspect_runtime_resources(
    const atp_graph_stats &graph_stats,
    const std::vector<std::filesystem::path> &durable_paths,
    const ResourceOverrides &overrides);
RuntimeResourceStatus inspect_runtime_resources(const atp_graph_stats &graph_stats,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides);
RuntimeResourceStatus inspect_runtime_resources(
    const LanguageGraph &graph,
    const std::vector<std::filesystem::path> &durable_paths,
    const ResourceOverrides &overrides);
RuntimeResourceStatus inspect_runtime_resources(const LanguageGraph &graph,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides);

/* Probe + derive and apply #9 node/edge ceilings to the live graph. */
RuntimeResourceStatus refresh_runtime_resources(
    LanguageGraph &graph, const std::vector<std::filesystem::path> &durable_paths,
    const ResourceOverrides &overrides);
RuntimeResourceStatus refresh_runtime_resources(LanguageGraph &graph,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides);

/* Refuse new durable writes when any destination is inside its safety reserve. */
void require_runtime_write_headroom(const RuntimeResourceStatus &status);

/* Refuse an obviously unsafe snapshot load before allocating the graph. */
void require_runtime_snapshot_headroom(const RuntimeResourceStatus &status,
                                       std::uint64_t snapshot_bytes);

/*
 * Conservative learned-state memory footprint for hosting `architecture`
 * with `node_count` nodes and `edge_count` edges (issue #73). The estimate
 * includes value+importance storage for shared parameters and per-node
 * embeddings, training scratch, and conservative structural node/edge bytes.
 * An estimate, never a promise.
 */
std::uint64_t neural_memory_footprint(const atp_neural_architecture &architecture,
                                      std::uint64_t node_count,
                                      std::uint64_t edge_count) noexcept;

/*
 * Refuse to host an architecture when its learned-state footprint exceeds the
 * memory available after the safety reserve. Never shrinks a persisted
 * architecture: a smaller machine gets this explicit refusal, and an operator
 * override that over-parks on a small box is refused at creation.
 */
void require_runtime_neural_headroom(const RuntimeResourceStatus &status,
                                     const atp_neural_architecture &architecture,
                                     std::uint64_t node_count, std::uint64_t edge_count);

/*
 * First-run creation and guarded load (issue #73). `status` must be the
 * no-graph derivation for the same durable paths (see main).
 *
 * When the model does not exist, the current resource budget's neural
 * recommendation is translated into a validated C23 architecture and a fresh
 * graph is created at exactly that topology; the first save persists it. When
 * it exists, the persisted architecture is loaded and must FIT within current
 * headroom or the call refrains (explicit error) instead of shrinking it.
 * The caller still calls refresh_runtime_resources(graph, ...) afterwards to
 * apply node/edge ceilings and refresh live status with the graph loaded.
 */
LanguageGraph load_or_create_graph(const RuntimeResourceStatus &status,
                                   const std::filesystem::path &model_path);

/* Bound one-shot allocations driven by CLI input/result limits. */
void require_runtime_input_headroom(const RuntimeResourceStatus &status, std::uint64_t bytes);
void require_runtime_inspection_limit(const RuntimeResourceStatus &status, std::size_t items);

/* Human-readable diagnostics for `atperson resources`. */
void print_runtime_resources(std::ostream &out, const RuntimeResourceStatus &status);

/*
 * `atperson resources` with the live graph available: reports the active
 * (persisted) architecture, the current recommendation, and the expansion
 * headroom between them — the three values issue #73 requires to be kept
 * separate in inspection. `overrides` merely annotates operator-forced
 * recommendations.
 */
void print_runtime_resources(std::ostream &out, const RuntimeResourceStatus &status,
                             const LanguageGraph &graph, const ResourceOverrides &overrides);

} // namespace atperson

#endif
