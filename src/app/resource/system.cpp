#include "system.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace atperson {
namespace {

#if defined(__linux__)

std::optional<std::string> read_text_file(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::optional<std::uint64_t> parse_u64(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\n' || text.front() == '\r')) {
        text.remove_prefix(1u);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1u);
    }
    if (text.empty() || text == "max") {
        return std::nullopt;
    }
    std::uint64_t value = 0u;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

#endif

std::uint32_t hardware_threads() {
    const unsigned count = std::thread::hardware_concurrency();
    return count == 0u ? 1u : static_cast<std::uint32_t>(count);
}

std::filesystem::path existing_probe_path(std::filesystem::path path) {
    std::error_code ec;
    if (path.empty()) {
        return std::filesystem::current_path(ec);
    }
    while (!path.empty() && !std::filesystem::exists(path, ec)) {
        const auto parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    if (!path.empty() && std::filesystem::exists(path, ec)) {
        return path;
    }
    return std::filesystem::current_path(ec);
}

void probe_disk(SystemResources &resources, const std::filesystem::path &data_path) {
    std::error_code ec;
    const auto info = std::filesystem::space(existing_probe_path(data_path), ec);
    if (!ec) {
        resources.disk_capacity_bytes = info.capacity;
        resources.disk_available_bytes = info.available;
    }
}

#if defined(__linux__)

struct LinuxMemory {
    std::uint64_t total{};
    std::uint64_t available{};
};

LinuxMemory linux_memory() {
    LinuxMemory result;
    std::ifstream input("/proc/meminfo");
    std::string key;
    std::uint64_t value = 0u;
    std::string unit;
    while (input >> key >> value >> unit) {
        if (key == "MemTotal:") {
            result.total = value * 1024u;
        } else if (key == "MemAvailable:") {
            result.available = value * 1024u;
        }
    }
    if (result.total != 0u && result.available != 0u) {
        return result;
    }

    struct sysinfo info {};
    if (sysinfo(&info) == 0) {
        const std::uint64_t unit_bytes = info.mem_unit == 0u ? 1u : info.mem_unit;
        result.total = static_cast<std::uint64_t>(info.totalram) * unit_bytes;
        result.available = static_cast<std::uint64_t>(info.freeram + info.bufferram) * unit_bytes;
    }
    return result;
}

std::optional<std::string> linux_cgroup_relative(std::string_view controller, bool unified) {
    std::ifstream input("/proc/self/cgroup");
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t first = line.find(':');
        if (first == std::string::npos) {
            continue;
        }
        const std::size_t second = line.find(':', first + 1u);
        if (second == std::string::npos) {
            continue;
        }
        const std::string_view controllers(line.data() + first + 1u, second - first - 1u);
        const std::string path = line.substr(second + 1u);
        if (unified && controllers.empty()) {
            return path;
        }
        if (!unified) {
            std::size_t start = 0u;
            while (start <= controllers.size()) {
                const std::size_t end = controllers.find(',', start);
                const std::string_view item = controllers.substr(
                    start, end == std::string_view::npos ? controllers.size() - start : end - start);
                if (item == controller) {
                    return path;
                }
                if (end == std::string_view::npos) {
                    break;
                }
                start = end + 1u;
            }
        }
    }
    return std::nullopt;
}

std::filesystem::path cgroup_join(const std::filesystem::path &base,
                                  const std::optional<std::string> &relative) {
    if (!relative || relative->empty() || *relative == "/") {
        return base;
    }
    return base / std::filesystem::path(*relative).relative_path();
}

std::optional<std::uint64_t> read_u64_file(const std::filesystem::path &path) {
    const auto text = read_text_file(path);
    return text ? parse_u64(*text) : std::nullopt;
}

std::optional<double> read_cpu_max(const std::filesystem::path &path) {
    const auto text = read_text_file(path);
    if (!text) {
        return std::nullopt;
    }
    std::istringstream input(*text);
    std::string quota_text;
    std::uint64_t period = 0u;
    input >> quota_text >> period;
    if (quota_text.empty() || quota_text == "max" || period == 0u) {
        return std::nullopt;
    }
    const auto quota = parse_u64(quota_text);
    if (!quota) {
        return std::nullopt;
    }
    return static_cast<double>(*quota) / static_cast<double>(period);
}

std::size_t parse_cpu_set(std::string_view text) {
    std::size_t count = 0u;
    std::size_t position = 0u;
    while (position < text.size()) {
        while (position < text.size() && (text[position] == ' ' || text[position] == ',' ||
                                           text[position] == '\n' || text[position] == '\r')) {
            ++position;
        }
        if (position >= text.size()) {
            break;
        }
        const std::size_t end = text.find(',', position);
        const std::string_view item = text.substr(
            position, end == std::string_view::npos ? text.size() - position : end - position);
        const std::size_t dash = item.find('-');
        if (dash == std::string_view::npos) {
            if (parse_u64(item)) {
                ++count;
            }
        } else {
            const auto first = parse_u64(item.substr(0u, dash));
            const auto last = parse_u64(item.substr(dash + 1u));
            if (first && last && *last >= *first) {
                count += static_cast<std::size_t>(*last - *first + 1u);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        position = end + 1u;
    }
    return count;
}

std::optional<std::size_t> read_cpu_set_file(const std::filesystem::path &path) {
    const auto text = read_text_file(path);
    if (!text) {
        return std::nullopt;
    }
    const std::size_t count = parse_cpu_set(*text);
    return count == 0u ? std::nullopt : std::optional<std::size_t>{count};
}

void apply_linux_cgroups(SystemResources &resources) {
    const auto unified = linux_cgroup_relative({}, true);
    if (unified) {
        const auto root = cgroup_join("/sys/fs/cgroup", unified);
        auto memory_limit = read_u64_file(root / "memory.max");
        if (const auto high = read_u64_file(root / "memory.high");
            high && (!memory_limit || *high < *memory_limit)) {
            memory_limit = high;
        }
        const auto memory_current = read_u64_file(root / "memory.current");
        if (memory_limit && memory_current &&
            (resources.host_memory_total_bytes == 0u ||
             *memory_limit < resources.host_memory_total_bytes)) {
            resources.effective_memory_total_bytes = *memory_limit;
            const std::uint64_t cgroup_available =
                *memory_current < *memory_limit ? *memory_limit - *memory_current : 0u;
            resources.effective_memory_available_bytes =
                std::min(resources.effective_memory_available_bytes, cgroup_available);
            resources.memory_limited_by_container = true;
        }

        double cpu = resources.effective_cpu_capacity;
        if (const auto set = read_cpu_set_file(root / "cpuset.cpus.effective")) {
            cpu = std::min(cpu, static_cast<double>(*set));
        } else if (const auto set = read_cpu_set_file(root / "cpuset.cpus")) {
            cpu = std::min(cpu, static_cast<double>(*set));
        }
        if (const auto quota = read_cpu_max(root / "cpu.max")) {
            cpu = std::min(cpu, *quota);
        }
        if (cpu < resources.effective_cpu_capacity) {
            resources.cpu_limited_by_container = true;
            resources.effective_cpu_capacity = std::max(0.01, cpu);
        }
        return;
    }

    const auto memory_rel = linux_cgroup_relative("memory", false);
    const auto memory_root = cgroup_join("/sys/fs/cgroup/memory", memory_rel);
    const auto memory_limit = read_u64_file(memory_root / "memory.limit_in_bytes");
    const auto memory_current = read_u64_file(memory_root / "memory.usage_in_bytes");
    if (memory_limit && memory_current && resources.host_memory_total_bytes != 0u &&
        *memory_limit < resources.host_memory_total_bytes * 2u) {
        resources.effective_memory_total_bytes =
            std::min(resources.effective_memory_total_bytes, *memory_limit);
        const std::uint64_t cgroup_available =
            *memory_current < *memory_limit ? *memory_limit - *memory_current : 0u;
        resources.effective_memory_available_bytes =
            std::min(resources.effective_memory_available_bytes, cgroup_available);
        resources.memory_limited_by_container = true;
    }

    const auto cpu_rel = linux_cgroup_relative("cpu", false);
    std::filesystem::path cpu_root = cgroup_join("/sys/fs/cgroup/cpu", cpu_rel);
    if (!std::filesystem::exists(cpu_root)) {
        cpu_root = cgroup_join("/sys/fs/cgroup/cpu,cpuacct", cpu_rel);
    }
    const auto quota = read_u64_file(cpu_root / "cpu.cfs_quota_us");
    const auto period = read_u64_file(cpu_root / "cpu.cfs_period_us");
    if (quota && period && *period != 0u) {
        const double quota_cpus = static_cast<double>(*quota) / static_cast<double>(*period);
        if (quota_cpus > 0.0 && quota_cpus < resources.effective_cpu_capacity) {
            resources.effective_cpu_capacity = quota_cpus;
            resources.cpu_limited_by_container = true;
        }
    }

    const auto cpuset_rel = linux_cgroup_relative("cpuset", false);
    const auto cpuset_root = cgroup_join("/sys/fs/cgroup/cpuset", cpuset_rel);
    if (const auto set = read_cpu_set_file(cpuset_root / "cpuset.cpus")) {
        if (static_cast<double>(*set) < resources.effective_cpu_capacity) {
            resources.effective_cpu_capacity = static_cast<double>(*set);
            resources.cpu_limited_by_container = true;
        }
    }
}

#endif

void probe_memory_and_cpu(SystemResources &resources) {
    resources.host_logical_cpus = hardware_threads();
    resources.effective_cpu_capacity = static_cast<double>(resources.host_logical_cpus);

#if defined(__APPLE__)
    std::uint64_t total = 0u;
    std::size_t total_size = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &total_size, nullptr, 0) == 0) {
        resources.host_memory_total_bytes = total;
        resources.effective_memory_total_bytes = total;
    }

    vm_statistics64_data_t vm {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page_size = 0u;
    const mach_port_t host = mach_host_self();
    if (host_page_size(host, &page_size) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) ==
            KERN_SUCCESS) {
        resources.effective_memory_available_bytes =
            static_cast<std::uint64_t>(vm.free_count + vm.inactive_count + vm.speculative_count) *
            static_cast<std::uint64_t>(page_size);
    }
#elif defined(_WIN32)
    MEMORYSTATUSEX memory {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        resources.host_memory_total_bytes = memory.ullTotalPhys;
        resources.effective_memory_total_bytes = memory.ullTotalPhys;
        resources.effective_memory_available_bytes = memory.ullAvailPhys;
    }
#elif defined(__linux__)
    const LinuxMemory memory = linux_memory();
    resources.host_memory_total_bytes = memory.total;
    resources.effective_memory_total_bytes = memory.total;
    resources.effective_memory_available_bytes = memory.available;
    apply_linux_cgroups(resources);
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long available_pages = sysconf(_SC_AVPHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        resources.host_memory_total_bytes =
            static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
        resources.effective_memory_total_bytes = resources.host_memory_total_bytes;
    }
    if (available_pages > 0 && page_size > 0) {
        resources.effective_memory_available_bytes =
            static_cast<std::uint64_t>(available_pages) * static_cast<std::uint64_t>(page_size);
    }
#endif

    if (resources.effective_memory_total_bytes == 0u) {
        resources.effective_memory_total_bytes = resources.host_memory_total_bytes;
    }
    if (resources.effective_memory_available_bytes > resources.effective_memory_total_bytes &&
        resources.effective_memory_total_bytes != 0u) {
        resources.effective_memory_available_bytes = resources.effective_memory_total_bytes;
    }
    if (!(resources.effective_cpu_capacity > 0.0) ||
        !std::isfinite(resources.effective_cpu_capacity)) {
        resources.effective_cpu_capacity = 1.0;
    }
}

} // namespace

SystemResources probe_system_resources(const std::filesystem::path &data_path) {
    SystemResources resources;
    probe_memory_and_cpu(resources);
    probe_disk(resources, data_path);
    return resources;
}

} // namespace atperson
