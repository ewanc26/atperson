#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 42u;

    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    atp_graph_stats initial = atp_graph_get_stats(graph);
    assert(initial.node_count == 0u);
    assert(initial.edge_count == 0u);

    assert(atp_graph_observe_text(graph, "Moon light moon", "at://example/1") ==
           ATP_OK);

    atp_graph_stats learned = atp_graph_get_stats(graph);
    assert(learned.observations == 1u);
    assert(learned.token_observations == 3u);
    assert(learned.node_count == 2u);
    assert(learned.edge_count == 2u);
    assert(learned.training_steps >= 2u);

    atp_association association[4] = {0};
    size_t association_count = 0u;
    assert(atp_graph_associations(graph, "MOON", association, 4u,
                                  &association_count) == ATP_OK);
    assert(association_count == 1u);
    assert(strcmp(association[0].token, "light") == 0);
    assert(association[0].observations == 1u);

    const char *snapshot = "atperson-core-test.bin";
    assert(atp_graph_save(graph, snapshot) == ATP_OK);
    atp_graph_destroy(graph);

    atp_status load_status = ATP_OK;
    graph = atp_graph_load(snapshot, &load_status);
    assert(graph != NULL);
    assert(load_status == ATP_OK);

    atp_graph_stats restored = atp_graph_get_stats(graph);
    assert(restored.node_count == learned.node_count);
    assert(restored.edge_count == learned.edge_count);
    assert(restored.training_steps == learned.training_steps);

    atp_graph_destroy(graph);
    remove(snapshot);
    return 0;
}
