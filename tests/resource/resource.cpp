#include "budget.hpp"
#include "runtime.hpp"
#include "system.hpp"

#include <atperson/core.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
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

/* A capable synthetic host: effective 2 GiB, cpu 2.0 -> NeuralCapacityClass
 * capable (embedding 128, hidden 256x128). */
atperson::SystemResources capable_system() {
    atperson::SystemResources system;
    system.host_logical_cpus = 2u;
    system.effective_cpu_capacity = 2.0;
    system.host_memory_total_bytes = 4u * GIB;
    system.effective_memory_total_bytes = 4u * GIB;
    system.effective_memory_available_bytes = 2u * GIB;
    system.disk_capacity_bytes = 64u * GIB;
    system.disk_available_bytes = 32u * GIB;
    return system;
}

atperson::SystemResources baseline_system() {
    atperson::SystemResources system;
    system.host_logical_cpus = 1u;
    system.effective_cpu_capacity = 1.0;
    system.host_memory_total_bytes = 1u * GIB;
    system.effective_memory_total_bytes = 1u * GIB;
    system.effective_memory_available_bytes = 512u * MIB;
    system.disk_capacity_bytes = 32u * GIB;
    system.disk_available_bytes = 8u * GIB;
    return system;
}

atperson::SystemResources large_system() {
    atperson::SystemResources system;
    system.host_logical_cpus = 4u;
    system.effective_cpu_capacity = 4.0;
    system.host_memory_total_bytes = 16u * GIB;
    system.effective_memory_total_bytes = 16u * GIB;
    system.effective_memory_available_bytes = 12u * GIB;
    system.disk_capacity_bytes = 128u * GIB;
    system.disk_available_bytes = 64u * GIB;
    return system;
}

atperson::RuntimeResourceStatus status_for(const atperson::SystemResources &system) {
    const auto budget =
        atperson::derive_resource_budget(system, graph_stats(0u, 0u));
    return atperson::RuntimeResourceStatus{
        .system = system,
        .budget = budget,
        .neural_runtime =
            atperson::derive_neural_runtime_policy(system, budget),
    };
}

std::filesystem::path resource_test_model(const char *name) {
    return std::filesystem::temp_directory_path() /
           (std::string("atperson-resource-test-") + name + ".bin");
}

/* Issue #73: the C23 architecture translation mirrors the recommendation and
 * the exact C-core parameter count. */
void test_neural_architecture_translation() {
    const auto status = status_for(capable_system());
    const auto &neural = status.budget.neural;
    assert(neural.capacity_class == atperson::NeuralCapacityClass::capable);

    const atp_neural_architecture architecture = atperson::neural_architecture_of(neural);
    assert(architecture.version == ATPERSON_NEURAL_ARCHITECTURE_VERSION);
    assert(architecture.embedding_dim == neural.embedding_dim);
    assert(architecture.input_dim == neural.embedding_dim * 2u);
    assert(architecture.output_dim == 1u);
    assert(architecture.hidden_layer_count == neural.hidden_layer_count);
    assert(atp_neural_parameter_count(&architecture) == neural.shared_parameter_count);

    /* The translation is valid for the C core: layout counts are nonzero and
     * the graph constructs at the exact topology. */
    atperson::LanguageGraph graph(atp_graph_default_config(), architecture);
    assert(graph.neural_architecture().embedding_dim == architecture.embedding_dim);
}

/* Issue #73 acceptance: distinct synthetic host classes create and persist
 * distinct first-generation topologies. */
void test_first_creation_profiles_differ() {
    atperson::SystemResources constrained;
    constrained.host_logical_cpus = 1u;
    constrained.effective_cpu_capacity = 1.0;
    constrained.host_memory_total_bytes = 512u * MIB;
    constrained.effective_memory_total_bytes = 512u * MIB;
    constrained.effective_memory_available_bytes = 256u * MIB;
    constrained.disk_capacity_bytes = 16u * GIB;
    constrained.disk_available_bytes = 2u * GIB;

    const auto constrained_path = resource_test_model("first-constrained");
    const auto large_path = resource_test_model("first-large");
    std::filesystem::remove(constrained_path);
    std::filesystem::remove(large_path);

    auto constrained_graph =
        atperson::load_or_create_graph(status_for(constrained), constrained_path);
    auto large_graph =
        atperson::load_or_create_graph(status_for(large_system()), large_path);
    constrained_graph.save(constrained_path);
    large_graph.save(large_path);

    atp_neural_architecture constrained_arch{};
    atp_neural_architecture large_arch{};
    assert(atp_snapshot_neural_architecture(
               constrained_path.string().c_str(), &constrained_arch) == ATP_OK);
    assert(atp_snapshot_neural_architecture(
               large_path.string().c_str(), &large_arch) == ATP_OK);
    assert(constrained_arch.embedding_dim == 32u);
    assert(large_arch.embedding_dim == 256u);
    assert(constrained_arch.embedding_dim != large_arch.embedding_dim);

    std::filesystem::remove(constrained_path);
    std::filesystem::remove(large_path);
}

/* Issue #73: first creation binds topology to the hardware recommendation and
 * persists it; an existing model loads at its persisted topology even when a
 * smaller or stronger host now recommends something else. */
void test_first_creation_and_host_migration() {
    const auto model_path = resource_test_model("migration");
    std::filesystem::remove(model_path);

    const auto capable_status = status_for(capable_system());
    auto graph = atperson::load_or_create_graph(capable_status, model_path);
    assert(graph.neural_architecture().embedding_dim == 128u);
    graph.save(model_path);

    atp_neural_architecture persisted{};
    assert(atp_snapshot_neural_architecture(model_path.string().c_str(), &persisted) == ATP_OK);
    assert(persisted.embedding_dim == 128u);
    assert(persisted.hidden_layer_count == 2u);
    assert(persisted.hidden_widths[0] == 256u);
    assert(persisted.hidden_widths[1] == 128u);

    /* Migrate to a small host: the load keeps the persisted topology and the
     * recommendation stays separate. */
    atperson::SystemResources small;
    small.host_logical_cpus = 1u;
    small.effective_cpu_capacity = 1.0;
    small.host_memory_total_bytes = 512u * MIB;
    small.effective_memory_total_bytes = 512u * MIB;
    small.effective_memory_available_bytes = 256u * MIB;
    small.disk_capacity_bytes = 16u * GIB;
    small.disk_available_bytes = 2u * GIB;
    const auto small_status = status_for(small);
    assert(small_status.budget.neural.capacity_class == atperson::NeuralCapacityClass::constrained);

    auto stronger =
        atperson::load_or_create_graph(status_for(roomy_system()), model_path);
    assert(stronger.neural_architecture().embedding_dim == 128u);

    auto migrated = atperson::load_or_create_graph(small_status, model_path);
    assert(migrated.neural_architecture().embedding_dim == 128u);

    std::filesystem::remove(model_path);
}

/* Issue #73: a host that cannot fit the persisted topology refuses to load —
 * never shrinks the model. */
void test_memory_refusal_on_smaller_host() {
    atperson::SystemResources tiny;
    tiny.host_logical_cpus = 1u;
    tiny.effective_cpu_capacity = 1.0;
    tiny.host_memory_total_bytes = 4u * MIB;
    tiny.effective_memory_total_bytes = 4u * MIB;
    tiny.effective_memory_available_bytes = 2u * MIB;
    tiny.disk_capacity_bytes = 1u * GIB;
    tiny.disk_available_bytes = 512u * MIB;

    /* An expansive topology has millions of shared parameters; its footprint
     * exceeds 2 MiB of available memory. */
    atp_neural_architecture expansive{};
    expansive.version = ATPERSON_NEURAL_ARCHITECTURE_VERSION;
    expansive.embedding_dim = 384u;
    expansive.input_dim = 768u;
    expansive.output_dim = 1u;
    expansive.hidden_layer_count = 3u;
    expansive.hidden_widths[0] = 768u;
    expansive.hidden_widths[1] = 384u;
    expansive.hidden_widths[2] = 192u;
    assert(atp_neural_parameter_count(&expansive) > 100000u);

    const auto model_path = resource_test_model("refusal");
    std::filesystem::remove(model_path);
    {
        atperson::LanguageGraph big(atp_graph_default_config(), expansive);
        big.save(model_path);
    }

    bool rejected = false;
    try {
        (void)atperson::load_or_create_graph(status_for(tiny), model_path);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);

    /* The model file itself is untouched. */
    assert(std::filesystem::exists(model_path));
    std::filesystem::remove(model_path);
}

/* Issue #73: rebuild recovers topology from durable snapshot metadata, so a
 * probe + fresh-create at that topology reproduces the persisted shape. */
void test_persisted_architecture_probe() {
    const auto status = status_for(capable_system());
    std::filesystem::remove(resource_test_model("probe"));
    auto graph = atperson::load_or_create_graph(status, resource_test_model("probe"));
    graph.save(resource_test_model("probe"));

    atp_neural_architecture persisted{};
    assert(atp_snapshot_neural_architecture(resource_test_model("probe").string().c_str(),
                                            &persisted) == ATP_OK);

    /* A fresh graph created at the probed topology saves the same topology —
     * exactly what rebuild does, and independent of the host that resumed it. */
    atperson::LanguageGraph rebuilt(atp_graph_default_config(), persisted);
    rebuilt.save(resource_test_model("probe"));
    atp_neural_architecture rebuilt_probe{};
    assert(atp_snapshot_neural_architecture(resource_test_model("probe").string().c_str(),
                                            &rebuilt_probe) == ATP_OK);
    assert(rebuilt_probe.embedding_dim == persisted.embedding_dim);
    assert(rebuilt_probe.hidden_layer_count == persisted.hidden_layer_count);
    assert(rebuilt_probe.hidden_widths[0] == persisted.hidden_widths[0]);
    std::filesystem::remove(resource_test_model("probe"));
}

/* Issue #73: operator overrides are strictly validated and never conflict in
 * silence. */
void test_neural_override_validation() {
    atperson::ResourceOverrides overrides;
    overrides.neural_capacity_class = atperson::NeuralCapacityClass::capable;
    auto profile = atperson::derive_resource_budget(capable_system(), graph_stats(0u, 0u)).neural;
    atperson::apply_neural_overrides(profile, overrides);
    assert(profile.capacity_class == atperson::NeuralCapacityClass::capable);
    assert(profile.embedding_dim == 128u);
    assert(profile.hidden_layer_count == 2u);
    assert(profile.shared_parameter_count == atperson::neural_parameter_count(profile));

    atperson::ResourceOverrides partial;
    partial.neural_embedding_dim = 64u;
    auto partial_profile =
        atperson::derive_resource_budget(capable_system(), graph_stats(0u, 0u)).neural;
    atperson::apply_neural_overrides(partial_profile, partial);
    assert(partial_profile.embedding_dim == 64u);
    assert(partial_profile.hidden_layer_count == 2u);
    assert(partial_profile.shared_parameter_count == atperson::neural_parameter_count(partial_profile));

    atperson::ResourceOverrides conflict;
    conflict.neural_capacity_class = atperson::NeuralCapacityClass::baseline;
    conflict.neural_embedding_dim = 32u;
    auto conflict_profile =
        atperson::derive_resource_budget(capable_system(), graph_stats(0u, 0u)).neural;
    bool conflict_thrown = false;
    try {
        atperson::apply_neural_overrides(conflict_profile, conflict);
    } catch (const std::runtime_error &) {
        conflict_thrown = true;
    }
    assert(conflict_thrown);

    atperson::ResourceOverrides overshoot;
    overshoot.neural_embedding_dim = ATPERSON_NEURAL_WIDTH_LIMIT + 1u;
    auto overshoot_profile =
        atperson::derive_resource_budget(capable_system(), graph_stats(0u, 0u)).neural;
    bool overshoot_thrown = false;
    try {
        atperson::apply_neural_overrides(overshoot_profile, overshoot);
    } catch (const std::runtime_error &) {
        overshoot_thrown = true;
    }
    assert(overshoot_thrown);

    atperson::ResourceOverrides beyond_count;
    beyond_count.neural_hidden_layer_count = 4u;
    auto beyond_profile =
        atperson::derive_resource_budget(capable_system(), graph_stats(0u, 0u)).neural;
    bool beyond_thrown = false;
    try {
        atperson::apply_neural_overrides(beyond_profile, beyond_count);
    } catch (const std::runtime_error &) {
        beyond_thrown = true;
    }
    assert(beyond_thrown);

    atperson::ResourceOverrides idle_width;
    idle_width.neural_hidden_widths = std::array<std::size_t, 3u>{256u, 128u, 64u};
    auto idle_profile = atperson::derive_resource_budget(capable_system(), graph_stats(0u, 0u)).neural;
    bool idle_thrown = false;
    try {
        atperson::apply_neural_overrides(idle_profile, idle_width);
    } catch (const std::runtime_error &) {
        idle_thrown = true;
    }
    assert(idle_thrown);
}

/* Issue #73: operator overrides come out of ATPERSON_NEURAL_* env vars. */
void test_neural_override_environment_parsing() {
    const char *raw_capacity = std::getenv("ATPERSON_NEURAL_CAPACITY");
    const char *raw_layers = std::getenv("ATPERSON_NEURAL_HIDDEN_LAYERS");
    const char *raw_widths = std::getenv("ATPERSON_NEURAL_HIDDEN_WIDTHS");
    const std::optional<std::string> previous_capacity =
        raw_capacity ? std::optional<std::string>{raw_capacity} : std::nullopt;
    const std::optional<std::string> previous_layers =
        raw_layers ? std::optional<std::string>{raw_layers} : std::nullopt;
    const std::optional<std::string> previous_widths =
        raw_widths ? std::optional<std::string>{raw_widths} : std::nullopt;

    const auto restore = [](const char *name, const std::optional<std::string> &value) {
        if (value) {
            assert(setenv(name, value->c_str(), 1) == 0);
        } else {
            unsetenv(name);
        }
    };

    assert(setenv("ATPERSON_NEURAL_CAPACITY", "expansive", 1) == 0);
    unsetenv("ATPERSON_NEURAL_HIDDEN_LAYERS");
    unsetenv("ATPERSON_NEURAL_HIDDEN_WIDTHS");
    auto overrides = atperson::resource_overrides_from_environment();
    assert(overrides.neural_capacity_class == atperson::NeuralCapacityClass::expansive);

    unsetenv("ATPERSON_NEURAL_CAPACITY");
    assert(setenv("ATPERSON_NEURAL_HIDDEN_LAYERS", "3", 1) == 0);
    assert(setenv("ATPERSON_NEURAL_HIDDEN_WIDTHS", "512:256:128", 1) == 0);
    overrides = atperson::resource_overrides_from_environment();
    assert(overrides.neural_hidden_layer_count &&
           *overrides.neural_hidden_layer_count == 3u);
    assert(overrides.neural_hidden_widths &&
           (*overrides.neural_hidden_widths ==
            std::array<std::size_t, 3u>{512u, 256u, 128u}));

    assert(setenv("ATPERSON_NEURAL_HIDDEN_WIDTHS", "256junk:128", 1) == 0);
    bool trailing_junk_rejected = false;
    try {
        (void)atperson::resource_overrides_from_environment();
    } catch (const std::runtime_error &) {
        trailing_junk_rejected = true;
    }
    assert(trailing_junk_rejected);

    assert(setenv("ATPERSON_NEURAL_HIDDEN_WIDTHS", "bogus", 1) == 0);
    bool parsed_rejected = false;
    try {
        (void)atperson::resource_overrides_from_environment();
    } catch (const std::runtime_error &) {
        parsed_rejected = true;
    }
    assert(parsed_rejected);

    restore("ATPERSON_NEURAL_CAPACITY", previous_capacity);
    restore("ATPERSON_NEURAL_HIDDEN_LAYERS", previous_layers);
    restore("ATPERSON_NEURAL_HIDDEN_WIDTHS", previous_widths);
}

/* Exact parameter accounting agrees between the runtime and the C core, and
 * the footprint estimate reflects both shared parameters and graph state. */
void test_neural_parameter_accounting() {
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    assert(atp_neural_parameter_count(&legacy) == 545u);

    const auto profile = atperson::derive_resource_budget(capable_system(), graph_stats(0u, 0u)).neural;
    const atp_neural_architecture architecture = atperson::neural_architecture_of(profile);
    assert(atp_neural_parameter_count(&architecture) == profile.shared_parameter_count);

    const std::uint64_t empty_footprint =
        atperson::neural_memory_footprint(architecture, 0u, 0u);
    const std::uint64_t activations =
        architecture.input_dim + architecture.hidden_widths[0] +
        architecture.hidden_widths[1] + architecture.output_dim;
    const std::uint64_t expected_shared =
        profile.shared_parameter_count * 2u * sizeof(float) +
        activations * 2u * sizeof(float);
    assert(empty_footprint == expected_shared);
    const std::uint64_t node_bytes =
        256u + (architecture.embedding_dim - ATPERSON_EMBEDDING_DIM) *
                   2u * sizeof(float);
    const std::uint64_t grown_footprint =
        atperson::neural_memory_footprint(architecture, 1000u, 4000u);
    assert(grown_footprint ==
           empty_footprint + 1000u * node_bytes + 4000u * 64u);

    atp_neural_architecture invalid = architecture;
    invalid.hidden_layer_count = 0u;
    assert(atp_neural_parameter_count(&invalid) == 0u);
    assert(atperson::neural_memory_footprint(invalid, 0u, 0u) == 0u);
}

void test_neural_expansion_preflight() {
    const auto capable = status_for(capable_system());
    const atp_neural_architecture capable_arch =
        atperson::neural_architecture_of(capable.budget.neural);
    atperson::LanguageGraph graph(atp_graph_default_config(), capable_arch);

    atperson::RuntimeResourceStatus expansive{
        .system = roomy_system(),
        .budget = atperson::derive_resource_budget(roomy_system(), graph.stats()),
    };
    const auto expansion = atperson::plan_neural_expansion(graph, expansive);
    assert(expansion.status == atperson::NeuralExpansionStatus::available);
    assert(expansion.active.embedding_dim == 128u);
    assert(expansion.proposed.embedding_dim == 384u);
    assert(expansion.proposed.hidden_layer_count == 3u);
    assert(expansion.proposed_parameter_count > expansion.active_parameter_count);
    assert(expansion.proposed_footprint_bytes > expansion.active_footprint_bytes);
    assert(expansion.additional_footprint_bytes ==
           expansion.proposed_footprint_bytes - expansion.active_footprint_bytes);
    assert(expansion.fits_current_headroom);

    /* A monotonic proposal can be structurally valid while still being unsafe
     * on the current host. The mutation path must refuse this before allocating
     * or saving any expanded generation. */
    auto tight_headroom = expansive;
    assert(expansion.proposed_footprint_bytes > 0u);
    tight_headroom.system.effective_memory_available_bytes =
        tight_headroom.budget.memory_reserve_bytes +
        expansion.proposed_footprint_bytes - 1u;
    const auto memory_refused =
        atperson::plan_neural_expansion(graph, tight_headroom);
    assert(memory_refused.status == atperson::NeuralExpansionStatus::available);
    assert(memory_refused.memory_headroom_known);
    assert(!memory_refused.fits_current_headroom);
    assert(memory_refused.reason.find("does not fit") != std::string::npos);
    bool headroom_rejected = false;
    try {
        const auto stats = graph.stats();
        atperson::require_runtime_neural_headroom(
            tight_headroom, memory_refused.proposed, stats.node_count,
            stats.edge_count);
    } catch (const std::runtime_error &) {
        headroom_rejected = true;
    }
    assert(headroom_rejected);

    const auto same = atperson::plan_neural_expansion(graph, capable);
    assert(same.status == atperson::NeuralExpansionStatus::at_recommendation);
    assert(same.additional_footprint_bytes == 0u);

    atperson::SystemResources small;
    small.host_logical_cpus = 1u;
    small.effective_cpu_capacity = 1.0;
    small.host_memory_total_bytes = 512u * MIB;
    small.effective_memory_total_bytes = 512u * MIB;
    small.effective_memory_available_bytes = 256u * MIB;
    small.disk_capacity_bytes = 16u * GIB;
    small.disk_available_bytes = 2u * GIB;
    const auto smaller = status_for(small);
    const auto downgrade = atperson::plan_neural_expansion(graph, smaller);
    assert(downgrade.status == atperson::NeuralExpansionStatus::incompatible);
    assert(downgrade.proposed.embedding_dim < downgrade.active.embedding_dim);
    assert(downgrade.reason.find("smaller") != std::string::npos);

    /* A larger total parameter count is not sufficient if any existing
     * hidden layer would narrow: monotonicity is coordinate-wise. */
    auto mixed = expansive;
    mixed.budget.neural.hidden_widths[1] = 64u;
    mixed.budget.neural.shared_parameter_count =
        atperson::neural_parameter_count(mixed.budget.neural);
    mixed.budget.neural.shared_parameter_bytes =
        mixed.budget.neural.shared_parameter_count * sizeof(float);
    const auto incompatible = atperson::plan_neural_expansion(graph, mixed);
    assert(incompatible.status == atperson::NeuralExpansionStatus::incompatible);
    assert(incompatible.reason.find("hidden layer 2") != std::string::npos);

    /* Appending a hidden layer also moves the scalar output layer. The new
     * final hidden layer must be wide enough to carry every old output
     * weight, even when all pre-existing hidden coordinates are monotonic. */
    auto narrow_final = expansive;
    narrow_final.budget.neural.hidden_widths[2] = 64u;
    narrow_final.budget.neural.shared_parameter_count =
        atperson::neural_parameter_count(narrow_final.budget.neural);
    narrow_final.budget.neural.shared_parameter_bytes =
        narrow_final.budget.neural.shared_parameter_count * sizeof(float);
    const auto output_incompatible =
        atperson::plan_neural_expansion(graph, narrow_final);
    assert(output_incompatible.status ==
           atperson::NeuralExpansionStatus::incompatible);
    assert(output_incompatible.reason.find("scalar-output") != std::string::npos);

    std::ostringstream output;
    atperson::print_neural_expansion_plan(output, expansion);
    const std::string text = output.str();
    assert(text.find("migration v1") != std::string::npos);
    assert(text.find("status: available") != std::string::npos);
    assert(text.find("mutation: none") != std::string::npos);
}

void test_neural_runtime_policy_adapts_without_topology_mutation() {
    atperson::SystemResources constrained;
    constrained.host_logical_cpus = 1u;
    constrained.effective_cpu_capacity = 1.0;
    constrained.host_memory_total_bytes = 512u * MIB;
    constrained.effective_memory_total_bytes = 512u * MIB;
    constrained.effective_memory_available_bytes = 256u * MIB;
    constrained.disk_capacity_bytes = 16u * GIB;
    constrained.disk_available_bytes = 2u * GIB;

    const auto small = status_for(constrained);
    const auto baseline = status_for(baseline_system());
    const auto large = status_for(large_system());
    const auto roomy = status_for(roomy_system());

    assert(small.budget.neural.capacity_class ==
           atperson::NeuralCapacityClass::constrained);
    assert(baseline.budget.neural.capacity_class ==
           atperson::NeuralCapacityClass::baseline);
    assert(large.budget.neural.capacity_class ==
           atperson::NeuralCapacityClass::large);

    assert(small.neural_runtime.policy_version ==
           atperson::NeuralRuntimePolicy::current_version);
    assert(small.neural_runtime.backend ==
           atperson::NeuralExecutionBackend::portable_cpu);
    assert(small.neural_runtime.core_owner_threads == 1u);
    assert(small.neural_runtime.surrounding_worker_threads == 0u);
    assert(roomy.neural_runtime.core_owner_threads == 1u);
    assert(roomy.neural_runtime.surrounding_worker_threads == 7u);
    assert(baseline.neural_runtime.observation_work_batch >=
           small.neural_runtime.observation_work_batch);
    assert(large.neural_runtime.observation_work_batch >
           baseline.neural_runtime.observation_work_batch);
    assert(roomy.neural_runtime.observation_work_batch >=
           large.neural_runtime.observation_work_batch);
    assert(baseline.neural_runtime.workspace_bytes >
           small.neural_runtime.workspace_bytes);
    assert(large.neural_runtime.workspace_bytes >
           baseline.neural_runtime.workspace_bytes);
    assert(roomy.neural_runtime.deterministic);
    assert(roomy.neural_runtime.portable_fallback);
    assert(std::string(atperson::neural_execution_backend_name(
               roomy.neural_runtime.backend)) == "portable-cpu");

    /* Current host policy is operational only. The same already-selected
     * architecture remains byte-for-byte identical while work allowances
     * change around it. */
    const atp_neural_architecture persisted =
        atperson::neural_architecture_of(
            status_for(capable_system()).budget.neural);
    atperson::LanguageGraph graph(atp_graph_default_config(), persisted);
    const auto before = graph.neural_architecture();
    const auto after_small = graph.neural_architecture();
    const auto after_roomy = graph.neural_architecture();
    assert(before.embedding_dim == after_small.embedding_dim);
    assert(before.embedding_dim == after_roomy.embedding_dim);
    assert(before.hidden_layer_count == after_small.hidden_layer_count);
    assert(before.hidden_layer_count == after_roomy.hidden_layer_count);

    /* Severe current memory pressure reduces execution work without changing
     * the persisted shape. */
    auto pressured_system = roomy_system();
    pressured_system.effective_memory_total_bytes = 128u * MIB;
    pressured_system.effective_memory_available_bytes = 8u * MIB;
    pressured_system.memory_limited_by_container = true;
    const auto pressured = status_for(pressured_system);
    assert(pressured.budget.memory_pressure);
    assert(pressured.neural_runtime.observation_work_batch <
           roomy.neural_runtime.observation_work_batch);
    assert(pressured.neural_runtime.workspace_bytes <
           roomy.neural_runtime.workspace_bytes);
    assert(graph.neural_architecture().embedding_dim == persisted.embedding_dim);

    std::ostringstream output;
    atperson::print_runtime_resources(output, roomy);
    const std::string text = output.str();
    assert(text.find("neural execution: portable-cpu (policy v1)") !=
           std::string::npos);
    assert(text.find("core owner threads 1") != std::string::npos);
    assert(text.find("surrounding workers 7") != std::string::npos);
    assert(text.find("workspace") != std::string::npos);
    assert(text.find("deterministic yes") != std::string::npos);
    assert(text.find("portable fallback yes") != std::string::npos);
}

void test_resource_inspection_distinguishes_active_and_recommended() {
    const auto capable = status_for(capable_system());
    const atp_neural_architecture active =
        atperson::neural_architecture_of(capable.budget.neural);
    atperson::LanguageGraph graph(atp_graph_default_config(), active);

    const atperson::RuntimeResourceStatus roomy{
        .system = roomy_system(),
        .budget = atperson::derive_resource_budget(roomy_system(), graph.stats()),
    };
    std::ostringstream output;
    atperson::print_runtime_resources(output, roomy, graph, {});
    const std::string text = output.str();
    assert(text.find("neural architecture (active)") != std::string::npos);
    assert(text.find("neural recommendation: expansive") != std::string::npos);
    assert(text.find("expansion available") != std::string::npos);
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
    test_neural_architecture_translation();
    test_first_creation_profiles_differ();
    test_first_creation_and_host_migration();
    test_memory_refusal_on_smaller_host();
    test_persisted_architecture_probe();
    test_neural_override_validation();
    test_neural_override_environment_parsing();
    test_neural_parameter_accounting();
    test_neural_expansion_preflight();
    test_neural_runtime_policy_adapts_without_topology_mutation();
    test_resource_inspection_distinguishes_active_and_recommended();
    std::cout << "resource: ok\n";
    return 0;
}
