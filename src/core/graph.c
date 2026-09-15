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
    };
    return config;
}

atp_graph *atp_graph_create(const atp_graph_config *config) {
    atp_graph_config effective = config ? *config : atp_graph_default_config();
    if (effective.seed == 0u) {
        effective.seed = atp_graph_default_config().seed;
    }
    if (!(effective.learning_rate > 0.0f) ||
        !isfinite(effective.learning_rate)) {
        effective.learning_rate = atp_graph_default_config().learning_rate;
    }

    atp_graph *graph = calloc(1u, sizeof(*graph));
    if (!graph) {
        return NULL;
    }

    graph->config = effective;
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
    const unsigned char *cursor =
        (const unsigned char *)(source_id ? source_id : "");
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

    if (graph->node_count >= UINT32_MAX ||
        !atp_reserve_nodes(graph, graph->node_count + 1u)) {
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

int32_t atp_find_edge(const atp_graph *graph, uint32_t source,
                      uint32_t target) {
    for (size_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].source == source &&
            graph->edges[i].target == target) {
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
            const float negative_loss =
                atp_network_train(graph, source, negative, 0.0f);
            graph->loss_total += negative_loss;
            graph->training_steps++;
        }
    }

    atp_edge *edge = &graph->edges[edge_index];
    const float score = atp_network_score(graph, source, target);
    edge->strength =
        edge->observations == 0u ? score : (edge->strength * 0.90f + score * 0.10f);
    edge->observations++;
    edge->last_source_hash = source_hash;
    return ATP_OK;
}

static bool atp_is_token_byte(unsigned char byte) {
    return byte >= 0x80u || isalnum(byte) || byte == '\'' || byte == '-' ||
           byte == '_';
}

static unsigned char atp_normalize_ascii(unsigned char byte) {
    return byte < 0x80u ? (unsigned char)tolower(byte) : byte;
}

atp_status atp_graph_observe_text(atp_graph *graph, const char *text,
                                  const char *source_id) {
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
                    atp_observe_pair(graph, (uint32_t)previous,
                                     (uint32_t)current, source_hash);
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
        .mean_loss = graph->training_steps
                         ? graph->loss_total / (double)graph->training_steps
                         : 0.0,
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

static void atp_normalize_lookup(const char *input,
                                 char output[ATPERSON_TOKEN_BYTES]) {
    size_t len = 0u;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor && len + 1u < ATPERSON_TOKEN_BYTES; ++cursor) {
        if (atp_is_token_byte(*cursor)) {
            output[len++] = (char)atp_normalize_ascii(*cursor);
        }
    }
    output[len] = '\0';
}

atp_status atp_graph_associations(const atp_graph *graph, const char *token,
                                  atp_association *out, size_t capacity,
                                  size_t *out_count) {
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
        const float neural =
            atp_network_score(graph, edge->source, edge->target);
        ranked[next++] = (atp_ranked_edge){
            .edge = edge,
            .score = edge->strength * 0.7f + neural * 0.3f,
        };
    }

    qsort(ranked, candidate_count, sizeof(*ranked), atp_ranked_edge_compare);
    const size_t written =
        candidate_count < capacity ? candidate_count : capacity;
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
