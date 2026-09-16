#include "atperson/action.h"
#include "atperson/interaction.h"

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

static void cleanup_interaction_files(void) {
    remove("atperson-interaction-test-ledger.bin");
    remove("atperson-interaction-test-ledger.bin.off");
    remove("atperson-interaction-test-ledger.bin.tmp");
    remove("atperson-interaction-test-ledger.bin.off.tmp");
    remove("atperson-interaction-test-model.bin");
    remove("atperson-interaction-test-model.bin.tmp");
}

static uint64_t append_interaction_observation(atp_ledger *ledger, const char *source,
                                               const char *author, uint64_t observed_at,
                                               const char *text,
                                               atp_ledger_outcome outcome) {
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    const size_t text_len = strlen(text);
    const atp_ledger_result result =
        atp_ledger_append(ledger, source, author, observed_at,
                          atp_ledger_digest(text, text_len), ATPERSON_SCHEMA_VERSION,
                          outcome, text, text_len, &id, &status);
    assert(result == ATP_LEDGER_NEW);
    assert(status == ATP_OK);
    return id;
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

static void test_interaction_state_is_derived_persistent_and_withdrawable(void) {
    cleanup_interaction_files();

    atp_status status = ATP_OK;
    atp_ledger *ledger = atp_ledger_open("atperson-interaction-test-ledger.bin", &status);
    assert(ledger != NULL);
    assert(status == ATP_OK);

    append_interaction_observation(ledger, "at://did:plc:a/app.bsky.feed.post/one",
                                   "did:plc:a", 100u, "alpha beta",
                                   ATP_LEDGER_OUTCOME_LEARNED);
    append_interaction_observation(ledger, "at://did:plc:a/app.bsky.feed.post/two",
                                   "did:plc:a", 300u, "beta gamma",
                                   ATP_LEDGER_OUTCOME_LEARNED);
    append_interaction_observation(ledger, "at://did:plc:b/app.bsky.feed.post/one",
                                   "did:plc:b", 200u, "delta epsilon",
                                   ATP_LEDGER_OUTCOME_LEARNED);
    append_interaction_observation(ledger, "at://did:plc:a/app.bsky.feed.post/skipped",
                                   "did:plc:a", 400u, "ignored content",
                                   ATP_LEDGER_OUTCOME_SKIPPED);
    append_interaction_observation(ledger, "at://did:plc:a/app.bsky.feed.post/one",
                                   "did:plc:a", 500u, "alpha theta",
                                   ATP_LEDGER_OUTCOME_LEARNED);

    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    atp_replay_report report = {0};
    assert(atp_replay_ledger(ledger, graph, &report) == ATP_OK);
    assert(report.replayed == 4u);
    assert(report.mirrored == 1u);

    atp_interaction_state author_a = {0};
    assert(atp_graph_interaction_lookup(graph, ATP_INTERACTION_SUBJECT_AUTHOR,
                                        "did:plc:a", &author_a) == ATP_OK);
    assert(author_a.subject == ATP_INTERACTION_SUBJECT_AUTHOR);
    assert(strcmp(author_a.identifier, "did:plc:a") == 0);
    assert(author_a.encounter_count == 3u);
    assert(author_a.last_seen_at == 500u);
    assert(author_a.remembered_episode_count == 3u);
    assert(fabsf(author_a.familiarity - 0.75f) < 0.000001f);

    atp_interaction_state source_one = {0};
    assert(atp_graph_interaction_lookup(graph, ATP_INTERACTION_SUBJECT_SOURCE,
                                        "at://did:plc:a/app.bsky.feed.post/one",
                                        &source_one) == ATP_OK);
    assert(source_one.encounter_count == 2u);
    assert(source_one.last_seen_at == 500u);
    assert(source_one.remembered_episode_count == 2u);
    assert(fabsf(source_one.familiarity - (2.0f / 3.0f)) < 0.000001f);

    atp_interaction_state author_b = {0};
    assert(atp_graph_interaction_lookup(graph, ATP_INTERACTION_SUBJECT_AUTHOR,
                                        "did:plc:b", &author_b) == ATP_OK);
    assert(author_b.encounter_count == 1u);
    assert(author_b.last_seen_at == 200u);
    assert(author_b.remembered_episode_count == 1u);
    assert(fabsf(author_b.familiarity - 0.5f) < 0.000001f);

    /* Policy-skipped observations stay auditable but do not create learned
     * interaction state or inflate author familiarity. */
    atp_interaction_state skipped = {0};
    assert(atp_graph_interaction_lookup(graph, ATP_INTERACTION_SUBJECT_SOURCE,
                                        "at://did:plc:a/app.bsky.feed.post/skipped",
                                        &skipped) == ATP_ERR_NOT_FOUND);
    assert(skipped.encounter_count == 0u);

    const atp_graph_stats before = atp_graph_get_stats(graph);
    const size_t ledger_before = atp_graph_ledger_count(graph);
    const size_t episodes_before = atp_graph_episode_count(graph);
    atp_interaction_state unknown = {0};
    assert(atp_graph_interaction_lookup(graph, ATP_INTERACTION_SUBJECT_AUTHOR,
                                        "did:plc:unknown", &unknown) == ATP_ERR_NOT_FOUND);
    assert(atp_graph_interaction_lookup(graph, (atp_interaction_subject)99,
                                        "did:plc:a", &unknown) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_interaction_lookup(graph, ATP_INTERACTION_SUBJECT_AUTHOR,
                                        "", &unknown) == ATP_ERR_INVALID_ARGUMENT);
    const atp_graph_stats after = atp_graph_get_stats(graph);
    assert(after.node_count == before.node_count);
    assert(after.edge_count == before.edge_count);
    assert(after.observations == before.observations);
    assert(after.token_observations == before.token_observations);
    assert(after.training_steps == before.training_steps);
    assert(atp_graph_ledger_count(graph) == ledger_before);
    assert(atp_graph_episode_count(graph) == episodes_before);

    /* No new snapshot section is required: the state is derived from the
     * already-persisted ledger mirror and episodes. */
    assert(atp_graph_save(graph, "atperson-interaction-test-model.bin") == ATP_OK);
    atp_status load_status = ATP_OK;
    atp_graph *loaded = atp_graph_load("atperson-interaction-test-model.bin", &load_status);
    assert(loaded != NULL);
    assert(load_status == ATP_OK);
    atp_interaction_state loaded_author = {0};
    assert(atp_graph_interaction_lookup(loaded, ATP_INTERACTION_SUBJECT_AUTHOR,
                                        "did:plc:a", &loaded_author) == ATP_OK);
    assert(loaded_author.encounter_count == author_a.encounter_count);
    assert(loaded_author.last_seen_at == author_a.last_seen_at);
    assert(loaded_author.remembered_episode_count == author_a.remembered_episode_count);
    assert(fabsf(loaded_author.familiarity - author_a.familiarity) < 0.000001f);
    atp_graph_destroy(loaded);

    /* Withdrawal intentionally has no live in-memory effect. Rebuild is the
     * point where withdrawn experience disappears from every learned facet. */
    assert(atp_ledger_withdraw_source(ledger,
                                      "at://did:plc:a/app.bsky.feed.post/one") == 2u);
    atp_graph *rebuilt = atp_graph_create(NULL);
    assert(rebuilt != NULL);
    atp_replay_report withdrawn_report = {0};
    assert(atp_replay_ledger(ledger, rebuilt, &withdrawn_report) == ATP_OK);
    assert(withdrawn_report.replayed == 2u);
    assert(withdrawn_report.mirrored == 1u);
    assert(withdrawn_report.excluded_withdrawn == 2u);

    atp_interaction_state rebuilt_author = {0};
    assert(atp_graph_interaction_lookup(rebuilt, ATP_INTERACTION_SUBJECT_AUTHOR,
                                        "did:plc:a", &rebuilt_author) == ATP_OK);
    assert(rebuilt_author.encounter_count == 1u);
    assert(rebuilt_author.last_seen_at == 300u);
    assert(rebuilt_author.remembered_episode_count == 1u);
    assert(fabsf(rebuilt_author.familiarity - 0.5f) < 0.000001f);
    assert(atp_graph_interaction_lookup(rebuilt, ATP_INTERACTION_SUBJECT_SOURCE,
                                        "at://did:plc:a/app.bsky.feed.post/one",
                                        &unknown) == ATP_ERR_NOT_FOUND);

    atp_graph_destroy(rebuilt);
    atp_graph_destroy(graph);
    atp_ledger_destroy(ledger);
    cleanup_interaction_files();
}

int main(void) {
    test_ranked_candidates_are_inspectable_and_read_only();
    test_unknown_context_and_capacity();
    test_planner_builds_bounded_inspectable_sequences();
    test_planner_cycles_stop_at_maximum_depth();
    test_planner_unknown_context_and_limit_validation();
    test_interaction_state_is_derived_persistent_and_withdrawable();
    printf("action/state model tests passed\n");
    return 0;
}
