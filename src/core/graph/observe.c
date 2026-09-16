#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* Shared tokenizer state for the observe walks. emit returns false to stop
 * the scan on the first error, carried out through walk->status. */
typedef struct atp_observe_walk {
    atp_graph *graph;
    uint64_t source_hash;
    int32_t previous;
    bool saw_token;
    atp_status status;
} atp_observe_walk;

/* Dry-run walk (issue #9): counts the new nodes and edges a text would
 * create without mutating anything. Observations that would cross a
 * resource ceiling are rejected whole before any learning happens, so a
 * rejected observation never leaves half-learned state behind. */
typedef struct atp_budget_walk {
    const atp_graph *graph;
    size_t new_nodes;
    size_t new_edges;
    int32_t previous;
} atp_budget_walk;

static bool atp_budget_emit(void *userdata, const char *token) {
    atp_budget_walk *walk = userdata;
    const int32_t current = atp_find_node(walk->graph, token);
    const uint32_t current_index =
        current >= 0 ? (uint32_t)current : (uint32_t)walk->graph->node_count;
    if (current < 0) {
        walk->new_nodes++;
    }
    if (walk->previous >= 0) {
        const uint32_t previous_index = (uint32_t)walk->previous;
        bool edge_exists = false;
        if (current >= 0) {
            edge_exists = atp_find_edge(walk->graph, previous_index, current_index) >= 0;
        } else {
            /* New node: every pair touching it is a new edge. */
            edge_exists = false;
        }
        if (!edge_exists) {
            walk->new_edges++;
        }
    }
    walk->previous = (int32_t)current_index;
    return true;
}

/* Reject the observation if it would cross a configured ceiling. Returns
 * ATP_OK when it fits (or no ceiling is configured). */
static atp_status atp_check_budget(atp_graph *graph, const char *text) {
    if (graph->config.node_capacity_max == 0u && graph->config.edge_capacity_max == 0u) {
        return ATP_OK;
    }

    atp_budget_walk walk = {
        .graph = graph,
        .previous = -1,
    };
    atp_tokenize(text, ATPERSON_SCHEMA_VERSION, atp_budget_emit, &walk);

    if (graph->config.node_capacity_max != 0u &&
        graph->node_count + walk.new_nodes > graph->config.node_capacity_max) {
        graph->capacity_rejections++;
        return ATP_ERR_CAPACITY;
    }
    if (graph->config.edge_capacity_max != 0u &&
        graph->edge_count + walk.new_edges > graph->config.edge_capacity_max) {
        graph->capacity_rejections++;
        return ATP_ERR_CAPACITY;
    }
    return ATP_OK;
}

static bool atp_observe_emit(void *userdata, const char *token) {
    atp_observe_walk *walk = userdata;
    atp_status status = ATP_OK;
    const int32_t current = atp_intern_node_checked(walk->graph, token, &status);
    if (current < 0) {
        walk->status = status;
        return false;
    }
    walk->graph->token_observations++;
    walk->saw_token = true;
    if (walk->previous >= 0) {
        const atp_status status = atp_observe_pair(
            walk->graph, (uint32_t)walk->previous, (uint32_t)current, walk->source_hash);
        if (status != ATP_OK) {
            walk->status = status;
            return false;
        }
    }
    walk->previous = current;
    return true;
}

atp_status atp_graph_observe_text(atp_graph *graph, const char *text, const char *source_id) {
    if (!graph || !text) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    /* Whole-observation budget check: a text that would cross a ceiling
     * is rejected before any learning happens. */
    const atp_status budget = atp_check_budget(graph, text);
    if (budget != ATP_OK) {
        return budget;
    }

    const uint64_t source_hash = atp_hash_source(source_id);
    atp_observe_walk walk = {
        .graph = graph,
        .source_hash = source_hash,
        .previous = -1,
        .saw_token = false,
    };

    atp_tokenize(text, ATPERSON_SCHEMA_VERSION, atp_observe_emit, &walk);

    if (walk.status != ATP_OK) {
        return walk.status;
    }
    if (walk.saw_token) {
        graph->observations++;
    }
    return ATP_OK;
}

/* -------- episodic memory -------- */

typedef struct atp_token_count {
    uint32_t node_index;
    uint32_t count;
} atp_token_count;

static atp_status atp_token_counts_bump(atp_token_count **items_out, size_t *count,
                                        size_t *capacity, uint32_t node_index) {
    atp_token_count *items = *items_out;
    for (size_t i = 0u; i < *count; ++i) {
        if (items[i].node_index == node_index) {
            items[i].count++;
            return ATP_OK;
        }
    }
    if (*count == *capacity) {
        const size_t next = *capacity ? *capacity * 2u : 16u;
        if (next < *count || next > SIZE_MAX / sizeof(*items)) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        atp_token_count *grown = realloc(items, next * sizeof(*grown));
        if (!grown) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        items = grown;
        *items_out = items;
        *capacity = next;
    }
    items[*count] = (atp_token_count){.node_index = node_index, .count = 1u};
    (*count)++;
    return ATP_OK;
}

static int atp_token_count_compare(const void *left, const void *right) {
    const atp_token_count *a = left;
    const atp_token_count *b = right;
    if (a->count < b->count) {
        return 1;
    }
    if (a->count > b->count) {
        return -1;
    }
    if (a->node_index < b->node_index) {
        return -1;
    }
    if (a->node_index > b->node_index) {
        return 1;
    }
    return 0;
}

static atp_episode atp_episode_build(const atp_token_count *sorted, size_t distinct,
                                     uint64_t ledger_id, const char *source_id,
                                     const char *author_did, uint64_t observed_at,
                                     uint64_t content_digest, uint32_t schema_version) {
    atp_episode episode = {0};
    episode.ledger_id = ledger_id;
    episode.observed_at = observed_at;
    episode.content_digest = content_digest;
    episode.schema_version = schema_version;
    strncpy(episode.source_id, source_id ? source_id : "", sizeof(episode.source_id) - 1u);
    strncpy(episode.author_did, author_did ? author_did : "", sizeof(episode.author_did) - 1u);
    const size_t kept =
        distinct < ATPERSON_EPISODE_SUMMARY_SIZE ? distinct : ATPERSON_EPISODE_SUMMARY_SIZE;
    for (size_t i = 0u; i < kept; ++i) {
        episode.summary[i] = (atp_episode_token){
            .node_index = sorted[i].node_index,
            .weight = (float)sorted[i].count,
        };
    }
    episode.token_count = (uint32_t)kept;
    return episode;
}

/* Evict the least-recalled episode; ties go to the oldest, then the smallest
 * ledger id, so eviction is deterministic and inspectable. */
static void atp_evict_episode(atp_graph *graph) {
    size_t victim = 0u;
    for (size_t i = 1u; i < graph->episode_count; ++i) {
        const atp_episode *current = &graph->episodes[i];
        const atp_episode *best = &graph->episodes[victim];
        if (current->recall_count < best->recall_count ||
            (current->recall_count == best->recall_count &&
             current->observed_at < best->observed_at) ||
            (current->recall_count == best->recall_count &&
             current->observed_at == best->observed_at && current->ledger_id < best->ledger_id)) {
            victim = i;
        }
    }
    if (victim + 1u < graph->episode_count) {
        memmove(&graph->episodes[victim], &graph->episodes[victim + 1u],
                (graph->episode_count - victim - 1u) * sizeof(graph->episodes[0]));
    }
    graph->episode_count--;
    graph->episode_evictions++;
}

/* Episode-building variant of the observe walk: also counts distinct tokens
 * per observation for the episode's token histogram. */
typedef struct atp_memory_walk {
    atp_observe_walk base;
    atp_token_count *counts;
    size_t distinct;
    size_t counts_capacity;
} atp_memory_walk;

static bool atp_memory_emit(void *userdata, const char *token) {
    atp_memory_walk *walk = userdata;
    if (!atp_observe_emit(userdata, token)) {
        return false;
    }
    const atp_status bumped = atp_token_counts_bump(&walk->counts, &walk->distinct,
                                                    &walk->counts_capacity,
                                                    (uint32_t)walk->base.previous);
    if (bumped != ATP_OK) {
        walk->base.status = bumped;
        return false;
    }
    return true;
}

atp_status atp_graph_observe_with_memory(atp_graph *graph, const char *text, const char *source_id,
                                         const char *author_did, uint64_t observed_at,
                                         uint64_t content_digest, uint32_t schema_version,
                                         uint64_t ledger_id, bool *out_remembered) {
    if (out_remembered) {
        *out_remembered = false;
    }
    if (!graph || !text) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    /* Whole-observation budget check: a text that would cross a ceiling
     * is rejected before any learning happens. */
    const atp_status budget = atp_check_budget(graph, text);
    if (budget != ATP_OK) {
        return budget;
    }

    const uint64_t source_hash = atp_hash_source(source_id);
    const size_t nodes_before = graph->node_count;

    atp_memory_walk walk = {
        .base =
            {
                .graph = graph,
                .source_hash = source_hash,
                .previous = -1,
                .status = ATP_OK,
            },
    };

    /* Tokenization follows the entry's schema version so replay of a
     * schema-1 ledger reproduces schema-1 token identity exactly. */
    atp_tokenize(text, schema_version, atp_memory_emit, &walk);

    if (walk.base.status != ATP_OK) {
        free(walk.counts);
        return walk.base.status;
    }
    if (walk.base.saw_token) {
        graph->observations++;
    }

    /* Count-based selection: new vocabulary or at least two distinct tokens.
     * No hidden thresholds on meaning, sentiment, or topic. */
    const bool remembered = graph->node_count > nodes_before || walk.distinct >= 2u;
    if (!remembered) {
        free(walk.counts);
        return ATP_OK;
    }
    if (out_remembered) {
        *out_remembered = true;
    }

    qsort(walk.counts, walk.distinct, sizeof(*walk.counts), atp_token_count_compare);
    const atp_episode episode =
        atp_episode_build(walk.counts, walk.distinct, ledger_id, source_id, author_did, observed_at,
                          content_digest, schema_version);
    free(walk.counts);

    if (graph->episode_count >= graph->episode_max) {
        atp_evict_episode(graph);
    }
    if (!atp_reserve_episodes(graph, graph->episode_count + 1u)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    graph->episodes[graph->episode_count++] = episode;
    return ATP_OK;
}
