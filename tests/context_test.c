#include "atperson/context.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void cleanup_files(void) {
    remove("atperson-context-test-ledger.bin");
    remove("atperson-context-test-ledger.bin.off");
    remove("atperson-context-test-ledger.bin.tmp");
    remove("atperson-context-test-ledger.bin.off.tmp");
}

static uint64_t append_observation(atp_ledger *ledger, const char *source, const char *author,
                                   uint64_t observed_at, const char *text) {
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    const size_t length = strlen(text);
    const atp_ledger_result result =
        atp_ledger_append(ledger, source, author, observed_at,
                          atp_ledger_digest(text, length), ATPERSON_SCHEMA_VERSION,
                          ATP_LEDGER_OUTCOME_LEARNED, text, length, &id, &status);
    assert(result == ATP_LEDGER_NEW);
    assert(status == ATP_OK);
    return id;
}

static atp_episode episode_by_ledger_id(const atp_graph *graph, uint64_t ledger_id) {
    for (size_t i = 0u; i < atp_graph_episode_count(graph); ++i) {
        atp_episode episode = {0};
        assert(atp_graph_episode_at(graph, i, &episode) == ATP_OK);
        if (episode.ledger_id == ledger_id) {
            return episode;
        }
    }
    assert(!"episode not found");
    return (atp_episode){0};
}

static const atp_context_item *find_kind(const atp_context_selection *selection,
                                         atp_context_item_kind kind) {
    for (size_t i = 0u; i < selection->item_count; ++i) {
        if (selection->items[i].kind == kind) {
            return &selection->items[i];
        }
    }
    return NULL;
}

static const atp_context_item *find_memory(const atp_context_selection *selection,
                                           uint64_t ledger_id) {
    for (size_t i = 0u; i < selection->item_count; ++i) {
        if (selection->items[i].kind == ATP_CONTEXT_ITEM_MEMORY &&
            selection->items[i].ledger_id == ledger_id) {
            return &selection->items[i];
        }
    }
    return NULL;
}

static void test_structured_context_uses_recall_and_interaction_state_read_only(void) {
    cleanup_files();

    atp_status status = ATP_OK;
    atp_ledger *ledger = atp_ledger_open("atperson-context-test-ledger.bin", &status);
    assert(ledger != NULL);
    assert(status == ATP_OK);

    const uint64_t cafe_id = append_observation(
        ledger, "at://did:plc:a/app.bsky.feed.post/cafe", "did:plc:a", 100u,
        "café weather");
    append_observation(ledger, "at://did:plc:a/app.bsky.feed.post/rain", "did:plc:a",
                       200u, "rain cloud");
    const uint64_t unrelated_id = append_observation(
        ledger, "at://did:plc:b/app.bsky.feed.post/stone", "did:plc:b", 150u,
        "unrelated stone");

    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    atp_replay_report report = {0};
    assert(atp_replay_ledger(ledger, graph, &report) == ATP_OK);
    assert(report.replayed == 3u);

    const atp_graph_stats before_stats = atp_graph_get_stats(graph);
    const atp_episode before_cafe = episode_by_ledger_id(graph, cafe_id);

    const atp_context_recent_input recent[] = {
        {.text = "latest note",
         .source_id = "at://did:plc:a/app.bsky.feed.post/recent",
         .author_did = "did:plc:a",
         .observed_at = 950u},
        {.text = "older note",
         .source_id = "at://did:plc:b/app.bsky.feed.post/old",
         .author_did = "did:plc:b",
         .observed_at = 100u},
    };
    const atp_context_request request = {
        .immediate_text = "CAFÉ question",
        .source_id = "at://did:plc:a/app.bsky.feed.post/cafe",
        .author_did = "did:plc:a",
        .recent = recent,
        .recent_count = 2u,
        .at_epoch = 1000u,
    };

    atp_context_selection selection = {0};
    assert(atp_graph_select_context(graph, &request, NULL, &selection) == ATP_OK);
    assert(selection.item_count >= 5u);
    assert(selection.recent_inputs_scored == 2u);
    assert(selection.memory_episodes_scanned == 3u);
    assert(!selection.memory_scan_truncated);

    const atp_context_item *immediate = find_kind(&selection, ATP_CONTEXT_ITEM_IMMEDIATE);
    assert(immediate != NULL);
    assert(immediate->reason == ATP_CONTEXT_REASON_IMMEDIATE_INPUT);
    assert(immediate->score == 1.0f);
    assert(immediate->available_tokens == 2u);
    assert(strcmp(immediate->author_did, "did:plc:a") == 0);

    const atp_context_item *memory = find_memory(&selection, cafe_id);
    assert(memory != NULL);
    assert(memory->reason == ATP_CONTEXT_REASON_EPISODIC_RECALL);
    assert(memory->recall.exact_token_matches > 0u);
    assert(strcmp(memory->source_id, "at://did:plc:a/app.bsky.feed.post/cafe") == 0);
    assert(find_memory(&selection, unrelated_id) == NULL);

    const atp_context_item *author = find_kind(&selection, ATP_CONTEXT_ITEM_AUTHOR_STATE);
    assert(author != NULL);
    assert(author->reason == ATP_CONTEXT_REASON_AUTHOR_FAMILIARITY);
    assert(author->interaction.encounter_count == 2u);
    assert(strcmp(author->interaction.identifier, "did:plc:a") == 0);

    const atp_context_item *source = find_kind(&selection, ATP_CONTEXT_ITEM_SOURCE_STATE);
    assert(source != NULL);
    assert(source->reason == ATP_CONTEXT_REASON_SOURCE_FAMILIARITY);
    assert(source->interaction.encounter_count == 1u);

    const atp_episode after_cafe = episode_by_ledger_id(graph, cafe_id);
    assert(after_cafe.recall_count == before_cafe.recall_count);
    assert(after_cafe.last_recall_at == before_cafe.last_recall_at);

    const atp_graph_stats after_stats = atp_graph_get_stats(graph);
    assert(after_stats.node_count == before_stats.node_count);
    assert(after_stats.edge_count == before_stats.edge_count);
    assert(after_stats.observations == before_stats.observations);
    assert(after_stats.training_steps == before_stats.training_steps);

    const atp_context_request unknown_request = {
        .immediate_text = "brandnewunknownword",
        .at_epoch = 1100u,
    };
    atp_context_selection unknown = {0};
    assert(atp_graph_select_context(graph, &unknown_request, NULL, &unknown) == ATP_OK);
    assert(unknown.item_count == 1u);
    assert(unknown.items[0].kind == ATP_CONTEXT_ITEM_IMMEDIATE);
    assert(atp_graph_get_stats(graph).node_count == before_stats.node_count);

    atp_graph_destroy(graph);
    atp_ledger_destroy(ledger);
    cleanup_files();
}

static void test_competing_recent_inputs_have_deterministic_ties(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    const atp_context_recent_input recent[] = {
        {.text = "same", .source_id = "at://b", .author_did = "did:plc:b", .observed_at = 50u},
        {.text = "same", .source_id = "at://a", .author_did = "did:plc:a", .observed_at = 50u},
    };
    const atp_context_request request = {
        .recent = recent,
        .recent_count = 2u,
        .at_epoch = 100u,
    };
    atp_context_config config = atp_context_default_config();
    config.max_items = 1u;
    config.max_recent_items = 2u;
    config.max_tokens = 4u;
    config.max_memory_items = 0u;
    config.max_memory_scan = 0u;

    atp_context_selection first = {0};
    atp_context_selection second = {0};
    assert(atp_graph_select_context(graph, &request, &config, &first) == ATP_OK);
    assert(atp_graph_select_context(graph, &request, &config, &second) == ATP_OK);
    assert(first.item_count == 1u);
    assert(second.item_count == 1u);
    assert(first.item_limit_reached);
    assert(second.item_limit_reached);
    assert(first.items[0].kind == ATP_CONTEXT_ITEM_RECENT);
    assert(first.items[0].input_index == 1u);
    assert(strcmp(first.items[0].source_id, "at://a") == 0);
    assert(memcmp(&first, &second, sizeof(first)) == 0);

    atp_graph_destroy(graph);
}

static void test_token_and_empty_context_budgets(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    atp_context_config config = atp_context_default_config();
    config.max_tokens = 1u;
    config.max_memory_items = 0u;
    config.max_memory_scan = 0u;

    const atp_context_request request = {
        .immediate_text = "one two three",
        .at_epoch = 10u,
    };
    atp_context_selection selection = {0};
    assert(atp_graph_select_context(graph, &request, &config, &selection) == ATP_OK);
    assert(selection.item_count == 1u);
    assert(selection.token_count == 1u);
    assert(selection.token_limit_reached);
    assert(selection.items[0].available_tokens == 3u);
    assert(selection.items[0].selected_tokens == 1u);
    assert(selection.items[0].truncated);

    const atp_context_request empty_request = {0};
    atp_context_selection empty = {0};
    assert(atp_graph_select_context(graph, &empty_request, NULL, &empty) == ATP_OK);
    assert(empty.item_count == 0u);
    assert(empty.token_count == 0u);

    atp_graph_destroy(graph);
}

static void test_memory_scan_limit_is_explicit(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    bool remembered = false;
    const char *texts[] = {"first apple", "second pear", "third plum"};
    for (size_t i = 0u; i < 3u; ++i) {
        const size_t length = strlen(texts[i]);
        assert(atp_graph_observe_with_memory(
                   graph, texts[i], i == 2u ? "at://third" : "at://older", "did:plc:test",
                   10u + (uint64_t)i, atp_ledger_digest(texts[i], length),
                   ATPERSON_SCHEMA_VERSION, (uint64_t)i + 1u, &remembered) == ATP_OK);
        assert(remembered);
    }

    atp_context_config config = atp_context_default_config();
    config.max_memory_scan = 1u;
    config.max_memory_items = 4u;
    const atp_context_request request = {
        .immediate_text = "third",
        .at_epoch = 100u,
    };
    const atp_episode before = episode_by_ledger_id(graph, 3u);

    atp_context_selection selection = {0};
    assert(atp_graph_select_context(graph, &request, &config, &selection) == ATP_OK);
    assert(selection.memory_episodes_scanned == 1u);
    assert(selection.memory_scan_truncated);
    assert(find_memory(&selection, 3u) != NULL);

    const atp_episode after = episode_by_ledger_id(graph, 3u);
    assert(after.recall_count == before.recall_count);
    assert(after.last_recall_at == before.last_recall_at);

    atp_graph_destroy(graph);
}

static void test_invalid_limits_are_rejected(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    const atp_context_request request = {.immediate_text = "hello"};
    atp_context_selection selection = {0};

    atp_context_config config = atp_context_default_config();
    config.max_items = 0u;
    assert(atp_graph_select_context(graph, &request, &config, &selection) ==
           ATP_ERR_INVALID_ARGUMENT);
    config = atp_context_default_config();
    config.max_tokens = ATPERSON_CONTEXT_MAX_TOKENS + 1u;
    assert(atp_graph_select_context(graph, &request, &config, &selection) ==
           ATP_ERR_INVALID_ARGUMENT);
    config = atp_context_default_config();
    config.max_memory_scan = ATPERSON_CONTEXT_MAX_MEMORY_SCAN + 1u;
    assert(atp_graph_select_context(graph, &request, &config, &selection) ==
           ATP_ERR_INVALID_ARGUMENT);

    atp_context_recent_input recent[ATPERSON_CONTEXT_MAX_RECENT_INPUTS + 1u] = {0};
    atp_context_request too_many = {
        .recent = recent,
        .recent_count = ATPERSON_CONTEXT_MAX_RECENT_INPUTS + 1u,
    };
    assert(atp_graph_select_context(graph, &too_many, NULL, &selection) ==
           ATP_ERR_INVALID_ARGUMENT);

    atp_graph_destroy(graph);
}

int main(void) {
    test_structured_context_uses_recall_and_interaction_state_read_only();
    test_competing_recent_inputs_have_deterministic_ties();
    test_token_and_empty_context_budgets();
    test_memory_scan_limit_is_explicit();
    test_invalid_limits_are_rejected();
    puts("planner context tests passed");
    return 0;
}
