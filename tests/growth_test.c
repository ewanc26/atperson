/*
 * Graph growth bounds tests (issue #9).
 *
 * Two suites:
 *   growth   — resource ceilings: whole-observation rejection with
 *              ATP_ERR_CAPACITY, no half-learned state, counters,
 *              snapshot roundtrip under ceilings.
 *   growth-pathology — one-off vocabulary bursts and high-degree hub
 *              nodes at scale, verifying index correctness.
 */

#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SNAPSHOT_PATH = "atperson-growth-test-snapshot.bin";

/* -------- ceilings -------- */

static void test_node_ceiling_whole_rejection(void) {
    atp_graph_config config = atp_graph_default_config();
    config.node_capacity_max = 10u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    /* Nine unique tokens fit. */
    for (unsigned i = 0u; i < 9u; ++i) {
        char text[32];
        snprintf(text, sizeof(text), "tok%u", i);
        assert(atp_graph_observe_text(graph, text, "at://g/1") == ATP_OK);
    }
    atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.node_count == 9u);
    assert(stats.capacity_rejections == 0u);

    /* Two new tokens in one observation would cross the ceiling of 10:
     * the whole observation is rejected, nothing is interned. */
    assert(atp_graph_observe_text(graph, "overflow1 overflow2", "at://g/2") == ATP_ERR_CAPACITY);
    stats = atp_graph_get_stats(graph);
    assert(stats.node_count == 9u);           /* no partial state */
    assert(stats.capacity_rejections == 1u);  /* counted once, not per token */
    assert(stats.observations == 9u);          /* rejected text not counted */

    /* One new token still fits. */
    assert(atp_graph_observe_text(graph, "tok9", "at://g/3") == ATP_OK);
    stats = atp_graph_get_stats(graph);
    assert(stats.node_count == 10u);

    /* At the ceiling: existing tokens observe fine, new ones reject. */
    assert(atp_graph_observe_text(graph, "tok0 tok1", "at://g/4") == ATP_OK);
    assert(atp_graph_observe_text(graph, "tok0 brandnew", "at://g/5") == ATP_ERR_CAPACITY);
    stats = atp_graph_get_stats(graph);
    assert(stats.node_count == 10u);
    assert(stats.capacity_rejections == 2u);

    atp_graph_destroy(graph);
}

static void test_edge_ceiling_whole_rejection(void) {
    atp_graph_config config = atp_graph_default_config();
    config.edge_capacity_max = 4u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    /* "a b" = 1 edge; "b c" = 1; "c d" = 1; "d a" = 1. Four edges. */
    assert(atp_graph_observe_text(graph, "a b", "at://g/1") == ATP_OK);
    assert(atp_graph_observe_text(graph, "b c", "at://g/2") == ATP_OK);
    assert(atp_graph_observe_text(graph, "c d", "at://g/3") == ATP_OK);
    assert(atp_graph_observe_text(graph, "d a", "at://g/4") == ATP_OK);
    atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.edge_count == 4u);

    /* "a c" would be edge 5: rejected whole. */
    assert(atp_graph_observe_text(graph, "a c", "at://g/5") == ATP_ERR_CAPACITY);
    stats = atp_graph_get_stats(graph);
    assert(stats.edge_count == 4u);
    assert(stats.capacity_rejections == 1u);

    /* Repeating an existing pair trains without a new edge: fits. */
    assert(atp_graph_observe_text(graph, "a b", "at://g/6") == ATP_OK);
    stats = atp_graph_get_stats(graph);
    assert(stats.edge_count == 4u);
    assert(stats.capacity_rejections == 1u);

    atp_graph_destroy(graph);
}

static void test_ceiling_snapshot_roundtrip(void) {
    atp_graph_config config = atp_graph_default_config();
    config.node_capacity_max = 100u;
    config.seed = 99u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    for (unsigned i = 0u; i < 80u; ++i) {
        char text[32];
        snprintf(text, sizeof(text), "node%u other%u", i, i % 8u);
        assert(atp_graph_observe_text(graph, text, "at://g/1") == ATP_OK);
    }
    atp_graph_stats before = atp_graph_get_stats(graph);
    assert(before.node_count == 88u);
    assert(before.capacity_rejections == 0u);

    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    atp_status status = ATP_OK;
    graph = atp_graph_load(SNAPSHOT_PATH, &status);
    assert(graph != NULL);
    assert(status == ATP_OK);
    remove(SNAPSHOT_PATH);

    /* Ceilings are deployment policy, not graph data: the loaded graph
     * starts unlimited. Reapply the budget. */
    atp_graph_set_capacity(graph, 100u, 0u);

    /* The loaded graph keeps the ceiling: new vocabulary still rejects.
     * 88 existing + 13 fresh crosses 100. */
    char burst[96];
    int off = 0;
    for (unsigned i = 0u; i < 13u; ++i) {
        off += snprintf(burst + off, sizeof(burst) - (size_t)off, " fresh%u", i);
    }
    assert(atp_graph_observe_text(graph, burst, "at://g/2") == ATP_ERR_CAPACITY);
    atp_graph_stats after = atp_graph_get_stats(graph);
    assert(after.node_count == 88u);
    assert(after.capacity_rejections == 1u);

    /* Existing vocabulary still observes. */
    assert(atp_graph_observe_text(graph, "node0 other0", "at://g/3") == ATP_OK);

    atp_graph_destroy(graph);
}

/* -------- pathology -------- */

static void test_one_off_vocabulary_burst(void) {
    /* Every token unique: pure insertion pressure. The index must stay
     * correct across growth rehashes. */
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    const unsigned count = 20000u;
    for (unsigned i = 0u; i < count; ++i) {
        char text[32];
        snprintf(text, sizeof(text), "unique%u", i);
        assert(atp_graph_observe_text(graph, text, "at://g/1") == ATP_OK);
    }
    atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.node_count == count);

    /* Every interned token must still be findable: associations resolve
     * without ATP_ERR_NOT_FOUND. */
    for (unsigned i = 0u; i < count; i += 997u) {
        char token[32];
        snprintf(token, sizeof(token), "unique%u", i);
        atp_association assoc[1];
        size_t found = 0u;
        assert(atp_graph_associations(graph, token, assoc, 1u, &found) == ATP_OK);
    }

    /* Unknown tokens stay unknown. */
    atp_association assoc[1];
    size_t found = 1u;
    assert(atp_graph_associations(graph, "unique999999", assoc, 1u, &found) == ATP_ERR_NOT_FOUND);
    assert(found == 0u);

    atp_graph_destroy(graph);
}

static void test_hub_node(void) {
    /* One token paired with thousands of others: high-degree node.
     * Correctness matters more than degree here; the association query
     * must return the hub's edges. */
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    const unsigned fanout = 5000u;
    for (unsigned i = 0u; i < fanout; ++i) {
        char text[32];
        snprintf(text, sizeof(text), "hub spoke%u", i);
        assert(atp_graph_observe_text(graph, text, "at://g/1") == ATP_OK);
    }
    atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.node_count == fanout + 1u);
    assert(stats.edge_count == fanout);

    /* The hub resolves and reports associations. */
    atp_association assoc[4];
    size_t found = 0u;
    assert(atp_graph_associations(graph, "hub", assoc, 4u, &found) == ATP_OK);
    assert(found == 4u);

    /* Edges are directional (hub -> spoke): the hub's fan-out is
     * queryable; spokes have no outgoing edges. */
    size_t spoke_found = 1u;
    assert(atp_graph_associations(graph, "spoke0", assoc, 4u, &spoke_found) == ATP_OK);
    assert(spoke_found == 0u);

    atp_graph_destroy(graph);
}

static void test_snapshot_rebuild_index_equivalence(void) {
    /* A saved-and-reloaded graph must answer lookups identically to the
     * in-memory graph: the index rebuild after load is exact. */
    atp_graph_config config = atp_graph_default_config();
    config.seed = 5u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    for (unsigned i = 0u; i < 500u; ++i) {
        char text[48];
        snprintf(text, sizeof(text), "alpha%u beta%u gamma%u", i % 40u, i % 25u, i % 60u);
        assert(atp_graph_observe_text(graph, text, "at://g/1") == ATP_OK);
    }
    atp_graph_stats before = atp_graph_get_stats(graph);
    assert(before.node_count == 125u);

    /* Collect association answers in memory. */
    atp_association mem[8];
    size_t mem_count = 0u;
    assert(atp_graph_associations(graph, "alpha0", mem, 8u, &mem_count) == ATP_OK);

    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    atp_status status = ATP_OK;
    graph = atp_graph_load(SNAPSHOT_PATH, &status);
    assert(graph != NULL && status == ATP_OK);
    remove(SNAPSHOT_PATH);

    atp_graph_stats after = atp_graph_get_stats(graph);
    assert(after.node_count == before.node_count);
    assert(after.edge_count == before.edge_count);

    atp_association loaded[8];
    size_t loaded_count = 0u;
    assert(atp_graph_associations(graph, "alpha0", loaded, 8u, &loaded_count) == ATP_OK);
    assert(loaded_count == mem_count);
    for (size_t i = 0u; i < mem_count; ++i) {
        assert(strcmp(mem[i].token, loaded[i].token) == 0);
    }

    atp_graph_destroy(graph);
}

static void test_ceiling_replay_equivalence(void) {
    /* Replay under a ceiling that admits every entry must produce the
     * same retained state as replay with no ceiling at all: the policy
     * is fail-closed, so an admitting ceiling changes nothing. */
    const char *LEDGER_PATH = "atperson-growth-test-ledger.bin";

    atp_status status = ATP_OK;
    atp_ledger *ledger = atp_ledger_open(LEDGER_PATH, &status);
    assert(ledger != NULL && status == ATP_OK);
    for (unsigned i = 0u; i < 50u; ++i) {
        char source[32];
        char text[32];
        snprintf(source, sizeof(source), "at://g/%u", i);
        snprintf(text, sizeof(text), "tok%u pair%u", i, i % 7u);
        uint64_t id = 0u;
        const size_t len = strlen(text);
        const atp_ledger_result result =
            atp_ledger_append(ledger, source, "did:plc:g", 100u,
                              atp_ledger_digest(text, len), ATPERSON_SCHEMA_VERSION,
                              ATP_LEDGER_OUTCOME_LEARNED, text, len, &id, &status);
        assert(result == ATP_LEDGER_NEW && status == ATP_OK);
    }

    atp_graph_config config = atp_graph_default_config();
    config.seed = 7u;
    atp_graph *plain = atp_graph_create(&config);
    atp_replay_report report = {0};
    assert(atp_replay_ledger(ledger, plain, &report) == ATP_OK);
    atp_graph_stats plain_stats = atp_graph_get_stats(plain);

    atp_graph *budgeted = atp_graph_create(&config);
    atp_graph_set_capacity(budgeted, 1000u, 1000u); /* admits everything */
    atp_replay_report budget_report = {0};
    assert(atp_replay_ledger(ledger, budgeted, &budget_report) == ATP_OK);
    atp_graph_stats budget_stats = atp_graph_get_stats(budgeted);

    assert(budget_stats.node_count == plain_stats.node_count);
    assert(budget_stats.edge_count == plain_stats.edge_count);
    assert(budget_stats.token_observations == plain_stats.token_observations);
    assert(budget_stats.capacity_rejections == 0u);

    /* A ceiling that would reject some entries fails the replay
     * loudly rather than silently dropping vocabulary. */
    atp_graph *tight = atp_graph_create(&config);
    atp_graph_set_capacity(tight, 5u, 5u);
    atp_replay_report tight_report = {0};
    assert(atp_replay_ledger(ledger, tight, &tight_report) != ATP_OK);
    atp_graph_stats tight_stats = atp_graph_get_stats(tight);
    assert(tight_stats.capacity_rejections > 0u);

    atp_graph_destroy(plain);
    atp_graph_destroy(budgeted);
    atp_graph_destroy(tight);
    atp_ledger_destroy(ledger);
    remove(LEDGER_PATH);
    remove("atperson-growth-test-ledger.bin.off");
    remove("atperson-growth-test-ledger.bin.off.tmp");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <suite>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "growth") == 0) {
        test_node_ceiling_whole_rejection();
        test_edge_ceiling_whole_rejection();
        test_ceiling_snapshot_roundtrip();
        test_ceiling_replay_equivalence();
        puts("growth: ok");
        return 0;
    }
    if (strcmp(argv[1], "growth-pathology") == 0) {
        test_one_off_vocabulary_burst();
        test_hub_node();
        test_snapshot_rebuild_index_equivalence();
        puts("growth-pathology: ok");
        return 0;
    }

    fprintf(stderr, "unknown suite %s\n", argv[1]);
    return 1;
}
