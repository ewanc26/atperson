#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *atp_strdup_local(const char *value) {
    if (!value) {
        return NULL;
    }
    const size_t len = strlen(value);
    char *copy = malloc(len + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len + 1u);
    return copy;
}

atp_graph_config atp_graph_default_config(void) {
    atp_graph_config config = {
        .seed = UINT64_C(0x4154504552534f4e),
        .learning_rate = 0.025f,
        .episode_capacity = ATPERSON_EPISODE_DEFAULT_CAPACITY,
    };
    return config;
}

atp_graph *atp_graph_create(const atp_graph_config *config) {
    atp_graph_config effective = config ? *config : atp_graph_default_config();
    if (effective.seed == 0u) {
        effective.seed = atp_graph_default_config().seed;
    }
    if (!(effective.learning_rate > 0.0f) || !isfinite(effective.learning_rate)) {
        effective.learning_rate = atp_graph_default_config().learning_rate;
    }
    if (effective.episode_capacity == 0u) {
        effective.episode_capacity = ATPERSON_EPISODE_DEFAULT_CAPACITY;
    }

    atp_graph *graph = calloc(1u, sizeof(*graph));
    if (!graph) {
        return NULL;
    }

    graph->config = effective;
    graph->episode_max = effective.episode_capacity;
    graph->rng_state = effective.seed;
    atp_network_init(graph);
    return graph;
}

void atp_graph_destroy(atp_graph *graph) {
    if (!graph) {
        return;
    }

    for (size_t i = 0; i < graph->node_count; ++i) {
        free(graph->nodes[i].token);
    }
    free(graph->nodes);
    free(graph->edges);
    free(graph->ledger_entries);
    free(graph->episodes);
    free(graph);
}

uint64_t atp_rng_next(atp_graph *graph) {
    uint64_t x = graph->rng_state;
    x ^= x >> 12u;
    x ^= x << 25u;
    x ^= x >> 27u;
    graph->rng_state = x;
    return x * UINT64_C(2685821657736338717);
}

float atp_rng_signed(atp_graph *graph) {
    const uint64_t value = atp_rng_next(graph) >> 40u;
    const float unit = (float)value / (float)UINT32_C(16777215);
    return (unit * 2.0f) - 1.0f;
}

uint64_t atp_hash_source(const char *source_id) {
    const unsigned char *cursor = (const unsigned char *)(source_id ? source_id : "");
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*cursor) {
        hash ^= (uint64_t)*cursor++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool atp_reserve_nodes(atp_graph *graph, size_t needed) {
    if (needed <= graph->node_capacity) {
        return true;
    }

    size_t capacity = graph->node_capacity ? graph->node_capacity : 32u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    atp_node *nodes = realloc(graph->nodes, capacity * sizeof(*nodes));
    if (!nodes) {
        return false;
    }

    graph->nodes = nodes;
    graph->node_capacity = capacity;
    return true;
}

bool atp_reserve_edges(atp_graph *graph, size_t needed) {
    if (needed <= graph->edge_capacity) {
        return true;
    }

    size_t capacity = graph->edge_capacity ? graph->edge_capacity : 64u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    atp_edge *edges = realloc(graph->edges, capacity * sizeof(*edges));
    if (!edges) {
        return false;
    }

    graph->edges = edges;
    graph->edge_capacity = capacity;
    return true;
}

bool atp_reserve_ledger_entries(atp_graph *graph, size_t needed) {
    if (needed <= graph->ledger_capacity) {
        return true;
    }

    size_t capacity = graph->ledger_capacity ? graph->ledger_capacity : 16u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    atp_ledger_entry *entries = realloc(graph->ledger_entries, capacity * sizeof(*entries));
    if (!entries) {
        return false;
    }

    graph->ledger_entries = entries;
    graph->ledger_capacity = capacity;
    return true;
}

bool atp_reserve_episodes(atp_graph *graph, size_t needed) {
    if (needed > graph->episode_max) {
        return false;
    }
    if (needed <= graph->episode_capacity) {
        return true;
    }
    size_t capacity = graph->episode_capacity ? graph->episode_capacity : 16u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    atp_episode *episodes = realloc(graph->episodes, capacity * sizeof(*episodes));
    if (!episodes) {
        return false;
    }
    graph->episodes = episodes;
    graph->episode_capacity = capacity;
    return true;
}

int32_t atp_find_node(const atp_graph *graph, const char *token) {
    if (!graph || !token) {
        return -1;
    }
    for (size_t i = 0; i < graph->node_count; ++i) {
        if (strcmp(graph->nodes[i].token, token) == 0) {
            if (i > INT32_MAX) {
                return -1;
            }
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t atp_intern_node(atp_graph *graph, const char *token) {
    const int32_t existing = atp_find_node(graph, token);
    if (existing >= 0) {
        graph->nodes[existing].observations++;
        return existing;
    }

    if (graph->node_count >= UINT32_MAX || !atp_reserve_nodes(graph, graph->node_count + 1u)) {
        return -1;
    }

    atp_node *node = &graph->nodes[graph->node_count];
    memset(node, 0, sizeof(*node));
    node->token = atp_strdup_local(token);
    if (!node->token) {
        return -1;
    }
    node->observations = 1u;
    for (size_t i = 0; i < ATPERSON_EMBEDDING_DIM; ++i) {
        node->embedding[i] = atp_rng_signed(graph) * 0.05f;
    }

    const int32_t index = (int32_t)graph->node_count;
    graph->node_count++;
    return index;
}

int32_t atp_find_edge(const atp_graph *graph, uint32_t source, uint32_t target) {
    for (size_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].source == source && graph->edges[i].target == target) {
            if (i > INT32_MAX) {
                return -1;
            }
            return (int32_t)i;
        }
    }
    return -1;
}

atp_status atp_observe_pair(atp_graph *graph, uint32_t source, uint32_t target,
                            uint64_t source_hash) {
    if (!graph || source >= graph->node_count || target >= graph->node_count) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    int32_t edge_index = atp_find_edge(graph, source, target);
    if (edge_index < 0) {
        if (!atp_reserve_edges(graph, graph->edge_count + 1u)) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        edge_index = (int32_t)graph->edge_count++;
        graph->edges[edge_index] = (atp_edge){
            .source = source,
            .target = target,
            .observations = 0u,
            .last_source_hash = source_hash,
            .strength = 0.0f,
        };
    }

    const float positive_loss = atp_network_train(graph, source, target, 1.0f);
    graph->loss_total += positive_loss;
    graph->training_steps++;

    if (graph->node_count > 2u) {
        uint32_t negative = 0u;
        for (unsigned attempt = 0; attempt < 8u; ++attempt) {
            negative = (uint32_t)(atp_rng_next(graph) % graph->node_count);
            if (negative != source && negative != target) {
                break;
            }
        }
        if (negative != source && negative != target) {
            const float negative_loss = atp_network_train(graph, source, negative, 0.0f);
            graph->loss_total += negative_loss;
            graph->training_steps++;
        }
    }

    atp_edge *edge = &graph->edges[edge_index];
    const float score = atp_network_score(graph, source, target);
    edge->strength = edge->observations == 0u ? score : (edge->strength * 0.90f + score * 0.10f);
    edge->observations++;
    edge->last_source_hash = source_hash;
    return ATP_OK;
}

static bool atp_is_token_byte(unsigned char byte) {
    return byte >= 0x80u || isalnum(byte) || byte == '\'' || byte == '-' || byte == '_';
}

static unsigned char atp_normalize_ascii(unsigned char byte) {
    return byte < 0x80u ? (unsigned char)tolower(byte) : byte;
}

atp_status atp_graph_observe_text(atp_graph *graph, const char *text, const char *source_id) {
    if (!graph || !text) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const uint64_t source_hash = atp_hash_source(source_id);
    char token[ATPERSON_TOKEN_BYTES];
    size_t token_len = 0u;
    int32_t previous = -1;
    bool saw_token = false;

    for (const unsigned char *cursor = (const unsigned char *)text;; ++cursor) {
        const unsigned char byte = *cursor;
        const bool token_byte = byte != '\0' && atp_is_token_byte(byte);

        if (token_byte) {
            if (token_len + 1u < sizeof(token)) {
                token[token_len++] = (char)atp_normalize_ascii(byte);
            }
        }

        if ((!token_byte || byte == '\0') && token_len > 0u) {
            token[token_len] = '\0';
            const int32_t current = atp_intern_node(graph, token);
            if (current < 0) {
                return ATP_ERR_OUT_OF_MEMORY;
            }

            graph->token_observations++;
            saw_token = true;
            if (previous >= 0) {
                const atp_status status =
                    atp_observe_pair(graph, (uint32_t)previous, (uint32_t)current, source_hash);
                if (status != ATP_OK) {
                    return status;
                }
            }
            previous = current;
            token_len = 0u;
        }

        if (byte == '\0') {
            break;
        }
    }

    if (saw_token) {
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

    const uint64_t source_hash = atp_hash_source(source_id);
    char token[ATPERSON_TOKEN_BYTES];
    size_t token_len = 0u;
    int32_t previous = -1;
    bool saw_token = false;
    const size_t nodes_before = graph->node_count;

    atp_token_count *counts = NULL;
    size_t distinct = 0u;
    size_t counts_capacity = 0u;

    for (const unsigned char *cursor = (const unsigned char *)text;; ++cursor) {
        const unsigned char byte = *cursor;
        const bool token_byte = byte != '\0' && atp_is_token_byte(byte);

        if (token_byte) {
            if (token_len + 1u < sizeof(token)) {
                token[token_len++] = (char)atp_normalize_ascii(byte);
            }
        }

        if ((!token_byte || byte == '\0') && token_len > 0u) {
            token[token_len] = '\0';
            const int32_t current = atp_intern_node(graph, token);
            if (current < 0) {
                free(counts);
                return ATP_ERR_OUT_OF_MEMORY;
            }
            graph->token_observations++;
            saw_token = true;
            if (previous >= 0) {
                const atp_status status =
                    atp_observe_pair(graph, (uint32_t)previous, (uint32_t)current, source_hash);
                if (status != ATP_OK) {
                    free(counts);
                    return status;
                }
            }
            previous = current;
            token_len = 0u;
            const atp_status bumped =
                atp_token_counts_bump(&counts, &distinct, &counts_capacity, (uint32_t)current);
            if (bumped != ATP_OK) {
                free(counts);
                return bumped;
            }
        }

        if (byte == '\0') {
            break;
        }
    }

    if (saw_token) {
        graph->observations++;
    }

    /* Count-based selection: new vocabulary or at least two distinct tokens.
     * No hidden thresholds on meaning, sentiment, or topic. */
    const bool remembered = graph->node_count > nodes_before || distinct >= 2u;
    if (!remembered) {
        free(counts);
        return ATP_OK;
    }
    if (out_remembered) {
        *out_remembered = true;
    }

    qsort(counts, distinct, sizeof(*counts), atp_token_count_compare);
    const atp_episode episode =
        atp_episode_build(counts, distinct, ledger_id, source_id, author_did, observed_at,
                          content_digest, schema_version);
    free(counts);

    if (graph->episode_count >= graph->episode_max) {
        atp_evict_episode(graph);
    }
    if (!atp_reserve_episodes(graph, graph->episode_count + 1u)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    graph->episodes[graph->episode_count++] = episode;
    return ATP_OK;
}

typedef struct atp_recall_match {
    size_t episode_index;
    float score;
    uint64_t observed_at;
    uint64_t ledger_id;
} atp_recall_match;

static int atp_recall_match_compare(const void *left, const void *right) {
    const atp_recall_match *a = left;
    const atp_recall_match *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->observed_at < b->observed_at) {
        return 1;
    }
    if (a->observed_at > b->observed_at) {
        return -1;
    }
    if (a->ledger_id < b->ledger_id) {
        return 1;
    }
    if (a->ledger_id > b->ledger_id) {
        return -1;
    }
    return 0;
}

atp_status atp_graph_recall(atp_graph *graph, const char *query, uint64_t at_epoch,
                            atp_episode *out, size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !query || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (capacity == 0u) {
        return ATP_OK;
    }
    if (graph->episode_count == 0u) {
        return ATP_OK;
    }

    /* Distinct query node indices; tokens unknown to the vocabulary cannot
     * match anything and do not mutate the graph. */
    uint32_t *query_nodes = NULL;
    size_t query_count = 0u;
    size_t query_capacity = 0u;
    char token[ATPERSON_TOKEN_BYTES];
    size_t token_len = 0u;
    for (const unsigned char *cursor = (const unsigned char *)query;; ++cursor) {
        const unsigned char byte = *cursor;
        const bool token_byte = byte != '\0' && atp_is_token_byte(byte);
        if (token_byte) {
            if (token_len + 1u < sizeof(token)) {
                token[token_len++] = (char)atp_normalize_ascii(byte);
            }
        }
        if ((!token_byte || byte == '\0') && token_len > 0u) {
            token[token_len] = '\0';
            token_len = 0u;
            const int32_t node = atp_find_node(graph, token);
            if (node >= 0) {
                bool seen = false;
                for (size_t i = 0u; i < query_count && !seen; ++i) {
                    seen = query_nodes[i] == (uint32_t)node;
                }
                if (!seen) {
                    if (query_count == query_capacity) {
                        const size_t next = query_capacity ? query_capacity * 2u : 8u;
                        if (next < query_count || next > SIZE_MAX / sizeof(*query_nodes)) {
                            free(query_nodes);
                            return ATP_ERR_OUT_OF_MEMORY;
                        }
                        uint32_t *grown = realloc(query_nodes, next * sizeof(*grown));
                        if (!grown) {
                            free(query_nodes);
                            return ATP_ERR_OUT_OF_MEMORY;
                        }
                        query_nodes = grown;
                        query_capacity = next;
                    }
                    query_nodes[query_count++] = (uint32_t)node;
                }
            }
        }
        if (byte == '\0') {
            break;
        }
    }

    if (query_count == 0u) {
        free(query_nodes);
        return ATP_OK;
    }

    atp_recall_match *matches = malloc(graph->episode_count * sizeof(*matches));
    if (!matches) {
        free(query_nodes);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    size_t match_count = 0u;
    for (size_t i = 0u; i < graph->episode_count; ++i) {
        const atp_episode *episode = &graph->episodes[i];
        float score = 0.0f;
        for (uint32_t s = 0u; s < episode->token_count; ++s) {
            const uint32_t node_index = episode->summary[s].node_index;
            if (node_index >= graph->node_count) {
                continue;
            }
            for (size_t q = 0u; q < query_count; ++q) {
                if (query_nodes[q] == node_index) {
                    score += episode->summary[s].weight;
                    break;
                }
            }
        }
        if (score > 0.0f) {
            matches[match_count++] = (atp_recall_match){
                .episode_index = i,
                .score = score,
                .observed_at = episode->observed_at,
                .ledger_id = episode->ledger_id,
            };
        }
    }
    free(query_nodes);

    qsort(matches, match_count, sizeof(*matches), atp_recall_match_compare);
    const size_t written = match_count < capacity ? match_count : capacity;
    for (size_t i = 0u; i < written; ++i) {
        atp_episode *episode = &graph->episodes[matches[i].episode_index];
        if (out) {
            out[i] = *episode;
        }
        episode->recall_count++;
        episode->last_recall_at = at_epoch;
    }
    free(matches);
    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}

size_t atp_graph_episode_count(const atp_graph *graph) {
    return graph ? graph->episode_count : 0u;
}

atp_status atp_graph_episode_at(const atp_graph *graph, size_t index, atp_episode *out_episode) {
    if (!graph || !out_episode) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->episode_count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_episode = graph->episodes[index];
    return ATP_OK;
}

atp_graph_stats atp_graph_get_stats(const atp_graph *graph) {
    if (!graph) {
        return (atp_graph_stats){0};
    }
    return (atp_graph_stats){
        .node_count = graph->node_count,
        .edge_count = graph->edge_count,
        .observations = graph->observations,
        .token_observations = graph->token_observations,
        .training_steps = graph->training_steps,
        .mean_loss =
            graph->training_steps ? graph->loss_total / (double)graph->training_steps : 0.0,
        .episode_count = graph->episode_count,
        .episode_capacity = graph->episode_max,
        .episode_evictions = graph->episode_evictions,
    };
}

typedef struct atp_ranked_edge {
    const atp_edge *edge;
    float score;
} atp_ranked_edge;

static int atp_ranked_edge_compare(const void *left, const void *right) {
    const atp_ranked_edge *a = left;
    const atp_ranked_edge *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    return 0;
}

static void atp_normalize_lookup(const char *input, char output[ATPERSON_TOKEN_BYTES]) {
    size_t len = 0u;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor && len + 1u < ATPERSON_TOKEN_BYTES; ++cursor) {
        if (atp_is_token_byte(*cursor)) {
            output[len++] = (char)atp_normalize_ascii(*cursor);
        }
    }
    output[len] = '\0';
}

atp_status atp_graph_associations(const atp_graph *graph, const char *token, atp_association *out,
                                  size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !token || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    char normalized[ATPERSON_TOKEN_BYTES];
    atp_normalize_lookup(token, normalized);
    const int32_t source = atp_find_node(graph, normalized);
    if (source < 0) {
        return ATP_ERR_NOT_FOUND;
    }

    size_t candidate_count = 0u;
    for (size_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].source == (uint32_t)source) {
            candidate_count++;
        }
    }
    if (candidate_count == 0u || capacity == 0u) {
        return ATP_OK;
    }

    atp_ranked_edge *ranked = malloc(candidate_count * sizeof(*ranked));
    if (!ranked) {
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t next = 0u;
    for (size_t i = 0; i < graph->edge_count; ++i) {
        const atp_edge *edge = &graph->edges[i];
        if (edge->source != (uint32_t)source) {
            continue;
        }
        const float neural = atp_network_score(graph, edge->source, edge->target);
        ranked[next++] = (atp_ranked_edge){
            .edge = edge,
            .score = edge->strength * 0.7f + neural * 0.3f,
        };
    }

    qsort(ranked, candidate_count, sizeof(*ranked), atp_ranked_edge_compare);
    const size_t written = candidate_count < capacity ? candidate_count : capacity;
    for (size_t i = 0; i < written; ++i) {
        const atp_edge *edge = ranked[i].edge;
        const atp_node *target = &graph->nodes[edge->target];
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].token, target->token, sizeof(out[i].token) - 1u);
        out[i].score = ranked[i].score;
        out[i].observations = edge->observations;
        out[i].last_source_hash = edge->last_source_hash;
    }

    free(ranked);
    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}

atp_status atp_graph_add_ledger_entry(atp_graph *graph, const atp_ledger_entry *entry) {
    if (!graph || !entry) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const size_t source_len = strlen(entry->source_id);
    if (source_len == 0u || source_len >= ATPERSON_LEDGER_SOURCE_BYTES) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const size_t author_len = strlen(entry->author_did);
    if (author_len >= ATPERSON_LEDGER_AUTHOR_BYTES) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (!atp_reserve_ledger_entries(graph, graph->ledger_count + 1u)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    graph->ledger_entries[graph->ledger_count] = *entry;
    graph->ledger_count++;
    return ATP_OK;
}

size_t atp_graph_ledger_count(const atp_graph *graph) {
    return graph ? graph->ledger_count : 0u;
}

atp_status atp_graph_ledger_entry(const atp_graph *graph, size_t index,
                                  atp_ledger_entry *out_entry) {
    if (!graph || !out_entry) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->ledger_count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_entry = graph->ledger_entries[index];
    return ATP_OK;
}

const char *atp_status_string(atp_status status) {
    switch (status) {
    case ATP_OK:
        return "ok";
    case ATP_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ATP_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case ATP_ERR_IO:
        return "I/O error";
    case ATP_ERR_FORMAT:
        return "invalid snapshot format";
    case ATP_ERR_NOT_FOUND:
        return "not found";
    }
    return "unknown error";
}
