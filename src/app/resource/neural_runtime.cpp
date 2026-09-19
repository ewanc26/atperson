#include "neural_runtime.hpp"

#include <algorithm>
#include <cmath>

namespace atperson {
namespace {

constexpr std::uint64_t MIB = UINT64_C(1024) * UINT64_C(1024);
constexpr std::uint64_t MAX_WORKSPACE_BYTES = UINT64_C(256) * MIB;
constexpr std::uint64_t BYTES_PER_BATCH_STEP = MIB;

std::uint64_t subtract_floor(std::uint64_t value,
                             std::uint64_t amount) noexcept {
    return value > amount ? value - amount : 0u;
}

std::size_t whole_effective_cpus(const SystemResources &system) noexcept {
    const double effective = std::max(0.0, system.effective_cpu_capacity);
    std::size_t cpus = static_cast<std::size_t>(std::floor(effective));
    if (cpus == 0u && effective > 0.0) {
        cpus = 1u;
    }
    const std::size_t host =
        std::max<std::size_t>(1u, system.host_logical_cpus);
    return std::min(cpus == 0u ? 1u : cpus, host);
}

} // namespace

const char *neural_execution_backend_name(
    NeuralExecutionBackend backend) noexcept {
    switch (backend) {
    case NeuralExecutionBackend::portable_cpu:
        return "portable-cpu";
    }
    return "unknown";
}

NeuralRuntimePolicy derive_neural_runtime_policy(
    const SystemResources &system, const ResourceBudget &budget) noexcept {
    NeuralRuntimePolicy policy;

    const std::size_t effective_cpus = whole_effective_cpus(system);
    policy.core_owner_threads = 1u;
    policy.surrounding_worker_threads =
        effective_cpus > policy.core_owner_threads
            ? effective_cpus - policy.core_owner_threads
            : 0u;

    /*
     * Workspace is a conservative transient allowance, bounded both by the
     * graph-growth budget and by memory available after the global reserve.
     * Unknown available-memory telemetry falls back to the already-derived
     * growth budget rather than inventing capacity.
     */
    const bool memory_known =
        system.effective_memory_available_bytes != 0u;
    const std::uint64_t available_after_reserve =
        memory_known
            ? subtract_floor(system.effective_memory_available_bytes,
                             budget.memory_reserve_bytes)
            : budget.memory_growth_budget_bytes;
    policy.workspace_bytes = std::min(
        {MAX_WORKSPACE_BYTES, budget.memory_growth_budget_bytes / 4u,
         available_after_reserve / 4u});

    /*
     * The owner still processes observations one-by-one in durable ledger
     * order. This value bounds surrounding staged work only. CPU and workspace
     * independently constrain it so current pressure can reduce throughput
     * without reshaping learned state.
     */
    const std::size_t cpu_batch = std::clamp<std::size_t>(
        static_cast<std::size_t>(
            std::ceil(std::max(0.01, system.effective_cpu_capacity) * 16.0)),
        1u, 256u);
    const std::size_t memory_batch =
        policy.workspace_bytes == 0u
            ? 1u
            : std::clamp<std::size_t>(
                  static_cast<std::size_t>(
                      policy.workspace_bytes / BYTES_PER_BATCH_STEP),
                  1u, 256u);
    policy.observation_work_batch = std::min(cpu_batch, memory_batch);
    if (budget.memory_pressure) {
        policy.observation_work_batch =
            std::min<std::size_t>(policy.observation_work_batch, 4u);
    }

    return policy;
}

} // namespace atperson
