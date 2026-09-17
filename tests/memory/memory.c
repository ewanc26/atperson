#include "atperson/core.h"
#include "atperson/memory.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
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

static const atp_recall_result *find_recall_result(const atp_recall_result *results, size_t count,
                                                   uint64_t ledger_id) {
    for (size_t i = 0u; i < count; ++i) {
        if (results[i].episode.ledger_id == ledger_id) {
            return &results[i];
        }
    }
    return NULL;
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
    assert(atp_graph_recall(graph, "alpha beta gamma delta", 100u, NULL, NULL, results, 3u, &count) == ATP_OK);
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
    assert(atp_graph_recall(graph, "alpha", 999u, NULL, NULL, results, 3u, &count) == ATP_OK);
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
    assert(atp_graph_recall(graph, "quux nope", 1000u, NULL, NULL, results, 3u, &count) == ATP_OK);
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
    assert(atp_graph_recall(graph, "only one", 5u, NULL, NULL, results, 2u, &count) == ATP_OK);
    assert(count == 1u);

    /* Unknown query tokens cannot match anything. */
    count = 0u;
    assert(atp_graph_recall(graph, "absent", 6u, NULL, NULL, results, 2u, &count) == ATP_OK);
    assert(count == 0u);
    assert(atp_graph_episode_count(graph) == 1u);

    atp_graph_destroy(graph);
}

static void test_recall_gate_config(void) {
    /* Default config must reproduce the eager behaviour exactly. */
    atp_graph *graph = graph_with_capacity(0u);
    bool remembered = false;
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://t/1", "did:plc:a", 10u, 3001u,
                                         ATPERSON_SCHEMA_VERSION, 1u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://t/2", "did:plc:a", 20u, 3002u,
                                         ATPERSON_SCHEMA_VERSION, 2u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "gamma delta", "at://t/3", "did:plc:a", 30u, 3003u,
                                         ATPERSON_SCHEMA_VERSION, 3u, &remembered) == ATP_OK);

    atp_episode results[3] = {0};
    size_t count = 0u;
    atp_recall_report report = {0};

    const atp_recall_config defaults = atp_recall_default_config();
    assert(defaults.min_overlap == 0.0f);
    assert(defaults.max_prefilter == 0u);
    assert(!defaults.disable);

    /* Gate off: NULL config and default config agree, byte-identically. */
    atp_episode null_results[3] = {0};
    size_t null_count = 0u;
    assert(atp_graph_recall(graph, "alpha", 50u, NULL, NULL, null_results, 3u, &null_count) ==
           ATP_OK);
    assert(atp_graph_recall(graph, "alpha", 50u, &defaults, &report, results, 3u, &count) ==
           ATP_OK);
    assert(count == null_count);
    for (size_t i = 0u; i < count; ++i) {
        assert(results[i].ledger_id == null_results[i].ledger_id);
    }
    assert(report.episodes_total == 3u);
    assert(report.episodes_scanned == 3u);
    assert(report.episodes_matched == 2u);
    assert(report.episodes_returned == 2u);
    assert(report.gate == ATP_RECALL_GATE_NONE);

    /* Disable: no results, named gate reason, no counter mutation. */
    atp_recall_config disabled = defaults;
    disabled.disable = true;
    count = 0u;
    memset(&report, 0, sizeof(report));
    assert(atp_graph_recall(graph, "alpha", 51u, &disabled, &report, results, 3u, &count) ==
           ATP_OK);
    assert(count == 0u);
    assert(report.gate == ATP_RECALL_GATE_DISABLED);
    assert(report.episodes_total == 3u);
    assert(report.episodes_scanned == 0u);
    atp_episode probe = {0};
    assert(atp_graph_episode_at(graph, 0u, &probe) == ATP_OK);
    /* Two eager calls above (NULL config + default config) each matched
     * episode 1; the disabled call must not have bumped anything. */
    assert(probe.recall_count == 2u);
    assert(probe.last_recall_at == 50u);

    /* Min-overlap gate excludes all matches: empty result, named reason. */
    atp_recall_config strict = defaults;
    strict.min_overlap = 100.0f;
    count = 0u;
    memset(&report, 0, sizeof(report));
    assert(atp_graph_recall(graph, "alpha", 52u, &strict, &report, results, 3u, &count) ==
           ATP_OK);
    assert(count == 0u);
    assert(report.gate == ATP_RECALL_GATE_MIN_OVERLAP);
    assert(report.episodes_matched == 0u);
    assert(report.episodes_returned == 0u);

    /* Min-overlap gate with partial matches: only strong episodes survive.
     * "alpha beta gamma" scores 2 in episodes 1 and 2, 1 in episode 3. */
    atp_recall_config partial = defaults;
    partial.min_overlap = 1.5f;
    count = 0u;
    memset(&report, 0, sizeof(report));
    assert(atp_graph_recall(graph, "alpha beta gamma", 53u, &partial, &report, results, 3u,
                            &count) == ATP_OK);
    assert(count == 2u);
    assert(results[0].ledger_id == 2u); /* recency breaks the 2-vs-2 tie */
    assert(results[1].ledger_id == 1u);
    assert(report.gate == ATP_RECALL_GATE_MIN_OVERLAP);
    assert(report.episodes_matched == 2u);
    assert(report.episodes_returned == 2u);

    /* Prefilter bound: only the most recent N episodes are scanned. */
    atp_recall_config bounded = defaults;
    bounded.max_prefilter = 1u;
    count = 0u;
    memset(&report, 0, sizeof(report));
    assert(atp_graph_recall(graph, "alpha", 54u, &bounded, &report, results, 3u, &count) ==
           ATP_OK);
    assert(count == 0u); /* episode 3 (gamma delta) is the only one scanned */
    assert(report.gate == ATP_RECALL_GATE_PREFILTER);
    assert(report.episodes_scanned == 1u);
    assert(report.episodes_total == 3u);

    /* Repeated calls with a fixed config are deterministic. */
    atp_episode first_pass[3] = {0};
    atp_episode second_pass[3] = {0};
    size_t first_count = 0u;
    size_t second_count = 0u;
    atp_recall_config repeatable = defaults;
    repeatable.min_overlap = 0.5f;
    assert(atp_graph_recall(graph, "alpha beta gamma", 55u, &repeatable, NULL, first_pass, 3u,
                            &first_count) == ATP_OK);
    assert(atp_graph_recall(graph, "alpha beta gamma", 55u, &repeatable, NULL, second_pass, 3u,
                            &second_count) == ATP_OK);
    assert(first_count == second_count);
    for (size_t i = 0u; i < first_count; ++i) {
        assert(first_pass[i].ledger_id == second_pass[i].ledger_id);
    }

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
    assert(atp_graph_recall(graph, "two", 50u, NULL, NULL, results, 1u, &count) == ATP_OK);
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
    assert(atp_graph_recall(graph, "round trip", 77u, NULL, NULL, results, 2u, &count) == ATP_OK);
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

static void test_ranked_recall_surfaces_semantic_memory_with_components(void) {
    const char *path = "atperson-ranked-memory-test.bin";
    atp_graph *graph = graph_with_capacity(8u);
    bool remembered = false;

    assert(atp_graph_observe_with_memory(graph, "moon silver", "at://semantic/1", "did:plc:a",
                                         100u, 4001u, ATPERSON_SCHEMA_VERSION, 1u,
                                         &remembered) == ATP_OK);
    assert(remembered);
    assert(atp_graph_observe_with_memory(graph, "silver fire", "at://semantic/2", "did:plc:b",
                                         200u, 4002u, ATPERSON_SCHEMA_VERSION, 2u,
                                         &remembered) == ATP_OK);
    assert(remembered);
    assert(atp_graph_observe_with_memory(graph, "stone river", "at://semantic/3", "did:plc:c",
                                         300u, 4003u, ATPERSON_SCHEMA_VERSION, 3u,
                                         &remembered) == ATP_OK);
    assert(remembered);

    /* Save before recall so the loaded graph starts from exactly the same
     * counters and learned association state as the live graph. */
    assert(atp_graph_save(graph, path) == ATP_OK);
    atp_status load_status = ATP_OK;
    atp_graph *loaded = atp_graph_load(path, &load_status);
    assert(loaded != NULL);
    assert(load_status == ATP_OK);

    const size_t nodes_before = atp_graph_get_stats(graph).node_count;
    atp_recall_result live[4] = {0};
    atp_recall_result restored[4] = {0};
    size_t live_count = 0u;
    size_t restored_count = 0u;
    assert(atp_graph_recall_ranked(graph, "moon", 86400u, live, 4u, &live_count) == ATP_OK);
    assert(atp_graph_recall_ranked(loaded, "moon", 86400u, restored, 4u,
                                   &restored_count) == ATP_OK);
    assert(live_count == 2u);
    assert(restored_count == live_count);

    for (size_t i = 0u; i < live_count; ++i) {
        assert(live[i].episode.ledger_id == restored[i].episode.ledger_id);
        assert(fabsf(live[i].score - restored[i].score) < 0.000001f);
        assert(fabsf(live[i].exact_score - restored[i].exact_score) < 0.000001f);
        assert(fabsf(live[i].association_score - restored[i].association_score) < 0.000001f);
        assert(fabsf(live[i].familiarity_score - restored[i].familiarity_score) < 0.000001f);
        assert(fabsf(live[i].recency_score - restored[i].recency_score) < 0.000001f);
        assert(fabsf(live[i].use_score - restored[i].use_score) < 0.000001f);
    }

    const atp_recall_result *exact = find_recall_result(live, live_count, 1u);
    const atp_recall_result *semantic = find_recall_result(live, live_count, 2u);
    assert(exact != NULL);
    assert(semantic != NULL);
    assert(exact->exact_score > 0.0f);
    assert(exact->exact_token_matches == 1u);
    assert(semantic->exact_score == 0.0f);
    assert(semantic->association_score > 0.0f);
    assert(semantic->association_token_matches >= 1u);
    assert(strcmp(semantic->episode.source_id, "at://semantic/2") == 0);
    assert(strcmp(semantic->episode.author_did, "did:plc:b") == 0);
    assert(find_recall_result(live, live_count, 3u) == NULL);

    for (size_t i = 0u; i < live_count; ++i) {
        const float recomposed =
            live[i].exact_score * ATPERSON_RECALL_EXACT_WEIGHT +
            live[i].association_score * ATPERSON_RECALL_ASSOCIATION_WEIGHT +
            live[i].familiarity_score * ATPERSON_RECALL_FAMILIARITY_WEIGHT +
            live[i].recency_score * ATPERSON_RECALL_RECENCY_WEIGHT +
            live[i].use_score * ATPERSON_RECALL_USE_WEIGHT;
        assert(fabsf(live[i].score - recomposed) < 0.000001f);
        assert(live[i].familiarity_score >= 0.0f && live[i].familiarity_score <= 1.0f);
        assert(live[i].recency_score >= 0.0f && live[i].recency_score <= 1.0f);
        assert(live[i].use_score >= 0.0f && live[i].use_score <= 1.0f);
    }

    /* Unknown query vocabulary stays unknown and cannot mutate learned state. */
    atp_recall_result unknown[2] = {0};
    size_t unknown_count = 99u;
    assert(atp_graph_recall_ranked(graph, "quux-nope", 90000u, unknown, 2u,
                                   &unknown_count) == ATP_OK);
    assert(unknown_count == 0u);
    assert(atp_graph_get_stats(graph).node_count == nodes_before);

    atp_graph_destroy(loaded);
    atp_graph_destroy(graph);
    remove(path);
}

static void test_ranked_recall_ties_and_use_are_deterministic(void) {
    atp_graph *graph = graph_with_capacity(8u);
    bool remembered = false;

    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://tie/1", "did:plc:a", 100u,
                                         5001u, ATPERSON_SCHEMA_VERSION, 1u,
                                         &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://tie/2", "did:plc:b", 100u,
                                         5002u, ATPERSON_SCHEMA_VERSION, 2u,
                                         &remembered) == ATP_OK);

    atp_recall_result first[2] = {0};
    atp_recall_result second[2] = {0};
    size_t first_count = 0u;
    size_t second_count = 0u;
    assert(atp_graph_recall_ranked(graph, "alpha beta", 200u, first, 2u,
                                   &first_count) == ATP_OK);
    assert(first_count == 2u);
    assert(first[0].episode.ledger_id == 2u);
    assert(first[1].episode.ledger_id == 1u);
    assert(first[0].use_score == 0.0f);
    assert(first[1].use_score == 0.0f);

    assert(atp_graph_recall_ranked(graph, "alpha beta", 200u, second, 2u,
                                   &second_count) == ATP_OK);
    assert(second_count == first_count);
    assert(second[0].episode.ledger_id == first[0].episode.ledger_id);
    assert(second[1].episode.ledger_id == first[1].episode.ledger_id);
    assert(second[0].use_score > first[0].use_score);
    assert(second[1].use_score > first[1].use_score);

    /* A zero-capacity probe is explicitly non-mutating. */
    size_t zero_count = 99u;
    atp_episode before = {0};
    atp_episode after = {0};
    assert(atp_graph_episode_at(graph, 0u, &before) == ATP_OK);
    assert(atp_graph_recall_ranked(graph, "alpha", 300u, NULL, 0u, &zero_count) == ATP_OK);
    assert(zero_count == 0u);
    assert(atp_graph_episode_at(graph, 0u, &after) == ATP_OK);
    assert(after.recall_count == before.recall_count);
    assert(after.last_recall_at == before.last_recall_at);

    atp_graph_destroy(graph);
}

static void test_episode_groups(void) {
    /* Empty graph: zero groups, recall report carries zero group counts. */
    atp_graph *graph = graph_with_capacity(0u);
    atp_episode_group groups[8] = {0};
    size_t group_count = 0u;
    assert(atp_graph_episode_groups(graph, groups, 8u, &group_count) == ATP_OK);
    assert(group_count == 0u);

    atp_recall_report report = {0};
    atp_episode results[4] = {0};
    size_t count = 0u;
    assert(atp_graph_recall(graph, "anything", 10u, NULL, &report, results, 4u, &count) ==
           ATP_OK);
    assert(count == 0u);
    assert(report.groups_total == 0u);
    assert(report.groups_scanned == 0u);

    /* Disjoint groups: distinct summary sets land in distinct groups. */
    bool remembered = false;
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://t/1", "did:plc:a", 10u, 5001u,
                                         ATPERSON_SCHEMA_VERSION, 1u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://t/2", "did:plc:a", 20u, 5002u,
                                         ATPERSON_SCHEMA_VERSION, 2u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "gamma delta", "at://t/3", "did:plc:a", 30u, 5003u,
                                         ATPERSON_SCHEMA_VERSION, 3u, &remembered) == ATP_OK);
    assert(atp_graph_episode_count(graph) == 3u);

    assert(atp_graph_episode_groups(graph, groups, 8u, &group_count) == ATP_OK);
    assert(group_count == 2u); /* {alpha,beta} twice, {gamma,delta} once */

    /* Members are listed in insertion order with ledger ids. */
    uint64_t members[4] = {0};
    size_t member_count = 0u;
    assert(atp_graph_episode_group_members(graph, 0u, members, 4u, &member_count) == ATP_OK);
    assert(member_count == 2u);
    assert(members[0] == 1u);
    assert(members[1] == 2u);
    assert(atp_graph_episode_group_members(graph, 1u, members, 4u, &member_count) == ATP_OK);
    assert(member_count == 1u);
    assert(members[0] == 3u);

    /* Out-of-range group id is NOT_FOUND. */
    assert(atp_graph_episode_group_members(graph, 9u, members, 4u, &member_count) ==
           ATP_ERR_NOT_FOUND);

    /* Group prefilter: a query overlapping only group 0 skips group 1. */
    memset(&report, 0, sizeof(report));
    count = 0u;
    assert(atp_graph_recall(graph, "alpha", 40u, NULL, &report, results, 4u, &count) == ATP_OK);
    assert(count == 2u);
    assert(report.groups_total == 2u);
    assert(report.groups_scanned == 1u);
    /* Results identical to the linear scan: both alpha-beta episodes. */
    assert(results[0].ledger_id == 2u);
    assert(results[1].ledger_id == 1u);

    /* A query overlapping nothing scans zero groups. */
    memset(&report, 0, sizeof(report));
    count = 0u;
    assert(atp_graph_recall(graph, "omega absent", 41u, NULL, &report, results, 4u, &count) ==
           ATP_OK);
    assert(count == 0u);
    assert(report.groups_total == 2u);
    assert(report.groups_scanned == 0u);

    atp_graph_destroy(graph);
}

static void test_episode_groups_snapshot_roundtrip_and_eviction(void) {
    /* Groups are derived state: a snapshot round-trip reproduces them. */
    atp_graph *graph = graph_with_capacity(3u);
    bool remembered = false;
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://t/1", "did:plc:a", 10u, 6001u,
                                         ATPERSON_SCHEMA_VERSION, 1u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "alpha beta", "at://t/2", "did:plc:a", 20u, 6002u,
                                         ATPERSON_SCHEMA_VERSION, 2u, &remembered) == ATP_OK);
    assert(atp_graph_observe_with_memory(graph, "gamma delta", "at://t/3", "did:plc:a", 30u, 6003u,
                                         ATPERSON_SCHEMA_VERSION, 3u, &remembered) == ATP_OK);

    char path[] = "atperson-groups-test.bin";
    remove(path);

    atp_graph *loaded = NULL;
    atp_status status = ATP_OK;
    assert(atp_graph_save(graph, path) == ATP_OK);
    loaded = atp_graph_load(path, &status);
    assert(loaded != NULL);
    assert(status == ATP_OK);

    atp_episode_group before[8] = {0};
    atp_episode_group after[8] = {0};
    size_t before_count = 0u;
    size_t after_count = 0u;
    assert(atp_graph_episode_groups(graph, before, 8u, &before_count) == ATP_OK);
    assert(atp_graph_episode_groups(loaded, after, 8u, &after_count) == ATP_OK);
    assert(before_count == after_count);
    assert(before_count == 2u);
    for (size_t g = 0u; g < before_count; ++g) {
        assert(before[g].key == after[g].key);
        assert(before[g].token_count == after[g].token_count);
        for (uint32_t t = 0u; t < before[g].token_count; ++t) {
            assert(before[g].tokens[t] == after[g].tokens[t]);
        }
    }

    /* Recall on the loaded graph returns the same ranked episodes. */
    atp_episode live_results[4] = {0};
    atp_episode loaded_results[4] = {0};
    size_t live_count = 0u;
    size_t loaded_count = 0u;
    assert(atp_graph_recall(graph, "alpha gamma", 50u, NULL, NULL, live_results, 4u,
                           &live_count) == ATP_OK);
    assert(atp_graph_recall(loaded, "alpha gamma", 50u, NULL, NULL, loaded_results, 4u,
                           &loaded_count) == ATP_OK);
    assert(live_count == loaded_count);
    for (size_t i = 0u; i < live_count; ++i) {
        assert(live_results[i].ledger_id == loaded_results[i].ledger_id);
    }

    /* Eviction with group membership: filling capacity evicts the
     * least-recalled episode and keeps membership consistent. */
    assert(atp_graph_observe_with_memory(loaded, "epsilon zeta", "at://t/4", "did:plc:a", 40u,
                                         6004u, ATPERSON_SCHEMA_VERSION, 4u, &remembered) ==
           ATP_OK);
    assert(atp_graph_episode_count(loaded) == 3u); /* one eviction happened */
    assert(atp_graph_episode_groups(loaded, after, 8u, &after_count) == ATP_OK);
    size_t total_members = 0u;
    for (size_t g = 0u; g < after_count; ++g) {
        uint64_t members[4] = {0};
        size_t member_count = 0u;
        assert(atp_graph_episode_group_members(loaded, (uint32_t)g, members, 4u, &member_count) ==
               ATP_OK);
        total_members += member_count;
    }
    assert(total_members == 3u); /* every episode in exactly one group */

    atp_graph_destroy(graph);
    atp_graph_destroy(loaded);
    remove(path);
}

int main(void) {
    test_selection_policy();
    test_recall_ordering_and_counters();
    test_recall_limit_and_empty();
    test_recall_gate_config();
    test_episode_groups();
    test_episode_groups_snapshot_roundtrip_and_eviction();
    test_eviction_prefers_least_recalled();
    test_snapshot_roundtrip();
    test_ranked_recall_surfaces_semantic_memory_with_components();
    test_ranked_recall_ties_and_use_are_deterministic();
    printf("memory tests passed\n");
    return 0;
}
