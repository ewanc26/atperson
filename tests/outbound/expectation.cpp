/* Pre-action expectation derivation tests (#149): the closed-vocabulary
 * mapping from an outbound document's decision evidence onto a
 * JournalExpectation, including the no-evidence (operator-authored)
 * nullopt contract, the kind floors, the clamped reply likelihood and the
 * distinct/token-capped text reduction. Offline, no network. */

#include "journal/store.hpp"
#include "outbound/action.hpp"
#include "outbound/expectation.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using atperson::JournalExpectation;
using atperson::OutboundAction;
using atperson::OutboundActionKind;
using atperson::derive_action_expectation;

OutboundAction document(double *plan_score, const std::string &text) {
    OutboundAction action;
    action.kind = OutboundActionKind::Post;
    action.text = text;
    action.rkey = "3lzc7a2pfxn2c";
    action.created_at = "2026-09-17T09:00:00Z";
    action.digest = "0123456789abcdef";
    if (plan_score != nullptr) {
        action.plan_score = *plan_score;
    }
    return action;
}

void test_operator_authored_predicts_nothing() {
    /* No decision evidence: an operator-authored document must never derive
     * an expectation the entity did not itself hold. */
    const std::optional<JournalExpectation> none = derive_action_expectation(document(nullptr, "hello"));
    assert(!none.has_value());
    std::printf("ok operator authored predicts nothing\n");
}

void test_kind_floors_and_likelihood() {
    double score = 0.9; /* above the approach floor */
    const std::optional<JournalExpectation> approach =
        derive_action_expectation(document(&score, "the moon is a loyal companion"));
    assert(approach.has_value());
    assert(approach->kind == "approach");
    assert(approach->reply_likelihood == 0.9f);

    score = 0.6; /* exactly the approach floor */
    assert(derive_action_expectation(document(&score, "x"))->kind == "approach");

    score = 0.5; /* between the floors: interaction */
    const std::optional<JournalExpectation> interaction =
        derive_action_expectation(document(&score, "the moon is a loyal companion"));
    assert(interaction.has_value());
    assert(interaction->kind == "interaction");
    assert(interaction->reply_likelihood == 0.5f);

    score = 0.35; /* exactly the interaction floor */
    assert(derive_action_expectation(document(&score, "x"))->kind == "interaction");

    score = 0.2; /* below the floors: plain action */
    const std::optional<JournalExpectation> action =
        derive_action_expectation(document(&score, "the moon is a loyal companion"));
    assert(action.has_value());
    assert(action->kind == "action");
    assert(action->reply_likelihood == 0.2f);

    /* Extrema are clamped into [0, 1] before judging: a negative plan score
     * is the weakest action, never a negative (impossible) likelihood. */
    score = -0.3;
    const std::optional<JournalExpectation> weak =
        derive_action_expectation(document(&score, "x"));
    assert(weak.has_value());
    assert(weak->kind == "action");
    assert(weak->reply_likelihood == 0.0f);

    score = 1.5;
    const std::optional<JournalExpectation> strong =
        derive_action_expectation(document(&score, "x"));
    assert(strong.has_value());
    assert(strong->kind == "approach");
    assert(strong->reply_likelihood == 1.0f);

    assert(approach->tokens == std::vector<std::string>({"the", "moon", "is", "a", "loyal",
                                                         "companion"}));
    std::printf("ok kind floors and likelihood\n");
}

void test_tokens_distinct_and_capped() {
    /* Repeated words collapse to first-appearance order. */
    double score = 0.7;
    const std::optional<JournalExpectation> expected =
        derive_action_expectation(document(&score, "moon moon the the moon"));
    assert(expected.has_value());
    assert(expected->tokens == std::vector<std::string>({"moon", "the"}));

    /* An arbitrarily long plan contributes only the first kExpectationMaxTokens
     * distinct tokens, in order. */
    const std::string long_text =
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda";
    const std::optional<JournalExpectation> capped =
        derive_action_expectation(document(&score, long_text));
    assert(capped.has_value());
    assert(capped->tokens.size() == atperson::kExpectationMaxTokens);
    assert(capped->tokens.front() == "alpha");
    assert(capped->tokens.back() == "theta");
    std::printf("ok tokens distinct and capped\n");
}

} // namespace

int main() {
    test_operator_authored_predicts_nothing();
    test_kind_floors_and_likelihood();
    test_tokens_distinct_and_capped();
    std::printf("atperson-expectation: all tests passed\n");
    return 0;
}