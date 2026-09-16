#ifndef ATPERSON_SYSTEM_RESOURCES_HPP
#define ATPERSON_SYSTEM_RESOURCES_HPP

#include <cstdint>
#include <filesystem>

namespace atperson {

/*
 * One point-in-time view of the resources the current process can actually
 * use. Host totals are kept for diagnostics; effective values fold in tighter
 * container/cgroup limits when the platform exposes them.
 */
struct SystemResources {
    std::uint32_t host_logical_cpus{1u};
    double effective_cpu_capacity{1.0};

    std::uint64_t host_memory_total_bytes{};
    std::uint64_t effective_memory_total_bytes{};
    std::uint64_t effective_memory_available_bytes{};

    std::uint64_t disk_capacity_bytes{};
    std::uint64_t disk_available_bytes{};

    bool memory_limited_by_container{};
    bool cpu_limited_by_container{};
};

/*
 * Probe the current system. `data_path` selects the filesystem whose capacity
 * matters for snapshots, ledgers and cursor state; a missing path is resolved
 * through the nearest existing parent. The function does no allocation based
 * on the returned values and has no learning side effects.
 */
SystemResources probe_system_resources(const std::filesystem::path &data_path);

} // namespace atperson

#endif
