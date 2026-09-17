// CLI read-only graph inspection commands: assoc, candidates, familiarity,
// recall, groups.
//
// Implementation of the contracts in graph.hpp. Every body here is
// moved byte-faithfully from the original single-file dispatch in
// src/app/main.cpp; behaviour, ordering and output text are unchanged.

#include "graph.hpp"

#include "config.hpp"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <ostream>

namespace atperson {
namespace cli {

int run_assoc(std::ostream &out, const RuntimeResourceStatus &resource_status,
              const LanguageGraph &graph, std::string_view token, const char *limit_value) {
    const int limit = limit_value ? parse_limit(limit_value, 10) : 10;
    atperson::require_runtime_inspection_limit(resource_status,
                                               static_cast<std::size_t>(limit));
    for (const auto &association :
         graph.associations(token, static_cast<std::size_t>(limit))) {
        out << association.token << '\t' << std::fixed << std::setprecision(4)
            << association.score << '\t' << association.observations << '\t' << std::hex
            << association.last_source_hash << std::dec << '\n';
    }
    return 0;
}

int run_candidates(std::ostream &out, const RuntimeResourceStatus &resource_status,
                   const LanguageGraph &graph, std::string_view context,
                   const char *limit_value) {
    const int limit = limit_value ? parse_limit(limit_value, 10) : 10;
    atperson::require_runtime_inspection_limit(resource_status,
                                               static_cast<std::size_t>(limit));
    for (const auto &candidate :
         graph.action_candidates(context, static_cast<std::size_t>(limit))) {
        out << candidate.token << '\t' << std::fixed << std::setprecision(4)
            << candidate.score << '\t' << candidate.association_score << '\t'
            << candidate.familiarity_score << '\t' << candidate.support_score << '\t'
            << candidate.supporting_observations << '\t' << candidate.context_matches << '\n';
    }
    return 0;
}

int run_familiarity(std::ostream &out, const LanguageGraph &graph, std::string_view token) {
    out << std::fixed << std::setprecision(4) << graph.familiarity(token) << '\n';
    return 0;
}

namespace {

float parse_float(const char *value, const char *name) {
    const std::string_view text(value);
    float parsed = 0.0f;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(parsed)) {
        throw std::runtime_error(std::string(name) + " must be a finite number");
    }
    return parsed;
}

const char *recall_gate_name(atp_recall_gate gate) {
    switch (gate) {
    case ATP_RECALL_GATE_NONE:
        return "none";
    case ATP_RECALL_GATE_DISABLED:
        return "disabled";
    case ATP_RECALL_GATE_PREFILTER:
        return "prefilter";
    case ATP_RECALL_GATE_MIN_OVERLAP:
        return "min-overlap";
    }
    return "unknown";
}

} // namespace

int run_recall(std::ostream &out, const RuntimeResourceStatus &resource_status,
               LanguageGraph &graph, std::string_view query, const char *limit_value,
               const char *min_overlap_value, const char *max_prefilter_value,
               const char *enable_value, std::uint64_t at_epoch) {
    const int limit = limit_value ? parse_limit(limit_value, 10) : 10;
    atperson::require_runtime_inspection_limit(resource_status,
                                               static_cast<std::size_t>(limit));

    atp_recall_config config = atp_recall_default_config();
    if (min_overlap_value) {
        config.min_overlap = parse_float(min_overlap_value, "min-overlap");
        if (config.min_overlap < 0.0f) {
            throw std::runtime_error("min-overlap must be >= 0");
        }
    }
    if (max_prefilter_value) {
        /* 0 means unbounded (the default), so parse directly rather than
         * through parse_limit's positive-only check. */
        const std::string_view text(max_prefilter_value);
        std::size_t parsed = 0u;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            throw std::runtime_error("max-prefilter must be a non-negative integer");
        }
        config.max_prefilter = parsed;
    }
    if (enable_value && enable_value == std::string_view("off")) {
        config.disable = true;
    }

    atp_recall_report report;
    for (const auto &episode :
         graph.recall(query, at_epoch, &config, &report, static_cast<std::size_t>(limit))) {
        out << "ledger " << episode.ledger_id << '\t' << "at " << episode.observed_at << '\t'
            << "recall " << episode.recall_count << '\t' << episode.source_id << '\t';
        for (std::uint32_t t = 0u; t < episode.token_count; ++t) {
            out << '<' << episode.summary[t].node_index << ':' << std::fixed
                << std::setprecision(1) << episode.summary[t].weight << "> ";
        }
        out << '\n';
    }
    out << "recall-report gate=" << recall_gate_name(report.gate)
        << " total=" << report.episodes_total << " scanned=" << report.episodes_scanned
        << " matched=" << report.episodes_matched << " returned=" << report.episodes_returned
        << " groups-total=" << report.groups_total
        << " groups-scanned=" << report.groups_scanned
        << " min-overlap=" << std::fixed << std::setprecision(2) << config.min_overlap
        << " max-prefilter=" << config.max_prefilter << '\n';
    return 0;
}

int run_groups(std::ostream &out, const RuntimeResourceStatus &resource_status,
               const LanguageGraph &graph) {
    const auto groups = graph.episode_groups();
    atperson::require_runtime_inspection_limit(resource_status, groups.size() + 1u);
    for (std::size_t g = 0u; g < groups.size(); ++g) {
        const auto members = graph.episode_group_members(static_cast<std::uint32_t>(g));
        out << "group " << g << '\t' << "key 0x" << std::hex << groups[g].key << std::dec
            << '\t' << "tokens " << groups[g].token_count << '\t' << "members " << members.size()
            << '\t' << "evictions " << groups[g].evictions << '\t';
        for (std::size_t m = 0u; m < members.size(); ++m) {
            if (m > 0u) {
                out << ',';
            }
            out << members[m];
        }
        out << '\n';
    }
    return 0;
}

} // namespace cli
} // namespace atperson
