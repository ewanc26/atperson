#include "rotation.hpp"

#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {

namespace {

std::uint64_t parse_cap(const char *name) {
    const std::string raw = env_or(name, "");
    if (raw.empty()) {
        return 0u;
    }
    char *end = nullptr;
    const unsigned long long value = std::strtoull(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') {
        throw std::runtime_error(std::string(name) + " must be a non-negative byte count");
    }
    return static_cast<std::uint64_t>(value);
}

constexpr int kMaxModelPasses = 3;

/* The model keeps a mirror of the ledger's per-entry metadata that pruning
 * vocabulary cannot shrink (see docs/jetstream.md, "Bounding disk use"). */
constexpr std::uint64_t kMirrorBytesPerEntry = 130u;

/* A pass that shrinks the file by less than this fraction is not worth the
 * forgetting it costs. */
constexpr double kMinUsefulShrink = 0.02;

/* Nodes to keep for one pass: scale only the prunable (non-mirror) part of the
 * file. Callers have already checked size > cap and floor < cap. */
std::size_t model_keep_target(std::size_t nodes, std::uint64_t size, std::uint64_t floor,
                              std::uint64_t cap) {
    const double keep = 0.8 * static_cast<double>(cap - floor) / static_cast<double>(size - floor);
    return std::max<std::size_t>(1u, static_cast<std::size_t>(static_cast<double>(nodes) * keep));
}

} // namespace

SizeCaps size_caps_from_env() {
    SizeCaps caps;
    caps.ledger_max_bytes = parse_cap("ATPERSON_LEDGER_MAX_BYTES");
    caps.model_max_bytes = parse_cap("ATPERSON_MODEL_MAX_BYTES");
    return caps;
}

void rotate_by_size(std::ostream &out, Ledger &ledger, LanguageGraph &graph,
                    const std::filesystem::path &model_path, const SizeCaps &caps) {
    if (caps.ledger_max_bytes != 0u) {
        const auto report = ledger.release_payloads(caps.ledger_max_bytes);
        if (report.payloads_released != 0u) {
            out << "rotate: released " << report.payloads_released
                << " ledger payload(s), ledger " << report.bytes_before << " -> "
                << report.bytes_after << " bytes\n";
        }
        if (report.bytes_after > caps.ledger_max_bytes) {
            out << "rotate: warning: ledger is " << report.bytes_after
                << " bytes, above the " << caps.ledger_max_bytes
                << "-byte cap; per-entry metadata cannot be released\n";
        }
    }

    if (caps.model_max_bytes == 0u || !std::filesystem::exists(model_path)) {
        return;
    }
    std::uint64_t size = std::filesystem::file_size(model_path);
    /* Only the vocabulary-dependent part of the file can be pruned away. If the
     * cap sits at or below the ledger mirror, no amount of pruning reaches it;
     * refuse rather than erode the vocabulary while chasing it. */
    const std::uint64_t floor = ledger.count() * kMirrorBytesPerEntry;
    if (size > caps.model_max_bytes && floor >= caps.model_max_bytes) {
        out << "rotate: warning: model cap " << caps.model_max_bytes
            << " bytes is below the ~" << floor
            << "-byte ledger mirror; not pruning vocabulary\n";
        return;
    }
    for (int pass = 0; pass < kMaxModelPasses && size > caps.model_max_bytes && size > floor;
         ++pass) {
        const std::size_t target = model_keep_target(graph.stats().node_count, size, floor,
                                                     caps.model_max_bytes);
        const auto report = graph.prune_vocabulary(target);
        if (report.nodes_after == report.nodes_before) {
            break;
        }
        graph.save(model_path);
        const std::uint64_t before = size;
        size = std::filesystem::file_size(model_path);
        out << "rotate: pruned vocabulary " << report.nodes_before << " -> "
            << report.nodes_after << " node(s), " << report.edges_before << " -> "
            << report.edges_after << " edge(s), model " << before << " -> " << size
            << " bytes\n";
        if (size + static_cast<std::uint64_t>(kMinUsefulShrink * static_cast<double>(before)) >
            before) {
            break;
        }
    }
    if (size > caps.model_max_bytes) {
        out << "rotate: warning: model is " << size << " bytes, above the "
            << caps.model_max_bytes << "-byte cap\n";
    }
}

void plan_rotation(std::ostream &out, const Ledger &ledger, const LanguageGraph &graph,
                   const std::filesystem::path &model_path, const SizeCaps &caps) {
    out << "rotate (dry run): nothing will be changed\n";
    if (caps.ledger_max_bytes != 0u) {
        const auto plan = ledger.release_plan(caps.ledger_max_bytes);
        if (plan.payloads_released == 0u) {
            out << "rotate: ledger " << plan.bytes_before << " bytes, cap "
                << caps.ledger_max_bytes
                << ": would release no payloads"
                << (plan.bytes_before > caps.ledger_max_bytes
                        ? " (per-entry metadata alone exceeds the cap)"
                        : "")
                << '\n';
        } else {
            out << "rotate: ledger " << plan.bytes_before << " bytes, cap "
                << caps.ledger_max_bytes << ": would release " << plan.payloads_released
                << " payload(s), ~" << plan.bytes_after << " bytes after\n";
        }
    }
    if (caps.model_max_bytes != 0u && std::filesystem::exists(model_path)) {
        const std::uint64_t size = std::filesystem::file_size(model_path);
        const std::uint64_t floor = ledger.count() * kMirrorBytesPerEntry;
        const std::size_t nodes = graph.stats().node_count;
        if (size <= caps.model_max_bytes) {
            out << "rotate: model " << size << " bytes, cap " << caps.model_max_bytes
                << ": within the cap\n";
        } else if (floor >= caps.model_max_bytes) {
            out << "rotate: model " << size << " bytes, cap " << caps.model_max_bytes
                << " is below the ~" << floor
                << "-byte ledger mirror: would not prune vocabulary\n";
        } else {
            out << "rotate: model " << size << " bytes, cap " << caps.model_max_bytes
                << ": would prune vocabulary from " << nodes << " to ~"
                << model_keep_target(nodes, size, floor, caps.model_max_bytes)
                << " node(s) on the first pass (up to " << kMaxModelPasses << " passes)\n";
        }
    }
}

} // namespace cli
} // namespace atperson
