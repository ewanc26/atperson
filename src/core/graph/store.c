#include "graph_internal.h"

/* Dup a token for the vocabulary node table. Returns NULL on NULL input or
 * allocation failure. Caller frees via atp_graph_destroy. */
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

bool atp_node_allocate_vectors(const atp_graph *graph, atp_node *node) {
    if (!graph || !node || node->embedding || node->embedding_importance) {
        return false;
    }
    const size_t dimension = graph->neural_architecture.embedding_dim;
    if (dimension == 0u || dimension > SIZE_MAX / sizeof(float)) {
        return false;
    }

    float *embedding = malloc(dimension * sizeof(*embedding));
    float *importance = calloc(dimension, sizeof(*importance));
    if (!embedding || !importance) {
        free(embedding);
        free(importance);
        return false;
    }
    node->embedding = embedding;
    node->embedding_importance = importance;
    return true;
}

void atp_node_destroy(atp_node *node) {
    if (!node) {
        return;
    }
    free(node->token);
    free(node->embedding);
    free(node->embedding_importance);
    memset(node, 0, sizeof(*node));
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
    atp_conversation_context *contexts =
        realloc(graph->ledger_contexts, capacity * sizeof(*contexts));
    if (!contexts) {
        /* The entry buffer already grew; keep it (ledger_capacity is a
         * lower bound on the allocation, not an exact size) and report
         * failure so the caller treats this as OOM. */
        graph->ledger_entries = entries;
        return false;
    }

    /* Zero the newly grown context slots: entries added without context
     * (pre-#24 snapshots, plain add) must read back empty, not garbage. */
    memset(contexts + graph->ledger_count, 0,
           (capacity - graph->ledger_count) * sizeof(*contexts));

    graph->ledger_entries = entries;
    graph->ledger_contexts = contexts;
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

int32_t atp_intern_node_checked(atp_graph *graph, const char *token, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (!graph || !token) {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return -1;
    }

    const int32_t existing = atp_find_node(graph, token);
    if (existing >= 0) {
        graph->nodes[existing].observations++;
        graph->nodes[existing].familiarity =
            graph->nodes[existing].familiarity * graph->config.familiarity_decay + 1.0f;
        return existing;
    }

    /* Resource ceiling: reject before any mutation. The caller rejects
     * the whole observation; nothing is half-interned. */
    if (graph->config.node_capacity_max != 0u &&
        graph->node_count >= graph->config.node_capacity_max) {
        graph->capacity_rejections++;
        if (status) {
            *status = ATP_ERR_CAPACITY;
        }
        return -1;
    }

    if (graph->node_count >= UINT32_MAX || !atp_reserve_nodes(graph, graph->node_count + 1u)) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return -1;
    }
    if (!atp_node_index_maybe_grow(graph)) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return -1;
    }

    atp_node *node = &graph->nodes[graph->node_count];
    memset(node, 0, sizeof(*node));
    node->token = atp_strdup_local(token);
    if (!node->token || !atp_node_allocate_vectors(graph, node)) {
        atp_node_destroy(node);
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return -1;
    }
    node->observations = 1u;
    node->familiarity = 1.0f;
    for (size_t i = 0; i < graph->neural_architecture.embedding_dim; ++i) {
        node->embedding[i] = atp_rng_signed(graph) * 0.05f;
    }

    const int32_t index = (int32_t)graph->node_count;
    graph->node_count++;
    atp_node_index_insert(graph, (uint32_t)index);
    return index;
}

int32_t atp_intern_node(atp_graph *graph, const char *token) {
    atp_status status = ATP_OK;
    return atp_intern_node_checked(graph, token, &status);
}

atp_status atp_observe_pair(atp_graph *graph, uint32_t source, uint32_t target,
                            uint64_t source_hash) {
    if (!graph || source >= graph->node_count || target >= graph->node_count) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    int32_t edge_index = atp_find_edge(graph, source, target);
    if (edge_index < 0) {
        /* Resource ceiling: reject before any mutation. */
        if (graph->config.edge_capacity_max != 0u &&
            graph->edge_count >= graph->config.edge_capacity_max) {
            graph->capacity_rejections++;
            return ATP_ERR_CAPACITY;
        }
        if (!atp_reserve_edges(graph, graph->edge_count + 1u)) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        if (!atp_edge_index_maybe_grow(graph)) {
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
        atp_edge_index_insert(graph, (uint32_t)edge_index);
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
    const float score = atp_network_score_owned(graph, source, target);
    edge->strength = edge->observations == 0u ? score : (edge->strength * 0.90f + score * 0.10f);
    edge->observations++;
    edge->last_source_hash = source_hash;
    return ATP_OK;
}
