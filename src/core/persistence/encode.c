#include "internal.h"
#include "persistence/encode_common.h"
#include "persistence/encode_v6.h"
#include "persistence/format.h"
#include "persistence/wire.h"
#include "io/portable.h"

#include <stdlib.h>

/*
 * Snapshot v5 encoding and the public save dispatch.
 *
 * v5 is the legacy fragment format: a one-hidden-layer graph at the named
 * legacy architecture, encoded with its historical wire shape. atp_graph_save
 * writes legacy graphs as byte-identical v5 and non-legacy graphs as v6
 * (encode_v6.c); the two formats share their non-topology sections and the
 * atomic rename write (encode_common.c).
 *
 * Encoding runs fully into a growable caller-local buffer before any
 * destination file is replaced. Allocation failure aborts before I/O; the
 * atomic write removes the temporary file on failure and always leaves the
 * previous snapshot untouched.
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

static bool atp_encode_network(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_NETWORK)) {
        return false;
    }

    /*
     * v5 wire compatibility: first-layer weights, first hidden biases,
     * hidden-to-output weights, output bias. Public v5 graphs are legacy-only.
     */
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

static bool atp_encode_nodes(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_NODES)) {
        return false;
    }
    bool ok = atp_buffer_u64(buffer, (uint64_t)graph->node_count);
    for (size_t i = 0u; ok && i < graph->node_count; ++i) {
        const atp_node *node = &graph->nodes[i];
        ok = atp_buffer_string(buffer, node->token) &&
             atp_buffer_u64(buffer, node->observations) &&
             atp_buffer_f32(buffer, node->familiarity);
        for (size_t d = 0u; ok && d < graph->neural_architecture.embedding_dim; ++d) {
            ok = atp_buffer_f32(buffer, node->embedding[d]);
        }
    }
    atp_section_end(&section);
    return ok;
}

static bool atp_encode_snapshot_v5(const atp_graph *graph, atp_buffer *buffer) {
    return atp_buffer_put(buffer, ATP_SNAPSHOT_MAGIC, sizeof(ATP_SNAPSHOT_MAGIC) - 1u) &&
           atp_buffer_u32(buffer, ATPERSON_SNAPSHOT_VERSION_V5) &&
           atp_encode_header(graph, buffer) && atp_encode_network(graph, buffer) &&
           atp_encode_nodes(graph, buffer) && atp_encode_edges(graph, buffer) &&
           atp_encode_ledger(graph, buffer) && atp_encode_context(graph, buffer) &&
           atp_encode_episodes(graph, buffer) &&
           atp_encode_valence(graph, buffer) && atp_encode_schema(buffer) &&
           atp_buffer_u64(buffer, atp_fnv1a64(buffer->data, buffer->size));
}

atp_status atp_graph_save(const atp_graph *graph, const char *path) {
    if (!graph || !path || path[0] == '\0') {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_buffer buffer = {0};
    const bool encoded = atp_neural_architecture_is_legacy(&graph->neural_architecture)
                             ? atp_encode_snapshot_v5(graph, &buffer)
                             : atp_encode_snapshot_v6(graph, &buffer);
    if (!encoded) {
        free(buffer.data);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    return atp_snapshot_write(path, &buffer) ? ATP_OK : ATP_ERR_IO;
}