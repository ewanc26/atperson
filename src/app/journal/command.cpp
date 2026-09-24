// Action/outcome journal: command implementation (#27).
//
// The listing subcommands render the journal's append order with stable
// field widths. `apply` and `map` (#56) are the valence-mutating paths:
// they apply one (or a table of) explicit valence events through the C23
// API, append the journal entries, and save the model under the writer
// lock. `resolve` (#149) runs the idempotent expectation-resolution pass,
// appending only terminal resolution lines under the writer lock. The
// valence API throws on unknown tokens; the command surface returns 2 for
// bad arguments and throws for I/O.

#include "command.hpp"

#include "action/tokens.hpp"
#include "atperson/core.h"
#include "lock.hpp"
#include "resolve.hpp"
#include "rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <ios>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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
            << std::setw(10) << entry.reason;
        /* The predicted outcome (#149): kind, reply likelihood (as a
         * percentage) and expected-token count. Absent for operator-written
         * actions. */
        if (entry.expectation.has_value()) {
            out << " exp=" << entry.expectation->kind << ":"
                << std::fixed << std::setprecision(0)
                << (entry.expectation->reply_likelihood * 100.0f)
                << " tokens=" << entry.expectation->tokens.size();
        }
        out << "\n";
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

void print_resolutions(std::ostream &out, const JournalContents &journal,
                       std::size_t limit) {
    const std::size_t count = std::min(journal.resolutions.size(), limit);
    for (std::size_t i = 0u; i < count; ++i) {
        const JournalResolution &entry = journal.resolutions[i];
        out << entry.at << " " << std::setw(12) << entry.action_id << " "
            << std::setw(8) << journal_expectation_state_name(entry.state)
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

/* The `journal map` runner (#56): apply an operator-authored rule table to
 * the journal's recorded outcomes and append one valence entry per
 * (action, rule, token) triple. The caller holds the writer lock; the
 * graph, journal and model save commit under it as one unit.
 *
 * Idempotency: an existing valence entry with the same source, provenance
 * and token means the rule already fired for that action and token, so the
 * run skips it. Running `map` twice therefore derives nothing new. */
int run_map(std::ostream &out, LanguageGraph &graph,
            const std::filesystem::path &journal_path,
            const std::filesystem::path &model_path, std::string_view rule_file,
            std::int64_t now_unix, std::string_view now_rfc3339) {
    const RuleTable table = load_rule_table(std::filesystem::path(rule_file));
    const JournalContents journal = load_journal(journal_path);

    /* Dedup key: source + provenance + token. */
    std::unordered_set<std::string> applied;
    for (const JournalValence &entry : journal.valence) {
        applied.insert(entry.source + "\x1f" + entry.provenance + "\x1f" + entry.token);
    }

    std::size_t appended = 0u;
    std::size_t skipped_unmapped = 0u;
    std::size_t skipped_unknown_tokens = 0u;
    for (const JournalAction &action : journal.actions) {
        const ValenceRule *rule = first_matching_rule(table, action, journal.events, now_unix);
        if (rule == nullptr) {
            ++skipped_unmapped;
            continue;
        }
        const std::string provenance = "map:" + rule->id;
        const std::string kind_name = valence_kind_name(rule->kind);
        for (const std::string &token : action::distinct_tokens(action.text)) {
            /* The valence contract: events attach to experienced subjects
             * only. A token the entity has never observed is skipped, not
             * interned — one mapping run must not create learned state. */
            if (!graph.has_token(token)) {
                ++skipped_unknown_tokens;
                continue;
            }
            if (!applied.insert(action.id + "\x1f" + provenance + "\x1f" + token).second) {
                continue;
            }
            graph.valence_event(token, rule->kind, rule->signal,
                                static_cast<std::uint64_t>(now_unix), action.id);
            JournalValence entry;
            entry.token = token;
            entry.kind = kind_name;
            entry.signal = rule->signal;
            entry.source = action.id;
            entry.at_epoch = static_cast<std::uint64_t>(now_unix);
            entry.at = std::string(now_rfc3339);
            entry.provenance = provenance;
            append_journal_valence(journal_path, entry);
            ++appended;
        }
    }

    graph.save(model_path);
    out << "mapped " << appended << " valence event(s) from " << journal.actions.size()
        << " action(s) (" << skipped_unmapped << " unmapped, " << skipped_unknown_tokens
        << " unknown-token skips)\n";
    return 0;
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
    if (subcommand == "resolutions") {
        const std::size_t limit =
            argument_count >= 1u ? std::stoul(std::string(arguments[0])) : kDefaultListLimit;
        print_resolutions(out, journal, limit);
        return 0;
    }

    if (subcommand == "resolve") {
        /* The expectation pass (#149) appends terminal `met`/`unmet`/
         * `expired` resolution lines. It is idempotent and touches only the
         * journal, so it takes the writer lock and never reads a session,
         * the graph or the model. */
        const StateLock writer_lock(data_dir);
        const ResolutionReport report =
            resolve_expectations(journal_path, now_unix, now_rfc3339);
        out << "resolved " << report.evaluated << " expectation(s): " << report.pending
            << " pending, " << report.met_written << " met, " << report.unmet_written
            << " unmet, " << report.expired_written << " expired, " << report.state_changed
            << " state change(s), " << report.skipped_already << " already recorded\n";
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

    if (subcommand == "map") {
        if (argument_count < 1u) {
            return 2;
        }
        const StateLock writer_lock(data_dir);
        return run_map(out, graph, journal_path, model_path, arguments[0], now_unix,
                       now_rfc3339);
    }

    return 2;
}

} // namespace journal
} // namespace atperson
