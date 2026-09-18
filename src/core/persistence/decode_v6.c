#include "persistence/decode_v6.h"

#include "internal.h"
#include "persistence/decode_common.h"
#include "persistence/format.h"
#include "io/portable.h"

#include <stdlib.h>
#include <string.h>

/*
 * Snapshot v6 decoder (persistence/decode_v6.h).
 *
 * v6 generalizes the v5 wire shape to the explicit neural architecture
 * carried by ATP_SECTION_ARCH. HEADER is config-only (the topology lives in
 * ARCH); ARCH must precede NETWORK and NODES because it creates the graph
 * at the persisted topology before any learned state is read. NETWORK and
 * NODES carry the generic layer stack and per-node embeddings together with
 * their plasticity-importance values (issue #59), so a v6 round-trip
 * reproduces the trained state exactly.
 *
 * Integrity before interpretation: the trailing FNV-1a digest covers the
 * whole image, so bitrot is rejected before any byte influences decoding.
 * Corruption guards mirror the v5 loader: duplicate sections, reordered
 * dependencies, truncated payloads, and counts exceeding the remaining
 * bytes all fail as ATP_ERR_FORMAT.
 */

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
        if (!atp_reader_section(&reader, &section)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 11u && seen[section.tag]) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 11u) {
            seen[section.tag] = true;
        }
        const size_t payload_end = reader.position + (size_t)section.length;

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

    if (!graph || !seen[ATP_SECTION_HEADER] || !seen[ATP_SECTION_ARCH] ||
        !seen[ATP_SECTION_NETWORK] || !seen[ATP_SECTION_NODES] || !seen[ATP_SECTION_EDGES] ||
        !seen[ATP_SECTION_LEDGER] || !seen[ATP_SECTION_EPISODES]) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    if (!atp_schema_can_replay(learning_schema)) {
        return atp_load_failure(graph, status, ATP_ERR_SCHEMA);
    }

    /* The entry digest check already verified the trailing bytes; here only
     * the framing is confirmed: exactly the digest remains. */
    if (reader.size - reader.position != 8u) {
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
