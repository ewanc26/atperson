#include "resource_runtime.hpp"

#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>

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
    return inspect_runtime_resources(graph.stats(), durable_paths, overrides);
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

} // namespace atperson
