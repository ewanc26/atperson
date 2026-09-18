#include "budget.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {
namespace {

constexpr std::uint64_t KIB = 1024u;
constexpr std::uint64_t MIB = 1024u * KIB;
constexpr std::uint64_t GIB = 1024u * MIB;

/* Conservative budgeting reservations, deliberately above the issue #9
 * benchmark's observed ~176B/node and ~48B/edge footprints. They include
 * headroom for hash-index slots, allocator overhead and future layout drift. */
constexpr std::uint64_t NODE_GROWTH_BYTES = 256u;
constexpr std::uint64_t EDGE_GROWTH_BYTES = 64u;
constexpr std::uint64_t OBSERVATION_DISK_RESERVE_BYTES = 512u * KIB;

std::optional<std::uint64_t> environment_u64(const char *name) {
    const char *raw = std::getenv(name);
    if (!raw || raw[0] == '\0') {
        return std::nullopt;
    }
    const std::string_view text(raw);
    std::uint64_t value = 0u;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string(name) + " must be an unsigned integer");
    }
    /* Zero means automatic so the generated .env template can be filled with
     * an explicit 0 without disabling dynamic resource management. */
    return value == 0u ? std::nullopt : std::optional<std::uint64_t>{value};
}

std::optional<std::size_t> environment_size(const char *name) {
    const auto value = environment_u64(name);
    if (!value) {
        return std::nullopt;
    }
    if (*value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(std::string(name) + " exceeds this platform's size_t range");
    }
    return static_cast<std::size_t>(*value);
}

std::uint64_t subtract_floor(std::uint64_t value, std::uint64_t amount) {
    return value > amount ? value - amount : 0u;
}

std::uint64_t automatic_memory_reserve(std::uint64_t total) {
    if (total == 0u) {
        return 64u * MIB;
    }
    const std::uint64_t minimum = std::min(64u * MIB, total / 4u);
    return std::min(2u * GIB, std::max(minimum, total / 8u));
}

std::uint64_t automatic_disk_reserve(std::uint64_t capacity) {
    if (capacity == 0u) {
        return 256u * MIB;
    }
    const std::uint64_t minimum = std::min(256u * MIB, capacity / 4u);
    return std::min(8u * GIB, std::max(minimum, capacity / 20u));
}

std::size_t saturating_add(std::size_t current, std::uint64_t extra) {
    const std::uint64_t room =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() - current);
    return current + static_cast<std::size_t>(std::min(extra, room));
}

int automatic_sync_page_size(const SystemResources &system, const ResourceBudget &budget) {
    /* Page size is a transport/work-batching policy, not learned semantics.
     * Let fractional CPU quotas and severe memory pressure reduce it all the
     * way to one instead of preserving a desktop-oriented floor. */
    const double cpu_capacity = std::max(0.01, system.effective_cpu_capacity);
    const int cpu_bound = std::clamp(
        static_cast<int>(std::ceil(cpu_capacity * 12.0)), 1, 100);

    const std::uint64_t memory_steps = budget.memory_growth_budget_bytes / (8u * MIB);
    const int memory_bound = static_cast<int>(
        std::clamp<std::uint64_t>(1u + memory_steps, 1u, 100u));

    const std::uint64_t disk_items =
        budget.disk_write_budget_bytes / OBSERVATION_DISK_RESERVE_BYTES;
    const int disk_bound = static_cast<int>(
        std::clamp<std::uint64_t>(disk_items, 1u, 100u));

    return std::min({cpu_bound, memory_bound, disk_bound});
}

std::uint64_t shared_parameter_count(const NeuralCapacityRecommendation &profile) {
    const std::uint64_t input_width = static_cast<std::uint64_t>(profile.embedding_dim) * 2u;
    std::uint64_t count = 0u;
    std::uint64_t previous = input_width;
    for (std::size_t layer = 0u; layer < profile.hidden_layer_count; ++layer) {
        const std::uint64_t width = static_cast<std::uint64_t>(profile.hidden_widths[layer]);
        count += previous * width; /* weights */
        count += width;            /* bias */
        previous = width;
    }
    count += previous; /* final scalar-output weights */
    count += 1u;       /* final scalar-output bias */
    return count;
}

NeuralCapacityRecommendation neural_recommendation(const SystemResources &system,
                                                   const ResourceBudget &budget) {
    NeuralCapacityRecommendation profile;

    /* Keep neural recommendation headroom distinct from the graph's growth
     * ceiling. This is a recommendation only in the current fixed-shape core;
     * issue #63 owns the later durable architecture migration. */
    profile.memory_budget_bytes =
        std::min<std::uint64_t>(4u * GIB, budget.memory_growth_budget_bytes / 4u);
    if (budget.memory_growth_budget_bytes >= 32u * MIB) {
        profile.memory_budget_bytes =
            std::max<std::uint64_t>(8u * MIB, profile.memory_budget_bytes);
    }

    const double cpu = std::max(0.01, system.effective_cpu_capacity);
    if (profile.memory_budget_bytes >= 2u * GIB && cpu >= 8.0) {
        profile.capacity_class = NeuralCapacityClass::expansive;
        profile.embedding_dim = 384u;
        profile.hidden_layer_count = 3u;
        profile.hidden_widths = {768u, 384u, 192u};
        profile.runtime_batch_observations = 256u;
    } else if (profile.memory_budget_bytes >= 512u * MIB && cpu >= 4.0) {
        profile.capacity_class = NeuralCapacityClass::large;
        profile.embedding_dim = 256u;
        profile.hidden_layer_count = 2u;
        profile.hidden_widths = {512u, 256u, 0u};
        profile.runtime_batch_observations = 128u;
    } else if (profile.memory_budget_bytes >= 128u * MIB && cpu >= 2.0) {
        profile.capacity_class = NeuralCapacityClass::capable;
        profile.embedding_dim = 128u;
        profile.hidden_layer_count = 2u;
        profile.hidden_widths = {256u, 128u, 0u};
        profile.runtime_batch_observations = 64u;
    } else if (profile.memory_budget_bytes >= 32u * MIB && cpu >= 1.0) {
        profile.capacity_class = NeuralCapacityClass::baseline;
        profile.embedding_dim = 64u;
        profile.hidden_layer_count = 2u;
        profile.hidden_widths = {128u, 64u, 0u};
        profile.runtime_batch_observations = 32u;
    } else {
        profile.capacity_class = NeuralCapacityClass::constrained;
        profile.embedding_dim = 32u;
        profile.hidden_layer_count = 1u;
        profile.hidden_widths = {64u, 0u, 0u};
        profile.runtime_batch_observations =
            cpu < 0.5 || budget.memory_pressure ? 1u : 8u;
    }

    profile.shared_parameter_count = shared_parameter_count(profile);
    profile.shared_parameter_bytes = profile.shared_parameter_count * sizeof(float);
    return profile;
}

} // namespace

const char *neural_capacity_class_name(NeuralCapacityClass capacity_class) noexcept {
    switch (capacity_class) {
    case NeuralCapacityClass::constrained:
        return "constrained";
    case NeuralCapacityClass::baseline:
        return "baseline";
    case NeuralCapacityClass::capable:
        return "capable";
    case NeuralCapacityClass::large:
        return "large";
    case NeuralCapacityClass::expansive:
        return "expansive";
    }
    return "unknown";
}

ResourceOverrides resource_overrides_from_environment() {
    ResourceOverrides overrides;
    overrides.memory_growth_budget_bytes = environment_u64("ATPERSON_MEMORY_BUDGET_BYTES");
    overrides.disk_reserve_bytes = environment_u64("ATPERSON_DISK_RESERVE_BYTES");
    overrides.node_capacity_max = environment_size("ATPERSON_NODE_CAPACITY");
    overrides.edge_capacity_max = environment_size("ATPERSON_EDGE_CAPACITY");

    if (const auto page = environment_u64("ATPERSON_SYNC_PAGE_SIZE")) {
        if (*page > 100u) {
            throw std::runtime_error("ATPERSON_SYNC_PAGE_SIZE must be between 1 and 100");
        }
        overrides.sync_page_size = static_cast<int>(*page);
    }
    overrides.sync_max_observations = environment_u64("ATPERSON_SYNC_MAX_OBSERVATIONS");
    return overrides;
}

ResourceBudget derive_resource_budget(const SystemResources &system,
                                      const atp_graph_stats &graph,
                                      const ResourceOverrides &overrides) {
    ResourceBudget budget;

    const std::uint64_t total_memory = system.effective_memory_total_bytes;
    const std::uint64_t available_memory = system.effective_memory_available_bytes;
    budget.memory_reserve_bytes = automatic_memory_reserve(total_memory);
    const std::uint64_t memory_headroom =
        subtract_floor(available_memory, budget.memory_reserve_bytes);

    if (overrides.memory_growth_budget_bytes) {
        if (available_memory != 0u && *overrides.memory_growth_budget_bytes > memory_headroom) {
            throw std::runtime_error(
                "ATPERSON_MEMORY_BUDGET_BYTES exceeds memory available after the safety reserve");
        }
        budget.memory_growth_budget_bytes = *overrides.memory_growth_budget_bytes;
    } else {
        const std::uint64_t total_bound = total_memory == 0u ? memory_headroom : total_memory / 4u;
        budget.memory_growth_budget_bytes = std::min(total_bound, memory_headroom / 2u);
    }
    budget.memory_pressure =
        available_memory <= budget.memory_reserve_bytes || budget.memory_growth_budget_bytes < 16u * MIB;

    bool have_disk = false;
    const auto consider_disk = [&](const std::filesystem::path &path, std::uint64_t capacity,
                                   std::uint64_t available) {
        const std::uint64_t reserve =
            overrides.disk_reserve_bytes.value_or(automatic_disk_reserve(capacity));
        if (capacity != 0u && reserve > capacity) {
            throw std::runtime_error(
                "ATPERSON_DISK_RESERVE_BYTES exceeds filesystem capacity for " + path.string());
        }
        const std::uint64_t write_budget = subtract_floor(available, reserve) / 2u;
        if (!have_disk || write_budget < budget.disk_write_budget_bytes) {
            have_disk = true;
            budget.limiting_disk_path = path;
            budget.limiting_disk_capacity_bytes = capacity;
            budget.limiting_disk_available_bytes = available;
            budget.disk_reserve_bytes = reserve;
            budget.disk_write_budget_bytes = write_budget;
        }
    };

    if (!system.filesystems.empty()) {
        for (const auto &filesystem : system.filesystems) {
            consider_disk(filesystem.path, filesystem.capacity_bytes, filesystem.available_bytes);
        }
    } else {
        consider_disk({}, system.disk_capacity_bytes, system.disk_available_bytes);
    }
    budget.disk_pressure = !have_disk || budget.disk_write_budget_bytes < 16u * MIB;

    const std::uint64_t node_budget = budget.memory_growth_budget_bytes * 3u / 10u;
    const std::uint64_t edge_budget = budget.memory_growth_budget_bytes - node_budget;
    budget.node_capacity_max =
        saturating_add(graph.node_count, node_budget / NODE_GROWTH_BYTES);
    budget.edge_capacity_max =
        saturating_add(graph.edge_count, edge_budget / EDGE_GROWTH_BYTES);

    /* `0` means unlimited to the C core. Auto mode must never accidentally
     * translate critical memory pressure into an unlimited graph. */
    if (budget.node_capacity_max == 0u) {
        budget.node_capacity_max = 1u;
    }
    if (budget.edge_capacity_max == 0u) {
        budget.edge_capacity_max = 1u;
    }

    if (overrides.node_capacity_max) {
        budget.node_capacity_max = std::max(graph.node_count, *overrides.node_capacity_max);
    }
    if (overrides.edge_capacity_max) {
        budget.edge_capacity_max = std::max(graph.edge_count, *overrides.edge_capacity_max);
    }

    budget.max_input_bytes = std::clamp<std::uint64_t>(
        budget.memory_growth_budget_bytes / 4u, 64u * KIB, 64u * MIB);
    budget.inspection_item_limit = static_cast<std::size_t>(
        std::clamp<std::uint64_t>(budget.memory_growth_budget_bytes / (64u * KIB), 16u, 10000u));

    budget.sync_page_size =
        overrides.sync_page_size.value_or(automatic_sync_page_size(system, budget));

    const std::uint64_t automatic_observations = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(budget.sync_page_size),
        budget.disk_write_budget_bytes / OBSERVATION_DISK_RESERVE_BYTES);
    budget.sync_max_observations =
        overrides.sync_max_observations.value_or(automatic_observations);
    if (budget.sync_max_observations == 0u) {
        budget.sync_max_observations = 1u;
    }

    budget.neural = neural_recommendation(system, budget);
    return budget;
}

} // namespace atperson
