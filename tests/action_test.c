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

int main(void) {
    test_ranked_candidates_are_inspectable_and_read_only();
    test_unknown_context_and_capacity();
    printf("action model tests passed\n");
    return 0;
}
