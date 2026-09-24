#include "reflect/pass.hpp"

#include "state/time.hpp"

#include "atperson/core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace atperson {
namespace {

/* Author names shown inline before the pass collapses the rest. */
inline constexpr std::size_t kAuthorNamesShown = 5u;
/* Encounters at or below this floor make an author "unfamiliar": exposure
 * below two observations gives no track record to speak from. */
inline constexpr std::size_t kUnfamiliarEncountersFloor = 1u;

std::string join_authors(const std::vector<std::string> &authors) {
    std::ostringstream out;
    const std::size_t shown = std::min(authors.size(), kAuthorNamesShown);
    for (std::size_t i = 0u; i < shown; ++i) {
        if (i > 0u) {
            out << ", ";
        }
        out << authors[i];
    }
    if (authors.size() > shown) {
        out << " and " << (authors.size() - shown) << " more";
    }
    return out.str();
}

std::size_t ratio_percent(float ratio) {
    return static_cast<std::size_t>(std::lroundf(ratio * 100.0f));
}

bool already_has(const std::vector<Thought> &stores, const std::string &kind,
                 const std::string &topic, const std::string &span_end) {
    return std::any_of(stores.begin(), stores.end(), [&](const Thought &entry) {
        return entry.kind == kind && entry.topic == topic && entry.span_end == span_end;
    });
}

} // namespace

ReflectionReport run_reflection_pass(const std::filesystem::path &thoughts_path,
                                     const JournalContents &journal, const LanguageGraph &graph,
                                     const ReflectionConfig &config, std::uint64_t now) {
    ReflectionReport report;
    const ThoughtContents contents = load_thoughts(thoughts_path);

    const std::uint64_t window = static_cast<std::uint64_t>(config.window_seconds);
    const std::uint64_t start = window > now ? 0u : now - window;
    const std::uint64_t prev_start = (2u * window) > now ? 0u : now - 2u * window;

    const std::string now_string = rfc3339_from_unix(static_cast<std::int64_t>(now));
    const std::string start_string = rfc3339_from_unix(static_cast<std::int64_t>(start));

    /* --- read-only window metrics ------------------------------------ */

    struct ValenceGroup {
        std::string token;
        std::string kind;
        float sum{0.0f};
        std::size_t count{0u};
    };
    std::vector<ValenceGroup> groups;
    std::vector<std::string> valence_tokens;
    {
        std::map<std::string, std::map<std::string, ValenceGroup>> by_token;
        std::vector<std::string> seen_order;
        for (const JournalValence &entry : journal.valence) {
            if (entry.at_epoch == 0u || entry.at_epoch < start || entry.at_epoch > now) {
                continue;
            }
            ++report.valence_updates_in_window;
            ValenceGroup &group = by_token[entry.token][entry.kind];
            group.token = entry.token;
            group.kind = entry.kind;
            group.sum += entry.signal;
            ++group.count;
            if (group.count == 1u) {
                seen_order.push_back(group.token);
            }
        }
        for (const std::string &token : seen_order) {
            valence_tokens.push_back(token);
        }
        for (auto &[token, kinds] : by_token) {
            (void)token;
            for (auto &[kind, group] : kinds) {
                (void)kind;
                groups.push_back(std::move(group));
            }
        }
    }
    report.valence_tokens_in_window = valence_tokens.size();

    /* Author exposure from the ledger: LEARNED entries only, matching the
     * familiarity semantics the entity exposes — encounters, not trust. */
    std::map<std::string, std::size_t> encounters;
    for (const atp_ledger_entry &entry : graph.ledger_entries()) {
        if (entry.outcome != ATP_LEDGER_OUTCOME_LEARNED) {
            continue;
        }
        const std::string author = entry.author_did;
        if (!author.empty()) {
            ++encounters[author];
        }
    }

    std::vector<std::string> unfamiliar;
    {
        std::vector<std::string> window_authors;
        for (const atp_episode &episode : graph.episodes()) {
            if (episode.observed_at < start || episode.observed_at > now) {
                continue;
            }
            ++report.episodes_in_window;
            const std::string author = episode.author_did;
            if (author.empty()) {
                continue;
            }
            if (std::find(window_authors.begin(), window_authors.end(), author) ==
                window_authors.end()) {
                window_authors.push_back(author);
            }
            const auto found = encounters.find(author);
            const std::size_t exposure = found == encounters.end() ? 0u : found->second;
            if (exposure <= kUnfamiliarEncountersFloor &&
                std::find(unfamiliar.begin(), unfamiliar.end(), author) == unfamiliar.end()) {
                unfamiliar.push_back(author);
            }
        }
        report.authors_in_window = window_authors.size();
    }
    std::sort(unfamiliar.begin(), unfamiliar.end());

    std::size_t trailing_total = 0u;
    std::size_t trailing_replies = 0u;
    std::size_t previous_total = 0u;
    std::size_t previous_replies = 0u;
    for (const JournalEvent &event : journal.events) {
        const std::optional<std::uint64_t> epoch = parse_rfc3339_epoch(event.at);
        if (!epoch.has_value()) {
            continue;
        }
        if (*epoch >= start && *epoch <= now) {
            ++trailing_total;
            if (event.via == "reply") {
                ++trailing_replies;
            }
        } else if (*epoch >= prev_start && *epoch < start) {
            ++previous_total;
            if (event.via == "reply") {
                ++previous_replies;
            }
        }
    }
    report.events_in_window = trailing_total;

    for (const JournalResolution &resolution : journal.resolutions) {
        if (resolution.at_epoch != 0u && resolution.at_epoch >= start &&
            resolution.at_epoch <= now) {
            ++report.resolutions_in_window;
        }
    }

    /* --- movement triggers (evaluated every pass) -------------------- */

    /* Determinism bound: a window produces its movement burst at most once.
     * A rerun over the same inputs and clock writes nothing new, so the
     * write set of two passes over identical state is identical. */
    const bool movements_in_window = std::any_of(
        contents.thoughts.begin(), contents.thoughts.end(), [&](const Thought &entry) {
            return entry.kind == "movement" && entry.span_end == now_string;
        });

    std::vector<ReflectionResult> candidates;

    /* Reply ratios are also shown in the consolidation text, so they are
     * computed once, outside the movement gate. */
    const float trailing_ratio =
        trailing_total > 0u ? static_cast<float>(trailing_replies) / static_cast<float>(trailing_total)
                            : 0.0f;
    const float previous_ratio =
        previous_total > 0u ? static_cast<float>(previous_replies) / static_cast<float>(previous_total)
                            : 0.0f;

    if (!movements_in_window) {
        std::vector<ValenceGroup> triggered;
        for (const ValenceGroup &group : groups) {
            if (std::fabs(group.sum) >= config.valence_delta_min) {
                triggered.push_back(group);
            }
        }
        std::sort(triggered.begin(), triggered.end(), [](const ValenceGroup &a,
                                                         const ValenceGroup &b) {
            const float ma = std::fabs(a.sum);
            const float mb = std::fabs(b.sum);
            if (ma != mb) {
                return ma > mb;
            }
            if (a.token != b.token) {
                return a.token < b.token;
            }
            return a.kind < b.kind;
        });
        for (const ValenceGroup &group : triggered) {
            const std::string topic = "valence:" + group.kind + ":" + group.token;
            if (already_has(contents.thoughts, "movement", topic, now_string)) {
                continue;
            }
            std::ostringstream text;
            text << "valence movement: " << group.token << " (" << group.kind << ") moved "
                 << (group.sum >= 0.0f ? '+' : '-') << std::fixed << std::setprecision(2)
                 << std::fabs(group.sum) << " across " << group.count << " update(s) in the "
                 << config.window_seconds << "s window ending " << now_string << '.';
            ReflectionResult result;
            result.from = topic;
            result.thought.kind = "movement";
            result.thought.topic = topic;
            result.thought.text = text.str();
            result.thought.span_start = start_string;
            result.thought.span_end = now_string;
            candidates.push_back(std::move(result));
        }

        if ((trailing_total > 0u && previous_total > 0u) &&
            std::fabs(trailing_ratio - previous_ratio) >= config.reply_ratio_shift_min) {
            const std::string topic = "reply";
            if (!already_has(contents.thoughts, "movement", topic, now_string)) {
                std::ostringstream text;
                text << "reply ratio shifted: " << ratio_percent(trailing_ratio) << "% to "
                     << ratio_percent(previous_ratio) << "% across " << trailing_total
                     << " linked event(s) in the " << config.window_seconds << "s window ending "
                     << now_string << " (previous window " << previous_total << " event(s)).";
                ReflectionResult result;
                result.from = topic;
                result.thought.kind = "movement";
                result.thought.topic = topic;
                result.thought.text = text.str();
                result.thought.span_start = start_string;
                result.thought.span_end = now_string;
                candidates.push_back(std::move(result));
            }
        }

        if (unfamiliar.size() >= config.unfamiliar_authors_min) {
            const std::string topic = "authors";
            if (!already_has(contents.thoughts, "movement", topic, now_string)) {
                std::ostringstream text;
                text << "unfamiliar authors: " << unfamiliar.size()
                     << " author(s) with exposure below " << (kUnfamiliarEncountersFloor + 1u)
                     << " encounter(s) appeared in the " << config.window_seconds
                     << "s window ending " << now_string << ": " << join_authors(unfamiliar)
                     << '.';
                ReflectionResult result;
                result.from = topic;
                result.thought.kind = "movement";
                result.thought.topic = topic;
                result.thought.text = text.str();
                result.thought.span_start = start_string;
                result.thought.span_end = now_string;
                candidates.push_back(std::move(result));
            }
        }
    }

    /* --- scheduled consolidation (cadence-gated) ---------------------- */

    std::uint64_t last_consolidation = 0u;
    for (const Thought &entry : contents.thoughts) {
        if (entry.kind != "consolidation") {
            continue;
        }
        const std::optional<std::uint64_t> epoch = parse_rfc3339_epoch(entry.at);
        if (epoch.has_value() && *epoch > last_consolidation) {
            last_consolidation = *epoch;
        }
    }
    const std::uint64_t cadence = static_cast<std::uint64_t>(config.cadence_seconds);
    const bool consolidation_due =
        last_consolidation == 0u || now < last_consolidation ||
        now - last_consolidation >= cadence;

    /* --- bounded write phase ----------------------------------------- */

    std::size_t budget = config.max_thoughts;
    for (auto &candidate : candidates) {
        if (budget == 0u) {
            break;
        }
        candidate.thought.id = new_thought_id();
        candidate.thought.at = now_string;
        write_thought(thoughts_path, candidate.thought);
        ++report.thoughts_written;
        report.written.push_back(std::move(candidate));
        --budget;
    }

    if (consolidation_due && budget > 0u) {
        std::ostringstream text;
        text << "periodic reflection (" << config.window_seconds << "s window) ending "
             << now_string << ": " << report.valence_updates_in_window
             << " valence update(s) across " << report.valence_tokens_in_window
             << " token(s), " << report.episodes_in_window << " episode(s) from "
             << report.authors_in_window << " author(s), " << report.events_in_window
             << " linked event(s), " << report.resolutions_in_window << " resolution(s).";
        if (trailing_total > 0u) {
            text << " reply ratio " << ratio_percent(trailing_ratio) << '%';
            if (previous_total > 0u) {
                text << " (was " << ratio_percent(previous_ratio) << "%).";
            } else {
                text << '.';
            }
        }
        ReflectionResult result;
        result.from = "consolidation";
        result.thought.kind = "consolidation";
        result.thought.topic = "consolidation";
        result.thought.span_start = start_string;
        result.thought.span_end = now_string;
        result.thought.text = text.str();
        result.thought.id = new_thought_id();
        result.thought.at = now_string;
        write_thought(thoughts_path, result.thought);
        ++report.thoughts_written;
        report.written.push_back(std::move(result));
    }

    return report;
}

} // namespace atperson