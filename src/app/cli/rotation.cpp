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
    for (int pass = 0; pass < kMaxModelPasses && size > caps.model_max_bytes; ++pass) {
        const std::size_t nodes = graph.stats().node_count;
        const double keep = 0.8 * static_cast<double>(caps.model_max_bytes) /
                            static_cast<double>(size);
        const std::size_t target =
            std::max<std::size_t>(1u, static_cast<std::size_t>(static_cast<double>(nodes) * keep));
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
    }
    if (size > caps.model_max_bytes) {
        out << "rotate: warning: model is " << size << " bytes, above the "
            << caps.model_max_bytes << "-byte cap\n";
    }
}

} // namespace cli
} // namespace atperson
