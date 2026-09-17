#include "atperson/core.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Issue #13 acceptance: empty start, explicit-event-only updates, reversal
 * under contrary evidence, persistence round-trip, and inspectable
 * provenance.
 */

static const float RATE = 0.25f; /* default valence_rate */

static void test_empty_start(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    /* Fresh graph: no valence records at all. */
    assert(atp_graph_valence_count(graph) == 0u);

    /* Observation alone never creates valence. */
    assert(atp_graph_observe_text(graph, "alpha beta", "at://t/1") == ATP_OK);
    assert(atp_graph_valence_count(graph) == 0u);

    atp_valence_state state;
    /* Known but never valued: NOT_FOUND, not a fake neutral record. */
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_ERR_NOT_FOUND);
    /* Unknown token: NOT_FOUND. */
    assert(atp_graph_valence(graph, "absent", &state) == ATP_ERR_NOT_FOUND);

    atp_graph_destroy(graph);
}

static void test_unknown_token_rejected(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    /* An event for a token the entity has never observed must not intern
     * vocabulary or create state: one event cannot create learned state. */
    assert(atp_graph_valence_event(graph, "stranger", ATP_VALENCE_INTERACTION, 1.0f, 100u,
                                   "at://e/1") == ATP_ERR_NOT_FOUND);
    assert(atp_graph_valence_count(graph) == 0u);

    /* Invalid arguments. */
    assert(atp_graph_valence_event(NULL, "x", ATP_VALENCE_ACTION, 1.0f, 1u, "s") ==
           ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_valence_event(graph, NULL, ATP_VALENCE_ACTION, 1.0f, 1u, "s") ==
           ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_valence_event(graph, "x", (atp_valence_kind)99, 1.0f, 1u, "s") ==
           ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_valence_event(graph, "x", ATP_VALENCE_ACTION, NAN, 1u, "s") ==
           ATP_ERR_INVALID_ARGUMENT);

    atp_graph_destroy(graph);
}

static void test_learning_and_bounds(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha", "at://t/1") == ATP_OK);

    /* First event from neutral: v = rate * signal. */
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, 1.0f, 10u, "at://e/1") ==
           ATP_OK);
    atp_valence_state state;
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    assert(fabsf(state.valence - RATE) < 0.0001f);
    assert(state.event_count == 1u);
    assert(state.positive_events == 1u);
    assert(state.negative_events == 0u);
    assert(state.last_event_at == 10u);
    assert(strcmp(state.token, "alpha") == 0);

    /* Second event: v = v + rate * (s - v). */
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_INTERACTION, 1.0f, 20u,
                                   "at://e/2") == ATP_OK);
    const float expected = RATE + RATE * (1.0f - RATE);
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    assert(fabsf(state.valence - expected) < 0.0001f);
    assert(state.event_count == 2u);
    assert(state.positive_events == 2u);

    /* Signal clamping to [-1, 1]. */
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, 50.0f, 30u, "at://e/3") ==
           ATP_OK);
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    assert(state.valence <= 1.0f);
    assert(fabsf(state.valence - (expected + RATE * (1.0f - expected))) < 0.0001f);

    atp_graph_destroy(graph);
}

static void test_reversal(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha", "at://t/1") == ATP_OK);

    /* Strong positive history. */
    for (int i = 0; i < 8; ++i) {
        assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, 1.0f, (uint64_t)i,
                                       "at://e/p") == ATP_OK);
    }
    atp_valence_state state;
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    /* EMA of 1.0 at rate 0.25 after 8 events: 1 - 0.75^8 = 0.8999. */
    assert(state.valence > 0.89f);

    /* Contrary evidence reverses the score across zero. */
    for (int i = 0; i < 12; ++i) {
        assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_INTERACTION, -1.0f,
                                       (uint64_t)(100 + i), "at://e/n") == ATP_OK);
    }
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    assert(state.valence < -0.9f);
    assert(state.positive_events == 8u);
    assert(state.negative_events == 12u);
    assert(state.event_count == 20u);

    /* Counters make the competition inspectable even after reversal. */
    assert(state.last_event_at == 111u);

    atp_graph_destroy(graph);
}

static void test_neutral_evidence(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha", "at://t/1") == ATP_OK);

    /* A zero signal is a recorded event that pulls toward neutral. */
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, 1.0f, 1u, "at://e/1") ==
           ATP_OK);
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_INTERACTION, 0.0f, 2u, "at://e/2") ==
           ATP_OK);
    atp_valence_state state;
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    assert(fabsf(state.valence - (RATE * (1.0f - RATE))) < 0.0001f);
    assert(state.event_count == 2u);
    assert(state.positive_events == 1u);
    assert(state.negative_events == 0u);

    atp_graph_destroy(graph);
}

static void test_configurable_rate(void) {
    atp_graph_config config = atp_graph_default_config();
    config.valence_rate = 1.0f; /* jump straight to the signal */
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha", "at://t/1") == ATP_OK);

    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, -0.5f, 1u, "at://e/1") ==
           ATP_OK);
    atp_valence_state state;
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    assert(fabsf(state.valence - (-0.5f)) < 0.0001f);

    /* Invalid rates fall back to the default. */
    config.valence_rate = 0.0f;
    atp_graph *fallback = atp_graph_create(&config);
    assert(fallback != NULL);
    assert(atp_graph_observe_text(fallback, "beta", "at://t/2") == ATP_OK);
    assert(atp_graph_valence_event(fallback, "beta", ATP_VALENCE_ACTION, 1.0f, 1u, "at://e/1") ==
           ATP_OK);
    assert(atp_graph_valence(fallback, "beta", &state) == ATP_OK);
    assert(fabsf(state.valence - RATE) < 0.0001f);
    atp_graph_destroy(fallback);

    atp_graph_destroy(graph);
}

static void test_persistence_roundtrip(void) {
    const char *path = "atperson-valence-test.bin";
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha beta", "at://t/1") == ATP_OK);
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, 1.0f, 10u, "at://e/1") ==
           ATP_OK);
    assert(atp_graph_valence_event(graph, "beta", ATP_VALENCE_INTERACTION, -0.5f, 20u,
                                   "at://e/2") == ATP_OK);
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_APPROACH, 0.5f, 30u, "at://e/3") ==
           ATP_OK);

    atp_valence_state before_alpha, before_beta;
    assert(atp_graph_valence(graph, "alpha", &before_alpha) == ATP_OK);
    assert(atp_graph_valence(graph, "beta", &before_beta) == ATP_OK);

    assert(atp_graph_save(graph, path) == ATP_OK);
    atp_graph_destroy(graph);

    atp_status status = ATP_OK;
    graph = atp_graph_load(path, &status);
    assert(graph != NULL);
    assert(status == ATP_OK);

    /* Folded state survives exactly. */
    atp_valence_state after_alpha, after_beta;
    assert(atp_graph_valence(graph, "alpha", &after_alpha) == ATP_OK);
    assert(atp_graph_valence(graph, "beta", &after_beta) == ATP_OK);
    assert(after_alpha.valence == before_alpha.valence);
    assert(after_alpha.event_count == before_alpha.event_count);
    assert(after_alpha.positive_events == before_alpha.positive_events);
    assert(after_alpha.negative_events == before_alpha.negative_events);
    assert(after_alpha.last_event_at == before_alpha.last_event_at);
    assert(after_beta.valence == before_beta.valence);
    assert(atp_graph_valence_count(graph) == 2u);

    /* Provenance log survives, newest first. */
    atp_valence_event log[3];
    size_t log_count = 0u;
    assert(atp_graph_valence_log(graph, log, 3u, &log_count) == ATP_OK);
    assert(log_count == 3u);
    assert(log[0].kind == ATP_VALENCE_APPROACH);
    assert(fabsf(log[0].signal - 0.5f) < 0.0001f);
    assert(log[0].at_epoch == 30u);
    assert(strcmp(log[0].token, "alpha") == 0);
    assert(strcmp(log[0].source_id, "at://e/3") == 0);
    assert(log[2].kind == ATP_VALENCE_ACTION);
    assert(strcmp(log[2].source_id, "at://e/1") == 0);

    /* Updates continue from the loaded state with the loaded rate. */
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, 1.0f, 40u, "at://e/4") ==
           ATP_OK);
    assert(atp_graph_valence(graph, "alpha", &after_alpha) == ATP_OK);
    assert(fabsf(after_alpha.valence -
                 (before_alpha.valence + RATE * (1.0f - before_alpha.valence))) < 0.0001f);

    atp_graph_destroy(graph);
    remove(path);
}

static void test_log_bounds_and_eviction(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha", "at://t/1") == ATP_OK);

    /* More events than the log capacity: oldest evicted, counted. */
    for (int i = 0; i < (int)ATPERSON_VALENCE_EVENT_CAPACITY + 10; ++i) {
        assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, 0.1f, (uint64_t)i,
                                       "at://e") == ATP_OK);
    }
    assert(atp_graph_valence_log_evictions(graph) == 10u);

    atp_valence_event log[4];
    size_t log_count = 0u;
    assert(atp_graph_valence_log(graph, log, 4u, &log_count) == ATP_OK);
    assert(log_count == 4u);
    /* Newest first: the last events issued. */
    assert(log[0].at_epoch == (uint64_t)(ATPERSON_VALENCE_EVENT_CAPACITY + 9));
    assert(log[3].at_epoch == (uint64_t)(ATPERSON_VALENCE_EVENT_CAPACITY + 6));

    /* The folded counters keep the full accounting (never evicted). */
    atp_valence_state state;
    assert(atp_graph_valence(graph, "alpha", &state) == ATP_OK);
    assert(state.event_count == (uint64_t)ATPERSON_VALENCE_EVENT_CAPACITY + 10u);
    assert(state.positive_events == state.event_count);

    /* Capacity query truncates, out_count reports the truth. */
    atp_valence_event one;
    assert(atp_graph_valence_log(graph, &one, 1u, &log_count) == ATP_OK);
    assert(log_count == 1u);

    atp_graph_destroy(graph);
}

static void test_valence_at(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha beta gamma", "at://t/1") == ATP_OK);
    assert(atp_graph_valence_event(graph, "gamma", ATP_VALENCE_ACTION, 0.5f, 1u, "at://e/1") ==
           ATP_OK);
    assert(atp_graph_valence_event(graph, "alpha", ATP_VALENCE_ACTION, -0.5f, 2u, "at://e/2") ==
           ATP_OK);

    /* Records enumerate in vocabulary node-index order: alpha was
     * interned before gamma, so it comes first regardless of which
     * token received its event first. */
    assert(atp_graph_valence_count(graph) == 2u);
    atp_valence_state state;
    assert(atp_graph_valence_at(graph, 0u, &state) == ATP_OK);
    assert(strcmp(state.token, "alpha") == 0);
    assert(atp_graph_valence_at(graph, 1u, &state) == ATP_OK);
    assert(strcmp(state.token, "gamma") == 0);
    assert(atp_graph_valence_at(graph, 2u, &state) == ATP_ERR_NOT_FOUND);
    assert(atp_graph_valence_at(NULL, 0u, &state) == ATP_ERR_INVALID_ARGUMENT);

    atp_graph_destroy(graph);
}

int main(void) {
    test_empty_start();
    test_unknown_token_rejected();
    test_learning_and_bounds();
    test_reversal();
    test_neutral_evidence();
    test_configurable_rate();
    test_persistence_roundtrip();
    test_log_bounds_and_eviction();
    test_valence_at();
    printf("valence tests passed\n");
    return 0;
}
