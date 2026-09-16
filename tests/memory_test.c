#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Deterministic toy observations. Every token appears once per observation so
 * summary weights are 1 and score/ordering assertions are exact. */

static atp_graph *graph_with_capacity(size_t capacity) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 7u;
    config.episode_capacity = capacity;
    return atp_graph_create(&config);
}

static void set_fixed(atp_episode *episode, uint64_t ledger_id, uint64_t at, uint64_t digest,
                      const char *source) {
    memset(episode, 0, sizeof(*episode));
    episode->ledger_id = ledger_id;
    episode->observed_at = at;
    episode->content_digest = digest;
    episode->schema_version = ATPERSON_SCHEMA_VERSION;
    strncpy(episode->source_id, source, sizeof(episode->source_id) - 1u);
}

static void test_selection_policy(void) {
    atp_graph *graph = graph_with_capacity(0u);
    bool remembered = false;

    /* Novel vocabulary is remembered. */
    assert(atp_graph_observe_with_memory(graph, "one two three", "at://t/1", "did:plc:a", 10u,
                                         atp_ledger_digest("one two three", 13u),
                                         ATPERSON_SCHEMA_VERSION, 1u, &remembered) == ATP_OK);
    assert(remembered);
    assert(atp_graph_episode_count(graph) == 1u);

    /* A repeated single token introduces nothing, so it is not remembered. */
    assert(atp_graph_observe_with_memory(graph, "one", "at://t/2", "did:plc:a", 20u,
                                         atp_ledger_digest("one", 3u), ATPERSON_SCHEMA_VERSION, 2u,
                                         &remembered) == ATP_OK);
    assert(!remembered);
    assert(atp_graph_episode_count(graph) == 1u);

    /* At least two distinct tokens are remembered even without new vocabulary. */
    assert(atp_graph_observe_with_memory(graph, "one two", "at://t/3", "did:plc:a", 30u,
                                         atp_ledger_digest("one two", 7u), ATPERSON_SCHEMA_VERSION,
                                         3u, &remembered) == ATP_OK);
    assert(remembered);
    assert(atp_graph_episode_count(graph) == 2u);

    /* Empty text is never remembered. */
    assert(atp_graph_observe_with_memory(graph, "", "at://t/4", "did:plc:a", 40u,
                                         atp_ledger_digest("", 0u), ATPERSON_SCHEMA_VERSION, 4u,
                                         &remembered) == ATP_OK);
    assert(!remembered);
    assert(atp_graph_episode_count(graph) == 2u);

    atp_graph_destroy(graph);
}

static void test_recall_ordering_and_counters(void) {
    atp_graph *graph = graph_with_capacity(0u);
    bool remembered = false;

    assert(atp_graph_observe_with_memory(graph, "alpha beta gamma", "at://t/1", "did:plc:a", 10u,
                                         1001u, ATPERSON_SCHEMA_VERSION, 1u,
                                         &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://t/2", "did:plc:a", 20u, 1002u,
                                         ATPERSON_SCHEMA_VERSION, 2u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "gamma delta", "at://t/3", "did:plc:a", 30u, 1003u,
                                         ATPERSON_SCHEMA_VERSION, 3u, &remembered) == ATP_OK);
    assert(atp_graph_episode_count(graph) == 3u);
    const size_t nodes_before = atp_graph_get_stats(graph).node_count;

    /* alpha beta gamma delta: episode 1 scores 3, episodes 2 and 3 score 2.
     * Recency breaks the 2-vs-2 tie: episode 3 (t=30) before episode 2. */
    atp_episode results[3] = {0};
    size_t count = 0u;
    assert(atp_graph_recall(graph, "alpha beta gamma delta", 100u, results, 3u, &count) == ATP_OK);
    assert(count == 3u);
    assert(results[0].ledger_id == 1u);
    assert(results[1].ledger_id == 3u);
    assert(results[2].ledger_id == 2u);

    /* Recall is a use-based counter, and recall bumps it for the returned
     * set, newest recall time for all of them. */
    atp_episode first = {0};
    assert(atp_graph_episode_at(graph, 0u, &first) == ATP_OK);
    assert(first.recall_count == 1u);
    assert(first.last_recall_at == 100u);

    /* "alpha" scores 1 in both episode 2 (t=20) and episode 1 (t=10); recency
     * breaks the tie. The returned copies reflect the pre-increment state;
     * the stored episodes carry the incremented counters. */
    assert(atp_graph_recall(graph, "alpha", 999u, results, 3u, &count) == ATP_OK);
    assert(count == 2u);
    assert(results[0].ledger_id == 2u);
    assert(results[1].ledger_id == 1u);
    assert(results[0].recall_count == 1u);
    assert(results[1].recall_count == 1u);
    assert(results[0].last_recall_at == 100u);
    atp_episode stored = {0};
    assert(atp_graph_episode_at(graph, 0u, &stored) == ATP_OK);
    assert(stored.ledger_id == 1u);
    assert(stored.recall_count == 2u);
    assert(stored.last_recall_at == 999u);

    /* Query matching never mutates the vocabulary. */
    assert(atp_graph_recall(graph, "quux nope", 1000u, results, 3u, &count) == ATP_OK);
    assert(count == 0u);
    assert(atp_graph_get_stats(graph).node_count == nodes_before);

    atp_graph_destroy(graph);
}

static void test_recall_limit_and_empty(void) {
    atp_graph *graph = graph_with_capacity(0u);
    bool remembered = false;
    assert(atp_graph_observe_with_memory(graph, "only one", "at://t/1", "did:plc:a", 10u, 2001u,
                                         ATPERSON_SCHEMA_VERSION, 1u, &remembered) == ATP_OK);

    atp_episode results[2] = {0};
    size_t count = 0u;
    assert(atp_graph_recall(graph, "only one", 5u, results, 2u, &count) == ATP_OK);
    assert(count == 1u);

    /* Unknown query tokens cannot match anything. */
    count = 0u;
    assert(atp_graph_recall(graph, "absent", 6u, results, 2u, &count) == ATP_OK);
    assert(count == 0u);
    assert(atp_graph_episode_count(graph) == 1u);

    atp_graph_destroy(graph);
}

static void test_eviction_prefers_least_recalled(void) {
    atp_graph *graph = graph_with_capacity(3u);
    bool remembered = false;
    assert(atp_graph_observe_with_memory(graph, "one", "at://t/1", "did:plc:a", 10u, 1u,
                                         ATPERSON_SCHEMA_VERSION, 1u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "two", "at://t/2", "did:plc:a", 20u, 2u,
                                         ATPERSON_SCHEMA_VERSION, 2u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "three", "at://t/3", "did:plc:a", 30u, 3u,
                                         ATPERSON_SCHEMA_VERSION, 3u, &remembered) == ATP_OK);
    assert(atp_graph_episode_count(graph) == 3u);

    /* Bump the recall count of episode 2. */
    atp_episode results[1] = {0};
    size_t count = 0u;
    assert(atp_graph_recall(graph, "two", 50u, results, 1u, &count) == ATP_OK);
    assert(count == 1u);
    assert(results[0].ledger_id == 2u);

    /* Adding a fourth episode evicts the least-recalled (episode 1, never
     * recalled), not the most recent arrival. */
    assert(atp_graph_observe_with_memory(graph, "four four four four", "at://t/4", "did:plc:a", 40u,
                                         4u, ATPERSON_SCHEMA_VERSION, 4u, &remembered) == ATP_OK);
    assert(remembered);
    assert(atp_graph_episode_count(graph) == 3u);
    atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.episode_evictions == 1u);

    for (size_t i = 0u; i < atp_graph_episode_count(graph); ++i) {
        atp_episode episode = {0};
        assert(atp_graph_episode_at(graph, i, &episode) == ATP_OK);
        assert(strcmp(episode.source_id, "at://t/1") != 0);
    }

    atp_graph_destroy(graph);
}

static void test_snapshot_roundtrip(void) {
    const char *path = "atperson-memory-test.bin";
    atp_graph *graph = graph_with_capacity(5u);
    bool remembered = false;

    atp_episode expected[2] = {0};
    assert(atp_graph_observe_with_memory(graph, "round trip memory", "at://t/1", "did:plc:mem", 10u,
                                         3001u, ATPERSON_SCHEMA_VERSION, 1u,
                                         &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "round memory", "at://t/2", "did:plc:mem", 20u,
                                         3002u, ATPERSON_SCHEMA_VERSION, 2u,
                                         &remembered) == ATP_OK);

    atp_episode results[2] = {0};
    size_t count = 0u;
    assert(atp_graph_recall(graph, "round trip", 77u, results, 2u, &count) == ATP_OK);
    assert(count == 2u);
    assert(results[0].ledger_id == 1u);
    assert(results[1].ledger_id == 2u);

    /* Capture expected state after all mutations so the round-trip comparison
     * includes the recall counters. */
    assert(atp_graph_episode_at(graph, 0u, &expected[0]) == ATP_OK);
    assert(atp_graph_episode_at(graph, 1u, &expected[1]) == ATP_OK);
    assert(expected[0].recall_count == 1u);
    assert(expected[0].last_recall_at == 77u);

    assert(atp_graph_save(graph, path) == ATP_OK);
    atp_graph_destroy(graph);

    atp_status status = ATP_OK;
    graph = atp_graph_load(path, &status);
    assert(graph != NULL);
    assert(status == ATP_OK);

    assert(atp_graph_episode_count(graph) == 2u);
    for (size_t i = 0u; i < 2u; ++i) {
        atp_episode restored = {0};
        assert(atp_graph_episode_at(graph, i, &restored) == ATP_OK);
        assert(restored.ledger_id == expected[i].ledger_id);
        assert(restored.observed_at == expected[i].observed_at);
        assert(restored.content_digest == expected[i].content_digest);
        assert(restored.schema_version == expected[i].schema_version);
        assert(restored.recall_count == expected[i].recall_count);
        assert(restored.last_recall_at == expected[i].last_recall_at);
        assert(restored.token_count == expected[i].token_count);
        assert(strcmp(restored.source_id, expected[i].source_id) == 0);
        assert(strcmp(restored.author_did, expected[i].author_did) == 0);
        for (uint32_t t = 0u; t < restored.token_count; ++t) {
            assert(restored.summary[t].node_index == expected[i].summary[t].node_index);
            assert(restored.summary[t].weight == expected[i].summary[t].weight);
        }
    }
    assert(results[0].ledger_id == 1u);
    atp_episode recalled = {0};
    assert(atp_graph_episode_at(graph, 0u, &recalled) == ATP_OK);
    assert(recalled.recall_count == 1u);
    assert(recalled.last_recall_at == 77u);

    atp_graph_destroy(graph);
    remove(path);
}

int main(void) {
    test_selection_policy();
    test_recall_ordering_and_counters();
    test_recall_limit_and_empty();
    test_eviction_prefers_least_recalled();
    test_snapshot_roundtrip();
    printf("memory tests passed\n");
    return 0;
}