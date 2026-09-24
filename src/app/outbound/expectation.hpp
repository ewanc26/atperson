#ifndef ATPERSON_OUTBOUND_EXPECTATION_HPP
#define ATPERSON_OUTBOUND_EXPECTATION_HPP

// Pre-action expectation derivation (#149): turn an autonomous decision's
// frozen evidence into the predicted outcome the journal action entry
// records.
//
// An approved outbound action document carries the decision evidence —
// `plan_score` and `support_score` (#141) — and the exact text that will be
// published. From those alone this module derives a bounded, closed,
// deterministic `JournalExpectation`:
//   kind             — how strongly the decision expected the continuation:
//                      "approach" for a confident plan, "interaction" for a
//                      moderate one, "action" for a weak one (executed
//                      actions never expect "avoid"; it is reserved for
//                      negative prediction, which no executed action has);
//   reply_likelihood — the judged chance the record draws a reply, clamped
//                      to [0, 1] from the plan score;
//   tokens           — the distinct tokens of the plan text (what the entity
//                      expects the interaction to reinforce), first-
//                      appearance order, capped at kExpectationMaxTokens.
//
// Derivation is a pure function of the frozen document, so it yields the
// same expectation at the proposal boundary, at execution time and on
// reconstruct replay; it never reads the graph's live state. Operator-
// authored documents (no decision evidence) yield nullopt: the entity only
// expects what it self-decided.
//
// Ownership: no allocations beyond the owned strings/vector it returns.
// Failure modes: none beyond allocation failure.

#include "journal/store.hpp"
#include "outbound/action.hpp"

#include <optional>

namespace atperson {

/* Plan-score floors for the expectation kind (#149). Below the interaction
 * floor an autonomous post carries no confident interaction prediction and
 * records the weakest kind. */
inline constexpr double kApproachExpectationFloor = 0.6;
inline constexpr double kInteractionExpectationFloor = 0.35;

/* The predicted outcome of one outbound document, or nullopt when the
 * document carries no decision evidence (operator-authored). */
[[nodiscard]] std::optional<JournalExpectation>
derive_action_expectation(const OutboundAction &action);

} // namespace atperson

#endif