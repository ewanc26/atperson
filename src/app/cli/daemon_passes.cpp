#include "daemon_passes.hpp"

#include "atproto/session.hpp"
#include "atproto/writer.hpp"
#include "config.hpp"
#include "control/remote_poll.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace atperson::cli {

/* One autonomous scheduler cycle (#140) after a successful perception
 * cycle. Off unless ATPERSON_SCHEDULER=1. The writer is established lazily
 * inside the attempt atom, so a cycle with no approved proposal never
 * reads credentials or touches the network. */
SchedulerCycleReport run_scheduler_after_cycle(const std::filesystem::path &data_dir,
                                               const SchedulerConfig &config,
                                               const LanguageGraph &graph, const Ledger &ledger,
                                               std::int64_t now) {
    if (!config.enabled) {
        return {};
    }
    std::unique_ptr<WolframSession> session;
    std::unique_ptr<WolframWriter> writer;
    const auto writer_for = [&]() -> OutboundWriter & {
        if (!session) {
            const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
            session = std::make_unique<WolframSession>(service, required_env("ATPERSON_IDENTIFIER"),
                                                       required_env("ATPERSON_APP_PASSWORD"));
            writer = std::make_unique<WolframWriter>(*session);
        }
        return *writer;
    };
    const auto steady_ms = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    const SchedulerCycle cycle{
        data_dir,
        scheduler_proposals_path(),
        OutboundAttemptPaths{outbound_policy_path(), outbound_budget_path(),
                             control_state_path(), outbound_audit_path(), action_journal_path(),
                             authorization_envelopes_path(), offline_spool_path()},
        writer_for,
        now,
        steady_ms};
    const SchedulerCycleReport report =
        run_scheduler_cycle(config, cycle, graph, ledger);
    return report;
}

/* One remote operator poll (#143) per cycle, before the scheduler so a
 * pause the operator just issued is honoured by this cycle rather than the
 * next one. Off unless ATPERSON_OPERATOR_DID names a trusted DID — with no
 * DID the channel is inert, and this never opens a session, so a
 * deployment without the channel configured pays nothing. */
void run_remote_control_poll(std::ostream &out, const RuntimeResourceStatus &resource_status,
                             const std::string &account_did,
                             const std::filesystem::path &control_file,
                             const std::filesystem::path &cursor_file) {
    const char *raw_did = std::getenv("ATPERSON_OPERATOR_DID");
    if (raw_did == nullptr || raw_did[0] == '\0') {
        return;
    }
    /* Separation, before any session is opened: the operator must not be
     * this entity's own account, or the entity could command itself. */
    if (check_operator_channel(account_did, raw_did) == OperatorChannelStatus::Conflict) {
        out << "remote control: refused — "
            << operator_channel_denial(account_did, raw_did) << '\n';
        return;
    }
    try {
        require_runtime_write_headroom(resource_status);
    } catch (const std::exception &) {
        /* No headroom to persist a change: skip the poll rather than queue
         * commands. The operator's records stay on their PDS. */
        out << "remote control: skipped, insufficient write headroom\n";
        return;
    }

    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    std::unique_ptr<WolframSession> session;
    try {
        session = std::make_unique<WolframSession>(service, required_env("ATPERSON_IDENTIFIER"),
                                                    required_env("ATPERSON_APP_PASSWORD"));
    } catch (const std::exception &error) {
        /* A channel that cannot authenticate is a channel that cannot
         * command. The daemon keeps running on local control. */
        out << "remote control: unavailable (" << error.what() << ")\n";
        return;
    }

    RemotePollConfig config;
    config.operator_did = raw_did;
    /* The session's own DID, not the caller's: the poller re-checks
     * separation against what actually authenticated. */
    config.account_did = session->did();
    RemoteControlChannel channel(*session, config, control_file, cursor_file);
    try {
        const RemotePollReport report = channel.poll();
        if (report.conflict) {
            out << "remote control: refused — " << report.refusals.front().reason << '\n';
            return;
        }
        if (report.applied > 0u) {
            out << "remote control: applied " << report.applied << " request(s) ("
                << report.last_op << "), watermark " << report.watermark << '\n';
        }
        for (const RemoteRefusal &refusal : report.refusals) {
            out << "remote control: refused"
                << (refusal.rkey.empty() ? "" : " " + refusal.rkey) << ": " << refusal.reason
                << '\n';
        }
    } catch (const std::exception &error) {
        /* Transport failure or an unusable cursor: nothing was written, and
         * the next cycle retries from the same watermark. */
        out << "remote control: pass failed (" << error.what() << ")\n";
    }
}

void print_scheduler_report(std::ostream &out, const SchedulerCycleReport &report) {
    out << "scheduler: " << report.contexts_examined << " context(s), "
        << report.decisions << " decision(s), " << report.abstentions << " abstention(s), "
        << report.proposals_written << " proposal(s) written, " << report.executions_attempted
        << " execution attempt(s): " << report.executed << " executed, " << report.refused
        << " refused, " << report.failed << " failed — " << report.detail
        << (report.ordered_by_drives ? " (drive-ordered)" : "")
        << (report.ordered_by_intents ? " (intent-continuations)" : "")
        << "; expectations " << report.expectations_evaluated << " evaluated ("
        << report.expectations_pending << " pending), " << report.resolutions_written
        << " resolution(s) recorded";
    if (report.intents_evaluated > 0u || report.intents_expired > 0u ||
        report.intents_closed > 0u) {
        out << "; intents " << report.intents_evaluated << " evaluated ("
            << report.intents_expired << " expired, " << report.intents_closed << " closed)";
    }
    if (report.intents_opened > 0u || report.intents_continued > 0u ||
        report.intents_cap_reached > 0u) {
        out << "; intent mutations " << report.intents_opened << " opened, "
            << report.intents_continued << " continued, " << report.intents_cap_reached
            << " cap-reached";
    }
    out << "\n";
}

/* Deterministic reflection step (#151) report: printed only when the cycle
 * actually wrote thoughts, so an idle bound (cadence not yet elapsed, no
 * triggers) stays silent. */
void print_reflection_report(std::ostream &out, const ReflectionReport &report,
                             const ReflectionConfig &config, std::string_view now_rfc3339) {
    out << "reflect: wrote " << report.thoughts_written << " thought(s); window "
        << config.window_seconds << "s ending " << now_rfc3339 << ": "
        << report.valence_updates_in_window << " valence update(s) across "
        << report.valence_tokens_in_window << " token(s), " << report.episodes_in_window
        << " episode(s) from " << report.authors_in_window << " author(s), "
        << report.events_in_window << " linked event(s), " << report.resolutions_in_window
        << " resolution(s)\n";
    for (const ReflectionResult &reflection : report.written) {
        out << "  wrote " << reflection.thought.id << ' ' << reflection.thought.kind << " ("
            << reflection.from << ") " << reflection.thought.text << '\n';
    }
}

} // namespace atperson::cli
