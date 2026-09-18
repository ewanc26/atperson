#include "persistence/v6.h"
#include "persistence/sections.h"
#include "persistence/format.h"
#include "io/portable.h"

#include <stdlib.h>
#include <string.h>

/*
 * Snapshot v6 (persistence/v6.h). The format-agnostic sections live in
 * sections.c and are shared with the v5 loader; the section-walk
 * scaffolding lives in reader.c.
 *
 * Section order: HEADER, ARCH, NETWORK, NODES, then the format-agnostic
 * sections shared with v5, then the trailing FNV-1a digest.
 */

/* v6 HEADER: config and counters only; the topology is in ARCH. */
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

bool atp_encode_snapshot_v6(const atp_graph *graph, atp_buffer *buffer) {
    return atp_buffer_put(buffer, ATP_SNAPSHOT_MAGIC_V6, sizeof(ATP_SNAPSHOT_MAGIC_V6) - 1u) &&
           atp_buffer_u32(buffer, ATPERSON_SNAPSHOT_VERSION) &&
           atp_encode_header_v6(graph, buffer) && atp_encode_arch(graph, buffer) &&
           atp_encode_network_v6(graph, buffer) &&
           atp_encode_nodes(graph, buffer, true) && atp_encode_edges(graph, buffer) &&
           atp_encode_ledger(graph, buffer) &&
           atp_encode_context(graph, buffer) && atp_encode_episodes(graph, buffer) &&
           atp_encode_valence(graph, buffer) && atp_encode_schema(buffer) &&
           atp_buffer_u64(buffer, atp_fnv1a64(buffer->data, buffer->size));
}

/* v6 HEADER: config and counters only; the topology is in ARCH. */
static bool atp_decode_header_v6(atp_reader *reader, atp_graph_config *config,
                                 uint64_t *rng_state, uint64_t *observations,
                                 uint64_t *token_observations, uint64_t *training_steps,
                                 double *loss_total, uint64_t *episode_evictions) {
    uint32_t episode_capacity = 0u;
    const bool ok = atp_reader_u64(reader, &config->seed) &&
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

/* ARCH: the persisted topology descriptor. Validation is delegated to
 * atp_neural_layout_build through atp_graph_create_with_architecture, which
 * fails closed for any invalid descriptor; the version field must match the
 * descriptor shape this decoder understands. */
static bool atp_decode_arch(atp_reader *reader, atp_neural_architecture *architecture) {
    memset(architecture, 0, sizeof(*architecture));
    if (!atp_reader_u32(reader, &architecture->version) ||
        architecture->version != ATPERSON_NEURAL_ARCHITECTURE_VERSION ||
        !atp_reader_u32(reader, &architecture->embedding_dim) ||
        !atp_reader_u32(reader, &architecture->input_dim) ||
        !atp_reader_u32(reader, &architecture->output_dim) ||
        !atp_reader_u32(reader, &architecture->hidden_layer_count) ||
        architecture->hidden_layer_count > ATPERSON_NEURAL_MAX_HIDDEN_LAYERS) {
        return false;
    }
    for (size_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        if (!atp_reader_u32(reader, &architecture->hidden_widths[i])) {
            return false;
        }
    }
    return true;
}

/* Generic NETWORK: the layer stack in the kernel's index order, then the
 * matching importance arrays. Layer count is checked against the layout the
 * ARCH-created graph already carries; each read is bounded by the payload
 * the section framing guarantees. */
static bool atp_decode_network_v6(atp_reader *reader, atp_graph *graph) {
    atp_network *network = &graph->network;
    uint32_t layer_count = 0u;
    if (!atp_reader_u32(reader, &layer_count) ||
        (size_t)layer_count != network->layout.layer_count) {
        return false;
    }
    for (size_t layer = 0u; layer < network->layout.layer_count; ++layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        for (size_t o = 0u; o < output_width; ++o) {
            for (size_t i = 0u; i < input_width; ++i) {
                if (!atp_reader_f32(
                        reader,
                        &network->weights[atp_network_weight_index(network, layer, o, i)])) {
                    return false;
                }
            }
        }
        for (size_t o = 0u; o < output_width; ++o) {
            if (!atp_reader_f32(
                    reader, &network->biases[atp_network_bias_index(network, layer, o)])) {
                return false;
            }
        }
    }
    for (size_t layer = 0u; layer < network->layout.layer_count; ++layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        for (size_t o = 0u; o < output_width; ++o) {
            for (size_t i = 0u; i < input_width; ++i) {
                if (!atp_reader_f32(
                        reader,
                        &network->weight_importance[
                                 atp_network_weight_index(network, layer, o, i)])) {
                    return false;
                }
            }
        }
        for (size_t o = 0u; o < output_width; ++o) {
            if (!atp_reader_f32(
                    reader,
                    &network->bias_importance[atp_network_bias_index(network, layer, o)])) {
                return false;
            }
        }
    }
    return true;
}

atp_graph *atp_load_v6(const unsigned char *data, size_t size, atp_status *status) {
    /* Integrity before interpretation, as in the v5 loader. */
    if (size < 8u || atp_fnv1a64(data, size - 8u) != atp_load_u64le(data + size - 8u)) {
        return atp_load_failure(NULL, status, ATP_ERR_FORMAT);
    }

    atp_reader reader = {data, size, 12u};
    atp_graph *graph = NULL;
    bool seen[12] = {false};
    uint32_t learning_schema = 1u;

    /* HEADER is config-only; its fields wait here until ARCH supplies the
     * topology and the graph can be created. */
    atp_graph_config pending_config = {0};
    uint64_t pending_rng_state = 0u;
    uint64_t pending_observations = 0u;
    uint64_t pending_token_observations = 0u;
    uint64_t pending_training_steps = 0u;
    double pending_loss_total = 0.0;
    uint64_t pending_episode_evictions = 0u;

    while (reader.size - reader.position > 8u) {
        atp_section section;
        size_t payload_end = 0u;
        if (!atp_reader_next_section(&reader, seen, 11u, &section, &payload_end)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }

        bool ok = false;
        switch (section.tag) {
        case ATP_SECTION_HEADER: {
            ok = atp_decode_header_v6(&reader, &pending_config, &pending_rng_state,
                                     &pending_observations, &pending_token_observations,
                                     &pending_training_steps, &pending_loss_total,
                                     &pending_episode_evictions);
            break;
        }
        case ATP_SECTION_ARCH: {
            atp_neural_architecture architecture;
            if (!seen[ATP_SECTION_HEADER]) {
                break;
            }
            ok = atp_decode_arch(&reader, &architecture);
            if (ok) {
                graph = atp_graph_create_with_architecture(&pending_config, &architecture);
                if (!graph) {
                    /* Invalid descriptor: layout validation failed closed. */
                    return atp_load_failure(NULL, status, ATP_ERR_FORMAT);
                }
                graph->rng_state = pending_rng_state;
                graph->observations = pending_observations;
                graph->token_observations = pending_token_observations;
                graph->training_steps = pending_training_steps;
                graph->loss_total = pending_loss_total;
                graph->episode_evictions = pending_episode_evictions;
            }
            break;
        }
        case ATP_SECTION_NETWORK:
            /* NETWORK reads the layer stack the ARCH-created graph carries;
             * ARCH must precede it. */
            ok = graph && atp_decode_network_v6(&reader, graph);
            break;
        case ATP_SECTION_NODES:
            ok = graph && atp_decode_nodes(&reader, graph, true);
            break;
        case ATP_SECTION_EDGES:
            ok = graph && atp_decode_edges(&reader, graph);
            break;
        case ATP_SECTION_LEDGER:
            ok = graph && atp_decode_ledger(&reader, graph);
            break;
        case ATP_SECTION_CONTEXT:
            ok = graph && seen[ATP_SECTION_LEDGER] && atp_decode_context(&reader, graph);
            break;
        case ATP_SECTION_EPISODES:
            ok = graph && atp_decode_episodes(&reader, graph);
            break;
        case ATP_SECTION_VALENCE:
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
            reader.position = payload_end;
            ok = true;
            break;
        }
        if (!ok || reader.position != payload_end) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
    }

    return atp_load_finish(graph, seen, 0x43Fu, learning_schema, &reader, status);
}
