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

std::uint64_t node_growth_bytes(std::size_t embedding_dim) {
    if (embedding_dim <= ATPERSON_EMBEDDING_DIM) {
        return NODE_GROWTH_BYTES;
    }
    const std::uint64_t extra_dims =
        static_cast<std::uint64_t>(embedding_dim - ATPERSON_EMBEDDING_DIM);
    return NODE_GROWTH_BYTES + extra_dims * 2u * sizeof(float);
}

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

} // namespace

std::uint64_t neural_parameter_count(const NeuralCapacityRecommendation &profile) noexcept {
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

namespace {

/* The per-class canonical recommendation shape (issue #64). Automatic class
 * selection and forced-class overrides share these constants so a forced
 * class always matches what the same budget/CPU would have chosen. */
void apply_class_shape(NeuralCapacityRecommendation &profile, NeuralCapacityClass capacity_class,
                       bool constrained_low_cpu, bool memory_pressure) {
    profile.capacity_class = capacity_class;
    switch (capacity_class) {
    case NeuralCapacityClass::expansive:
        /* The production-scale profiles deliberately clear the minimum
         * 10-million shared-parameter floor.  Keep all widths aligned to
         * accelerator-friendly multiples while the portable C23 owner
         * remains the authoritative trainer. */
        profile.embedding_dim = 1024u;
        profile.hidden_layer_count = 3u;
        profile.hidden_widths = {3072u, 2560u, 1280u};
        profile.runtime_batch_observations = 256u;
        break;
    case NeuralCapacityClass::large:
        /* 10,361,089 shared parameters.  This is the normal desktop/server
         * target, selected only after the existing resource budget has
         * established sufficient headroom. */
        profile.embedding_dim = 768u;
        profile.hidden_layer_count = 3u;
        profile.hidden_widths = {2304u, 2048u, 1024u};
        profile.runtime_batch_observations = 128u;
        break;
    case NeuralCapacityClass::capable:
        profile.embedding_dim = 128u;
        profile.hidden_layer_count = 2u;
        profile.hidden_widths = {256u, 128u, 0u};
        profile.runtime_batch_observations = 64u;
        break;
    case NeuralCapacityClass::baseline:
        profile.embedding_dim = 64u;
        profile.hidden_layer_count = 2u;
        profile.hidden_widths = {128u, 64u, 0u};
        profile.runtime_batch_observations = 32u;
        break;
    case NeuralCapacityClass::constrained:
        profile.embedding_dim = 32u;
        profile.hidden_layer_count = 1u;
        profile.hidden_widths = {64u, 0u, 0u};
        profile.runtime_batch_observations =
            constrained_low_cpu || memory_pressure ? 1u : 8u;
        break;
    }
}

NeuralCapacityClass automatic_capacity_class(std::uint64_t memory_budget_bytes, double cpu) {
    if (memory_budget_bytes >= 2u * GIB && cpu >= 8.0) {
        return NeuralCapacityClass::expansive;
    }
    if (memory_budget_bytes >= 512u * MIB && cpu >= 4.0) {
        return NeuralCapacityClass::large;
    }
    if (memory_budget_bytes >= 128u * MIB && cpu >= 2.0) {
        return NeuralCapacityClass::capable;
    }
    if (memory_budget_bytes >= 32u * MIB && cpu >= 1.0) {
        return NeuralCapacityClass::baseline;
    }
    return NeuralCapacityClass::constrained;
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
    apply_class_shape(profile, automatic_capacity_class(profile.memory_budget_bytes, cpu),
                      cpu < 0.5, budget.memory_pressure);

    profile.shared_parameter_count = neural_parameter_count(profile);
    profile.shared_parameter_bytes = profile.shared_parameter_count * sizeof(float);
    return profile;
}

/* Runtime policy bounds for the first-creation architecture recommendation
 * (issue #73). The C core allows up to ATPERSON_NEURAL_MAX_HIDDEN_LAYERS
 * hidden layers, but the recommendation's width vector holds three, so the
 * runtime policy bound is three; the C-core width and parameter limits still
 * apply. */
void validate_neural_shape(const NeuralCapacityRecommendation &profile) {
    const std::string limits = "runtime recommendation bounds: embedding in [1, " +
                               std::to_string(ATPERSON_NEURAL_WIDTH_LIMIT) +
                               "], hidden layers in [1, 3], hidden widths in [1, " +
                               std::to_string(ATPERSON_NEURAL_WIDTH_LIMIT) +
                               "], parameters at most " +
                               std::to_string(ATPERSON_NEURAL_PARAMETER_LIMIT);
    if (profile.embedding_dim == 0u || profile.embedding_dim > ATPERSON_NEURAL_WIDTH_LIMIT) {
        throw std::runtime_error("neural embedding dim outside policy bounds: " + limits);
    }
    const std::uint64_t input_dim = static_cast<std::uint64_t>(profile.embedding_dim) * 2u;
    if (input_dim > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("neural input dim overflows the architecture encoding");
    }
    if (profile.hidden_layer_count == 0u || profile.hidden_layer_count > 3u) {
        throw std::runtime_error("neural hidden layer count outside policy bounds: " + limits);
    }
    for (std::size_t layer = 0u; layer < profile.hidden_layer_count; ++layer) {
        if (profile.hidden_widths[layer] == 0u ||
            profile.hidden_widths[layer] > ATPERSON_NEURAL_WIDTH_LIMIT) {
            throw std::runtime_error(
                "neural hidden layer " + std::to_string(layer + 1u) +
                " width outside policy bounds: " + limits);
        }
    }
    for (std::size_t layer = profile.hidden_layer_count; layer < 3u; ++layer) {
        if (profile.hidden_widths[layer] != 0u) {
            throw std::runtime_error(
                "neural hidden layer " + std::to_string(layer + 1u) +
                " must have zero width because it is beyond the declared hidden layer count");
        }
    }
    if (neural_parameter_count(profile) > ATPERSON_NEURAL_PARAMETER_LIMIT) {
        throw std::runtime_error("neural shared parameter count exceeds the C-core limit of " +
                                 std::to_string(ATPERSON_NEURAL_PARAMETER_LIMIT) + ": " + limits);
    }
}

std::optional<NeuralCapacityClass> neural_capacity_class_from_environment(const char *name) {
    const char *raw = std::getenv(name);
    if (!raw || raw[0] == '\0') {
        return std::nullopt;
    }
    const std::string_view text(raw);
    if (text == "auto" || text == "automatic") {
        return std::nullopt;
    }
    const std::string_view names[] = {"constrained", "baseline", "capable", "large", "expansive"};
    for (std::size_t i = 0u; i < std::size(names); ++i) {
        if (text == names[i]) {
            return static_cast<NeuralCapacityClass>(i);
        }
    }
    throw std::runtime_error(std::string(name) +
                             " must be one of: constrained, baseline, capable, large, expansive, "
                             "or auto");
}

/* Colon-separated hidden widths (e.g. "256:128"). At least one and at most
 * three entries; each parsed as a strict size_t. Zero-width entries are left
 * to the shared shape validation so mixed automatic/explicit shapes get one
 * coherent error. */
std::optional<std::array<std::size_t, 3u>> neural_hidden_widths_from_environment(const char *name) {
    const char *raw = std::getenv(name);
    if (!raw || raw[0] == '\0') {
        return std::nullopt;
    }
    const std::string_view text(raw);
    std::array<std::size_t, 3u> widths{0u, 0u, 0u};
    std::size_t count = 0u;
    std::size_t start = 0u;
    while (start <= text.size()) {
        const std::size_t colon = text.find(':', start);
        const std::size_t end = colon == std::string_view::npos ? text.size() : colon;
        if (count == widths.size()) {
            throw std::runtime_error(std::string(name) + " lists more than three hidden widths");
        }
        const std::string_view token = text.substr(start, end - start);
        std::size_t width = 0u;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), width);
        if (token.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size()) {
            throw std::runtime_error(std::string(name) + " entries must be unsigned integers");
        }
        widths[count++] = width;
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1u;
    }
    (void)count; /* trailing slots stay zero, which downstream validation rejects or treats as unset */
    return widths;
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
    overrides.neural_capacity_class = neural_capacity_class_from_environment("ATPERSON_NEURAL_CAPACITY");
    overrides.neural_embedding_dim = environment_size("ATPERSON_NEURAL_EMBEDDING_DIM");
    overrides.neural_hidden_layer_count = environment_size("ATPERSON_NEURAL_HIDDEN_LAYERS");
    overrides.neural_hidden_widths = neural_hidden_widths_from_environment("ATPERSON_NEURAL_HIDDEN_WIDTHS");

    if (const auto page = environment_u64("ATPERSON_SYNC_PAGE_SIZE")) {
        if (*page > 100u) {
            throw std::runtime_error("ATPERSON_SYNC_PAGE_SIZE must be between 1 and 100");
        }
        overrides.sync_page_size = static_cast<int>(*page);
    }
    overrides.sync_max_observations = environment_u64("ATPERSON_SYNC_MAX_OBSERVATIONS");
    return overrides;
}

void apply_neural_overrides(NeuralCapacityRecommendation &profile,
                            const ResourceOverrides &overrides) {
    const bool shape_requested = overrides.neural_embedding_dim ||
                                 overrides.neural_hidden_layer_count ||
                                 overrides.neural_hidden_widths;
    if (overrides.neural_capacity_class && shape_requested) {
        throw std::runtime_error(
            "ATPERSON_NEURAL_CAPACITY cannot be combined with "
            "ATPERSON_NEURAL_EMBEDDING_DIM, ATPERSON_NEURAL_HIDDEN_LAYERS or "
            "ATPERSON_NEURAL_HIDDEN_WIDTHS");
    }

    if (overrides.neural_capacity_class) {
        /* A forced class selects the exact canonical shape for that class;
         * the runtime batch lower bound is not applied because the operator
         * chose the class explicitly. */
        apply_class_shape(profile, *overrides.neural_capacity_class, false, false);
        validate_neural_shape(profile);
    } else if (shape_requested) {
        /* Unset shape fields keep the automatic class's shape; the combined
         * shape must then pass validation as a whole. */
        if (overrides.neural_embedding_dim) {
            profile.embedding_dim = *overrides.neural_embedding_dim;
        }
        if (overrides.neural_hidden_layer_count) {
            profile.hidden_layer_count = *overrides.neural_hidden_layer_count;
        }
        if (overrides.neural_hidden_widths) {
            profile.hidden_widths = *overrides.neural_hidden_widths;
        }
        validate_neural_shape(profile);
    } else {
        return;
    }

    profile.shared_parameter_count = neural_parameter_count(profile);
    profile.shared_parameter_bytes = profile.shared_parameter_count * sizeof(float);
}

atp_neural_architecture neural_architecture_of(const NeuralCapacityRecommendation &profile) {
    validate_neural_shape(profile);
    atp_neural_architecture architecture{};
    architecture.version = ATPERSON_NEURAL_ARCHITECTURE_VERSION;
    architecture.embedding_dim = static_cast<std::uint32_t>(profile.embedding_dim);
    architecture.input_dim = static_cast<std::uint32_t>(profile.embedding_dim) * 2u;
    architecture.output_dim = 1u;
    architecture.hidden_layer_count = static_cast<std::uint32_t>(profile.hidden_layer_count);
    for (std::size_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        architecture.hidden_widths[i] = static_cast<std::uint32_t>(
            i < profile.hidden_widths.size() ? profile.hidden_widths[i] : 0u);
    }
    return architecture;
}

ResourceBudget derive_resource_budget(
    const SystemResources &system, const atp_graph_stats &graph,
    const ResourceOverrides &overrides,
    const atp_neural_architecture *active_architecture) {
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
        available_memory <= budget.memory_reserve_bytes ||
        budget.memory_growth_budget_bytes < 16u * MIB;

    budget.neural = neural_recommendation(system, budget);
    apply_neural_overrides(budget.neural, overrides);

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
    const std::size_t growth_embedding_dim =
        active_architecture ? active_architecture->embedding_dim : budget.neural.embedding_dim;
    budget.node_capacity_max =
        saturating_add(graph.node_count, node_budget / node_growth_bytes(growth_embedding_dim));
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

    return budget;
}

} // namespace atperson
