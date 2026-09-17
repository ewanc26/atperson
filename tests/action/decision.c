#include "atperson/action.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void assert_decision_same(const atp_action_decision *a, const atp_action_decision *b) {
    assert(a->abstained == b->abstained);
    assert(a->abstain_reason == b->abstain_reason);
    assert(a->plan.step_count == b->plan.step_count);
    assert(a->plan.stop_reason == b->plan.stop_reason);
    assert(fabsf(a->plan.score - b->plan.score) < 0.000001f);
    assert(a->raw_plan_count == b->raw_plan_count);
    assert(a->viable_plan_count == b->viable_plan_count);
    assert(a->evidence.reason == b->evidence.reason);
    assert(a->evidence.step_index == b->evidence.step_index);
    assert(a->evidence.accepted_steps == b->evidence.accepted_steps);
    assert(a->evidence.cycle_start_index == b->evidence.cycle_start_index);
    assert(strcmp(a->evidence.token, b->evidence.token) == 0);
    for (size_t i = 0u; i < a->plan.step_count; ++i) {
        assert(strcmp(a->plan.steps[i].token, b->plan.steps[i].token) == 0);
        assert(fabsf(a->plan.steps[i].score - b->plan.steps[i].score) < 0.000001f);
    }
}

static atp_action_decision_config permissive_config(void) {
    atp_action_decision_config config = atp_action_decision_default_config();
    config.guards.min_candidate_score = 0.0f;
    config.guards.min_support_score = 0.0f;
    config.guards.max_score_drop = 1.0f;
    config.guards.max_consecutive_occurrences = 1u;
    return config;
}

static void test_empty_and_unknown_context_abstain_explicitly(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha beta", "at://decision/1") == ATP_OK);

    atp_action_decision decision = {0};
    assert(atp_graph_action_decide(graph, "", NULL, &decision) == ATP_OK);
    assert(decision.abstained);
    assert(decision.abstain_reason == ATP_ACTION_ABSTAIN_EMPTY_CONTEXT);
    assert(decision.raw_plan_count == 0u);
    assert(decision.viable_plan_count == 0u);

    assert(atp_graph_action_decide(graph, "unknown-token", NULL, &decision) == ATP_OK);
    assert(decision.abstained);
    assert(decision.abstain_reason == ATP_ACTION_ABSTAIN_NO_CANDIDATES);
    assert(decision.raw_plan_count == 0u);

    atp_graph_destroy(graph);
}

static void test_low_support_abstains_with_threshold_evidence(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha beta", "at://support/1") == ATP_OK);

    atp_action_decision_config config = permissive_config();
    config.guards.min_support_score = 0.75f;

    atp_action_decision decision = {0};
    assert(atp_graph_action_decide(graph, "alpha", &config, &decision) == ATP_OK);
    assert(decision.abstained);
    assert(decision.abstain_reason == ATP_ACTION_ABSTAIN_LOW_SUPPORT);
    assert(decision.raw_plan_count > 0u);
    assert(decision.viable_plan_count == 0u);
    assert(decision.plan.step_count == 0u);
    assert(decision.plan.stop_reason == ATP_ACTION_PLAN_STOP_LOW_SUPPORT);
    assert(decision.evidence.reason == ATP_ACTION_PLAN_STOP_LOW_SUPPORT);
    assert(strcmp(decision.evidence.token, "beta") == 0);
    assert(decision.evidence.support_score < decision.evidence.min_support_score);
    assert(fabsf(decision.evidence.min_support_score - 0.75f) < 0.000001f);
    assert(decision.evidence.accepted_steps == 0u);

    atp_graph_destroy(graph);
}

static void test_score_drop_truncates_supported_prefix(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    for (size_t i = 0u; i < 8u; ++i) {
        char source[32];
        snprintf(source, sizeof(source), "at://drop/%zu", i);
        assert(atp_graph_observe_text(graph, "start mid", source) == ATP_OK);
    }
    assert(atp_graph_observe_text(graph, "mid tail", "at://drop/tail") == ATP_OK);

    atp_action_plan_config planner = {.max_tokens = 3u, .beam_width = 1u};
    atp_action_plan raw[1] = {0};
    size_t raw_count = 0u;
    assert(atp_graph_action_plans(graph, "start", &planner, raw, 1u, &raw_count) == ATP_OK);
    assert(raw_count == 1u);
    assert(raw[0].step_count >= 2u);
    assert(raw[0].steps[0].score > raw[0].steps[1].score);
    const float observed_drop = raw[0].steps[0].score - raw[0].steps[1].score;

    atp_action_decision_config config = permissive_config();
    config.planner = planner;
    config.guards.max_score_drop = observed_drop * 0.5f;

    atp_action_decision decision = {0};
    assert(atp_graph_action_decide(graph, "start", &config, &decision) == ATP_OK);
    assert(!decision.abstained);
    assert(decision.plan.step_count == 1u);
    assert(strcmp(decision.plan.steps[0].token, "mid") == 0);
    assert(decision.plan.stop_reason == ATP_ACTION_PLAN_STOP_SCORE_DROP);
    assert(decision.evidence.reason == ATP_ACTION_PLAN_STOP_SCORE_DROP);
    assert(strcmp(decision.evidence.token, "tail") == 0);
    assert(decision.evidence.previous_score - decision.evidence.candidate_score >
           decision.evidence.max_score_drop);

    atp_graph_destroy(graph);
}

static void test_self_loop_stops_as_repetition(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "a a", "at://repeat/1") == ATP_OK);

    atp_action_decision_config config = permissive_config();
    config.planner.max_tokens = 6u;
    config.planner.beam_width = 2u;

    atp_action_decision decision = {0};
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_OK);
    assert(!decision.abstained);
    assert(decision.plan.step_count == 1u);
    assert(strcmp(decision.plan.steps[0].token, "a") == 0);
    assert(decision.plan.stop_reason == ATP_ACTION_PLAN_STOP_REPETITION);
    assert(decision.evidence.reason == ATP_ACTION_PLAN_STOP_REPETITION);
    assert(decision.evidence.step_index == 1u);
    assert(decision.evidence.accepted_steps == 1u);
    assert(decision.evidence.consecutive_occurrences == 2u);
    assert(decision.evidence.max_consecutive_occurrences == 1u);
    assert(strcmp(decision.evidence.token, "a") == 0);

    atp_graph_destroy(graph);
}

static void test_two_token_cycle_stops_before_revisit(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "a b a", "at://cycle/1") == ATP_OK);

    atp_action_decision_config config = permissive_config();
    config.planner.max_tokens = 6u;
    config.planner.beam_width = 2u;

    atp_action_decision decision = {0};
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_OK);
    assert(!decision.abstained);
    assert(decision.plan.step_count == 2u);
    assert(strcmp(decision.plan.steps[0].token, "b") == 0);
    assert(strcmp(decision.plan.steps[1].token, "a") == 0);
    assert(decision.plan.stop_reason == ATP_ACTION_PLAN_STOP_CYCLE);
    assert(decision.evidence.reason == ATP_ACTION_PLAN_STOP_CYCLE);
    assert(decision.evidence.step_index == 2u);
    assert(decision.evidence.accepted_steps == 2u);
    assert(decision.evidence.cycle_start_index == 0u);
    assert(strcmp(decision.evidence.token, "b") == 0);

    atp_graph_destroy(graph);
}

static void test_max_length_remains_explicit(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "a b c d e", "at://length/1") == ATP_OK);

    atp_action_decision_config config = permissive_config();
    config.planner.max_tokens = 2u;
    config.planner.beam_width = 1u;

    atp_action_decision decision = {0};
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_OK);
    assert(!decision.abstained);
    assert(decision.plan.step_count == 2u);
    assert(strcmp(decision.plan.steps[0].token, "b") == 0);
    assert(strcmp(decision.plan.steps[1].token, "c") == 0);
    assert(decision.plan.stop_reason == ATP_ACTION_PLAN_STOP_MAX_TOKENS);
    assert(decision.evidence.reason == ATP_ACTION_PLAN_STOP_MAX_TOKENS);
    assert(decision.evidence.accepted_steps == 2u);
    assert(decision.evidence.max_tokens == 2u);

    atp_graph_destroy(graph);
}

static void test_dead_end_remains_explicit(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "a b", "at://dead/1") == ATP_OK);

    atp_action_decision_config config = permissive_config();
    config.planner.max_tokens = 4u;
    config.planner.beam_width = 1u;

    atp_action_decision decision = {0};
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_OK);
    assert(!decision.abstained);
    assert(decision.plan.step_count == 1u);
    assert(strcmp(decision.plan.steps[0].token, "b") == 0);
    assert(decision.plan.stop_reason == ATP_ACTION_PLAN_STOP_DEAD_END);
    assert(decision.evidence.reason == ATP_ACTION_PLAN_STOP_DEAD_END);

    atp_graph_destroy(graph);
}

static void test_repeated_calls_are_deterministic_and_read_only(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "start alpha end", "at://stable/1") == ATP_OK);
    assert(atp_graph_observe_text(graph, "start beta finish", "at://stable/2") == ATP_OK);

    const atp_graph_stats before = atp_graph_get_stats(graph);
    atp_action_decision first = {0};
    atp_action_decision second = {0};
    assert(atp_graph_action_decide(graph, "start", NULL, &first) == ATP_OK);
    assert(atp_graph_action_decide(graph, "start", NULL, &second) == ATP_OK);
    assert_decision_same(&first, &second);

    const atp_graph_stats after = atp_graph_get_stats(graph);
    assert(after.node_count == before.node_count);
    assert(after.edge_count == before.edge_count);
    assert(after.observations == before.observations);
    assert(after.token_observations == before.token_observations);
    assert(after.training_steps == before.training_steps);

    atp_graph_destroy(graph);
}

static void test_guard_validation(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "a b", "at://validation/1") == ATP_OK);

    atp_action_decision decision = {0};
    atp_action_decision_config config = atp_action_decision_default_config();

    config.guards.min_candidate_score = -0.1f;
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_ERR_INVALID_ARGUMENT);
    config = atp_action_decision_default_config();
    config.guards.min_support_score = 1.1f;
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_ERR_INVALID_ARGUMENT);
    config = atp_action_decision_default_config();
    config.guards.max_score_drop = NAN;
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_ERR_INVALID_ARGUMENT);
    config = atp_action_decision_default_config();
    config.guards.max_consecutive_occurrences = 0u;
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_ERR_INVALID_ARGUMENT);
    config = atp_action_decision_default_config();
    config.guards.max_consecutive_occurrences = ATPERSON_ACTION_MAX_CONSECUTIVE_OCCURRENCES + 1u;
    assert(atp_graph_action_decide(graph, "a", &config, &decision) == ATP_ERR_INVALID_ARGUMENT);

    assert(atp_graph_action_decide(NULL, "a", NULL, &decision) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_action_decide(graph, NULL, NULL, &decision) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_action_decide(graph, "a", NULL, NULL) == ATP_ERR_INVALID_ARGUMENT);

    atp_graph_destroy(graph);
}

int main(void) {
    test_empty_and_unknown_context_abstain_explicitly();
    test_low_support_abstains_with_threshold_evidence();
    test_score_drop_truncates_supported_prefix();
    test_self_loop_stops_as_repetition();
    test_two_token_cycle_stops_before_revisit();
    test_max_length_remains_explicit();
    test_dead_end_remains_explicit();
    test_repeated_calls_are_deterministic_and_read_only();
    test_guard_validation();
    puts("action decision tests passed");
    return 0;
}
