#include "persistence/encode_v6.h"
#include "persistence/encode_common.h"
#include "persistence/format.h"
#include "io/portable.h"

/*
 * Snapshot v6 encoding (persistence/encode_v6.h).
 *
 * v6 generalizes snapshot v5 to the explicit neural architecture descriptor
 * carried by ATP_SECTION_ARCH, so every supported topology and its
 * plasticity-importance values round-trip exactly. HEADER is config-only
 * (the topology lives in ARCH); NETWORK persists the generic layer stack in
 * the kernel's index order followed by its importance arrays; NODES appends
 * the embedding-importance vector to each node.
 *
 * Legacy graphs never reach these encoders: atp_graph_save writes them as
 * byte-identical v5, keeping the historical wire shape authoritative.
 *
 * Section order: HEADER, ARCH, NETWORK, NODES, then the format-agnostic
 * sections shared with v5, then the trailing FNV-1a digest.
 */

static bool atp_encode_header_v6(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_HEADER)) {
        return false;
    }
    const bool ok = atp_buffer_u64(buffer, graph->config.seed) &&
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

static bool atp_encode_arch(const atp_graph *graph, atp_buffer *buffer) {
    const atp_neural_architecture *architecture = &graph->neural_architecture;
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_ARCH)) {
        return false;
    }
    bool ok = atp_buffer_u32(buffer, architecture->version) &&
              atp_buffer_u32(buffer, architecture->embedding_dim) &&
              atp_buffer_u32(buffer, architecture->input_dim) &&
              atp_buffer_u32(buffer, architecture->output_dim) &&
              atp_buffer_u32(buffer, architecture->hidden_layer_count);
    for (size_t i = 0u; ok && i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        ok = atp_buffer_u32(buffer, architecture->hidden_widths[i]);
    }
    atp_section_end(&section);
    return ok;
}

/* Generic network in the kernel's index order; importance arrays follow so
 * the plasticity state (issue #59) round-trips exactly. */
static bool atp_encode_network_v6(const atp_graph *graph, atp_buffer *buffer) {
    const atp_network *network = &graph->network;
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_NETWORK)) {
        return false;
    }
    bool ok = atp_buffer_u32(buffer, (uint32_t)network->layout.layer_count);
    for (size_t layer = 0u; ok && layer < network->layout.layer_count; ++layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        for (size_t o = 0u; ok && o < output_width; ++o) {
            for (size_t i = 0u; ok && i < input_width; ++i) {
                ok = atp_buffer_f32(
                    buffer, network->weights[atp_network_weight_index(network, layer, o, i)]);
            }
        }
        for (size_t o = 0u; ok && o < output_width; ++o) {
            ok = atp_buffer_f32(
                buffer, network->biases[atp_network_bias_index(network, layer, o)]);
        }
    }
    for (size_t layer = 0u; ok && layer < network->layout.layer_count; ++layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        for (size_t o = 0u; ok && o < output_width; ++o) {
            for (size_t i = 0u; ok && i < input_width; ++i) {
                ok = atp_buffer_f32(
                    buffer,
                    network->weight_importance[atp_network_weight_index(network, layer, o, i)]);
            }
        }
        for (size_t o = 0u; ok && o < output_width; ++o) {
            ok = atp_buffer_f32(
                buffer, network->bias_importance[atp_network_bias_index(network, layer, o)]);
        }
    }
    atp_section_end(&section);
    return ok;
}

static bool atp_encode_nodes_v6(const atp_graph *graph, atp_buffer *buffer) {
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
            ok = atp_buffer_f32(buffer, node->embedding[d]) &&
                 atp_buffer_f32(buffer, node->embedding_importance[d]);
        }
    }
    atp_section_end(&section);
    return ok;
}

bool atp_encode_snapshot_v6(const atp_graph *graph, atp_buffer *buffer) {
    return atp_buffer_put(buffer, ATP_SNAPSHOT_MAGIC_V6, sizeof(ATP_SNAPSHOT_MAGIC_V6) - 1u) &&
           atp_buffer_u32(buffer, ATPERSON_SNAPSHOT_VERSION) &&
           atp_encode_header_v6(graph, buffer) && atp_encode_arch(graph, buffer) &&
           atp_encode_network_v6(graph, buffer) && atp_encode_nodes_v6(graph, buffer) &&
           atp_encode_edges(graph, buffer) && atp_encode_ledger(graph, buffer) &&
           atp_encode_context(graph, buffer) && atp_encode_episodes(graph, buffer) &&
           atp_encode_valence(graph, buffer) && atp_encode_schema(buffer) &&
           atp_buffer_u64(buffer, atp_fnv1a64(buffer->data, buffer->size));
}