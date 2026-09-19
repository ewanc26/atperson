/*
 * Capacity/continual-learning benchmark foundation (issue #67).
 *
 * This benchmark deliberately crosses the C++ runtime capacity policy and the
 * authoritative C23 learning core: synthetic hardware profiles are translated
 * through derive_resource_budget()/neural_architecture_of(), then the exact
 * resulting topology is trained on the same deterministic two-domain fixture.
 *
 * "smoke" is small enough for normal CI and covers constrained/baseline/capable.
 * "bench" covers all five current capacity classes. Timing is reported but
 * never asserted; learned-state metrics are repeated and required to be exactly
 * deterministic for the same profile/seed/fixture.
 */

#include "resource/budget.hpp"
#include "internal.h"

#include <atperson/action.h>
#include <atperson/core.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t MIB = 1024ull * 1024ull;
constexpr std::uint64_t GIB = 1024ull * MIB;
constexpr std::uint32_t FIXTURE_VERSION = 1u;
constexpr std::uint64_t FIXTURE_SEED = UINT64_C(0x67cafe1234);

struct Profile {
    const char *name;
    atperson::SystemResources system;
};

struct Metrics {
    std::string profile;
    std::uint32_t policy_version{};
    std::uint32_t embedding_dim{};
    std::uint32_t hidden_layers{};
    std::array<std::uint32_t, ATPERSON_NEURAL_MAX_HIDDEN_LAYERS> hidden{};
    std::uint64_t parameters{};
    std::uint64_t neural_learned_bytes{};
    std::uint64_t observations{};
    std::uint64_t training_steps{};
    double mean_loss{};
    float domain_a_before{};
    float domain_a_after{};
    float domain_b_after{};
    float cross_domain_after{};
    std::size_t recall_returned{};
    std::size_t recall_matched{};
    std::size_t candidate_count{};
    std::string top_candidate;
    float top_candidate_score{};
    bool snapshot_exact{};
    double elapsed_ms{};
};

atperson::SystemResources system_for(double cpu, std::uint64_t total_memory,
                                     std::uint64_t available_memory) {
    atperson::SystemResources system;
    system.host_logical_cpus = static_cast<std::size_t>(std::ceil(cpu));
    system.effective_cpu_capacity = cpu;
    system.host_memory_total_bytes = total_memory;
    system.effective_memory_total_bytes = total_memory;
    system.effective_memory_available_bytes = available_memory;
    system.disk_capacity_bytes = 256u * GIB;
    system.disk_available_bytes = 128u * GIB;
    return system;
}

std::vector<Profile> profiles() {
    return {
        {"constrained", system_for(1.0, 512u * MIB, 256u * MIB)},
        {"baseline", system_for(1.0, 1u * GIB, 512u * MIB)},
        {"capable", system_for(2.0, 4u * GIB, 2u * GIB)},
        {"large", system_for(4.0, 16u * GIB, 12u * GIB)},
        {"expansive", system_for(8.0, 32u * GIB, 24u * GIB)},
    };
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void observe(atp_graph *graph, std::string_view text, std::uint64_t id,
             std::uint64_t observed_at) {
    const std::string source = "at://capacity-bench/" + std::to_string(id);
    bool remembered = false;
    const atp_status status = atp_graph_observe_with_memory(
        graph, std::string(text).c_str(), source.c_str(), "did:plc:capacity-bench",
        observed_at, atp_ledger_digest(text.data(), text.size()),
        ATPERSON_SCHEMA_VERSION, id, &remembered);
    require(status == ATP_OK,
            "observation failed: " + std::string(atp_status_string(status)));
}

float score_pair(const atp_graph *graph, const char *source, const char *target) {
    const int32_t source_index = atp_find_node(graph, source);
    const int32_t target_index = atp_find_node(graph, target);
    require(source_index >= 0 && target_index >= 0,
            std::string("missing benchmark token: ") + source + " or " + target);
    float score = 0.0f;
    const atp_status status =
        atp_network_score(graph, static_cast<std::uint32_t>(source_index),
                          static_cast<std::uint32_t>(target_index), &score);
    require(status == ATP_OK, "neural score failed");
    require(std::isfinite(score), "non-finite neural score");
    return score;
}

bool same_architecture(const atp_neural_architecture &a,
                       const atp_neural_architecture &b) {
    if (a.version != b.version || a.embedding_dim != b.embedding_dim ||
        a.input_dim != b.input_dim ||
        a.hidden_layer_count != b.hidden_layer_count ||
        a.output_dim != b.output_dim) {
        return false;
    }
    for (std::size_t i = 0; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        if (a.hidden_widths[i] != b.hidden_widths[i]) {
            return false;
        }
    }
    return true;
}

Metrics run_fixture(const Profile &profile, std::size_t repetitions,
                    bool measure_time) {
    const atp_graph_stats empty_stats{};
    const auto budget =
        atperson::derive_resource_budget(profile.system, empty_stats);
    require(std::string(atperson::neural_capacity_class_name(
                budget.neural.capacity_class)) == profile.name,
            std::string("synthetic system no longer selects expected profile ") +
                profile.name);

    const atp_neural_architecture architecture =
        atperson::neural_architecture_of(budget.neural);

    atp_graph_config config = atp_graph_default_config();
    config.seed = FIXTURE_SEED;
    config.enable_plasticity_control = true;
    config.plasticity_threshold = 2.0f;
    config.plasticity_scale = 0.25f;

    atp_graph *graph =
        atp_graph_create_with_architecture(&config, &architecture);
    require(graph != nullptr, "could not create capacity benchmark graph");

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t ledger_id = 1u;
    std::uint64_t at = 1000u;

    /* Keep the cross-domain negative target present before domain A training
     * without introducing an A<->B pair. */
    observe(graph, "forest", ledger_id++, at++);

    static constexpr std::array<std::string_view, 3> DOMAIN_A = {
        "moon silver night", "moon silver glow", "night moon silver"};
    for (std::size_t r = 0; r < repetitions; ++r) {
        observe(graph, DOMAIN_A[r % DOMAIN_A.size()], ledger_id++, at++);
    }

    const float domain_a_before = score_pair(graph, "moon", "silver");

    static constexpr std::array<std::string_view, 3> DOMAIN_B = {
        "forest green moss", "forest green fern", "moss forest green"};
    for (std::size_t r = 0; r < repetitions; ++r) {
        observe(graph, DOMAIN_B[r % DOMAIN_B.size()], ledger_id++, at++);
    }

    Metrics metrics;
    metrics.profile = profile.name;
    metrics.policy_version = budget.neural.policy_version;
    metrics.embedding_dim = architecture.embedding_dim;
    metrics.hidden_layers = architecture.hidden_layer_count;
    for (std::size_t i = 0; i < metrics.hidden.size(); ++i) {
        metrics.hidden[i] = architecture.hidden_widths[i];
    }
    metrics.parameters = atp_neural_parameter_count(&architecture);
    metrics.domain_a_before = domain_a_before;
    metrics.domain_a_after = score_pair(graph, "moon", "silver");
    metrics.domain_b_after = score_pair(graph, "forest", "green");
    metrics.cross_domain_after = score_pair(graph, "moon", "forest");

    atp_recall_report recall_report{};
    atp_episode recalled[8]{};
    std::size_t recalled_count = 0u;
    require(atp_graph_recall(graph, "moon silver", at + 100u, nullptr,
                             &recall_report, recalled, std::size(recalled),
                             &recalled_count) == ATP_OK,
            "recall benchmark failed");
    metrics.recall_returned = recalled_count;
    metrics.recall_matched = recall_report.episodes_matched;

    atp_action_candidate candidates[8]{};
    std::size_t candidate_count = 0u;
    require(atp_graph_action_candidates(graph, "moon", candidates,
                                        std::size(candidates),
                                        &candidate_count) == ATP_OK,
            "action candidate benchmark failed");
    metrics.candidate_count = candidate_count;
    if (candidate_count > 0u) {
        metrics.top_candidate = candidates[0].token;
        metrics.top_candidate_score = candidates[0].score;
    }

    const atp_graph_stats stats = atp_graph_get_stats(graph);
    metrics.observations = stats.observations;
    metrics.training_steps = stats.training_steps;
    metrics.mean_loss = stats.mean_loss;

    atp_neural_architecture_report neural_report{};
    require(atp_graph_neural_report(graph, &neural_report) == ATP_OK,
            "neural report failed");
    metrics.neural_learned_bytes =
        neural_report.shared_learned_state_bytes +
        static_cast<std::uint64_t>(stats.node_count) *
            neural_report.per_node_learned_state_bytes;

    const std::filesystem::path snapshot =
        std::filesystem::temp_directory_path() /
        ("atperson-capacity-bench-" + metrics.profile + ".bin");
    std::filesystem::remove(snapshot);
    require(atp_graph_save(graph, snapshot.string().c_str()) == ATP_OK,
            "capacity benchmark snapshot save failed");

    atp_status load_status = ATP_OK;
    atp_graph *loaded =
        atp_graph_load(snapshot.string().c_str(), &load_status);
    std::filesystem::remove(snapshot);
    require(loaded != nullptr && load_status == ATP_OK,
            "capacity benchmark snapshot load failed");

    atp_neural_architecture loaded_arch{};
    require(atp_graph_neural_architecture(loaded, &loaded_arch) == ATP_OK,
            "loaded architecture inspection failed");
    const atp_graph_stats loaded_stats = atp_graph_get_stats(loaded);
    metrics.snapshot_exact =
        same_architecture(architecture, loaded_arch) &&
        loaded_stats.node_count == stats.node_count &&
        loaded_stats.edge_count == stats.edge_count &&
        loaded_stats.observations == stats.observations &&
        loaded_stats.training_steps == stats.training_steps &&
        loaded_stats.mean_loss == stats.mean_loss &&
        score_pair(loaded, "moon", "silver") == metrics.domain_a_after &&
        score_pair(loaded, "forest", "green") == metrics.domain_b_after;

    atp_graph_destroy(loaded);
    atp_graph_destroy(graph);

    if (measure_time) {
        const auto ended = std::chrono::steady_clock::now();
        metrics.elapsed_ms =
            std::chrono::duration<double, std::milli>(ended - started).count();
    }
    return metrics;
}

bool deterministic_equal(const Metrics &a, const Metrics &b) {
    return a.profile == b.profile &&
           a.policy_version == b.policy_version &&
           a.embedding_dim == b.embedding_dim &&
           a.hidden_layers == b.hidden_layers &&
           a.hidden == b.hidden &&
           a.parameters == b.parameters &&
           a.neural_learned_bytes == b.neural_learned_bytes &&
           a.observations == b.observations &&
           a.training_steps == b.training_steps &&
           a.mean_loss == b.mean_loss &&
           a.domain_a_before == b.domain_a_before &&
           a.domain_a_after == b.domain_a_after &&
           a.domain_b_after == b.domain_b_after &&
           a.cross_domain_after == b.cross_domain_after &&
           a.recall_returned == b.recall_returned &&
           a.recall_matched == b.recall_matched &&
           a.candidate_count == b.candidate_count &&
           a.top_candidate == b.top_candidate &&
           a.top_candidate_score == b.top_candidate_score &&
           a.snapshot_exact == b.snapshot_exact;
}

void print_json(const Metrics &m, std::string_view mode) {
    std::cout << std::setprecision(9)
              << "{\"fixture_version\":" << FIXTURE_VERSION
              << ",\"mode\":\"" << mode << "\""
              << ",\"profile\":\"" << m.profile << "\""
              << ",\"policy_version\":" << m.policy_version
              << ",\"embedding_dim\":" << m.embedding_dim
              << ",\"hidden_layers\":" << m.hidden_layers
              << ",\"hidden_widths\":[";
    for (std::size_t i = 0; i < m.hidden_layers; ++i) {
        if (i != 0u) {
            std::cout << ',';
        }
        std::cout << m.hidden[i];
    }
    std::cout << "]"
              << ",\"parameters\":" << m.parameters
              << ",\"neural_learned_bytes\":" << m.neural_learned_bytes
              << ",\"observations\":" << m.observations
              << ",\"training_steps\":" << m.training_steps
              << ",\"mean_loss\":" << m.mean_loss
              << ",\"domain_a_before\":" << m.domain_a_before
              << ",\"domain_a_after\":" << m.domain_a_after
              << ",\"retention_delta\":"
              << (m.domain_a_after - m.domain_a_before)
              << ",\"domain_b_after\":" << m.domain_b_after
              << ",\"cross_domain_after\":" << m.cross_domain_after
              << ",\"domain_b_margin\":"
              << (m.domain_b_after - m.cross_domain_after)
              << ",\"recall_returned\":" << m.recall_returned
              << ",\"recall_matched\":" << m.recall_matched
              << ",\"candidate_count\":" << m.candidate_count
              << ",\"top_candidate\":\"" << m.top_candidate << "\""
              << ",\"top_candidate_score\":" << m.top_candidate_score
              << ",\"snapshot_exact\":"
              << (m.snapshot_exact ? "true" : "false")
              << ",\"elapsed_ms\":" << m.elapsed_ms << "}\n";
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            std::cerr << "usage: " << argv[0] << " <smoke|bench>\n";
            return 2;
        }
        const std::string_view mode = argv[1];
        if (mode != "smoke" && mode != "bench") {
            std::cerr << "usage: " << argv[0] << " <smoke|bench>\n";
            return 2;
        }

        const auto all_profiles = profiles();
        const std::size_t profile_count =
            mode == "smoke" ? 3u : all_profiles.size();
        const std::size_t repetitions = mode == "smoke" ? 4u : 16u;

        for (std::size_t i = 0; i < profile_count; ++i) {
            const Metrics first =
                run_fixture(all_profiles[i], repetitions, true);
            const Metrics repeat =
                run_fixture(all_profiles[i], repetitions, false);
            require(deterministic_equal(first, repeat),
                    std::string("non-deterministic learned metrics for ") +
                        all_profiles[i].name);
            require(first.snapshot_exact,
                    std::string("snapshot mismatch for ") +
                        all_profiles[i].name);
            require(first.recall_returned > 0u,
                    std::string("recall produced no evidence for ") +
                        all_profiles[i].name);
            require(first.candidate_count > 0u,
                    std::string("planner produced no candidates for ") +
                        all_profiles[i].name);
            print_json(first, mode);
        }

        std::cout << "capacity-benchmark: ok\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "capacity-benchmark: " << error.what() << '\n';
        return 1;
    }
}
