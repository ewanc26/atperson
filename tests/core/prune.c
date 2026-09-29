#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static atp_graph *build(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    char text[128];
    for (int i = 0; i < 40; ++i) {
        snprintf(text, sizeof text, "common shared words rare%d unique%d", i, i);
        char id[32];
        snprintf(id, sizeof id, "at://t/%d", i);
        assert(atp_graph_observe_text(graph, text, id) == ATP_OK);
    }
    return graph;
}

static void test_noop_and_invalid(void) {
    atp_graph *graph = build();
    const atp_graph_stats before = atp_graph_get_stats(graph);
    atp_prune_report report;
    assert(atp_graph_prune_vocabulary(graph, before.node_count + 5u, &report) == ATP_OK);
    assert(report.nodes_before == before.node_count && report.nodes_after == before.node_count);
    assert(atp_graph_prune_vocabulary(graph, 0u, &report) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_prune_vocabulary(NULL, 10u, &report) == ATP_ERR_INVALID_ARGUMENT);
    atp_graph_destroy(graph);
}

static void test_prune_keeps_common_and_valence(void) {
    atp_graph *graph = build();
    assert(atp_graph_valence_event(graph, "rare7", ATP_VALENCE_INTERACTION, 1.0f, 100u,
                                   "at://e/1") == ATP_OK);
    const atp_graph_stats before = atp_graph_get_stats(graph);
    atp_prune_report report;
    assert(atp_graph_prune_vocabulary(graph, 12u, &report) == ATP_OK);
    const atp_graph_stats after = atp_graph_get_stats(graph);
    assert(after.node_count == 12u);
    assert(after.node_count == report.nodes_after);
    assert(after.edge_count < before.edge_count);
    assert(report.edges_after == after.edge_count);
    assert(after.episode_count <= before.episode_count);

    atp_valence_state state;
    assert(atp_graph_valence(graph, "rare7", &state) == ATP_OK);
    assert(atp_graph_valence_count(graph) == 1u);

    /* The graph stays usable: new observation reuses surviving vocabulary. */
    assert(atp_graph_observe_text(graph, "common shared words fresh", "at://t/new") == ATP_OK);

    char path[64];
    snprintf(path, sizeof path, "/tmp/atperson-prune-%ld.model", (long)getpid());
    assert(atp_graph_save(graph, path) == ATP_OK);
    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(path, &status);
    assert(loaded != NULL && status == ATP_OK);
    assert(atp_graph_get_stats(loaded).node_count == atp_graph_get_stats(graph).node_count);
    assert(atp_graph_get_stats(loaded).edge_count == atp_graph_get_stats(graph).edge_count);
    assert(atp_graph_valence(loaded, "rare7", &state) == ATP_OK);
    remove(path);
    atp_graph_destroy(loaded);
    atp_graph_destroy(graph);
}

static void test_protected_exceed_bound(void) {
    atp_graph *graph = build();
    assert(atp_graph_valence_event(graph, "common", ATP_VALENCE_INTERACTION, 1.0f, 1u, "at://e/1") == ATP_OK);
    assert(atp_graph_valence_event(graph, "shared", ATP_VALENCE_INTERACTION, 1.0f, 2u, "at://e/2") == ATP_OK);
    atp_prune_report report;
    assert(atp_graph_prune_vocabulary(graph, 1u, &report) == ATP_OK);
    assert(report.nodes_after >= 2u);
    atp_valence_state state;
    assert(atp_graph_valence(graph, "common", &state) == ATP_OK);
    assert(atp_graph_valence(graph, "shared", &state) == ATP_OK);
    atp_graph_destroy(graph);
}

int main(void) {
    test_noop_and_invalid();
    test_prune_keeps_common_and_valence();
    test_protected_exceed_bound();
    puts("prune: ok");
    return 0;
}
