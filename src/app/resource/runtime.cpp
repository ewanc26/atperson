#include "runtime.hpp"

#include "budget.hpp"

#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {
namespace {

void print_bytes(std::ostream &out, std::uint64_t bytes) {
    constexpr double KIB = 1024.0;
    constexpr double MIB = 1024.0 * KIB;
    constexpr double GIB = 1024.0 * MIB;
    if (static_cast<double>(bytes) >= GIB) {
        out << std::fixed << std::setprecision(2) << static_cast<double>(bytes) / GIB << " GiB";
    } else if (static_cast<double>(bytes) >= MIB) {
        out << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / MIB << " MiB";
    } else if (static_cast<double>(bytes) >= KIB) {
        out << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / KIB << " KiB";
    } else {
        out << bytes << " B";
    }
}

void print_hidden(std::ostream &out, const std::uint32_t *widths, std::uint32_t count) {
    for (std::uint32_t layer = 0u; layer < count; ++layer) {
        if (layer != 0u) {
            out << 'x';
        }
        out << widths[layer];
    }
}

std::string hidden_string(const std::array<std::size_t, 3u> &widths, std::size_t count) {
    std::string result;
    for (std::size_t layer = 0u; layer < count; ++layer) {
        if (layer != 0u) {
            result += 'x';
        }
        result += std::to_string(widths[layer]);
    }
    return result;
}

bool neural_overrides_active(const ResourceOverrides &overrides) {
    return overrides.neural_capacity_class || overrides.neural_embedding_dim ||
           overrides.neural_hidden_layer_count || overrides.neural_hidden_widths;
}

std::uint64_t saturating_add_u64(std::uint64_t a, std::uint64_t b) noexcept {
    return b > std::numeric_limits<std::uint64_t>::max() - a
               ? std::numeric_limits<std::uint64_t>::max()
               : a + b;
}

std::uint64_t saturating_mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
    if (a == 0u || b == 0u) {
        return 0u;
    }
    return a > std::numeric_limits<std::uint64_t>::max() / b
               ? std::numeric_limits<std::uint64_t>::max()
               : a * b;
}

} // namespace

RuntimeResourceStatus inspect_runtime_resources(
    const atp_graph_stats &graph_stats,
    const std::vector<std::filesystem::path> &durable_paths,
    const ResourceOverrides &overrides) {
    RuntimeResourceStatus status;
    status.system = probe_system_resources(durable_paths);
    status.budget = derive_resource_budget(status.system, graph_stats, overrides);
    return status;
}

RuntimeResourceStatus inspect_runtime_resources(const atp_graph_stats &graph_stats,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides) {
    return inspect_runtime_resources(
        graph_stats, std::vector<std::filesystem::path>{data_path}, overrides);
}

RuntimeResourceStatus inspect_runtime_resources(
    const LanguageGraph &graph,
    const std::vector<std::filesystem::path> &durable_paths,
    const ResourceOverrides &overrides) {
    RuntimeResourceStatus status;
    status.system = probe_system_resources(durable_paths);
    const atp_neural_architecture active = graph.neural_architecture();
    status.budget =
        derive_resource_budget(status.system, graph.stats(), overrides, &active);
    return status;
}

RuntimeResourceStatus inspect_runtime_resources(const LanguageGraph &graph,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides) {
    return inspect_runtime_resources(
        graph, std::vector<std::filesystem::path>{data_path}, overrides);
}

RuntimeResourceStatus refresh_runtime_resources(
    LanguageGraph &graph, const std::vector<std::filesystem::path> &durable_paths,
    const ResourceOverrides &overrides) {
    RuntimeResourceStatus status = inspect_runtime_resources(graph, durable_paths, overrides);
    graph.set_capacity(status.budget.node_capacity_max, status.budget.edge_capacity_max);
    return status;
}

RuntimeResourceStatus refresh_runtime_resources(LanguageGraph &graph,
                                                const std::filesystem::path &data_path,
                                                const ResourceOverrides &overrides) {
    return refresh_runtime_resources(
        graph, std::vector<std::filesystem::path>{data_path}, overrides);
}

void require_runtime_write_headroom(const RuntimeResourceStatus &status) {
    if (status.budget.disk_pressure) {
        const std::string destination = status.budget.limiting_disk_path.empty()
                                            ? std::string("durable storage")
                                            : status.budget.limiting_disk_path.string();
        throw std::runtime_error(
            destination +
            " is inside atperson's dynamic filesystem safety reserve; refusing durable "
            "mutation until more space is available or ATPERSON_DISK_RESERVE_BYTES is "
            "explicitly adjusted");
    }
}

void require_runtime_snapshot_headroom(const RuntimeResourceStatus &status,
                                       std::uint64_t snapshot_bytes) {
    if (snapshot_bytes == 0u || status.system.effective_memory_available_bytes == 0u) {
        return;
    }
    const std::uint64_t available_after_reserve =
        status.system.effective_memory_available_bytes > status.budget.memory_reserve_bytes
            ? status.system.effective_memory_available_bytes - status.budget.memory_reserve_bytes
            : 0u;
    const std::uint64_t required =
        snapshot_bytes > std::numeric_limits<std::uint64_t>::max() / 2u
            ? std::numeric_limits<std::uint64_t>::max()
            : snapshot_bytes * 2u;
    if (required > available_after_reserve) {
        throw std::runtime_error(
            "model snapshot is too large for current memory headroom (conservative load "
            "estimate " +
            std::to_string(required) + " bytes, " + std::to_string(available_after_reserve) +
            " bytes available after reserve)");
    }
}

std::uint64_t neural_memory_footprint(const atp_neural_architecture &architecture,
                                      std::uint64_t node_count,
                                      std::uint64_t edge_count) noexcept {
    constexpr std::uint64_t LEGACY_NODE_BYTES = 256u;
    constexpr std::uint64_t EDGE_BYTES = 64u;
    const std::uint64_t parameters = atp_neural_parameter_count(&architecture);
    if (parameters == 0u) {
        return 0u;
    }

    std::uint64_t footprint =
        saturating_mul_u64(parameters, 2u * sizeof(float));
    std::uint64_t activations =
        static_cast<std::uint64_t>(architecture.input_dim) + architecture.output_dim;
    for (std::uint32_t i = 0u; i < architecture.hidden_layer_count; ++i) {
        activations = saturating_add_u64(activations, architecture.hidden_widths[i]);
    }
    footprint = saturating_add_u64(
        footprint, saturating_mul_u64(activations, 2u * sizeof(float)));

    std::uint64_t node_bytes = LEGACY_NODE_BYTES;
    if (architecture.embedding_dim > ATPERSON_EMBEDDING_DIM) {
        const std::uint64_t extra_dims =
            static_cast<std::uint64_t>(architecture.embedding_dim - ATPERSON_EMBEDDING_DIM);
        node_bytes = saturating_add_u64(
            node_bytes, saturating_mul_u64(extra_dims, 2u * sizeof(float)));
    }
    footprint = saturating_add_u64(
        footprint, saturating_mul_u64(node_count, node_bytes));
    footprint = saturating_add_u64(
        footprint, saturating_mul_u64(edge_count, EDGE_BYTES));
    return footprint;
}

void require_runtime_neural_headroom(const RuntimeResourceStatus &status,
                                     const atp_neural_architecture &architecture,
                                     std::uint64_t node_count, std::uint64_t edge_count) {
    if (status.system.effective_memory_available_bytes == 0u) {
        return;
    }
    const std::uint64_t footprint =
        neural_memory_footprint(architecture, node_count, edge_count);
    const std::uint64_t available_after_reserve =
        status.system.effective_memory_available_bytes > status.budget.memory_reserve_bytes
            ? status.system.effective_memory_available_bytes - status.budget.memory_reserve_bytes
            : 0u;
    if (footprint > available_after_reserve) {
        std::ostringstream message;
        message << "neural architecture (embedding " << architecture.embedding_dim << ", hidden ";
        print_hidden(message, architecture.hidden_widths, architecture.hidden_layer_count);
        message << ") needs ~" << footprint << " bytes of learned state, exceeding the "
                << available_after_reserve
                << " bytes available after the safety reserve; refusing to load instead of "
                   "silently shrinking a persisted model. Free memory or move this generation "
                   "to a larger machine; ATPERSON_NEURAL_* overrides do not reshape persisted "
                   "models";
        throw std::runtime_error(message.str());
    }
}

LanguageGraph load_or_create_graph(const RuntimeResourceStatus &status,
                                   const std::filesystem::path &model_path) {
    if (std::filesystem::exists(model_path)) {
        std::error_code error;
        const auto snapshot_bytes = std::filesystem::file_size(model_path, error);
        if (!error) {
            require_runtime_snapshot_headroom(status, snapshot_bytes);
        }
        atp_neural_architecture persisted{};
        const atp_status probe =
            atp_snapshot_neural_architecture(model_path.string().c_str(), &persisted);
        if (probe != ATP_OK) {
            throw std::runtime_error(
                "cannot read persisted neural architecture from " + model_path.string() +
                ": " + atp_status_string(probe));
        }
        require_runtime_neural_headroom(status, persisted, 0u, 0u);

        LanguageGraph graph = LanguageGraph::load(model_path);
        const atp_graph_stats stats = graph.stats();
        require_runtime_neural_headroom(status, graph.neural_architecture(), stats.node_count,
                                        stats.edge_count);
        return graph;
    }

    if (const auto parent = model_path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const atp_neural_architecture architecture = neural_architecture_of(status.budget.neural);
    require_runtime_neural_headroom(status, architecture, 0u, 0u);
    return LanguageGraph(atp_graph_default_config(), architecture);
}

void require_runtime_input_headroom(const RuntimeResourceStatus &status, std::uint64_t bytes) {
    if (bytes > status.budget.max_input_bytes) {
        throw std::runtime_error(
            "input exceeds the current dynamic memory budget (" + std::to_string(bytes) +
            " bytes requested, " + std::to_string(status.budget.max_input_bytes) +
            " bytes permitted)");
    }
}

void require_runtime_inspection_limit(const RuntimeResourceStatus &status, std::size_t items) {
    if (items > status.budget.inspection_item_limit) {
        throw std::runtime_error(
            "requested inspection limit exceeds the current dynamic memory budget (" +
            std::to_string(items) + " requested, " +
            std::to_string(status.budget.inspection_item_limit) + " permitted)");
    }
}

void print_runtime_resources(std::ostream &out, const RuntimeResourceStatus &status) {
    out << "cpu: " << status.system.host_logical_cpus << " host logical, " << std::fixed
        << std::setprecision(2) << status.system.effective_cpu_capacity << " effective"
        << (status.system.cpu_limited_by_container ? " (container-limited)" : "") << '\n';

    out << "memory: ";
    print_bytes(out, status.system.effective_memory_available_bytes);
    out << " available / ";
    print_bytes(out, status.system.effective_memory_total_bytes);
    out << " effective";
    if (status.system.memory_limited_by_container) {
        out << " (container-limited; host ";
        print_bytes(out, status.system.host_memory_total_bytes);
        out << ')';
    }
    out << '\n' << "memory reserve: ";
    print_bytes(out, status.budget.memory_reserve_bytes);
    out << "; graph growth budget: ";
    print_bytes(out, status.budget.memory_growth_budget_bytes);
    out << (status.budget.memory_pressure ? " (pressure)" : "") << '\n';

    const auto &neural = status.budget.neural;
    out << "neural recommendation: " << neural_capacity_class_name(neural.capacity_class)
        << " (policy v" << neural.policy_version << "), budget ";
    print_bytes(out, neural.memory_budget_bytes);
    out << "; embedding " << neural.embedding_dim << "; hidden ";
    for (std::size_t layer = 0u; layer < neural.hidden_layer_count; ++layer) {
        if (layer != 0u) {
            out << 'x';
        }
        out << neural.hidden_widths[layer];
    }
    out << "; shared params " << neural.shared_parameter_count << " / ";
    print_bytes(out, neural.shared_parameter_bytes);
    out << "; runtime batch " << neural.runtime_batch_observations << " observations\n";

    const auto &runtime = status.budget.neural_runtime;
    out << "neural runtime policy: " << runtime.backend << " v" << runtime.policy_version
        << "; core owners " << runtime.core_owner_threads << "; surrounding workers "
        << runtime.surrounding_worker_allowance << "; work batch "
        << runtime.observation_work_batch << "; transient workspace ";
    print_bytes(out, runtime.transient_workspace_bytes);
    out << "; deterministic " << (runtime.deterministic ? "yes" : "no")
        << "; portable fallback " << (runtime.portable_fallback ? "yes" : "no") << '\n';

    out << "limiting disk";
    if (!status.budget.limiting_disk_path.empty()) {
        out << " (" << status.budget.limiting_disk_path.string() << ')';
    }
    out << ": ";
    print_bytes(out, status.budget.limiting_disk_available_bytes);
    out << " available / ";
    print_bytes(out, status.budget.limiting_disk_capacity_bytes);
    out << "; reserve ";
    print_bytes(out, status.budget.disk_reserve_bytes);
    out << "; per-run write budget ";
    print_bytes(out, status.budget.disk_write_budget_bytes);
    out << (status.budget.disk_pressure ? " (pressure)" : "") << '\n';

    out << "graph ceilings: nodes " << status.budget.node_capacity_max << ", edges "
        << status.budget.edge_capacity_max << '\n'
        << "one-shot limits: input " << status.budget.max_input_bytes << " bytes, inspection "
        << status.budget.inspection_item_limit << " items\n"
        << "sync budget: page size " << status.budget.sync_page_size << ", observations "
        << status.budget.sync_max_observations << '\n';
}

void print_runtime_resources(std::ostream &out, const RuntimeResourceStatus &status,
                             const LanguageGraph &graph, const ResourceOverrides &overrides) {
    print_runtime_resources(out, status);

    const atp_neural_architecture active = graph.neural_architecture();
    const std::uint64_t active_params = atp_neural_parameter_count(&active);
    const atp_graph_stats stats = graph.stats();
    out << "neural architecture (active): embedding " << active.embedding_dim << "; hidden ";
    print_hidden(out, active.hidden_widths, active.hidden_layer_count);
    out << "; params " << active_params << " / ";
    print_bytes(out, active_params * sizeof(float));
    out << "; total footprint ~";
    print_bytes(out, neural_memory_footprint(active, stats.node_count, stats.edge_count));
    out << '\n';

    const auto &neural = status.budget.neural;
    out << "neural recommendation: " << neural_capacity_class_name(neural.capacity_class)
        << " (policy v" << neural.policy_version << ")";
    if (neural_overrides_active(overrides)) {
        out << " (operator override)";
    }
    out << "; embedding " << neural.embedding_dim << "; hidden "
        << hidden_string(neural.hidden_widths, neural.hidden_layer_count)
        << "; params " << neural.shared_parameter_count;
    if (neural.shared_parameter_count > active_params) {
        out << "; expansion available: +" << (neural.shared_parameter_count - active_params)
            << " params (~";
        print_bytes(out, (neural.shared_parameter_count - active_params) * sizeof(float));
        out << ')';
    } else if (neural.shared_parameter_count < active_params) {
        out << "; active architecture is above current recommended capacity";
    } else {
        out << "; at recommended capacity";
    }
    out << '\n';
}

} // namespace atperson
