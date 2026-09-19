#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_plasticity_control(void) {
    /* 1. Default config: plasticity control is disabled. */
    atp_graph_config config = atp_graph_default_config();
    assert(config.enable_plasticity_control == false);
    config.seed = 100u;

    atp_graph *g_off = atp_graph_create(&config);
    assert(g_off != NULL);
    assert(atp_graph_observe_text(g_off, "alpha beta gamma", "at://1") == ATP_OK);

    atp_plasticity_report report_off = {0};
    assert(atp_graph_plasticity_report(g_off, &report_off) == ATP_OK);
    assert(report_off.steps_total == 0u);
    assert(report_off.steps_protected == 0u);
    assert(report_off.parameters_protected == 0u);
    atp_graph_destroy(g_off);

    /* 2. Plasticity control enabled: repeated training accumulates importance. */
    config.enable_plasticity_control = true;
    config.plasticity_threshold = 0.05f;
    config.plasticity_scale = 0.0f; /* full protection when threshold exceeded */

    atp_graph *g_on = atp_graph_create(&config);
    assert(g_on != NULL);

    for (int i = 0; i < 25; ++i) {
        assert(atp_graph_observe_text(g_on, "alpha beta gamma delta", "at://batch") == ATP_OK);
    }

    atp_plasticity_report report_on = {0};
    assert(atp_graph_plasticity_report(g_on, &report_on) == ATP_OK);
    assert(report_on.steps_total > 0u);
    assert(report_on.steps_protected > 0u);
    assert(report_on.parameters_protected > 0u);

    atp_association assoc_before[4] = {0};
    size_t assoc_count = 0u;
    assert(atp_graph_associations(g_on, "alpha", assoc_before, 4u, &assoc_count) == ATP_OK);
    assert(assoc_count > 0u);
    float initial_beta_score = assoc_before[0].score;

    /* Observe a new conflicting sequence "alpha zebra". Protected parameters scale down update. */
    for (int i = 0; i < 20; ++i) {
        assert(atp_graph_observe_text(g_on, "alpha zebra", "at://conflicting") == ATP_OK);
    }

    atp_association assoc_after[4] = {0};
    assert(atp_graph_associations(g_on, "alpha", assoc_after, 4u, &assoc_count) == ATP_OK);
    assert(assoc_count > 0u);

    /* 3. Rebuild/repeat determinism test. */
    atp_graph *g_replay = atp_graph_create(&config);
    for (int i = 0; i < 25; ++i) {
        assert(atp_graph_observe_text(g_replay, "alpha beta gamma delta", "at://batch") == ATP_OK);
    }
    for (int i = 0; i < 20; ++i) {
        assert(atp_graph_observe_text(g_replay, "alpha zebra", "at://conflicting") == ATP_OK);
    }

    atp_plasticity_report report_replay = {0};
    assert(atp_graph_plasticity_report(g_replay, &report_replay) == ATP_OK);
    atp_plasticity_report report_final = {0};
    assert(atp_graph_plasticity_report(g_on, &report_final) == ATP_OK);

    assert(report_replay.steps_total == report_final.steps_total);
    assert(report_replay.steps_protected == report_final.steps_protected);
    assert(report_replay.parameters_protected == report_final.parameters_protected);

    atp_graph_destroy(g_on);
    atp_graph_destroy(g_replay);
}

int main(void) {
    test_plasticity_control();

    atp_graph_config config = atp_graph_default_config();
    config.seed = 42u;

    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    atp_graph_stats initial = atp_graph_get_stats(graph);
    assert(initial.node_count == 0u);
    assert(initial.edge_count == 0u);

    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    assert(legacy.version == ATPERSON_NEURAL_ARCHITECTURE_VERSION);
    assert(legacy.embedding_dim == 16u);
    assert(legacy.input_dim == 32u);
    assert(legacy.hidden_layer_count == 1u);
    assert(legacy.hidden_widths[0] == 16u);
    assert(legacy.output_dim == 1u);

    atp_neural_architecture active = {0};
    assert(atp_graph_neural_architecture(graph, &active) == ATP_OK);
    assert(memcmp(&active, &legacy, sizeof(active)) == 0);

    atp_neural_architecture_report neural = {0};
    assert(atp_graph_neural_report(graph, &neural) == ATP_OK);
    assert(neural.shared_parameter_count == 545u);
    assert(neural.shared_parameter_bytes == 545u * sizeof(float));
    assert(neural.shared_learned_state_bytes == 1090u * sizeof(float));
    assert(neural.per_node_embedding_bytes == 16u * sizeof(float));
    assert(neural.per_node_learned_state_bytes == 32u * sizeof(float));

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
    assert(ATPERSON_SNAPSHOT_VERSION == 7u);
    assert(ATPERSON_SNAPSHOT_VERSION_V6 == 6u);

    atp_graph_destroy(graph);
    remove(snapshot);
    return 0;
}
