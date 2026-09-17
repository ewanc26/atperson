#include "atperson/core.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static const float ATP_FAMILIARITY_DECAY = 0.98f;

static void test_first_exposure(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha", "at://t/1") == ATP_OK);
    assert(atp_graph_familiarity(graph, "alpha") == 1.0f);
    assert(atp_graph_familiarity(graph, "absent") == 0.0f);
    atp_graph_destroy(graph);
}

static void test_repeated_exposure(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    /* familiarity = familiarity * decay + 1 on each exposure. */
    float expected = 0.0f;
    for (int i = 1; i <= 5; ++i) {
        assert(atp_graph_observe_text(graph, "alpha", "at://t/1") == ATP_OK);
        expected = expected * ATP_FAMILIARITY_DECAY + 1.0f;
        assert(fabsf(atp_graph_familiarity(graph, "alpha") - expected) < 0.0001f);
    }
    /* Without decay the score would be 5; with decay it stays bounded. */
    assert(atp_graph_familiarity(graph, "alpha") < 5.0f);
    assert(atp_graph_familiarity(graph, "alpha") > 1.0f);
    atp_graph_destroy(graph);
}

static void test_configurable_decay(void) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 9u;
    config.familiarity_decay = 0.5f;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    assert(atp_graph_observe_text(graph, "alpha alpha", "at://t/1") == ATP_OK);
    /* 1 exposure: 0.5 * 0 + 1 = 1; 2nd: 0.5 * 1 + 1 = 1.5. */
    assert(atp_graph_familiarity(graph, "alpha") == 1.5f);
    atp_graph_destroy(graph);
}

static void test_roundtrip(void) {
    const char *path = "atperson-familiarity-test.bin";
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    assert(atp_graph_observe_text(graph, "alpha beta", "at://t/1") == ATP_OK);
    for (int i = 0; i < 3; ++i) {
        assert(atp_graph_observe_text(graph, "alpha", "at://t/2") == ATP_OK);
    }
    const float alpha = atp_graph_familiarity(graph, "alpha");
    const float beta = atp_graph_familiarity(graph, "beta");
    assert(alpha > 1.0f);
    assert(beta == 1.0f);

    assert(atp_graph_save(graph, path) == ATP_OK);
    atp_graph_destroy(graph);

    atp_status status = ATP_OK;
    graph = atp_graph_load(path, &status);
    assert(graph != NULL);
    assert(status == ATP_OK);
    assert(ATPERSON_SNAPSHOT_VERSION == 5u);
    assert(fabsf(atp_graph_familiarity(graph, "alpha") - alpha) < 0.0001f);
    assert(fabsf(atp_graph_familiarity(graph, "beta") - beta) < 0.0001f);

    /* The decay used for future updates is loaded with the snapshot. */
    assert(atp_graph_observe_text(graph, "alpha", "at://t/3") == ATP_OK);
    const float expected = alpha * ATP_FAMILIARITY_DECAY + 1.0f;
    assert(fabsf(atp_graph_familiarity(graph, "alpha") - expected) < 0.0001f);

    atp_graph_destroy(graph);
    remove(path);
}

int main(void) {
    test_first_exposure();
    test_repeated_exposure();
    test_configurable_decay();
    test_roundtrip();
    printf("familiarity tests passed\n");
    return 0;
}