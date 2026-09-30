#include "cycle.hpp"

#include "action/decision_env.hpp"
#include "action/inspection.hpp"
#include "control/envelope.hpp"
#include "control/state.hpp"
#include "drives.hpp"
#include "engagement.hpp"
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
#include "breaker.hpp"
#include "text_guard.hpp"

#include <atperson/core.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
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

/* Digest for a graduated like proposal (#152): binds to the subject and
 * the abstention evidence that produced it — kind, subject, abstain
 * reason and the raw/viable plan counts — so the approval binds to the
 * exact below-floor decision the operator inspected. Distinct by
 * construction from any post/reply decision digest. */
std::string graduated_like_digest(std::string_view subject,
                                  const atp_action_decision &decision) {
    std::string canonical = "like";
    canonical.append(subject);
    canonical.push_back('\0');
    canonical.append(reinterpret_cast<const char *>(&decision.abstain_reason),
                     sizeof(decision.abstain_reason));
    canonical.append(reinterpret_cast<const char *>(&decision.raw_plan_count),
                     sizeof(decision.raw_plan_count));
    canonical.append(reinterpret_cast<const char *>(&decision.viable_plan_count),
                     sizeof(decision.viable_plan_count));
    const std::uint64_t digest = atp_ledger_digest(canonical.data(), canonical.size());
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(digest));
    return std::string(hex, 16u);
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



constexpr const char *kBreakerFileName = "scheduler-breaker.json";
constexpr const char *kQuarantineDirName = "quarantine";

std::size_t count_proposals(const std::filesystem::path &dir) {
    std::size_t count = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".json") {
            ++count;
        }
    }
    return count;
}

/* Set a poison proposal aside so it is not retried and does not starve the
 * queue behind it. It stays on disk, with the reason, for inspection. */
void quarantine_proposal(const std::filesystem::path &proposal, std::string_view reason,
                         std::int64_t now) {
    const std::filesystem::path dir = proposal.parent_path() / kQuarantineDirName;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path target = dir / proposal.filename();
    std::filesystem::rename(proposal, target, ec);
    if (ec) {
        throw std::runtime_error("cannot quarantine " + proposal.string() + ": " + ec.message());
    }
    std::ofstream note(target.string() + ".reason", std::ios::trunc);
    note << rfc3339_from_unix(now) << ' ' << reason << '\n';
}

/* Guard configuration for this cycle. The denylist is re-read every cycle so an
 * operator's edit takes effect on the next one without a restart. */
TextGuardConfig cycle_text_guard(const SchedulerConfig &config, const SchedulerCycle &cycle) {
    TextGuardConfig guard = config.text_guard;
    const char *override_path = std::getenv("ATPERSON_OUTPUT_DENYLIST");
    const std::filesystem::path path = override_path != nullptr && override_path[0] != '\0'
                                           ? std::filesystem::path(override_path)
                                           : cycle.data_dir / "output-denylist.txt";
    guard.denied_terms = load_denylist(path);
    return guard;
}

/* The do-not-engage list, re-read every cycle so an addition is a live brake. */
DoNotEngage cycle_do_not_engage(const SchedulerCycle &cycle) {
    const char *override_path = std::getenv("ATPERSON_DO_NOT_ENGAGE");
    return load_do_not_engage(override_path != nullptr && override_path[0] != '\0'
                                  ? std::filesystem::path(override_path)
                                  : cycle.data_dir / "do-not-engage.txt");
}

/* Texts the entity already published, from the journal's executed actions. */
std::vector<RecentText> executed_texts(const std::filesystem::path &journal_file) {
    std::vector<RecentText> texts;
    for (const JournalAction &action : load_journal(journal_file).actions) {
        if (action.outcome == JournalActionOutcome::Executed && !action.text.empty()) {
            texts.push_back({action.text, static_cast<std::int64_t>(
                                              parse_rfc3339_epoch(action.at).value_or(0u))});
        }
    }
    return texts;
}

bool is_text_kind(OutboundActionKind kind) noexcept {
    return kind == OutboundActionKind::Post || kind == OutboundActionKind::Reply;
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
    const TextGuardConfig text_guard = cycle_text_guard(config, cycle);
    const DoNotEngage opted_out = cycle_do_not_engage(cycle);
    const std::set<std::string> invited = invited_authors(load_journal(cycle.attempt.journal_file));
    report.engagement_invalid_lines = opted_out.invalid_lines;
    /* Repeat detection sees what was published and what is already queued, so
     * two contexts that decide the same words never both go out. */
    std::size_t pending_proposals = count_proposals(cycle.proposals_dir);
    std::vector<RecentText> recent_texts = executed_texts(cycle.attempt.journal_file);
    {
        std::error_code queued_ec;
        for (std::filesystem::directory_iterator it(cycle.proposals_dir, queued_ec), end;
             !queued_ec && it != end; it.increment(queued_ec)) {
            if (it->is_regular_file(queued_ec) && it->path().extension() == ".json") {
                try {
                    const OutboundAction queued = load_outbound_action(it->path());
                    if (!queued.text.empty()) {
                        recent_texts.push_back({queued.text, cycle.now});
                    }
                } catch (const std::exception &) {
                    /* An unreadable proposal is reported by execution; it has
                     * no text to compare. */
                }
            }
        }
    }
    for (const std::size_t index : order) {
        if (report.proposals_written >= config.max_proposals) {
            break;
        }
        /* Counted only when it is the cap, not the per-cycle limit, that
         * stopped a proposal from being made. */
        if (pending_proposals >= config.max_pending_proposals) {
            ++report.proposals_capped;
            break;
        }
        const drives::ContextCandidate &candidate = candidates[index];
        const atp_action_decision decision = graph.action_decide(candidate.payload, config.decision);
        if (decision.abstained) {
            /* Graduated actions (#152): a below-floor abstention on a
             * likeable subject is itself inspectable evidence. Compose an
             * explicit like proposal — a distinct decision with its own
             * digest, never a rewrite of the abstained plan — when the
             * operator enabled it. The like still passes every execution
             * gate unchanged. */
            if (!config.graduated_likes || report.proposals_written >= config.max_proposals) {
                ++report.abstentions;
                continue;
            }
            /* Every abstention except EMPTY_CONTEXT is a below-text-floor
             * outcome: no viable candidates, or candidates that failed the
             * score/support floors. EMPTY_CONTEXT means the payload itself
             * carried nothing to engage with - nothing to like either. */
            const bool below_text_floor =
                decision.abstain_reason != ATP_ACTION_ABSTAIN_EMPTY_CONTEXT;
            const bool likeable = candidate.source_id.rfind("at://", 0u) == 0u &&
                                 candidate.source_id.find("/app.bsky.feed.post/") !=
                                     std::string::npos;
            if (!below_text_floor || !likeable) {
                ++report.abstentions;
                continue;
            }
            if (const EngagementRefusal refusal = check_engagement(
                    OutboundActionKind::Like, candidate.author_did, config.engagement, invited,
                    opted_out);
                refusal != EngagementRefusal::None) {
                ++report.engagement_refused;
                report.last_engagement_refusal = engagement_refusal_name(refusal);
                ++report.abstentions;
                continue;
            }
            const std::string like_digest =
                graduated_like_digest(candidate.source_id, decision);
            const std::filesystem::path like_proposal =
                cycle.proposals_dir / (like_digest + ".json");
            if (std::filesystem::exists(like_proposal) ||
                std::filesystem::exists(cycle.proposals_dir / kQuarantineDirName /
                                        (like_digest + ".json"))) {
                ++report.proposals_existing;
                ++report.abstentions;
                continue;
            }
            OutboundAction like;
            like.kind = OutboundActionKind::Like;
            like.subject = candidate.source_id;
            like.rkey = proposal_tid(cycle.now, like_digest);
            like.created_at = now_rfc3339;
            like.digest = like_digest;
            write_proposal(like_proposal, serialise_outbound_action(like));
            ++report.proposals_written;
            ++pending_proposals;
            ++report.graduated_likes_written;
            ++report.abstentions;
            continue;
        }
        ++report.decisions;
        const std::string digest = decision_digest(candidate.payload, decision);
        const std::filesystem::path proposal = cycle.proposals_dir / (digest + ".json");
        if (std::filesystem::exists(proposal) ||
            std::filesystem::exists(cycle.proposals_dir / kQuarantineDirName / (digest + ".json"))) {
            /* Already queued, or set aside as a poison proposal: never
             * re-proposed, or the same failure would repeat forever. */
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
        if (action.kind == OutboundActionKind::Reply) {
            if (const EngagementRefusal refusal =
                    check_engagement(OutboundActionKind::Reply, candidate.author_did,
                                     config.engagement, invited, opted_out);
                refusal != EngagementRefusal::None) {
                ++report.engagement_refused;
                report.last_engagement_refusal = engagement_refusal_name(refusal);
                continue;
            }
        }
        action.text = plan_text(decision);
        if (const TextVerdict verdict =
                check_output_text(action.text, text_guard, recent_texts, cycle.now);
            !verdict.allowed()) {
            ++report.text_refused;
            report.last_text_refusal = text_refusal_name(verdict.refusal);
            continue;
        }
        recent_texts.push_back({action.text, cycle.now});
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
        ++pending_proposals;
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
    const std::filesystem::path breaker_file = cycle.data_dir / kBreakerFileName;
    BreakerState breaker = load_breaker_state(breaker_file);
    bool breaker_dirty = false;
    const BreakerGate gate = breaker_gate(breaker, cycle.now);
    report.breaker_gate = breaker_gate_name(gate);
    if (gate == BreakerGate::Open) {
        report.detail = "circuit breaker open until " + rfc3339_from_unix(breaker.open_until) +
                        " after " + std::to_string(breaker.consecutive_failures) +
                        " consecutive failure(s): " + breaker.last_failure_detail;
    }
    if (config.max_executions > 0u && gate != BreakerGate::Open) {
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

        std::optional<std::vector<RecentText>> execution_texts;
        {
            std::set<std::string> queued;
            for (const std::filesystem::path &proposal : proposals) {
                queued.insert(proposal.stem().string());
            }
            retain_proposals(breaker, queued);
        }
        bool stop_execution = false;
        for (const std::filesystem::path &proposal : proposals) {
            if (stop_execution || report.executions_attempted >= config.max_executions) {
                break;
            }
            /* Half-open: exactly one attempt to test the dependency. */
            if (gate == BreakerGate::HalfOpen && report.executions_attempted >= 1u) {
                break;
            }
            if (config.max_cycle_ms > 0 && cycle.steady_ms &&
                cycle.steady_ms() > config.max_cycle_ms) {
                report.detail = "scheduler cycle wall-clock bound reached";
                break;
            }
            const OutboundAction action = load_outbound_action(proposal);
            /* The guard runs again at execution: a proposal frozen earlier is
             * checked against today's denylist, and against anything published
             * since it was written. */
            if (is_text_kind(action.kind)) {
                if (!execution_texts.has_value()) {
                    execution_texts = executed_texts(cycle.attempt.journal_file);
                }
                if (const TextVerdict verdict = check_output_text(
                        action.text, text_guard, *execution_texts, cycle.now);
                    !verdict.allowed()) {
                    ++report.text_refused;
                    report.last_text_refusal = text_refusal_name(verdict.refusal);
                    continue;
                }
            }
            /* Whom it is directed at is checked again at execution: a person
             * added to the do-not-engage list after the proposal was frozen is
             * never interacted with. */
            if (const EngagementRefusal refusal =
                    check_engagement(action.kind, action_target_did(action), config.engagement,
                                     invited, opted_out);
                refusal != EngagementRefusal::None) {
                ++report.engagement_refused;
                report.last_engagement_refusal = engagement_refusal_name(refusal);
                continue;
            }
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
                if (execution_texts.has_value() && !action.text.empty()) {
                    execution_texts->push_back({action.text, cycle.now});
                }
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
                record_success(breaker, action.digest);
                breaker_dirty = true;
                break;
            case OutboundExecutionOutcome::DryRun:
            case OutboundExecutionOutcome::Denied:
            case OutboundExecutionOutcome::Deferred:
                ++report.refused;
                break;
            case OutboundExecutionOutcome::Failed: {
                ++report.failed;
                const FailureEffect effect = record_failure(
                    breaker, config.breaker, action.digest,
                    result.reason_code + (result.detail.empty() ? "" : ": " + result.detail),
                    cycle.now);
                breaker_dirty = true;
                if (effect.quarantine) {
                    quarantine_proposal(proposal, result.detail, cycle.now);
                    ++report.quarantined;
                }
                if (effect.tripped) {
                    ++report.breaker_trips;
                    stop_execution = true; /* stop hammering it this cycle */
                    report.detail = "circuit breaker opened until " +
                                    rfc3339_from_unix(breaker.open_until);
                }
                break;
            }
            }
        }
    }
    if (breaker_dirty) {
        save_breaker_state(breaker, breaker_file);
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
    const char *graduated = std::getenv("ATPERSON_SCHEDULER_GRADUATED_LIKES");
    config.graduated_likes = graduated != nullptr && std::string_view(graduated) == "1";
    config.intents = intent_config_from_environment();
    apply_decision_env(config.decision);
    auto positive_env = [](const char *name, long long &target, long long minimum) {
        const char *raw = std::getenv(name);
        if (raw == nullptr || raw[0] == '\0') {
            return;
        }
        char *end = nullptr;
        const long long value = std::strtoll(raw, &end, 10);
        if (end == raw || *end != '\0' || value < minimum) {
            throw std::runtime_error(std::string(name) + " must be a whole number of at least " +
                                     std::to_string(minimum));
        }
        target = value;
    };
    {
        long long threshold = config.breaker.failure_threshold;
        long long cooldown = config.breaker.base_cooldown_seconds;
        long long max_cooldown = config.breaker.max_cooldown_seconds;
        long long limit = config.breaker.proposal_failure_limit;
        long long pending = static_cast<long long>(config.max_pending_proposals);
        positive_env("ATPERSON_BREAKER_THRESHOLD", threshold, 1);
        positive_env("ATPERSON_BREAKER_COOLDOWN", cooldown, 1);
        positive_env("ATPERSON_BREAKER_MAX_COOLDOWN", max_cooldown, 1);
        positive_env("ATPERSON_PROPOSAL_FAILURE_LIMIT", limit, 1);
        positive_env("ATPERSON_SCHEDULER_MAX_PENDING", pending, 1);
        if (max_cooldown < cooldown) {
            throw std::runtime_error(
                "ATPERSON_BREAKER_MAX_COOLDOWN must not be below ATPERSON_BREAKER_COOLDOWN");
        }
        config.breaker.failure_threshold = static_cast<std::uint32_t>(std::min(threshold, 1000000ll));
        config.breaker.base_cooldown_seconds = cooldown;
        config.breaker.max_cooldown_seconds = max_cooldown;
        config.breaker.proposal_failure_limit = static_cast<std::uint32_t>(std::min(limit, 1000000ll));
        config.max_pending_proposals = static_cast<std::size_t>(pending);
    }
    if (const char *mode = std::getenv("ATPERSON_ENGAGEMENT"); mode != nullptr && mode[0] != '\0') {
        const std::string value(mode);
        if (value == "invited") {
            config.engagement.mode = EngagementMode::Invited;
        } else if (value == "open") {
            config.engagement.mode = EngagementMode::Open;
        } else {
            throw std::runtime_error("ATPERSON_ENGAGEMENT must be 'invited' or 'open'");
        }
    }
    if (const char *window = std::getenv("ATPERSON_OUTPUT_REPEAT_WINDOW");
        window != nullptr && window[0] != '\0') {
        char *end = nullptr;
        const long long seconds = std::strtoll(window, &end, 10);
        if (end == window || *end != '\0' || seconds < 0) {
            throw std::runtime_error(
                "ATPERSON_OUTPUT_REPEAT_WINDOW must be a non-negative number of seconds");
        }
        config.text_guard.repeat_window_seconds = seconds;
    }
    return config;
}

} // namespace atperson
