#include "outbound.hpp"

#include "control/state.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/evaluate.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {
namespace {

[[noreturn]] void usage_error() {
    throw std::runtime_error(
        "outbound usage: outbound <status [kind]|rules|evaluate <kind> [target] [digest]|"
        "admit <kind> [target] [digest]>");
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

void print_control_gate(std::ostream &out, const std::filesystem::path &control_file,
                        std::string_view digest) {
    std::string verdict;
    try {
        const ControlState control = load_control_state(control_file);
        if (control.paused) {
            verdict = "blocked (runtime is paused)";
        } else if (!control.writes_enabled) {
            verdict = "blocked (network writes are disabled)";
        } else if (control.dry_run) {
            verdict = "blocked (dry-run mode is on)";
        } else if (control.approval_required && !is_digest_approved(control, digest)) {
            verdict = "blocked (action digest is not approved)";
        } else {
            verdict = "would pass";
        }
    } catch (const std::exception &error) {
        verdict = std::string("blocked (control state unreadable: ") + error.what() + ")";
    }
    out << "control gate: " << verdict << '\n';
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
