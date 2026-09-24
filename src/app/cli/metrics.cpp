#include "cli/metrics.hpp"

#include "control/state.hpp"
#include "journal/store.hpp"
#include "selfeval/config.hpp"
#include "selfeval/pass.hpp"
#include "selfeval/store.hpp"
#include "state/lock.hpp"
#include "state/time.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>

namespace atperson {
namespace cli {
namespace {

inline constexpr std::size_t kMetricsListDefaultLimit = 50u;
inline constexpr std::size_t kMetricsListMaximumLimit = 1000u;

/* Deltas of one snapshot against its immediate predecessor; empty when no
 * predecessor is available. Computed on the display path alone. */
struct MetricDeltas {
    bool has{false};
    std::int64_t actions_exec{0};
    double actions_success{0.0};
    double interaction_success{0.0};
    double reply_ratio{0.0};
    double valence_drift{0.0};
    std::int64_t familiarity_authors{0};
};

MetricDeltas snapshot_deltas(const MetricSnapshot &current, const MetricSnapshot *previous) {
    MetricDeltas deltas;
    if (previous == nullptr) {
        return deltas;
    }
    deltas.has = true;
    deltas.actions_exec = static_cast<std::int64_t>(current.actions.executed) -
                          static_cast<std::int64_t>(previous->actions.executed);
    deltas.actions_success = current.actions.success_rate - previous->actions.success_rate;
    deltas.interaction_success =
        current.interaction.success_rate - previous->interaction.success_rate;
    deltas.reply_ratio = current.reply_ratio.ratio - previous->reply_ratio.ratio;
    deltas.valence_drift = current.valence.drift - previous->valence.drift;
    deltas.familiarity_authors = static_cast<std::int64_t>(current.familiarity.authors) -
                                 static_cast<std::int64_t>(previous->familiarity.authors);
    return deltas;
}

void print_signed_delta(std::ostream &out, double value) {
    if (value >= 0.0) {
        out << '+';
    }
    out << std::fixed << std::setprecision(2) << value;
}

void print_pct(std::ostream &out, double ratio) {
    out << std::fixed << std::setprecision(1) << (ratio * 100.0) << '%';
}

void print_snapshot(std::ostream &out, const MetricSnapshot &snapshot,
                    const MetricSnapshot *previous) {
    const MetricDeltas deltas = snapshot_deltas(snapshot, previous);
    out << snapshot.id << '\n';
    out << "  at=" << snapshot.at << " span=" << snapshot.period_start << ".."
        << snapshot.period_end << " cadence=" << snapshot.cadence_seconds << 's';
    if (snapshot.has_previous) {
        out << " previous=" << snapshot.previous_id << " (" << snapshot.previous_at << ')';
        if (previous == nullptr) {
            out << " [predecessor missing from store]";
        }
    } else {
        out << " first-snapshot";
    }
    out << '\n';
    out << "  actions      admitted=" << snapshot.actions.attempts
        << " executed=" << snapshot.actions.executed << " denied=" << snapshot.actions.denied
        << " deferred=" << snapshot.actions.deferred << " failed=" << snapshot.actions.failed
        << " dry-run=" << snapshot.actions.dry_run << " success=";
    print_pct(out, snapshot.actions.success_rate);
    if (deltas.has) {
        out << " (delta exec " << std::showpos << deltas.actions_exec << std::noshowpos
            << ", delta success ";
        print_signed_delta(out, deltas.actions_success);
        out << ')';
    }
    out << '\n';
    out << "  interaction  invites=" << snapshot.interaction.invites
        << " replied=" << snapshot.interaction.invites_replied
        << " expired=" << snapshot.interaction.invites_expired
        << " pending=" << snapshot.interaction.invites_pending << " success=";
    print_pct(out, snapshot.interaction.success_rate);
    if (deltas.has) {
        out << " (delta success ";
        print_signed_delta(out, deltas.interaction_success);
        out << ')';
    }
    out << '\n';
    out << "  reply_ratio  ";
    print_pct(out, snapshot.reply_ratio.ratio);
    out << " replies over " << snapshot.reply_ratio.events << " linked event(s)";
    if (deltas.has) {
        out << " (delta ";
        print_signed_delta(out, deltas.reply_ratio);
        out << ')';
    }
    out << '\n';
    out << "  valence      drift ";
    print_signed_delta(out, snapshot.valence.drift);
    out << " over " << snapshot.valence.updates << " update(s) on "
        << snapshot.valence.tokens << " token(s)";
    if (deltas.has) {
        out << " (delta drift ";
        print_signed_delta(out, deltas.valence_drift);
        out << ')';
    }
    out << '\n';
    out << "  familiarity  authors=" << snapshot.familiarity.authors
        << " (+" << snapshot.familiarity.new_authors << " this period, accretion ";
    print_pct(out, snapshot.familiarity.accretion);
    out << ") encounters=" << snapshot.familiarity.encounters;
    if (deltas.has) {
        out << " (delta authors " << std::showpos << deltas.familiarity_authors
            << std::noshowpos << ')';
    }
    out << '\n';
    out << "  trace        window " << snapshot.trace.episodes << " episode(s) "
        << snapshot.trace.events << " event(s) " << snapshot.trace.resolutions
        << " resolution(s) " << snapshot.trace.valence_updates << " valence update(s)";
    if (!snapshot.trace.new_authors.empty()) {
        out << " new authors: " << snapshot.trace.new_authors.front();
        for (std::size_t i = 1u; i < snapshot.trace.new_authors.size(); ++i) {
            out << ", " << snapshot.trace.new_authors[i];
        }
        if (snapshot.trace.new_authors.size() < snapshot.familiarity.new_authors) {
            out << " (+" << (snapshot.familiarity.new_authors - snapshot.trace.new_authors.size())
                << " more)";
        }
    }
    out << '\n';
    for (const MetricGroup &group : snapshot.trace.top_valence) {
        out << "  trace        valence <" << group.token << "> (" << group.kind << ") sum ";
        print_signed_delta(out, group.signal_sum);
        out << " across " << group.count << " update(s)\n";
    }
}

} // namespace

std::filesystem::path metrics_path(const std::filesystem::path &data_dir) {
    return data_dir / "metrics";
}

int run_metrics_list(std::ostream &out, const std::filesystem::path &data_dir,
                     const std::string_view *arguments, std::size_t argument_count) {
    std::size_t limit = kMetricsListDefaultLimit;
    std::string since;

    for (std::size_t i = 0u; i < argument_count; ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "--since") {
            if (++i >= argument_count) {
                std::cerr << "metrics: --since requires an RFC 3339 timestamp\n";
                return 2;
            }
            since = std::string(arguments[i]);
            if (!parse_rfc3339_epoch(since).has_value()) {
                std::cerr << "metrics: --since must be an RFC 3339 timestamp\n";
                return 2;
            }
            continue;
        }
        if (argument.starts_with("--")) {
            std::cerr << "metrics: unexpected argument " << argument << '\n';
            return 2;
        }
        const std::string owned(argument);
        const char *end = nullptr;
        const unsigned long long parsed = std::strtoull(owned.c_str(), const_cast<char **>(&end),
                                                        10);
        if (end == owned.c_str() || *end != '\0') {
            std::cerr << "metrics: expected a limit, got " << argument << '\n';
            return 2;
        }
        limit = std::min<std::size_t>(static_cast<std::size_t>(parsed),
                                      kMetricsListMaximumLimit);
    }

    std::vector<MetricSnapshot> snapshots = load_metric_snapshots(metrics_path(data_dir));
    std::sort(snapshots.begin(), snapshots.end(),
              [](const MetricSnapshot &a, const MetricSnapshot &b) { return a.id > b.id; });
    if (!since.empty()) {
        const std::optional<std::uint64_t> since_epoch = parse_rfc3339_epoch(since);
        std::vector<MetricSnapshot> filtered;
        filtered.reserve(snapshots.size());
        for (const MetricSnapshot &entry : snapshots) {
            const std::optional<std::uint64_t> at = parse_rfc3339_epoch(entry.at);
            if (at.has_value() && *at >= *since_epoch) {
                filtered.push_back(entry);
            }
        }
        snapshots = std::move(filtered);
    }
    if (snapshots.size() > limit) {
        snapshots.resize(limit);
    }

    out << "metrics: format=atperson-metrics version=" << kMetricFormatVersion
        << " snapshots=" << snapshots.size() << '\n';
    std::vector<MetricSnapshot> all = load_metric_snapshots(metrics_path(data_dir));
    std::map<std::string, const MetricSnapshot *> by_id;
    for (const MetricSnapshot &candidate : all) {
        by_id[candidate.id] = &candidate;
    }
    for (const MetricSnapshot &entry : snapshots) {
        const MetricSnapshot *previous = nullptr;
        if (entry.has_previous) {
            const auto found = by_id.find(entry.previous_id);
            if (found != by_id.end()) {
                previous = found->second;
            }
        }
        print_snapshot(out, entry, previous);
    }
    return 0;
}

int run_self_eval_command(std::ostream &out, const std::filesystem::path &data_dir,
                          const std::filesystem::path &journal_path,
                          const LanguageGraph &graph, std::int64_t now_unix,
                          std::string_view now_rfc3339) {
    const SelfEvalConfig config = self_eval_config_from_environment();
    const StateLock writers_lock(data_dir, "metrics-lock");
    const JournalContents journal = load_journal(journal_path);

    const SelfEvalReport report =
        run_self_eval_pass(metrics_path(data_dir), journal, graph, config,
                           static_cast<std::uint64_t>(now_unix));

    if (!report.due) {
        out << "self-eval: no snapshot written (" << report.reason << ", cadence "
            << config.cadence_seconds << "s ending " << now_rfc3339 << ", "
            << report.prior_snapshots << " prior snapshot(s))\n";
        return 0;
    }

    out << "self-eval: snapshot due (" << report.reason << "), cadence "
        << config.cadence_seconds << 's' << '\n';
    print_snapshot(out, *report.snapshot, nullptr);
    return 0;
}

} // namespace cli
} // namespace atperson