#ifndef ATPERSON_OUTBOUND_EVALUATE_HPP
#define ATPERSON_OUTBOUND_EVALUATE_HPP

// Outbound policy evaluation (#23): the deterministic decision layer between
// an accepted core plan and any network action.
//
// `evaluate_outbound_policy` is read-only: it inspects the proposal, the
// operator policy and the recorded budget state, and returns allow, deny or
// defer with a stable reason code and the budget state used. It never mutates
// learned C23 state and never touches the network.
//
// `admit_outbound_action` is the single admission point a real write path
// must call: it evaluates and, only on Allow, records the action in the
// budget state. It is single-writer by contract (mutate the state through one
// owner thread or an explicit serialisation point); the returned budget
// status is the state measured *before* admission.
//
// Nothing here performs I/O or consults a clock: `now` is passed in Unix
// seconds, so decisions are reproducible in tests.

#include "actions.hpp"
#include "budget.hpp"
#include "config.hpp"

#include <cstdint>
#include <string>

namespace atperson {

/* Inspectable budget state behind a decision. */
struct OutboundBudgetStatus {
    bool enabled{false};
    std::uint32_t used_in_window{0};
    std::uint32_t max_in_window{0};
    std::int64_t window_seconds{0};
    std::int64_t min_interval_seconds{0};
    /* Seconds since the last admitted action of this kind; -1 when none. */
    std::int64_t seconds_since_last{-1};
};

struct OutboundPolicyDecision {
    OutboundOutcome outcome{OutboundOutcome::Deny};
    OutboundReason reason{OutboundReason::KindDisabled};
    /* Seconds until a deferred action may be retried; 0 for allow/deny. */
    std::int64_t retry_after_seconds{0};
    OutboundBudgetStatus budget{};
};

[[nodiscard]] OutboundBudgetStatus outbound_budget_status(const OutboundPolicy &policy,
                                                          const OutboundBudgetState &state,
                                                          OutboundActionKind kind,
                                                          std::int64_t now);

/* Read-only evaluation. Deterministic for fixed policy, state and `now`. */
[[nodiscard]] OutboundPolicyDecision
evaluate_outbound_policy(const OutboundPolicy &policy, const OutboundBudgetState &state,
                         const OutboundActionProposal &proposal, std::int64_t now);

/* Evaluate and, on Allow only, record against `state`. Returns the decision
 * taken before any recording. */
[[nodiscard]] OutboundPolicyDecision admit_outbound_action(const OutboundPolicy &policy,
                                                           OutboundBudgetState &state,
                                                           const OutboundActionProposal &proposal,
                                                           std::int64_t now);

/* Deny decision for a kind name the runtime does not know: default-deny at
 * the boundary, expressed in the same inspectable shape. */
[[nodiscard]] OutboundPolicyDecision unsupported_outbound_decision();

/* One-sentence human explanation of a decision's reason. */
[[nodiscard]] std::string describe_outbound_decision(const OutboundPolicyDecision &decision);

} // namespace atperson

#endif
