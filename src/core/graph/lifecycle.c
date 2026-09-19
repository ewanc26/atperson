#include "graph_internal.h"

/* atp_neural_architecture is defined in atperson/core.h. */
atp_neural_architecture atp_neural_legacy_architecture(void) {
    return (atp_neural_architecture){
        .version = ATPERSON_NEURAL_ARCHITECTURE_VERSION,
        .embedding_dim = ATPERSON_EMBEDDING_DIM,
        .input_dim = ATPERSON_INPUT_DIM,
        .hidden_layer_count = 1u,
        .hidden_widths = {ATPERSON_HIDDEN_DIM, 0u, 0u, 0u},
        .output_dim = 1u,
    };
}

atp_graph_config atp_graph_default_config(void) {
    atp_graph_config config = {
        .seed = UINT64_C(0x4154504552534F4E),
        .learning_rate = 0.025f,
        .familiarity_decay = 0.98f,
        .valence_rate = 0.25f,
        .episode_capacity = ATPERSON_EPISODE_DEFAULT_CAPACITY,
    };
    return config;
}

static atp_graph_config atp_graph_effective_config(const atp_graph_config *config) {
    atp_graph_config effective = config ? *config : atp_graph_default_config();
    if (effective.seed == 0u) {
        effective.seed = atp_graph_default_config().seed;
    }
    if (!(effective.learning_rate > 0.0f) || !isfinite(effective.learning_rate)) {
        effective.learning_rate = atp_graph_default_config().learning_rate;
    }
    if (!(effective.familiarity_decay > 0.0f) || effective.familiarity_decay >= 1.0f) {
        effective.familiarity_decay = atp_graph_default_config().familiarity_decay;
    }
    if (!(effective.valence_rate > 0.0f) || effective.valence_rate > 1.0f ||
        !isfinite(effective.valence_rate)) {
        effective.valence_rate = atp_graph_default_config().valence_rate;
    }
    if (effective.episode_capacity == 0u) {
        effective.episode_capacity = ATPERSON_EPISODE_DEFAULT_CAPACITY;
    }
    return effective;
}

static atp_graph *atp_graph_create_impl(const atp_graph_config *config,
                                        const atp_neural_architecture *architecture) {
    const atp_graph_config effective = atp_graph_effective_config(config);

    atp_neural_layout layout = {0};
    if (!atp_neural_layout_build(architecture, &layout)) {
        return NULL;
    }

    atp_graph *graph = calloc(1u, sizeof(*graph));
    if (!graph) {
        return NULL;
    }

    graph->config = effective;
    graph->episode_max = effective.episode_capacity;
    graph->rng_state = effective.seed;
    graph->neural_architecture = *architecture;
    if (!atp_network_init(graph)) {
        free(graph);
        return NULL;
    }
    return graph;
}

atp_graph *atp_graph_create(const atp_graph_config *config) {
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    return atp_graph_create_impl(config, &legacy);
}

atp_graph *atp_graph_create_with_architecture(const atp_graph_config *config,
                                              const atp_neural_architecture *architecture) {
    if (!architecture) {
        return NULL;
    }
    return atp_graph_create_impl(config, architecture);
}

void atp_graph_destroy(atp_graph *graph) {
    if (!graph) {
        return;
    }

    for (size_t i = 0; i < graph->node_count; ++i) {
        atp_node_destroy(&graph->nodes[i]);
    }
    free(graph->nodes);
    atp_network_destroy(&graph->network);
    free(graph->edges);
    free(graph->node_index_slots);
    free(graph->edge_index_slots);
    free(graph->ledger_entries);
    free(graph->ledger_contexts);
    free(graph->episodes);
    atp_episode_groups_destroy(graph);
    free(graph->valence_records);
    free(graph->valence_events);
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

void atp_graph_set_capacity(atp_graph *graph, size_t node_capacity_max,
                             size_t edge_capacity_max) {
    if (!graph) {
        return;
    }
    graph->config.node_capacity_max = node_capacity_max;
    graph->config.edge_capacity_max = edge_capacity_max;
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
        .capacity_rejections = graph->capacity_rejections,
    };
}

atp_status atp_graph_neural_architecture(const atp_graph *graph,
                                         atp_neural_architecture *out_architecture) {
    if (!graph || !out_architecture) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    *out_architecture = graph->neural_architecture;
    return ATP_OK;
}

atp_status atp_graph_neural_report(const atp_graph *graph,
                                   atp_neural_architecture_report *out_report) {
    if (!graph || !out_report) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const uint64_t parameter_count = atp_neural_parameter_count(&graph->neural_architecture);
    const uint64_t parameter_bytes = parameter_count * (uint64_t)sizeof(float);
    const uint64_t embedding_bytes =
        (uint64_t)graph->neural_architecture.embedding_dim * (uint64_t)sizeof(float);

    *out_report = (atp_neural_architecture_report){
        .architecture = graph->neural_architecture,
        .shared_parameter_count = parameter_count,
        .shared_parameter_bytes = parameter_bytes,
        .shared_learned_state_bytes = parameter_bytes * 2u,
        .per_node_embedding_bytes = embedding_bytes,
        .per_node_learned_state_bytes = embedding_bytes * 2u,
    };
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
    case ATP_ERR_SCHEMA:
        return "learning schema not replayable by this build";
    case ATP_ERR_CAPACITY:
        return "configured resource ceiling reached";
    }
    return "unknown error";
}

atp_status atp_graph_add_ledger_entry(atp_graph *graph, const atp_ledger_entry *entry) {
    return atp_graph_add_ledger_entry_with_context(graph, entry, NULL);
}

atp_status atp_graph_add_ledger_entry_with_context(atp_graph *graph,
                                                    const atp_ledger_entry *entry,
                                                    const atp_conversation_context *context) {
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
    if (context) {
        for (size_t i = 0u; i < 3u; ++i) {
            const char *uri = i == 0u   ? context->reply_root_uri
                              : i == 1u ? context->reply_parent_uri
                                        : context->quote_uri;
            if (strlen(uri) >= ATPERSON_CONTEXT_URI_BYTES) {
                return ATP_ERR_INVALID_ARGUMENT;
            }
        }
    }
    if (!atp_reserve_ledger_entries(graph, graph->ledger_count + 1u)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    graph->ledger_entries[graph->ledger_count] = *entry;
    if (context) {
        graph->ledger_contexts[graph->ledger_count] = *context;
    } else {
        memset(&graph->ledger_contexts[graph->ledger_count], 0,
               sizeof(atp_conversation_context));
    }
    graph->ledger_count++;
    return ATP_OK;
}

atp_status atp_graph_ledger_context(const atp_graph *graph, size_t index,
                                   atp_conversation_context *out_context) {
    if (!graph || !out_context) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->ledger_count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_context = graph->ledger_contexts[index];
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
