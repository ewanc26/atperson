#ifndef ATPERSON_RESOURCE_NEURAL_RUNTIME_HPP
#define ATPERSON_RESOURCE_NEURAL_RUNTIME_HPP

#include "budget.hpp"
#include "system.hpp"

#include <cstddef>
#include <cstdint>

namespace atperson {

/*
 * Execution backend is operational policy, never durable learned topology.
 * Version 1 intentionally exposes only the portable deterministic CPU path;
 * future SIMD/accelerator backends must define numeric/replay compatibility
 * before becoming selectable.
 */
enum class NeuralExecutionBackend {
    portable_cpu,
};

struct NeuralRuntimePolicy {
    static constexpr std::uint32_t current_version = 1u;

    std::uint32_t policy_version{current_version};
    NeuralExecutionBackend backend{NeuralExecutionBackend::portable_cpu};

    /* The authoritative C23 graph/network remains owner-thread-only. */
    std::size_t core_owner_threads{1u};

    /*
     * Whole effective CPUs left for orchestration/fetch work after reserving
     * the core owner. Zero means use the sequential surrounding path.
     */
    std::size_t surrounding_worker_threads{};

    /*
     * Maximum observations staged as one runtime work unit around the serial
     * core. This may change with current pressure; it does not alter learning
     * order or persisted architecture.
     */
    std::size_t observation_work_batch{1u};

    /* Transient runtime workspace allowance, not preallocated/persisted. */
    std::uint64_t workspace_bytes{};

    bool deterministic{true};
    bool portable_fallback{true};
};

const char *neural_execution_backend_name(
    NeuralExecutionBackend backend) noexcept;

/*
 * Derive execution-only policy from the resources available now. Fixed inputs
 * produce a fixed policy. No graph or architecture mutation occurs.
 */
NeuralRuntimePolicy derive_neural_runtime_policy(
    const SystemResources &system, const ResourceBudget &budget) noexcept;

} // namespace atperson

#endif
