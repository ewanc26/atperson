#include "cli/profile.hpp"

#include <iomanip>
#include <utility>
#include <vector>

#include "atperson/core.h"
#include "atperson/graph.hpp"

namespace atperson::cli {
namespace {

/* How many top-familiarity tokens the card lists. Bounded so the card stays
 * readable at any vocabulary size. */
constexpr std::size_t kProfileTopTokens = 10;

} // namespace

int run_profile(std::ostream &out, const LanguageGraph &graph) {
    const atp_graph_stats stats = graph.stats();
    const std::vector<std::pair<std::string, float>> top = graph.familiar_tokens(
        kProfileTopTokens);

    /* Stance summary from valence: aggregate every valence record into a
     * mean and a positive/negative balance. Fresh state has no records and
     * renders as no stance, never a seeded one. */
    const std::vector<atp_valence_state> valence = graph.valence_records();
    float valence_sum = 0.0f;
    std::uint64_t positive_events = 0u;
    std::uint64_t negative_events = 0u;
    for (const atp_valence_state &state : valence) {
        valence_sum += state.valence;
        positive_events += state.positive_events;
        negative_events += state.negative_events;
    }

    out << "profile: runtime-derived, no seeded text\n";
    out << "vocabulary: " << stats.node_count << " tokens, " << stats.edge_count
        << " associations, " << stats.observations << " observations\n";

    if (top.empty()) {
        out << "familiar: (nothing learned yet)\n";
    } else {
        out << "familiar:\n";
        const std::streamsize precision = out.precision();
        out << std::fixed << std::setprecision(4);
        for (const auto &[token, familiarity] : top) {
            out << "  " << token << " " << familiarity << '\n';
        }
        out.precision(precision);
    }

    if (valence.empty()) {
        out << "stance: (no valence events yet)\n";
    } else {
        const float mean = valence_sum / static_cast<float>(valence.size());
        out << "stance: mean valence " << std::fixed << std::setprecision(4) << mean
            << " over " << valence.size() << " subjects, " << positive_events
            << " positive / " << negative_events << " negative events\n";
    }
    return 0;
}

} // namespace atperson::cli
