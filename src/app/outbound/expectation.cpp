#include "expectation.hpp"

#include "action/tokens.hpp"

#include <algorithm>
#include <cmath>

namespace atperson {

std::optional<JournalExpectation> derive_action_expectation(const OutboundAction &action) {
    /* Only autonomous decisions carry the decision evidence a prediction
     * is derived from; operator-authored documents predict nothing. */
    if (!action.plan_score.has_value()) {
        return std::nullopt;
    }
    JournalExpectation expectation;
    const double plan_score = std::clamp(action.plan_score.value(), 0.0, 1.0);
    expectation.reply_likelihood = static_cast<float>(plan_score);
    if (plan_score >= kApproachExpectationFloor) {
        expectation.kind = "approach";
    } else if (plan_score >= kInteractionExpectationFloor) {
        expectation.kind = "interaction";
    } else {
        expectation.kind = "action";
    }
    expectation.tokens = action::distinct_tokens(action.text, kExpectationMaxTokens);
    return expectation;
}

} // namespace atperson