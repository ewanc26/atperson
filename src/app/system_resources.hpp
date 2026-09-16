#ifndef ATPERSON_SYSTEM_RESOURCES_HPP
#define ATPERSON_SYSTEM_RESOURCES_HPP

#include <cstdint>
#include <filesystem>
#include <vector>

namespace atperson {

struct FilesystemResources {
    std::filesystem::path path;
    std::uint64_t capacity_bytes{};
    std::uint64_t available_bytes{};
};

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

    /* Every filesystem that can receive durable atperson state. */
    std::vector<FilesystemResources> filesystems;

    /* Compatibility summary for callers that probe one path. */
    std::uint64_t disk_capacity_bytes{};
    std::uint64_t disk_available_bytes{};

    bool memory_limited_by_container{};
    bool cpu_limited_by_container{};
};

/*
 * Probe the current system. Each path selects a filesystem that can receive
 * durable state; missing paths are resolved through the nearest existing
 * parent. Duplicate filesystems are harmless and remain deterministic.
 */
SystemResources probe_system_resources(const std::vector<std::filesystem::path> &data_paths);
SystemResources probe_system_resources(const std::filesystem::path &data_path);

} // namespace atperson

#endif
