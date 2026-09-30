#include "outbound.hpp"

#include "control/state.hpp"
#include "config.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/action.hpp"
#include "outbound/evaluate.hpp"
#include "outbound/spool.hpp"

#include <algorithm>
#include <cstddef>
#include <system_error>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace atperson {
namespace cli {
namespace {

[[noreturn]] void usage_error() {
    throw std::runtime_error(
        "outbound usage: outbound <status [kind]|rules|evaluate <kind> [target] [digest]|"
        "admit <kind> [target] [digest]|spool|proposals [digest]>");
}

std::string seconds_text(std::int64_t seconds) {
    return seconds < 0 ? std::string("never") : std::to_string(seconds) + "s ago";
}

void print_budget(std::ostream &out, const OutboundBudgetStatus &budget) {
    out << "budget: ";
    if (!budget.enabled) {
        out << "disabled\n";
        return;
    }
    out << "used " << budget.used_in_window << '/' << budget.max_in_window << " in "
        << budget.window_seconds << "s window" << ", min interval " << budget.min_interval_seconds
        << "s" << ", last action " << seconds_text(budget.seconds_since_last) << '\n';
}

std::string control_gate_verdict(const std::filesystem::path &control_file,
                                 std::string_view digest) {
    try {
        const ControlState control = load_control_state(control_file);
        if (control.paused) {
            return "blocked (runtime is paused)";
        }
        if (!control.writes_enabled) {
            return "blocked (network writes are disabled)";
        }
        if (control.dry_run) {
            return "blocked (dry-run mode is on)";
        }
        if (control.approval_required && !is_digest_approved(control, digest)) {
            return "blocked (action digest is not approved)";
        }
        return "would pass";
    } catch (const std::exception &error) {
        return std::string("blocked (control state unreadable: ") + error.what() + ")";
    }
}

void print_control_gate(std::ostream &out, const std::filesystem::path &control_file,
                        std::string_view digest) {
    out << "control gate: " << control_gate_verdict(control_file, digest) << '\n';
}

void print_decision(std::ostream &out, const OutboundPolicyDecision &decision,
                    std::string_view attempted_kind) {
    out << "outcome: " << outbound_outcome_name(decision.outcome) << '\n'
        << "reason: " << outbound_reason_code(decision.reason) << '\n'
        << "detail: " << describe_outbound_decision(decision) << '\n';
    if (decision.reason == OutboundReason::UnsupportedKind && !attempted_kind.empty()) {
        out << "kind: " << attempted_kind << '\n';
    }
    if (decision.outcome == OutboundOutcome::Defer) {
        out << "retry after: " << decision.retry_after_seconds << "s\n";
    }
    print_budget(out, decision.budget);
}

std::optional<OutboundActionProposal> proposal_from(const std::vector<std::string_view> &arguments,
                                                    std::ostream &out) {
    const auto kind = parse_outbound_kind(arguments[0]);
    if (!kind) {
        const OutboundPolicyDecision decision = unsupported_outbound_decision();
        print_decision(out, decision, arguments[0]);
        return std::nullopt;
    }
    OutboundActionProposal proposal;
    proposal.kind = *kind;
    if (arguments.size() > 1) {
        proposal.target = std::string(arguments[1]);
    }
    if (arguments.size() > 2) {
        proposal.action_digest = std::string(arguments[2]);
    }
    return proposal;
}

} // namespace

int run_outbound_command(std::ostream &out, const std::filesystem::path &policy_file,
                         const std::filesystem::path &budget_file,
                         const std::filesystem::path &control_file, std::string_view sub,
                         const std::vector<std::string_view> &arguments, std::int64_t now) {
    if (sub == "spool") {
        /* #154 inspection: pending count, earliest pending time, denied
         * count and the last assigned sequence. Read-only. */
        const SpoolPaths paths{offline_spool_path()};
        const SpoolStatus status = spool_status(paths);
        const ControlState control = load_control_state(control_file);
        out << "offline mode: " << (control.offline_mode ? "on" : "off") << '\n'
            << "pending: " << status.pending_count << '\n';
        if (status.earliest_pending_at) {
            out << "earliest pending: " << *status.earliest_pending_at << '\n';
        }
        out << "denied: " << status.denied_count << '\n'
            << "last sequence: " << status.last_seq << '\n'
            << "spool directory: " << paths.root.string() << '\n';
        return 0;
    }

    if (sub == "proposals") {
        /* Operator review of what the scheduler has frozen and left for
         * approval. Strictly read-only: nothing is approved, admitted,
         * consumed or executed here, and no session is opened. The verdicts
         * are the same policy and control-gate answers `evaluate` gives for
         * the frozen action; a standing authorization envelope can also
         * cover a proposal and is evaluated only at execution time. */
        if (arguments.size() > 1u) {
            usage_error();
        }
        const std::filesystem::path directory = scheduler_proposals_path();
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".json") {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());

        const OutboundPolicy policy = load_outbound_policy(policy_file);
        OutboundBudgetState budget = load_outbound_budget_state(budget_file);
        prune_outbound_budget_state(budget, policy, now);

        const std::string wanted = arguments.empty() ? std::string{} : std::string(arguments[0]);
        std::size_t shown = 0u;
        for (const std::filesystem::path &file : files) {
            OutboundAction action;
            try {
                action = load_outbound_action(file);
            } catch (const std::exception &error) {
                if (wanted.empty()) {
                    out << file.filename().string() << ": unreadable (" << error.what() << ")\n";
                    ++shown;
                }
                continue;
            }
            if (!wanted.empty() && action.digest != wanted) {
                continue;
            }
            ++shown;
            const OutboundActionProposal proposal = outbound_action_proposal(action);
            const OutboundPolicyDecision decision =
                evaluate_outbound_policy(policy, budget, proposal, now);
            out << action.digest << ' ' << outbound_kind_name(action.kind) << " rkey "
                << action.rkey << " created " << action.created_at << '\n';
            if (!action.text.empty()) {
                constexpr std::size_t kPreview = 120u;
                if (!wanted.empty() || action.text.size() <= kPreview) {
                    out << "  text: " << action.text << '\n';
                } else {
                    out << "  text: " << action.text.substr(0, kPreview) << "... ("
                        << action.text.size() << " bytes; `outbound proposals " << action.digest
                        << "` shows all)\n";
                }
            }
            if (!action.reply_parent.empty()) {
                out << "  reply parent: " << action.reply_parent << "\n  reply root: "
                    << action.reply_root << '\n';
            }
            if (!action.subject.empty()) {
                out << "  subject: " << action.subject << '\n';
            }
            if (action.plan_score) {
                out << "  decision evidence: plan score " << *action.plan_score;
                if (action.support_score) {
                    out << ", support " << *action.support_score;
                }
                out << '\n';
            }
            out << "  policy: " << outbound_outcome_name(decision.outcome) << " ("
                << outbound_reason_code(decision.reason) << ")\n  control gate: "
                << control_gate_verdict(control_file, action.digest) << '\n';
        }
        if (shown == 0u) {
            out << (wanted.empty() ? "no pending scheduler proposals"
                                   : "no pending scheduler proposal with that digest")
                << " (" << directory.string() << ")\n";
        } else if (wanted.empty()) {
            out << shown << " pending proposal(s); this command approves and executes nothing "
                << "(approve a digest with `atperson control approve <digest>`)\n";
        }
        return 0;
    }

    if (sub == "rules") {
        const OutboundPolicy policy = load_outbound_policy(policy_file);
        out << "policy file: " << policy_file.string()
            << " (missing file means every kind is disabled)\n";
        out << serialise_outbound_policy(policy);
        return 0;
    }

    if (sub == "status") {
        const OutboundPolicy policy = load_outbound_policy(policy_file);
        OutboundBudgetState budget = load_outbound_budget_state(budget_file);
        prune_outbound_budget_state(budget, policy, now);
        if (!arguments.empty()) {
            const auto kind = parse_outbound_kind(arguments[0]);
            if (!kind) {
                throw std::runtime_error("outbound status: unknown action kind '" +
                                         std::string(arguments[0]) + "'");
            }
            const OutboundBudgetStatus status = outbound_budget_status(policy, budget, *kind, now);
            out << outbound_kind_name(*kind) << ": " << (status.enabled ? "enabled" : "disabled")
                << '\n';
            print_budget(out, status);
            return 0;
        }
        for (std::size_t index = 0; index < kOutboundActionKindCount; ++index) {
            const auto kind = static_cast<OutboundActionKind>(index);
            const OutboundBudgetStatus status = outbound_budget_status(policy, budget, kind, now);
            out << outbound_kind_name(kind) << ": " << (status.enabled ? "enabled" : "disabled");
            if (status.enabled) {
                out << ", used " << status.used_in_window << '/' << status.max_in_window << " in "
                    << status.window_seconds << "s" << ", min interval "
                    << status.min_interval_seconds << "s" << ", duplicate cooldown "
                    << budget_for(policy, kind).duplicate_cooldown_seconds << "s";
            }
            out << '\n';
        }
        return 0;
    }

    const bool admit = sub == "admit";
    if (sub != "evaluate" && !admit) {
        usage_error();
    }
    if (arguments.empty()) {
        usage_error();
    }

    const auto proposal = proposal_from(arguments, out);
    if (!proposal) {
        /* Unknown kind: default-deny at the boundary, reported in the same
         * shape and with no state mutation. */
        return 0;
    }

    const OutboundPolicy policy = load_outbound_policy(policy_file);
    OutboundBudgetState budget = load_outbound_budget_state(budget_file);
    prune_outbound_budget_state(budget, policy, now);

    if (!admit) {
        const OutboundPolicyDecision decision =
            evaluate_outbound_policy(policy, budget, *proposal, now);
        print_decision(out, decision, arguments[0]);
        print_control_gate(out, control_file, proposal->action_digest);
        return 0;
    }

    const OutboundPolicyDecision decision = admit_outbound_action(policy, budget, *proposal, now);
    const bool recorded = decision.outcome == OutboundOutcome::Allow;
    if (recorded) {
        budget.saved_at = now;
        save_outbound_budget_state(budget, budget_file);
    }
    print_decision(out, decision, arguments[0]);
    out << "recorded: " << (recorded ? "yes (budget consumed)" : "no") << '\n';
    print_control_gate(out, control_file, proposal->action_digest);
    return 0;
}

} // namespace cli
} // namespace atperson
