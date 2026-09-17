#include "evaluate.hpp"

#include <algorithm>
#include <optional>

namespace atperson {

OutboundBudgetStatus outbound_budget_status(const OutboundPolicy &policy,
                                            const OutboundBudgetState &state,
                                            OutboundActionKind kind, std::int64_t now) {
    const ActionBudget &budget = budget_for(policy, kind);

    OutboundBudgetStatus status;
    status.enabled = budget.enabled;
    status.max_in_window = budget.max_in_window;
    status.window_seconds = budget.window_seconds;
    status.min_interval_seconds = budget.min_interval_seconds;
    status.used_in_window =
        count_outbound_actions_in_window(state, kind, budget.window_seconds, now);

    const std::optional<std::int64_t> last = last_outbound_action_at(state, kind);
    if (last) {
        status.seconds_since_last = now >= *last ? now - *last : 0;
    }
    return status;
}

OutboundPolicyDecision evaluate_outbound_policy(const OutboundPolicy &policy,
                                                const OutboundBudgetState &state,
                                                const OutboundActionProposal &proposal,
                                                std::int64_t now) {
    const ActionBudget &budget = budget_for(policy, proposal.kind);

    OutboundPolicyDecision decision;
    decision.budget = outbound_budget_status(policy, state, proposal.kind, now);

    if (!budget.enabled) {
        decision.outcome = OutboundOutcome::Deny;
        decision.reason = OutboundReason::KindDisabled;
        return decision;
    }

    if (budget.duplicate_cooldown_seconds > 0) {
        if (const auto recorded = duplicate_outbound_recorded_at(state, proposal)) {
            /* A recorded time in the future only happens after a backwards
             * clock; treat the full cooldown as still active. */
            const std::int64_t elapsed = now > *recorded ? now - *recorded : 0;
            if (elapsed < budget.duplicate_cooldown_seconds) {
                decision.outcome = OutboundOutcome::Defer;
                decision.reason = OutboundReason::DuplicateSuppressed;
                decision.retry_after_seconds =
                    std::max<std::int64_t>(1, budget.duplicate_cooldown_seconds - elapsed);
                return decision;
            }
        }
    }

    if (budget.min_interval_seconds > 0 && decision.budget.seconds_since_last >= 0 &&
        decision.budget.seconds_since_last < budget.min_interval_seconds) {
        decision.outcome = OutboundOutcome::Defer;
        decision.reason = OutboundReason::CooldownActive;
        decision.retry_after_seconds = std::max<std::int64_t>(
            1, budget.min_interval_seconds - decision.budget.seconds_since_last);
        return decision;
    }

    if (decision.budget.used_in_window >= budget.max_in_window) {
        decision.outcome = OutboundOutcome::Defer;
        decision.reason = OutboundReason::WindowExhausted;
        if (const auto earliest = earliest_outbound_action_in_window(state, proposal.kind,
                                                                     budget.window_seconds, now)) {
            const std::int64_t age = now > *earliest ? now - *earliest : 0;
            decision.retry_after_seconds =
                std::max<std::int64_t>(1, budget.window_seconds - age + 1);
        } else {
            decision.retry_after_seconds = std::max<std::int64_t>(1, budget.window_seconds);
        }
        return decision;
    }

    decision.outcome = OutboundOutcome::Allow;
    decision.reason = OutboundReason::Allow;
    return decision;
}

OutboundPolicyDecision admit_outbound_action(const OutboundPolicy &policy,
                                             OutboundBudgetState &state,
                                             const OutboundActionProposal &proposal,
                                             std::int64_t now) {
    const OutboundPolicyDecision decision = evaluate_outbound_policy(policy, state, proposal, now);
    if (decision.outcome == OutboundOutcome::Allow) {
        record_outbound_action(state, proposal, budget_for(policy, proposal.kind), now);
    }
    return decision;
}

OutboundPolicyDecision unsupported_outbound_decision() {
    OutboundPolicyDecision decision;
    decision.outcome = OutboundOutcome::Deny;
    decision.reason = OutboundReason::UnsupportedKind;
    return decision;
}

std::string describe_outbound_decision(const OutboundPolicyDecision &decision) {
    switch (decision.reason) {
    case OutboundReason::Allow:
        return "action is permitted by policy";
    case OutboundReason::KindDisabled:
        return "action kind is disabled by policy (default-deny)";
    case OutboundReason::UnsupportedKind:
        return "action kind is not supported by this runtime";
    case OutboundReason::DuplicateSuppressed:
        return "an identical action was admitted within its duplicate cooldown";
    case OutboundReason::CooldownActive:
        return "the minimum interval between actions of this kind has not elapsed";
    case OutboundReason::WindowExhausted:
        return "the per-window rate limit for this kind is exhausted";
    }
    return "action is not permitted";
}

} // namespace atperson
