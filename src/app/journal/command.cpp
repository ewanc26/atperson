// Action/outcome journal: command implementation (#27).
//
// The listing subcommands render the journal's append order with stable
// field widths. `apply` is the only mutating path: it loads the journal
// (creates empty if missing), applies one valence event through the C23
// API, appends a JournalValence entry, and saves the model under the
// writer lock. The valence API throws on unknown tokens; the command
// surface returns 2 for bad arguments and throws for I/O.

#include "command.hpp"

#include "atperson/core.h"
#include "lock.hpp"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <ios>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {
namespace journal {
namespace {

constexpr std::size_t kDefaultListLimit = 20u;

void print_actions(std::ostream &out, const JournalContents &journal,
                   std::size_t limit) {
    const std::size_t count = std::min(journal.actions.size(), limit);
    for (std::size_t i = 0u; i < count; ++i) {
        const JournalAction &entry = journal.actions[i];
        out << entry.at << " " << std::setw(8) << entry.kind << " "
            << std::setw(12) << entry.id << " " << std::setw(8)
            << journal_action_outcome_name(entry.outcome) << " "
            << std::setw(10) << entry.reason << "\n";
    }
}

void print_events(std::ostream &out, const JournalContents &journal,
                  std::size_t limit) {
    const std::size_t count = std::min(journal.events.size(), limit);
    for (std::size_t i = 0u; i < count; ++i) {
        const JournalEvent &entry = journal.events[i];
        out << entry.at << " " << std::setw(12) << entry.action_id << " "
            << std::setw(8) << entry.via << " " << entry.author_did << " "
            << entry.event_uri << "\n";
    }
}

void print_valence(std::ostream &out, const JournalContents &journal,
                   std::size_t limit) {
    const std::size_t count = std::min(journal.valence.size(), limit);
    for (std::size_t i = 0u; i < count; ++i) {
        const JournalValence &entry = journal.valence[i];
        out << entry.at << " " << std::setw(16) << entry.token << " "
            << std::setw(12) << entry.kind << " " << std::fixed
            << std::setprecision(3) << entry.signal << " " << entry.source
            << "\n";
    }
}

[[nodiscard]] std::optional<float> parse_signal(std::string_view text) {
    try {
        const std::string owned(text);
        const double value = std::stod(owned);
        if (!std::isfinite(value) || value < -1.0 || value > 1.0) {
            return std::nullopt;
        }
        return static_cast<float>(value);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

int run_journal_command(std::ostream &out, LanguageGraph &graph,
                        const std::filesystem::path &journal_path,
                        const std::filesystem::path &data_dir,
                        const std::filesystem::path &model_path,
                        std::string_view subcommand,
                        const std::string_view *arguments, std::size_t argument_count,
                        std::int64_t now_unix, std::string_view now_rfc3339) {
    const JournalContents journal = load_journal(journal_path);

    if (subcommand == "actions") {
        const std::size_t limit =
            argument_count >= 1u ? std::stoul(std::string(arguments[0])) : kDefaultListLimit;
        print_actions(out, journal, limit);
        return 0;
    }
    if (subcommand == "events") {
        const std::size_t limit =
            argument_count >= 1u ? std::stoul(std::string(arguments[0])) : kDefaultListLimit;
        print_events(out, journal, limit);
        return 0;
    }
    if (subcommand == "valence") {
        const std::size_t limit =
            argument_count >= 1u ? std::stoul(std::string(arguments[0])) : kDefaultListLimit;
        print_valence(out, journal, limit);
        return 0;
    }

    if (subcommand == "apply") {
        if (argument_count < 4u) {
            return 2;
        }
        const std::string token(arguments[0]);
        const std::optional<atp_valence_kind> kind = valence_kind_from_name(arguments[1]);
        if (!kind.has_value()) {
            return 2;
        }
        const std::optional<float> signal = parse_signal(arguments[2]);
        if (!signal.has_value()) {
            return 2;
        }
        const std::string source(arguments[3]);

        const StateLock writer_lock(data_dir);
        graph.valence_event(token, kind.value(), signal.value(),
                            static_cast<std::uint64_t>(now_unix), source);

        JournalValence entry;
        entry.token = token;
        entry.kind = std::string(arguments[1]);
        entry.signal = signal.value();
        entry.source = source;
        entry.at_epoch = static_cast<std::uint64_t>(now_unix);
        entry.at = std::string(now_rfc3339);
        append_journal_valence(journal_path, entry);

        graph.save(model_path);
        out << "applied " << token << " " << arguments[1] << " " << arguments[2] << " from "
            << source << "\n";
        return 0;
    }

    return 2;
}

} // namespace journal
} // namespace atperson
