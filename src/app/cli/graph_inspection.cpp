// CLI read-only graph inspection commands: assoc, candidates, familiarity,
// recall.
//
// Implementation of the contracts in graph_inspection.hpp. Every body here is
// moved byte-faithfully from the original single-file dispatch in
// src/app/main.cpp; behaviour, ordering and output text are unchanged.

#include "graph_inspection.hpp"

#include "config.hpp"

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

int run_recall(std::ostream &out, const RuntimeResourceStatus &resource_status,
               LanguageGraph &graph, std::string_view query, const char *limit_value,
               std::uint64_t at_epoch) {
    const int limit = limit_value ? parse_limit(limit_value, 10) : 10;
    atperson::require_runtime_inspection_limit(resource_status,
                                               static_cast<std::size_t>(limit));
    for (const auto &episode :
         graph.recall(query, at_epoch, static_cast<std::size_t>(limit))) {
        out << "ledger " << episode.ledger_id << '\t' << "at " << episode.observed_at << '\t'
            << "recall " << episode.recall_count << '\t' << episode.source_id << '\t';
        for (std::uint32_t t = 0u; t < episode.token_count; ++t) {
            out << '<' << episode.summary[t].node_index << ':' << std::fixed
                << std::setprecision(1) << episode.summary[t].weight << "> ";
        }
        out << '\n';
    }
    return 0;
}

} // namespace cli
} // namespace atperson
