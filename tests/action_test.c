#include "atperson/action.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const atp_action_candidate *find_candidate(const atp_action_candidate *items, size_t count,
                                                  const char *token) {
    for (size_t i = 0u; i < count; ++i) {
        if (strcmp(items[i].token, token) == 0) {
            return &items[i];
        }
    }
    return NULL;
}

static const atp_action_plan *find_plan(const atp_action_plan *plans, size_t count,
                                        const char *first_token) {
    for (size_t i = 0u; i < count; ++i) {
        if (plans[i].step_count > 0u && strcmp(plans[i].steps[0].token, first_token) == 0) {
            return &plans[i];
        }
    }
    return NULL;
}

static void assert_same_candidate(const atp_action_candidate *a, const atp_action_candidate *b) {
    assert(strcmp(a->token, b->token) == 0);
    assert(fabsf(a->score - b->score) < 0.000001f);
    assert(fabsf(a->association_score - b->association_score) < 0.000001f);
    assert(fabsf(a->familiarity_score - b->familiarity_score) < 0.000001f);
    assert(fabsf(a->support_score - b->support_score) < 0.000001f);
    assert(a->supporting_observations == b->supporting_observations);
    assert(a->context_matches == b->context_matches);
}

static void assert_same_plan(const atp_action_plan *a, const atp_action_plan *b) {
    assert(a->step_count == b->step_count);
    assert(a->stop_reason == b->stop_reason);
    assert(fabsf(a->score - b->score) < 0.000001f);
    for (size_t i = 0u; i < a->step_count; ++i) {
        assert_same_candidate(&a->steps[i], &b->steps[i]);
    }
}

static void test_ranked_candidates_are_inspectable_and_read_only(void) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 42u;
    config.familiarity_decay = 0.5f;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    assert(atp_graph_observe_text(graph, "alpha beta", "at://t/1") == ATP_OK);
    assert(atp_graph_observe_text(graph, "alpha beta", "at://t/2") == ATP_OK);
    assert(atp_graph_observe_text(graph, "delta beta", "at://t/3") == ATP_OK);
    assert(atp_graph_observe_text(graph, "alpha gamma", "at://t/4") == ATP_OK);

    const atp_graph_stats before = atp_graph_get_stats(graph);
    atp_action_candidate candidates[8] = {0};
    size_t count = 0u;
    assert(atp_graph_action_candidates(graph, "alpha delta", candidates, 8u, &count) == ATP_OK);
    assert(count == 2u);

    const atp_action_candidate *beta = find_candidate(candidates, count, "beta");
    const atp_action_candidate *gamma = find_candidate(candidates, count, "gamma");
    assert(beta != NULL);
    assert(gamma != NULL);

    assert(beta->context_matches == 2u);
    assert(beta->supporting_observations == 3u);
    assert(gamma->context_matches == 1u);
    assert(gamma->supporting_observations == 1u);
    assert(beta->familiarity_score > gamma->familiarity_score);
    assert(beta->support_score > gamma->support_score);

    for (size_t i = 0u; i < count; ++i) {
        const atp_action_candidate *candidate = &candidates[i];
        assert(candidate->association_score >= 0.0f && candidate->association_score <= 1.0f);
        assert(candidate->familiarity_score >= 0.0f && candidate->familiarity_score <= 1.0f);
        assert(candidate->support_score >= 0.0f && candidate->support_score <= 1.0f);
        const float expected = (candidate->association_score + candidate->familiarity_score +
                                candidate->support_score) /
                               3.0f;
        assert(fabsf(candidate->score - expected) < 0.0001f);
    }

    const atp_graph_stats after = atp_graph_get_stats(graph);
    assert(after.node_count == before.node_count);
    assert(after.edge_count == before.edge_count);
    assert(after.observations == before.observations);
    assert(after.token_observations == before.token_observations);
    assert(after.training_steps == before.training_steps);

    atp_graph_destroy(graph);
}

static void test_unknown_context_and_capacity(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha beta alpha gamma", "at://t/1") == ATP_OK);

    atp_action_candidate candidates[2] = {0};
    size_t count = 99u;
    assert(atp_graph_action_candidates(graph, "unknown", candidates, 2u, &count) == ATP_OK);
    assert(count == 0u);

    assert(atp_graph_action_candidates(graph, "alpha", candidates, 1u, &count) == ATP_OK);
    assert(count == 1u);
    assert(candidates[0].token[0] != '\0');

    count = 99u;
    assert(atp_graph_action_candidates(graph, "alpha", NULL, 0u, &count) == ATP_OK);
    assert(count == 0u);

    assert(atp_graph_action_candidates(NULL, "alpha", candidates, 1u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_action_candidates(graph, NULL, candidates, 1u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);

    atp_graph_destroy(graph);
}

static void test_planner_builds_bounded_inspectable_sequences(void) {
    atp_graph_config graph_config = atp_graph_default_config();
    graph_config.seed = 17u;
    graph_config.familiarity_decay = 0.5f;
    atp_graph *graph = atp_graph_create(&graph_config);
    assert(graph != NULL);

    assert(atp_graph_observe_text(graph, "start alpha end", "at://p/1") == ATP_OK);
    assert(atp_graph_observe_text(graph, "start alpha end", "at://p/2") == ATP_OK);
    assert(atp_graph_observe_text(graph, "start alpha end", "at://p/3") == ATP_OK);
    assert(atp_graph_observe_text(graph, "start beta finish", "at://p/4") == ATP_OK);
    assert(atp_graph_observe_text(graph, "start beta finish", "at://p/5") == ATP_OK);

    atp_action_plan_config planner = atp_action_plan_default_config();
    assert(planner.max_tokens > 0u && planner.max_tokens <= ATPERSON_PLAN_MAX_TOKENS);
    assert(planner.beam_width > 0u && planner.beam_width <= ATPERSON_PLAN_MAX_BEAM_WIDTH);
    planner.max_tokens = 4u;
    planner.beam_width = 4u;

    const atp_graph_stats before = atp_graph_get_stats(graph);
    atp_action_plan first[8] = {0};
    atp_action_plan second[8] = {0};
    size_t first_count = 0u;
    size_t second_count = 0u;
    assert(atp_graph_action_plans(graph, "start", &planner, first, 8u, &first_count) == ATP_OK);
    assert(atp_graph_action_plans(graph, "start", &planner, second, 8u, &second_count) == ATP_OK);
    assert(first_count == 2u);
    assert(second_count == first_count);

    for (size_t i = 0u; i < first_count; ++i) {
        assert_same_plan(&first[i], &second[i]);
    }

    const atp_action_plan *alpha = find_plan(first, first_count, "alpha");
    const atp_action_plan *beta = find_plan(first, first_count, "beta");
    assert(alpha != NULL);
    assert(beta != NULL);
    assert(alpha->step_count == 2u);
    assert(beta->step_count == 2u);
    assert(strcmp(alpha->steps[1].token, "end") == 0);
    assert(strcmp(beta->steps[1].token, "finish") == 0);
    assert(alpha->stop_reason == ATP_ACTION_PLAN_STOP_DEAD_END);
    assert(beta->stop_reason == ATP_ACTION_PLAN_STOP_DEAD_END);

    for (size_t i = 0u; i < first_count; ++i) {
        double score_sum = 0.0;
        for (size_t j = 0u; j < first[i].step_count; ++j) {
            score_sum += (double)first[i].steps[j].score;
            assert(first[i].steps[j].token[0] != '\0');
        }
        assert(fabs((double)first[i].score - score_sum / (double)first[i].step_count) < 0.000001);
    }

    atp_action_plan truncated[1] = {0};
    size_t truncated_count = 0u;
    assert(atp_graph_action_plans(graph, "start", &planner, truncated, 1u, &truncated_count) ==
           ATP_OK);
    assert(truncated_count == 1u);
    assert_same_plan(&truncated[0], &first[0]);

    const atp_graph_stats after = atp_graph_get_stats(graph);
    assert(after.node_count == before.node_count);
    assert(after.edge_count == before.edge_count);
    assert(after.observations == before.observations);
    assert(after.token_observations == before.token_observations);
    assert(after.training_steps == before.training_steps);

    atp_graph_destroy(graph);
}

static void test_planner_cycles_stop_at_maximum_depth(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "a b a", "at://cycle/1") == ATP_OK);

    atp_action_plan_config planner = {.max_tokens = 3u, .beam_width = 2u};
    atp_action_plan plans[2] = {0};
    size_t count = 0u;
    assert(atp_graph_action_plans(graph, "a", &planner, plans, 2u, &count) == ATP_OK);
    assert(count == 1u);
    assert(plans[0].step_count == 3u);
    assert(strcmp(plans[0].steps[0].token, "b") == 0);
    assert(strcmp(plans[0].steps[1].token, "a") == 0);
    assert(strcmp(plans[0].steps[2].token, "b") == 0);
    assert(plans[0].stop_reason == ATP_ACTION_PLAN_STOP_MAX_TOKENS);

    atp_graph_destroy(graph);
}

static void test_planner_unknown_context_and_limit_validation(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha beta", "at://limits/1") == ATP_OK);

    atp_action_plan plans[2] = {0};
    size_t count = 99u;
    atp_action_plan_config planner = atp_action_plan_default_config();

    assert(atp_graph_action_plans(graph, "unknown", &planner, plans, 2u, &count) == ATP_OK);
    assert(count == 0u);

    count = 99u;
    assert(atp_graph_action_plans(graph, "alpha", &planner, NULL, 0u, &count) == ATP_OK);
    assert(count == 0u);

    atp_action_plan_config invalid = planner;
    invalid.max_tokens = 0u;
    assert(atp_graph_action_plans(graph, "alpha", &invalid, plans, 2u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);
    invalid = planner;
    invalid.max_tokens = ATPERSON_PLAN_MAX_TOKENS + 1u;
    assert(atp_graph_action_plans(graph, "alpha", &invalid, plans, 2u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);
    invalid = planner;
    invalid.beam_width = 0u;
    assert(atp_graph_action_plans(graph, "alpha", &invalid, plans, 2u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);
    invalid = planner;
    invalid.beam_width = ATPERSON_PLAN_MAX_BEAM_WIDTH + 1u;
    assert(atp_graph_action_plans(graph, "alpha", &invalid, plans, 2u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);

    char long_context[ATPERSON_PLAN_MAX_CONTEXT_BYTES + 2u];
    memset(long_context, 'x', sizeof(long_context));
    long_context[sizeof(long_context) - 1u] = '\0';
    assert(atp_graph_action_plans(graph, long_context, &planner, plans, 2u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);

    assert(atp_graph_action_plans(NULL, "alpha", &planner, plans, 2u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_action_plans(graph, NULL, &planner, plans, 2u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_action_plans(graph, "alpha", &planner, NULL, 1u, &count) ==
           ATP_ERR_INVALID_ARGUMENT);

    atp_graph_destroy(graph);
}

int main(void) {
    test_ranked_candidates_are_inspectable_and_read_only();
    test_unknown_context_and_capacity();
    test_planner_builds_bounded_inspectable_sequences();
    test_planner_cycles_stop_at_maximum_depth();
    test_planner_unknown_context_and_limit_validation();
    printf("action model tests passed\n");
    return 0;
}
