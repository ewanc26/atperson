#include "persistence/decode.h"
#include "persistence/decode_common.h"
#include "persistence/format.h"
#include "io/portable.h"

#include <stdlib.h>
#include <string.h>

/*
 * Snapshot v5 decoder. Owns legacy-format section semantics and validation.
 * It allocates one graph on success; every failure path destroys partial
 * state. The format-agnostic sections live in decode_common.c and are
 * shared with the v6 loader.
 */
static bool atp_decode_header(atp_reader *reader, atp_graph_config *config, uint64_t *rng_state,
                              uint64_t *observations, uint64_t *token_observations,
                              uint64_t *training_steps, double *loss_total,
                              uint64_t *episode_evictions) {
    uint32_t embedding_dim = 0u;
    uint32_t hidden_dim = 0u;
    uint32_t input_dim = 0u;
    uint32_t episode_capacity = 0u;
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    const bool ok = atp_reader_u32(reader, &embedding_dim) &&
           embedding_dim == legacy.embedding_dim &&
           atp_reader_u32(reader, &hidden_dim) && hidden_dim == legacy.hidden_widths[0] &&
           atp_reader_u32(reader, &input_dim) && input_dim == legacy.input_dim &&
           atp_reader_u64(reader, &config->seed) &&
           atp_reader_f32(reader, &config->learning_rate) &&
           atp_reader_f32(reader, &config->familiarity_decay) &&
           config->familiarity_decay > 0.0f && config->familiarity_decay < 1.0f &&
           atp_reader_u32(reader, &episode_capacity) && episode_capacity > 0u &&
           atp_reader_u64(reader, rng_state) && atp_reader_u64(reader, observations) &&
           atp_reader_u64(reader, token_observations) &&
           atp_reader_u64(reader, training_steps) && atp_reader_f64(reader, loss_total) &&
           atp_reader_u64(reader, episode_evictions);
    if (ok) {
        config->episode_capacity = episode_capacity;
    }
    return ok;
}

bool atp_decode_network(atp_reader *reader, atp_graph *graph) {
    atp_network *network = &graph->network;
    const size_t first_layer = 0u;
    const size_t output_layer = network->layout.layer_count - 1u;
    const size_t input_dim = network->layout.input_widths[first_layer];
    const size_t hidden_dim = network->layout.output_widths[first_layer];
    bool ok = true;

    for (size_t h = 0u; ok && h < hidden_dim; ++h) {
        for (size_t i = 0u; ok && i < input_dim; ++i) {
            ok = atp_reader_f32(
                reader, &network->weights[atp_network_weight_index(network, first_layer, h, i)]);
        }
    }
    for (size_t h = 0u; ok && h < hidden_dim; ++h) {
        ok = atp_reader_f32(
            reader, &network->biases[atp_network_bias_index(network, first_layer, h)]);
    }
    for (size_t h = 0u; ok && h < hidden_dim; ++h) {
        ok = atp_reader_f32(
            reader, &network->weights[atp_network_weight_index(network, output_layer, 0u, h)]);
    }
    return ok && atp_reader_f32(
                     reader, &network->biases[atp_network_bias_index(network, output_layer, 0u)]);
}

static bool atp_decode_nodes(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    const uint64_t min_entry =
        4u + 8u + 4u + (uint64_t)graph->neural_architecture.embedding_dim * 4u + 1u;
    if (!atp_reader_u64(reader, &count) || count > SIZE_MAX / sizeof(atp_node) ||
        count > (reader->size - reader->position) / min_entry) {
        return false;
    }
    if (!atp_reserve_nodes(graph, (size_t)count)) {
        return false;
    }
    for (size_t i = 0u; i < (size_t)count; ++i) {
        atp_node *node = &graph->nodes[graph->node_count];
        memset(node, 0, sizeof(*node));
        char token[ATPERSON_TOKEN_BYTES];
        if (!atp_reader_string(reader, token, sizeof(token)) ||
            !atp_reader_u64(reader, &node->observations) ||
            !atp_reader_f32(reader, &node->familiarity)) {
            return false;
        }
        node->token = strdup(token);
        if (!node->token || !atp_node_allocate_vectors(graph, node)) {
            atp_node_destroy(node);
            return false;
        }
        graph->node_count++;
        for (size_t d = 0u; d < graph->neural_architecture.embedding_dim; ++d) {
            if (!atp_reader_f32(reader, &node->embedding[d])) {
                return false;
            }
        }
    }
    return true;
}

atp_graph *atp_load_v5(const unsigned char *data, size_t size, atp_status *status) {
    /* Integrity before interpretation: the trailing FNV-1a digest covers
     * the whole image, which is already fully in memory, so any bitrot is
     * rejected as ATP_ERR_FORMAT before a corrupted byte can influence
     * section decoding or the schema-replayability decision. */
    if (size < 8u || atp_fnv1a64(data, size - 8u) != atp_load_u64le(data + size - 8u)) {
        return atp_load_failure(NULL, status, ATP_ERR_FORMAT);
    }

    atp_reader reader = {data, size, 12u};
    atp_graph *graph = NULL;
    bool seen[11] = {false};
    uint32_t learning_schema = 1u;

    while (reader.size - reader.position > 8u) {
        atp_section section;
        if (!atp_reader_section(&reader, &section)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 10u && seen[section.tag]) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 10u) {
            seen[section.tag] = true;
        }
        const size_t payload_end = reader.position + (size_t)section.length;

        bool ok = false;
        switch (section.tag) {
        case ATP_SECTION_HEADER: {
            atp_graph_config config = {0};
            uint64_t rng_state = 0u;
            uint64_t observations = 0u;
            uint64_t token_observations = 0u;
            uint64_t training_steps = 0u;
            double loss_total = 0.0;
            uint64_t episode_evictions = 0u;
            if (graph) {
                break;
            }
            ok = atp_decode_header(&reader, &config, &rng_state, &observations,
                                   &token_observations, &training_steps, &loss_total,
                                   &episode_evictions);
            if (ok) {
                graph = atp_graph_create(&config);
                if (!graph) {
                    return atp_load_failure(NULL, status, ATP_ERR_OUT_OF_MEMORY);
                }
                graph->rng_state = rng_state;
                graph->observations = observations;
                graph->token_observations = token_observations;
                graph->training_steps = training_steps;
                graph->loss_total = loss_total;
                graph->episode_evictions = episode_evictions;
            }
            break;
        }
        case ATP_SECTION_NETWORK:
            ok = graph && atp_decode_network(&reader, graph);
            break;
        case ATP_SECTION_NODES:
            ok = graph && atp_decode_nodes(&reader, graph);
            break;
        case ATP_SECTION_EDGES:
            ok = graph && atp_decode_edges(&reader, graph);
            break;
        case ATP_SECTION_LEDGER:
            ok = graph && atp_decode_ledger(&reader, graph);
            break;
        case ATP_SECTION_CONTEXT:
            /* Context rows are validated against the ledger mirror, so the
             * LEDGER section must already be decoded. Every writer emits
             * LEDGER before CONTEXT; a stream that reorders them is not a
             * snapshot this core produced. */
            ok = graph && seen[ATP_SECTION_LEDGER] && atp_decode_context(&reader, graph);
            break;
        case ATP_SECTION_EPISODES:
            ok = graph && atp_decode_episodes(&reader, graph);
            break;
        case ATP_SECTION_VALENCE:
            /* Node indexes are validated against the node table, so the
             * NODES section must already be decoded. Every writer emits
             * NODES before VALENCE; a stream that reorders them is not a
             * snapshot this core produced. */
            ok = graph && seen[ATP_SECTION_NODES] && atp_decode_valence(&reader, graph);
            break;
        case ATP_SECTION_SCHEMA: {
            uint32_t schema = 0u;
            ok = atp_reader_u32(&reader, &schema) && schema > 0u;
            if (ok) {
                learning_schema = schema;
            }
            break;
        }
        case ATP_SECTION_FAMILIARITY:
            reader.position = payload_end;
            ok = true;
            break;
        default:
            reader.position = payload_end;
            ok = true;
            break;
        }
        if (!ok || reader.position != payload_end) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
    }

    if (!graph || !seen[ATP_SECTION_HEADER] || !seen[ATP_SECTION_NETWORK] ||
        !seen[ATP_SECTION_NODES] || !seen[ATP_SECTION_EDGES] || !seen[ATP_SECTION_LEDGER] ||
        !seen[ATP_SECTION_EPISODES]) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    if (!atp_schema_can_replay(learning_schema)) {
        return atp_load_failure(graph, status, ATP_ERR_SCHEMA);
    }

    uint64_t stored_digest = 0u;
    if (reader.size - reader.position != 8u || !atp_reader_u64(&reader, &stored_digest) ||
        stored_digest != atp_fnv1a64(data, size - 8u)) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    if (!atp_graph_rebuild_indexes(graph)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }
    if (!atp_episode_groups_rebuild(graph)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }

    if (status) {
        *status = ATP_OK;
    }
    return graph;
}
