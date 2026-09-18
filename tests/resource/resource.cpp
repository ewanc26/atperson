#include "budget.hpp"
#include "runtime.hpp"
#include "system.hpp"

#include <atperson/core.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t MIB = 1024ull * 1024ull;
constexpr std::uint64_t GIB = 1024ull * MIB;

atp_graph_stats graph_stats(std::size_t nodes, std::size_t edges) {
    atp_graph_stats stats{};
    stats.node_count = nodes;
    stats.edge_count = edges;
    return stats;
}

atperson::SystemResources roomy_system() {
    atperson::SystemResources system;
    system.host_logical_cpus = 8u;
    system.effective_cpu_capacity = 8.0;
    system.host_memory_total_bytes = 32u * GIB;
    system.effective_memory_total_bytes = 32u * GIB;
    system.effective_memory_available_bytes = 24u * GIB;
    system.disk_capacity_bytes = 1ull * 1024ull * GIB;
    system.disk_available_bytes = 800u * GIB;
    return system;
}

void test_budget_scales_with_reported_resources() {
    atperson::SystemResources small;
    small.host_logical_cpus = 1u;
    small.effective_cpu_capacity = 1.0;
    small.host_memory_total_bytes = 512u * MIB;
    small.effective_memory_total_bytes = 512u * MIB;
    small.effective_memory_available_bytes = 256u * MIB;
    small.disk_capacity_bytes = 16u * GIB;
    small.disk_available_bytes = 2u * GIB;

    atperson::SystemResources large = roomy_system();
    large.host_logical_cpus = 16u;
    large.effective_cpu_capacity = 16.0;
    large.host_memory_total_bytes = 64u * GIB;
    large.effective_memory_total_bytes = 64u * GIB;
    large.effective_memory_available_bytes = 48u * GIB;
    large.disk_capacity_bytes = 2ull * 1024ull * GIB;
    large.disk_available_bytes = 1ull * 1024ull * GIB;

    const auto current = graph_stats(1000u, 4000u);
    const auto small_budget = atperson::derive_resource_budget(small, current);
    const auto large_budget = atperson::derive_resource_budget(large, current);

    assert(small_budget.node_capacity_max >= current.node_count);
    assert(small_budget.edge_capacity_max >= current.edge_count);
    assert(large_budget.memory_growth_budget_bytes > small_budget.memory_growth_budget_bytes);
    assert(large_budget.node_capacity_max > small_budget.node_capacity_max);
    assert(large_budget.edge_capacity_max > small_budget.edge_capacity_max);
    assert(large_budget.sync_page_size >= small_budget.sync_page_size);
    assert(large_budget.sync_page_size <= 100);
}

void test_neural_recommendation_scales_without_mutating_learned_shape() {
    atperson::SystemResources small;
    small.host_logical_cpus = 1u;
    small.effective_cpu_capacity = 1.0;
    small.host_memory_total_bytes = 512u * MIB;
    small.effective_memory_total_bytes = 512u * MIB;
    small.effective_memory_available_bytes = 256u * MIB;
    small.disk_capacity_bytes = 16u * GIB;
    small.disk_available_bytes = 2u * GIB;

    const auto small_budget = atperson::derive_resource_budget(small, graph_stats(0u, 0u));
    const auto large_budget =
        atperson::derive_resource_budget(roomy_system(), graph_stats(0u, 0u));

    assert(small_budget.neural.policy_version == 1u);
    assert(small_budget.neural.capacity_class == atperson::NeuralCapacityClass::constrained);
    assert(large_budget.neural.capacity_class == atperson::NeuralCapacityClass::expansive);
    assert(large_budget.neural.memory_budget_bytes > small_budget.neural.memory_budget_bytes);
    assert(large_budget.neural.embedding_dim > small_budget.neural.embedding_dim);
    assert(large_budget.neural.hidden_layer_count > small_budget.neural.hidden_layer_count);
    assert(large_budget.neural.shared_parameter_count > small_budget.neural.shared_parameter_count);
    assert(large_budget.neural.shared_parameter_bytes ==
           large_budget.neural.shared_parameter_count * sizeof(float));
    assert(large_budget.neural.runtime_batch_observations >
           small_budget.neural.runtime_batch_observations);
    assert(std::string(atperson::neural_capacity_class_name(
               large_budget.neural.capacity_class)) == "expansive");

    auto cpu_limited = roomy_system();
    cpu_limited.effective_cpu_capacity = 0.25;
    cpu_limited.cpu_limited_by_container = true;
    const auto limited_budget =
        atperson::derive_resource_budget(cpu_limited, graph_stats(0u, 0u));
    assert(limited_budget.neural.capacity_class == atperson::NeuralCapacityClass::constrained);
}

void test_fractional_cpu_can_reduce_page_to_one() {
    atperson::SystemResources constrained = roomy_system();
    constrained.host_logical_cpus = 8u;
    constrained.effective_cpu_capacity = 0.05;
    constrained.cpu_limited_by_container = true;

    const auto budget = atperson::derive_resource_budget(constrained, graph_stats(0u, 0u));
    assert(budget.sync_page_size == 1);
}

void test_existing_graph_is_never_evicted_by_pressure() {
    atperson::SystemResources constrained;
    constrained.host_logical_cpus = 1u;
    constrained.effective_cpu_capacity = 0.5;
    constrained.host_memory_total_bytes = 256u * MIB;
    constrained.effective_memory_total_bytes = 128u * MIB;
    constrained.effective_memory_available_bytes = 16u * MIB;
    constrained.disk_capacity_bytes = 4u * GIB;
    constrained.disk_available_bytes = 2u * GIB;
    constrained.memory_limited_by_container = true;
    constrained.cpu_limited_by_container = true;

    const auto current = graph_stats(25000u, 120000u);
    const auto budget = atperson::derive_resource_budget(constrained, current);

    assert(budget.memory_pressure);
    assert(budget.node_capacity_max >= current.node_count);
    assert(budget.edge_capacity_max >= current.edge_count);
    assert(budget.node_capacity_max != 0u);
    assert(budget.edge_capacity_max != 0u);
}

void test_disk_pressure_stops_normal_write_budget() {
    atperson::SystemResources constrained;
    constrained.host_logical_cpus = 2u;
    constrained.effective_cpu_capacity = 2.0;
    constrained.host_memory_total_bytes = 4u * GIB;
    constrained.effective_memory_total_bytes = 4u * GIB;
    constrained.effective_memory_available_bytes = 2u * GIB;
    constrained.disk_capacity_bytes = 2u * GIB;
    constrained.disk_available_bytes = 64u * MIB;

    const auto budget = atperson::derive_resource_budget(constrained, graph_stats(0u, 0u));
    assert(budget.disk_pressure);
    assert(budget.sync_page_size >= 1);
    assert(budget.sync_max_observations >= 1u);
}

void test_tightest_durable_filesystem_wins() {
    atperson::SystemResources system = roomy_system();
    system.filesystems = {
        {.path = "/large", .capacity_bytes = 1ull * 1024ull * GIB,
         .available_bytes = 100u * GIB},
        {.path = "/small", .capacity_bytes = 10u * GIB,
         .available_bytes = 400u * MIB},
    };

    const auto budget = atperson::derive_resource_budget(system, graph_stats(0u, 0u));
    assert(budget.limiting_disk_path == std::filesystem::path("/small"));
    assert(budget.limiting_disk_available_bytes == 400u * MIB);
    assert(budget.disk_pressure);
}

void test_explicit_overrides_win() {
    atperson::SystemResources system = roomy_system();

    atperson::ResourceOverrides overrides;
    overrides.memory_growth_budget_bytes = 512u * MIB;
    overrides.disk_reserve_bytes = 3u * GIB;
    overrides.node_capacity_max = 12345u;
    overrides.edge_capacity_max = 54321u;
    overrides.sync_page_size = 77;
    overrides.sync_max_observations = 1234u;

    const auto budget =
        atperson::derive_resource_budget(system, graph_stats(100u, 200u), overrides);
    assert(budget.memory_growth_budget_bytes == 512u * MIB);
    assert(budget.disk_reserve_bytes == 3u * GIB);
    assert(budget.node_capacity_max == 12345u);
    assert(budget.edge_capacity_max == 54321u);
    assert(budget.sync_page_size == 77);
    assert(budget.sync_max_observations == 1234u);
}

void test_snapshot_load_preflight() {
    atperson::SystemResources system;
    system.host_logical_cpus = 2u;
    system.effective_cpu_capacity = 2.0;
    system.host_memory_total_bytes = 1u * GIB;
    system.effective_memory_total_bytes = 1u * GIB;
    system.effective_memory_available_bytes = 512u * MIB;
    system.disk_capacity_bytes = 16u * GIB;
    system.disk_available_bytes = 8u * GIB;

    atperson::RuntimeResourceStatus status{
        .system = system,
        .budget = atperson::derive_resource_budget(system, graph_stats(0u, 0u)),
    };
    atperson::require_runtime_snapshot_headroom(status, 64u * MIB);

    bool rejected = false;
    try {
        atperson::require_runtime_snapshot_headroom(status, 256u * MIB);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
}

void test_probe_smoke() {
    const auto resources = atperson::probe_system_resources(std::filesystem::current_path());
    assert(resources.host_logical_cpus >= 1u);
    assert(resources.effective_cpu_capacity > 0.0);
    assert(resources.effective_memory_total_bytes == 0u ||
           resources.effective_memory_available_bytes <= resources.effective_memory_total_bytes);
    assert(resources.disk_capacity_bytes == 0u ||
           resources.disk_available_bytes <= resources.disk_capacity_bytes);

    const auto multiple = atperson::probe_system_resources(
        std::vector<std::filesystem::path>{std::filesystem::current_path(),
                                          std::filesystem::temp_directory_path()});
    assert(multiple.filesystems.size() == 2u);
}

} // namespace

int main() {
    test_budget_scales_with_reported_resources();
    test_neural_recommendation_scales_without_mutating_learned_shape();
    test_fractional_cpu_can_reduce_page_to_one();
    test_existing_graph_is_never_evicted_by_pressure();
    test_disk_pressure_stops_normal_write_budget();
    test_tightest_durable_filesystem_wins();
    test_explicit_overrides_win();
    test_snapshot_load_preflight();
    test_probe_smoke();
    std::cout << "resource: ok\n";
    return 0;
}
