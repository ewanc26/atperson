#include "cli/drives.hpp"

#include "journal/store.hpp"
#include "scheduler/drives.hpp"

#include <cstdlib>
#include <iomanip>
#include <ostream>
#include <string_view>

namespace atperson {
namespace cli {

int run_drives_command(std::ostream &out, const LanguageGraph &graph, const Ledger &ledger,
                       const std::filesystem::path &journal_path, std::int64_t now,
                       std::size_t max_contexts) {
    const std::vector<drives::ContextCandidate> candidates =
        drives::select_candidates(ledger, max_contexts);
    const JournalContents journal = load_journal(journal_path);
    const std::vector<drives::Signals> signals =
        drives::compute_drive_signals(graph, candidates, journal, now);
    const std::vector<std::size_t> order = drives::order_candidates(signals);

    const char *gate = std::getenv("ATPERSON_SCHEDULER_DRIVES");
    const bool scheduler_uses_drives = gate != nullptr && std::string_view(gate) == "1";
    out << "drives: " << candidates.size() << " candidate context(s); scheduler ordering "
        << (scheduler_uses_drives
                ? "enabled (ATPERSON_SCHEDULER_DRIVES=1)"
                : "off (set ATPERSON_SCHEDULER_DRIVES=1 to enable)")
        << "; reciprocity window " << (drives::kReciprocityWindowSeconds / 86400) << "d\n";
    for (const std::size_t index : order) {
        const drives::ContextCandidate &candidate = candidates[index];
        const drives::Signals &signal = signals[index];
        out << "  #" << candidate.ledger_id << " curiosity=" << std::fixed << std::setprecision(3)
            << signal.curiosity << " reciprocity=" << signal.reciprocity
            << " via=" << signal.reciprocity_source << " author=" << candidate.author_did
            << " source=" << candidate.source_id << '\n';
    }
    return 0;
}

} // namespace cli
} // namespace atperson