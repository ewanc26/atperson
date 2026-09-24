#include "selfeval/pass.hpp"

#include "state/records.hpp"
#include "state/time.hpp"

#include "atperson/core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace atperson {
namespace {

/* First-seen epoch of each author, from the ledger's LEARNED entries. */
std::map<std::string, std::uint64_t> author_first_seen(const LanguageGraph &graph) {
    std::map<std::string, std::uint64_t> first_seen;
    for (const atp_ledger_entry &entry : graph.ledger_entries()) {
        if (entry.outcome != ATP_LEDGER_OUTCOME_LEARNED) {
            continue;
        }
        const std::string author = entry.author_did;
        if (author.empty()) {
            continue;
        }
        auto found = first_seen.find(author);
        if (found == first_seen.end()) {
            first_seen[author] = entry.observed_at;
        } else if (entry.observed_at != 0u && entry.observed_at < found->second) {
            found->second = entry.observed_at;
        }
    }
    return first_seen;
}

} // namespace

SelfEvalReport run_self_eval_pass(const std::filesystem::path &metrics_dir,
                                  const JournalContents &journal,
                                  const LanguageGraph &graph, const SelfEvalConfig &config,
                                  std::uint64_t now) {
    SelfEvalReport report;
    const std::vector<MetricSnapshot> existing = load_metric_snapshots(metrics_dir);
    report.prior_snapshots = existing.size();

    const MetricSnapshot *previous = existing.empty() ? nullptr : &existing.back();
    std::uint64_t previous_at_epoch = 0u;
    if (previous != nullptr) {
        const std::optional<std::uint64_t> parsed = parse_rfc3339_epoch(previous->at);
        if (parsed.has_value()) {
            previous_at_epoch = *parsed;
        }
    }

    const std::uint64_t cadence = config.cadence_seconds > 0
                                      ? static_cast<std::uint64_t>(config.cadence_seconds)
                                      : 1ull;
    const bool due = previous == nullptr || now < previous_at_epoch ||
                     now - previous_at_epoch >= cadence;
    if (!due) {
        report.reason = "inside cadence";
        return report;
    }
    report.reason = previous == nullptr ? "no prior snapshot" : "cadence elapsed";

    const std::uint64_t start = cadence > now ? 0u : now - cadence;
    const std::string now_string = rfc3339_from_unix(static_cast<std::int64_t>(now));
    const std::string start_string = rfc3339_from_unix(static_cast<std::int64_t>(start));

    MetricSnapshot snapshot;
    snapshot.id = new_record_id();
    snapshot.at = now_string;
    snapshot.period_start = start_string;
    snapshot.period_end = now_string;
    snapshot.cadence_seconds = cadence;
    if (previous != nullptr) {
        snapshot.has_previous = true;
        snapshot.previous_id = previous->id;
        snapshot.previous_at = previous->at;
    }

    /* --- actions: executed vs. admitted, cumulative -------------------- */

    for (const JournalAction &action : journal.actions) {
        ++snapshot.actions.attempts;
        switch (action.outcome) {
            case JournalActionOutcome::Executed:
                ++snapshot.actions.executed;
                break;
            case JournalActionOutcome::Denied:
                ++snapshot.actions.denied;
                break;
            case JournalActionOutcome::Deferred:
                ++snapshot.actions.deferred;
                break;
            case JournalActionOutcome::Failed:
                ++snapshot.actions.failed;
                break;
            case JournalActionOutcome::DryRun:
                ++snapshot.actions.dry_run;
                break;
        }
    }
    if (snapshot.actions.attempts > 0u) {
        snapshot.actions.success_rate =
            static_cast<double>(snapshot.actions.executed) /
            static_cast<double>(snapshot.actions.attempts);
    }

    /* --- interaction: terminal intent outcomes (#150), cumulative ------- */

    std::vector<std::string> intent_order;
    std::map<std::string, const JournalIntent *> latest_intents;
    for (const JournalIntent &intent : journal.intents) {
        const auto inserted = latest_intents.emplace(intent.id, &intent);
        if (!inserted.second) {
            inserted.first->second = &intent;
        } else {
            intent_order.push_back(intent.id);
        }
    }
    for (const std::string &id : intent_order) {
        const auto found = latest_intents.find(id);
        if (found == latest_intents.end()) {
            continue;
        }
        const JournalIntent &intent = *found->second;
        ++snapshot.interaction.invites;
        switch (intent.state) {
            case IntentState::Open:
                ++snapshot.interaction.invites_pending;
                break;
            case IntentState::Expired:
                ++snapshot.interaction.invites_expired;
                break;
            case IntentState::Closed:
                ++snapshot.interaction.invites_replied;
                break;
        }
    }
    const std::size_t terminal = snapshot.interaction.invites_replied +
                                 snapshot.interaction.invites_expired;
    if (terminal > 0u) {
        snapshot.interaction.success_rate =
            static_cast<double>(snapshot.interaction.invites_replied) /
            static_cast<double>(terminal);
    }

    /* --- reply/quote ratio, cumulative ---------------------------------- */

    for (const JournalEvent &event : journal.events) {
        ++snapshot.reply_ratio.events;
        if (event.via == "reply") {
            ++snapshot.reply_ratio.replies;
        }
    }
    if (snapshot.reply_ratio.events > 0u) {
        snapshot.reply_ratio.ratio =
            static_cast<double>(snapshot.reply_ratio.replies) /
            static_cast<double>(snapshot.reply_ratio.events);
    }

    /* --- valence drift, cumulative (#13) -------------------------------- */

    {
        std::set<std::string> tokens;
        for (const JournalValence &entry : journal.valence) {
            ++snapshot.valence.updates;
            tokens.insert(entry.token);
            snapshot.valence.drift += static_cast<double>(entry.signal);
        }
        snapshot.valence.tokens = tokens.size();
    }

    /* --- familiarity growth, cumulative from the ledger ----------------- */

    {
        const std::map<std::string, std::uint64_t> first_seen = author_first_seen(graph);
        snapshot.familiarity.authors = first_seen.size();
        for (const auto &[author, epoch] : first_seen) {
            (void)author;
            if (epoch != 0u && epoch >= start && epoch <= now) {
                ++snapshot.familiarity.new_authors;
            }
        }
        for (const atp_ledger_entry &entry : graph.ledger_entries()) {
            if (entry.outcome == ATP_LEDGER_OUTCOME_LEARNED && entry.author_did[0] != '\0') {
                ++snapshot.familiarity.encounters;
            }
        }
        if (snapshot.familiarity.authors > 0u) {
            snapshot.familiarity.accretion =
                static_cast<double>(snapshot.familiarity.new_authors) /
                static_cast<double>(snapshot.familiarity.authors);
        }
    }

    /* --- bounded period trace ------------------------------------------- */

    std::vector<std::string> window_new_authors;
    {
        const std::map<std::string, std::uint64_t> first_seen = author_first_seen(graph);
        for (const auto &[author, epoch] : first_seen) {
            if (epoch != 0u && epoch >= start && epoch <= now) {
                window_new_authors.push_back(author);
            }
        }
        std::sort(window_new_authors.begin(), window_new_authors.end());
        if (window_new_authors.size() > config.max_trace_authors) {
            window_new_authors.resize(config.max_trace_authors);
        }
        snapshot.trace.new_authors = window_new_authors;
    }

    for (const atp_episode &episode : graph.episodes()) {
        if (episode.observed_at >= start && episode.observed_at <= now) {
            ++snapshot.trace.episodes;
        }
    }
    for (const JournalEvent &event : journal.events) {
        const std::optional<std::uint64_t> epoch = parse_rfc3339_epoch(event.at);
        if (epoch.has_value() && *epoch >= start && *epoch <= now) {
            ++snapshot.trace.events;
        }
    }
    for (const JournalResolution &resolution : journal.resolutions) {
        if (resolution.at_epoch != 0u && resolution.at_epoch >= start &&
            resolution.at_epoch <= now) {
            ++snapshot.trace.resolutions;
        }
    }

    struct WindowGroup {
        std::string token;
        std::string kind;
        double sum{0.0};
        std::size_t count{0u};
    };
    std::vector<WindowGroup> window_groups;
    {
        std::map<std::string, std::map<std::string, WindowGroup>> by_token;
        for (const JournalValence &entry : journal.valence) {
            if (entry.at_epoch != 0u && entry.at_epoch >= start && entry.at_epoch <= now) {
                ++snapshot.trace.valence_updates;
            }
            if (entry.at_epoch == 0u || entry.at_epoch < start || entry.at_epoch > now) {
                continue;
            }
            WindowGroup &group = by_token[entry.token][entry.kind];
            group.token = entry.token;
            group.kind = entry.kind;
            group.sum += static_cast<double>(entry.signal);
            ++group.count;
        }
        for (auto &[token, kinds] : by_token) {
            (void)token;
            for (auto &[kind, group] : kinds) {
                (void)kind;
                window_groups.push_back(std::move(group));
            }
        }
    }
    std::sort(window_groups.begin(), window_groups.end(),
              [](const WindowGroup &a, const WindowGroup &b) {
                  const double ma = std::fabs(a.sum);
                  const double mb = std::fabs(b.sum);
                  if (ma != mb) {
                      return ma > mb;
                  }
                  if (a.token != b.token) {
                      return a.token < b.token;
                  }
                  return a.kind < b.kind;
              });
    for (const WindowGroup &group : window_groups) {
        if (snapshot.trace.top_valence.size() >= config.max_trace_groups) {
            break;
        }
        MetricGroup trace_group;
        trace_group.token = group.token;
        trace_group.kind = group.kind;
        trace_group.signal_sum = group.sum;
        trace_group.count = group.count;
        snapshot.trace.top_valence.push_back(std::move(trace_group));
    }

    write_metric_snapshot(metrics_dir, snapshot);
    report.due = true;
    report.snapshot = std::move(snapshot);
    return report;
}

} // namespace atperson