#include "persistence/persistence_internal.h"

/*
 * Snapshot v5 (persistence_internal.h): legacy-format encoding and decoding.
 * The format-agnostic sections live in sections.c and are shared with the
 * v6 loader; the section-walk scaffolding lives in reader.c.
 */

static bool atp_encode_header(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_HEADER)) {
        return false;
    }
    const bool ok = atp_buffer_u32(buffer, graph->neural_architecture.embedding_dim) &&
                    atp_buffer_u32(buffer, graph->neural_architecture.hidden_widths[0]) &&
                    atp_buffer_u32(buffer, graph->neural_architecture.input_dim) &&
                    atp_buffer_u64(buffer, graph->config.seed) &&
                    atp_buffer_f32(buffer, graph->config.learning_rate) &&
                    atp_buffer_f32(buffer, graph->config.familiarity_decay) &&
                    atp_buffer_u32(buffer, (uint32_t)graph->config.episode_capacity) &&
                    atp_buffer_u64(buffer, graph->rng_state) &&
                    atp_buffer_u64(buffer, graph->observations) &&
                    atp_buffer_u64(buffer, graph->token_observations) &&
                    atp_buffer_u64(buffer, graph->training_steps) &&
                    atp_buffer_f64(buffer, graph->loss_total) &&
                    atp_buffer_u64(buffer, graph->episode_evictions);
    atp_section_end(&section);
    return ok;
}

/*
 * v5 wire compatibility: first-layer weights, first hidden biases,
 * hidden-to-output weights, output bias. Public v5 graphs are legacy-only.
 */
static bool atp_encode_network(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_NETWORK)) {
        return false;
    }

    const atp_network *network = &graph->network;
    const size_t first_layer = 0u;
    const size_t output_layer = network->layout.layer_count - 1u;
    const size_t input_dim = network->layout.input_widths[first_layer];
    const size_t hidden_dim = network->layout.output_widths[first_layer];
    bool ok = true;

    for (size_t h = 0u; ok && h < hidden_dim; ++h) {
        for (size_t i = 0u; ok && i < input_dim; ++i) {
            ok = atp_buffer_f32(
                buffer, network->weights[atp_network_weight_index(network, first_layer, h, i)]);
        }
    }
    for (size_t h = 0u; ok && h < hidden_dim; ++h) {
        ok = atp_buffer_f32(
            buffer, network->biases[atp_network_bias_index(network, first_layer, h)]);
    }
    for (size_t h = 0u; ok && h < hidden_dim; ++h) {
        ok = atp_buffer_f32(
            buffer, network->weights[atp_network_weight_index(network, output_layer, 0u, h)]);
    }
    ok = ok && atp_buffer_f32(
                   buffer, network->biases[atp_network_bias_index(network, output_layer, 0u)]);
    atp_section_end(&section);
    return ok;
}

bool atp_encode_snapshot_v5(const atp_graph *graph, atp_buffer *buffer) {
    return atp_buffer_put(buffer, ATP_SNAPSHOT_MAGIC_V5,
                          sizeof(ATP_SNAPSHOT_MAGIC_V5) - 1u) &&
           atp_buffer_u32(buffer, ATPERSON_SNAPSHOT_VERSION_V5) &&
           atp_encode_header(graph, buffer) && atp_encode_network(graph, buffer) &&
           atp_encode_nodes(graph, buffer, false) && atp_encode_edges(graph, buffer) &&
           atp_encode_ledger(graph, buffer) && atp_encode_context(graph, buffer) &&
           atp_encode_episodes(graph, buffer) &&
           atp_encode_valence(graph, buffer) && atp_encode_schema(buffer) &&
           atp_buffer_u64(buffer, atp_fnv1a64(buffer->data, buffer->size));
}

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
    bool seen[12] = {false};
    uint32_t learning_schema = 1u;

    while (reader.size - reader.position > 8u) {
        atp_section section;
        size_t payload_end = 0u;
        if (!atp_reader_next_section(&reader, seen, 10u, &section, &payload_end)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }

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
            ok = graph && atp_decode_nodes(&reader, graph, false);
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
        default:
            /* Unknown tag from a newer writer: skip, bounds already checked.
             * (Tag 7 was reserved for a familiarity section that no writer
             * ever emitted; it lands here too.) */
            reader.position = payload_end;
            ok = true;
            break;
        }
        if (!ok || reader.position != payload_end) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
    }

    return atp_load_finish(graph, seen, 0x3Fu, learning_schema, &reader, status);
}
