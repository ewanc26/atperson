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

    assert(atp_graph_observe_text(graph, "Moon light moon", "at://example/1") == ATP_OK);

    atp_graph_stats learned = atp_graph_get_stats(graph);
    assert(learned.observations == 1u);
    assert(learned.token_observations == 3u);
    assert(learned.node_count == 2u);
    assert(learned.edge_count == 2u);
    assert(learned.training_steps >= 2u);

    atp_association association[4] = {0};
    size_t association_count = 0u;
    assert(atp_graph_associations(graph, "MOON", association, 4u, &association_count) == ATP_OK);
    assert(association_count == 1u);
    assert(strcmp(association[0].token, "light") == 0);
    assert(association[0].observations == 1u);

    atp_ledger_entry mirror[2] = {0};
    strncpy(mirror[0].source_id, "at://example/1", sizeof(mirror[0].source_id) - 1u);
    strncpy(mirror[0].author_did, "did:plc:owner", sizeof(mirror[0].author_did) - 1u);
    mirror[0].id = 7u;
    mirror[0].observed_at = 1234u;
    mirror[0].content_digest = atp_ledger_digest("Moon light moon", 14u);
    mirror[0].schema_version = ATPERSON_SCHEMA_VERSION;
    mirror[0].outcome = ATP_LEDGER_OUTCOME_LEARNED;
    strncpy(mirror[1].source_id, "at://example/2", sizeof(mirror[1].source_id) - 1u);
    mirror[1].id = 8u;
    mirror[1].observed_at = 5678u;
    mirror[1].content_digest = 42u;
    mirror[1].schema_version = ATPERSON_SCHEMA_VERSION;
    mirror[1].outcome = ATP_LEDGER_OUTCOME_SKIPPED;
    assert(atp_graph_add_ledger_entry(graph, &mirror[0]) == ATP_OK);
    assert(atp_graph_add_ledger_entry(graph, &mirror[1]) == ATP_OK);
    assert(atp_graph_ledger_count(graph) == 2u);

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

    assert(atp_graph_ledger_count(graph) == 2u);
    atp_ledger_entry restored_mirror[2] = {0};
    assert(atp_graph_ledger_entry(graph, 0u, &restored_mirror[0]) == ATP_OK);
    assert(atp_graph_ledger_entry(graph, 1u, &restored_mirror[1]) == ATP_OK);
    assert(restored_mirror[0].id == mirror[0].id);
    assert(strcmp(restored_mirror[0].source_id, mirror[0].source_id) == 0);
    assert(strcmp(restored_mirror[0].author_did, mirror[0].author_did) == 0);
    assert(restored_mirror[0].observed_at == mirror[0].observed_at);
    assert(restored_mirror[0].content_digest == mirror[0].content_digest);
    assert(restored_mirror[0].schema_version == mirror[0].schema_version);
    assert(restored_mirror[0].outcome == mirror[0].outcome);
    assert(restored_mirror[1].id == mirror[1].id);
    assert(strcmp(restored_mirror[1].source_id, mirror[1].source_id) == 0);
    assert(restored_mirror[1].outcome == mirror[1].outcome);
    assert(ATPERSON_SNAPSHOT_VERSION == 5u);

    atp_graph_destroy(graph);
    remove(snapshot);
    return 0;
}
