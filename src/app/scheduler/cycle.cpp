#include "cycle.hpp"

#include "action/inspection.hpp"
#include "control/envelope.hpp"
#include "control/state.hpp"
#include "drives.hpp"
#include "intent/mutate.hpp"
#include "intent/state.hpp"
#include "intent/sweep.hpp"
#include "journal/resolve.hpp"
#include "journal/store.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/spool.hpp"
#include "outbound/action.hpp"
#include "outbound/actions.hpp"
#include "state/lock.hpp"
#include "state/time.hpp"

#include <atperson/core.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace atperson {
namespace {

/* TID alphabet (AT Protocol base32-sortable, RFC style: no 0/1/b/o so
 * lexicographic order matches numeric order). */
constexpr std::string_view kTidAlphabet = "234567abcdefghijklmnopqrstuvwxyz";

/* Deterministic AT Protocol TID for a proposal rkey: 38 bits of
 * microseconds from the injected clock, 6 bits of clockid derived from the
 * decision digest so two proposals in the same microsecond still get
 * distinct, chronologically sorting rkeys. Pure; no hidden clock. */
std::string proposal_tid(std::int64_t now, std::string_view digest) {
    std::uint64_t bits = 0u;
    for (const char c : digest) {
        bits = (bits << 4u) | static_cast<std::uint64_t>(c & 0x0fu);
    }
    const std::uint64_t clockid = bits & 0x3fu;
    const std::uint64_t microseconds =
        (static_cast<std::uint64_t>(now) * 1'000'000ull) & 0x3f'ffff'ffff'ffffull;
    std::uint64_t value = (microseconds << 6u) | clockid;
    std::string tid(13u, kTidAlphabet[0]);
    for (std::size_t i = 13u; i-- > 0u;) {
        tid[i] = kTidAlphabet[value & 0x1fu];
        value >>= 5u;
    }
    return tid;
}

/* The proposal post text: the accepted plan's tokens in order, joined by
 * single spaces. The operator inspects exactly these bytes before
 * approving; execution never regenerates them. */
std::string plan_text(const atp_action_decision &decision) {
    std::string text;
    for (std::size_t i = 0u; i < decision.plan.step_count; ++i) {
        const atp_action_candidate &step = decision.plan.steps[i];
        const std::size_t length = strnlen(step.token, ATPERSON_TOKEN_BYTES);
        if (i > 0u) {
            text.push_back(' ');
        }
        text.append(step.token, length);
    }
    return text;
}

/* Atomic write (temp + rename), the same pattern as the run-state and
 * graph snapshots: an interrupted cycle never leaves a partial proposal. */
void write_proposal(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    const std::string temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write scheduler proposal");
        output << contents << '\n';
        output.flush();
        if (!output) throw std::runtime_error("cannot flush scheduler proposal");
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) throw std::runtime_error("cannot commit scheduler proposal");
}

} // namespace

SchedulerCycleReport run_scheduler_cycle(const SchedulerConfig &config, const SchedulerCycle &cycle,
                                         const LanguageGraph &graph, const Ledger &ledger) {
    SchedulerCycleReport report;
    if (!config.enabled) {
        report.detail = "scheduler disabled";
        return report;
    }

    const std::string now_rfc3339 = rfc3339_from_unix(cycle.now);

    /* Spool drain (#154): online mode with a non-empty spool flushes it
     * first, in creation order, through the same attempt atom as every
     * live write — restoring online mode never needs a manual step. The
     * drain is bounded per cycle; offline mode never reaches here (the
     * attempt spools instead of publishing). */
    if (!cycle.attempt.spool_root.empty()) {
        const ControlState drain_control =
            load_control_state(cycle.attempt.control_file);
        if (!drain_control.offline_mode) {
            const SpoolDrainReport drained =
                spool_drain(SpoolPaths{cycle.attempt.spool_root}, cycle.attempt,
                            cycle.writer_for, cycle.now);
            report.spool_published = drained.published;
            report.spool_denied = drained.denied;
            report.spool_deferred = drained.deferred + drained.failed;
        }
    }

    /* Intent sweep (#150): before any new decision, expire/close intents
     * whose window or continuation budget ended. Idempotent and journal-only;
     * it appends terminal intent lines so the resolution pass below judges
     * the swept state. */
    if (config.intents.enabled) {
        const IntentSweepReport sweep =
            sweep_intents(cycle.attempt.journal_file, cycle.now, now_rfc3339);
        report.intents_evaluated = sweep.evaluated;
        report.intents_expired = sweep.expired_written;
        report.intents_closed = sweep.closed_written;
    }

    /* Expectation resolution (#149): after the intent sweep, resolve the
     * recorded predictions against the events that landed since the last
     * cycle (or never) and the intent state — an expired intent is an
     * ultimately unanswered invitation, so its action resolves `unmet`.
     * Idempotent and journal-only; it appends terminal
     * `met`/`unmet`/`expired` resolution lines and never touches the graph,
     * so it is safe at the top of every enabled cycle. */
    const ResolutionReport resolution = resolve_expectations(
        cycle.attempt.journal_file, cycle.now, now_rfc3339);
    report.expectations_evaluated = resolution.evaluated;
    report.expectations_pending = resolution.pending;
    report.resolutions_written = resolution.met_written + resolution.unmet_written +
                                 resolution.expired_written + resolution.state_changed;

    /* Contexts: the most recent committed ledger payloads, newest first.
     * The ledger is the durable authority for what the entity observed;
     * the scheduler keeps no parallel context store. When drive ordering is
     * enabled (#148), the same candidate list is reordered — intent
     * continuations first (#150), then reciprocity, then curiosity — before
     * decisions; the decision and gate bounds never change. */
    const std::vector<drives::ContextCandidate> candidates =
        drives::select_candidates(ledger, config.max_contexts);
    report.contexts_examined = candidates.size();

    std::optional<JournalContents> intents_journal;
    if (config.drives_enabled || config.intents.enabled) {
        intents_journal = load_journal(cycle.attempt.journal_file);
    }

    std::vector<std::size_t> order;
    order.reserve(candidates.size());
    report.ordered_by_drives = config.drives_enabled;
    if (config.drives_enabled) {
        const std::vector<drives::Signals> signals =
            drives::compute_drive_signals(graph, candidates, intents_journal.value(), cycle.now);
        order = drives::order_candidates(signals);
    } else {
        for (std::size_t i = 0u; i < candidates.size(); ++i) {
            order.push_back(i);
        }
    }

    /* Decision -> proposal. Abstention is a first-class outcome, counted
     * and reported; it is never an error. When the decided observation
     * continues an open pending intent (#150), the frozen document is a
     * reply into that conversation (root = the intent's thread root, parent
     * = the triggered observation). */
    const ControlState control = load_control_state(cycle.attempt.control_file);
    for (const std::size_t index : order) {
        if (report.proposals_written >= config.max_proposals) {
            break;
        }
        const drives::ContextCandidate &candidate = candidates[index];
        const atp_action_decision decision = graph.action_decide(candidate.payload);
        if (decision.abstained) {
            ++report.abstentions;
            continue;
        }
        ++report.decisions;
        const std::string digest = decision_digest(candidate.payload, decision);
        const std::filesystem::path proposal = cycle.proposals_dir / (digest + ".json");
        if (std::filesystem::exists(proposal)) {
            /* The approval binds to the exact bytes; an existing proposal
             * for the same digest is never rewritten. */
            ++report.proposals_existing;
            continue;
        }
        OutboundAction action;
        action.kind = OutboundActionKind::Post;
        if (config.intents.enabled && intents_journal.has_value()) {
            const JournalIntent *continuation = continuation_intent(
                intents_journal.value(), candidate.source_id, candidate.author_did, cycle.now);
            if (continuation != nullptr) {
                action.kind = OutboundActionKind::Reply;
                action.reply_root = continuation->id;
                action.reply_parent = candidate.source_id;
            }
        }
        action.text = plan_text(decision);
        action.rkey = proposal_tid(cycle.now, digest);
        action.created_at = now_rfc3339;
        action.digest = digest;
        /* Decision evidence (#141): recorded on the proposal so standing
         * authorization envelopes can apply score floors at execution
         * time. The first step's scores are the decision's evidence;
         * the digest already binds to the full decision. */
        if (decision.plan.step_count > 0u) {
            action.plan_score = decision.plan.steps[0].score;
            action.support_score = decision.plan.steps[0].support_score;
        }
        write_proposal(proposal, serialise_outbound_action(action));
        ++report.proposals_written;
        if (action.kind == OutboundActionKind::Reply) {
            report.ordered_by_intents = true;
        }
    }

    /* Execution: approved proposals only, oldest first, through the same
     * attempt atom the publish CLI uses. Gates are reloaded from disk per
     * attempt, so an operator pause or revocation between attempts always
     * wins. The outbound lock serialises the budget read-modify-write with
     * any concurrent `atperson publish`. */
    const bool envelopes_available = !cycle.attempt.envelopes_dir.empty();
    if (config.max_executions > 0u) {
        std::vector<std::filesystem::path> proposals;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(cycle.proposals_dir, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".json") {
                proposals.push_back(it->path());
            }
        }
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("cannot list scheduler proposals: " + ec.message());
        }
        std::sort(proposals.begin(), proposals.end());

        for (const std::filesystem::path &proposal : proposals) {
            if (report.executions_attempted >= config.max_executions) {
                break;
            }
            if (config.max_cycle_ms > 0 && cycle.steady_ms &&
                cycle.steady_ms() > config.max_cycle_ms) {
                report.detail = "scheduler cycle wall-clock bound reached";
                break;
            }
            const OutboundAction action = load_outbound_action(proposal);
            /* Per-digest approval or a standing envelope (#141): the
             * proposal is a candidate only when one of the two would
             * authorise it. The attempt atom re-evaluates coverage at
             * execution time, so this pre-check is a cheap filter, not
             * the authority. */
            bool authorizable = is_digest_approved(control, action.digest);
            if (!authorizable && envelopes_available) {
                const EnvelopeEvidence evidence{action.plan_score, action.support_score};
                /* Budget state for the pre-check: loaded fresh here; the
                 * attempt atom loads its own under the lock. */
                const OutboundPolicy policy = load_outbound_policy(cycle.attempt.policy_file);
                OutboundBudgetState budget =
                    load_outbound_budget_state(cycle.attempt.budget_file);
                prune_outbound_budget_state(budget, policy, cycle.now);
                const EnvelopeCoverage coverage = find_covering_envelope(
                    cycle.attempt.envelopes_dir, policy, action.kind, action.text, evidence,
                    budget, cycle.now);
                authorizable = coverage.covered;
            }
            if (!authorizable) {
                continue;
            }
            ++report.executions_attempted;
            const StateLock outbound_lock(cycle.data_dir, kSchedulerOutboundLockName);
            const OutboundExecutionResult result =
                attempt_outbound_action(action, cycle.attempt, cycle.writer_for, cycle.now);
            switch (result.outcome) {
            case OutboundExecutionOutcome::Executed:
                ++report.executed;
                /* Pending intent (#150): an executed post or reply records
                 * its conversation — opening a new intent (while under the
                 * cap), continuing the one it answered, or refusing when the
                 * cap is exhausted. Recording happens here, after the write,
                 * so operator `publish` never silently opens a conversation. */
                if (config.intents.enabled) {
                    switch (record_pending_intent(cycle.attempt.journal_file, action, result,
                                                  config.intents, cycle.now, now_rfc3339)) {
                    case IntentMutation::Opened:
                        ++report.intents_opened;
                        break;
                    case IntentMutation::Continued:
                        ++report.intents_continued;
                        break;
                    case IntentMutation::CapReached:
                        ++report.intents_cap_reached;
                        break;
                    case IntentMutation::Duplicate:
                    case IntentMutation::NotTracked:
                        break;
                    }
                }
                std::filesystem::remove(proposal);
                break;
            case OutboundExecutionOutcome::DryRun:
            case OutboundExecutionOutcome::Denied:
            case OutboundExecutionOutcome::Deferred:
                ++report.refused;
                break;
            case OutboundExecutionOutcome::Failed:
                ++report.failed;
                break;
            }
        }
    }

    if (report.detail.empty()) {
        report.detail = "scheduler cycle complete";
    }
    return report;
}

SchedulerConfig scheduler_config_from_environment() {
    SchedulerConfig config;
    const char *enabled = std::getenv("ATPERSON_SCHEDULER");
    config.enabled = enabled != nullptr && std::string_view(enabled) == "1";
    const char *drives = std::getenv("ATPERSON_SCHEDULER_DRIVES");
    config.drives_enabled = drives != nullptr && std::string_view(drives) == "1";
    config.intents = intent_config_from_environment();
    return config;
}

} // namespace atperson
