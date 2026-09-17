#include "persistence/decode.h"
#include "persistence/format.h"
#include "portable_io.h"

#include <stdlib.h>
#include <string.h>

/*
 * Snapshot v5 decoder. Owns current-format section semantics and validation.
 * It allocates one graph on success; every failure path destroys partial state.
 */
static bool atp_decode_header(atp_reader *reader, atp_graph_config *config, uint64_t *rng_state,
                              uint64_t *observations, uint64_t *token_observations,
                              uint64_t *training_steps, double *loss_total,
                              uint64_t *episode_evictions) {
    uint32_t embedding_dim = 0u;
    uint32_t hidden_dim = 0u;
    uint32_t input_dim = 0u;
    uint32_t episode_capacity = 0u;
    return atp_reader_u32(reader, &embedding_dim) && embedding_dim == ATPERSON_EMBEDDING_DIM &&
           atp_reader_u32(reader, &hidden_dim) && hidden_dim == ATPERSON_HIDDEN_DIM &&
           atp_reader_u32(reader, &input_dim) && input_dim == ATPERSON_INPUT_DIM &&
           atp_reader_u64(reader, &config->seed) &&
           atp_reader_f32(reader, &config->learning_rate) &&
           atp_reader_f32(reader, &config->familiarity_decay) &&
           config->familiarity_decay > 0.0f && config->familiarity_decay < 1.0f &&
           atp_reader_u32(reader, &episode_capacity) && episode_capacity > 0u &&
           atp_reader_u64(reader, rng_state) && atp_reader_u64(reader, observations) &&
           atp_reader_u64(reader, token_observations) &&
           atp_reader_u64(reader, training_steps) && atp_reader_f64(reader, loss_total) &&
           atp_reader_u64(reader, episode_evictions);
}

bool atp_decode_network(atp_reader *reader, atp_graph *graph) {
    bool ok = true;
    for (size_t h = 0u; ok && h < ATPERSON_HIDDEN_DIM; ++h) {
        for (size_t i = 0u; ok && i < ATPERSON_INPUT_DIM; ++i) {
            ok = atp_reader_f32(reader, &graph->network.input_hidden[h][i]);
        }
    }
    for (size_t h = 0u; ok && h < ATPERSON_HIDDEN_DIM; ++h) {
        ok = atp_reader_f32(reader, &graph->network.hidden_bias[h]);
    }
    for (size_t h = 0u; ok && h < ATPERSON_HIDDEN_DIM; ++h) {
        ok = atp_reader_f32(reader, &graph->network.hidden_output[h]);
    }
    return ok && atp_reader_f32(reader, &graph->network.output_bias);
}

static bool atp_decode_nodes(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    const uint64_t min_entry = 4u + 8u + 4u + (uint64_t)ATPERSON_EMBEDDING_DIM * 4u + 1u;
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
        if (!node->token) {
            return false;
        }
        for (size_t d = 0u; d < ATPERSON_EMBEDDING_DIM; ++d) {
            if (!atp_reader_f32(reader, &node->embedding[d])) {
                return false;
            }
        }
        graph->node_count++;
    }
    return true;
}

static bool atp_decode_edges(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    if (!atp_reader_u64(reader, &count) || count > SIZE_MAX / sizeof(atp_edge) ||
        count > (reader->size - reader->position) / 28u) {
        return false;
    }
    if (!atp_reserve_edges(graph, (size_t)count)) {
        return false;
    }
    for (size_t i = 0u; i < (size_t)count; ++i) {
        atp_edge *edge = &graph->edges[graph->edge_count];
        if (!atp_reader_u32(reader, &edge->source) || !atp_reader_u32(reader, &edge->target) ||
            !atp_reader_u64(reader, &edge->observations) ||
            !atp_reader_u64(reader, &edge->last_source_hash) ||
            !atp_reader_f32(reader, &edge->strength) || edge->source >= graph->node_count ||
            edge->target >= graph->node_count) {
            return false;
        }
        graph->edge_count++;
    }
    return true;
}

static bool atp_decode_ledger(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    const uint64_t min_entry = (4u + 1u) + (4u + 1u) + 8u + 8u + 4u + 4u + 8u;
    if (!atp_reader_u64(reader, &count) || count > SIZE_MAX / sizeof(atp_ledger_entry) ||
        count > (reader->size - reader->position) / min_entry) {
        return false;
    }
    if (!atp_reserve_ledger_entries(graph, (size_t)count)) {
        return false;
    }
    for (size_t i = 0u; i < (size_t)count; ++i) {
        atp_ledger_entry entry = {0};
        uint32_t outcome = 0u;
        if (!atp_reader_string(reader, entry.source_id, sizeof(entry.source_id)) ||
            !atp_reader_string_opt(reader, entry.author_did, sizeof(entry.author_did)) ||
            !atp_reader_u64(reader, &entry.observed_at) ||
            !atp_reader_u64(reader, &entry.content_digest) ||
            !atp_reader_u32(reader, &entry.schema_version) ||
            !atp_reader_u32(reader, &outcome) ||
            outcome > (uint32_t)ATP_LEDGER_OUTCOME_WITHDRAWN ||
            !atp_reader_u64(reader, &entry.id)) {
            return false;
        }
        entry.outcome = (atp_ledger_outcome)outcome;
        graph->ledger_entries[graph->ledger_count] = entry;
        graph->ledger_count++;
    }
    return true;
}

/*
 * Valence section (issue #13): folded per-token records plus the bounded
 * provenance log. Node indexes are validated against the already-decoded
 * node table (the VALENCE switch case requires the NODES section to have
 * been decoded first). Absent section = empty valence (pre-#13 snapshots
 * load neutral).
 */
static bool atp_decode_valence(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    const uint64_t min_record = 4u + 4u + 8u + 8u + 8u + 8u;
    if (!atp_reader_u64(reader, &count) || count > SIZE_MAX / sizeof(atp_valence_record) ||
        count > (reader->size - reader->position) / min_record) {
        return false;
    }
    if (graph->valence_records) {
        return false; /* duplicate section would leak */
    }
    atp_valence_record *records = NULL;
    if (count > 0u) {
        records = malloc((size_t)count * sizeof(*records));
        if (!records) {
            return false;
        }
    }
    bool ok = true;
    uint32_t previous_index = 0u;
    for (size_t i = 0u; ok && i < (size_t)count; ++i) {
        atp_valence_record *record = &records[i];
        memset(record, 0, sizeof(*record));
        ok = atp_reader_u32(reader, &record->node_index) && record->node_index < graph->node_count &&
             (i == 0u || record->node_index > previous_index) &&
             atp_reader_f32(reader, &record->valence) && record->valence >= -1.0f &&
             record->valence <= 1.0f && atp_reader_u64(reader, &record->event_count) &&
             record->event_count > 0u && atp_reader_u64(reader, &record->positive_events) &&
             atp_reader_u64(reader, &record->negative_events) &&
             atp_reader_u64(reader, &record->last_event_at);
        if (ok) {
            previous_index = record->node_index;
        }
    }
    if (ok) {
        graph->valence_records = records;
        graph->valence_record_count = (size_t)count;
        graph->valence_record_capacity = (size_t)count;
    } else {
        free(records);
        return false;
    }

    uint64_t event_count = 0u;
    const uint64_t min_event = 4u + 4u + 4u + 8u + 4u;
    ok = atp_reader_u64(reader, &event_count) && event_count <= ATPERSON_VALENCE_EVENT_CAPACITY &&
         atp_reader_u64(reader, &graph->valence_event_evictions) &&
         event_count <= (reader->size - reader->position) / min_event;
    if (!ok) {
        return false;
    }
    if (event_count > 0u) {
        graph->valence_events = malloc(ATPERSON_VALENCE_EVENT_CAPACITY * sizeof(*graph->valence_events));
        if (!graph->valence_events) {
            return false;
        }
        graph->valence_event_capacity = ATPERSON_VALENCE_EVENT_CAPACITY;
        graph->valence_event_count = (size_t)event_count;
    }
    for (size_t i = 0u; ok && i < (size_t)event_count; ++i) {
        atp_valence_event_log_entry *entry = &graph->valence_events[i];
        memset(entry, 0, sizeof(*entry));
        ok = atp_reader_u32(reader, &entry->node_index) && entry->node_index < graph->node_count &&
             atp_reader_u32(reader, &entry->kind) && entry->kind >= (uint32_t)ATP_VALENCE_ACTION &&
             entry->kind <= (uint32_t)ATP_VALENCE_AVOID && atp_reader_f32(reader, &entry->signal) &&
             entry->signal >= -1.0f && entry->signal <= 1.0f &&
             atp_reader_u64(reader, &entry->at_epoch) &&
             atp_reader_string_opt(reader, entry->source_id, sizeof(entry->source_id));
    }
    return ok;
}

static bool atp_decode_episodes(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    const uint64_t min_entry = 8u + 8u + 8u + 4u + 8u + 8u + 4u + (4u + 1u) + (4u + 1u);
    if (!atp_reader_u64(reader, &count) || count > SIZE_MAX / sizeof(atp_episode) ||
        count > (reader->size - reader->position) / min_entry) {
        return false;
    }
    if (!atp_reserve_episodes(graph, (size_t)count)) {
        return false;
    }
    for (size_t i = 0u; i < (size_t)count; ++i) {
        atp_episode episode = {0};
        if (!atp_reader_u64(reader, &episode.ledger_id) ||
            !atp_reader_u64(reader, &episode.observed_at) ||
            !atp_reader_u64(reader, &episode.content_digest) ||
            !atp_reader_u32(reader, &episode.schema_version) ||
            !atp_reader_u64(reader, &episode.recall_count) ||
            !atp_reader_u64(reader, &episode.last_recall_at) ||
            !atp_reader_u32(reader, &episode.token_count) ||
            episode.token_count > ATPERSON_EPISODE_SUMMARY_SIZE) {
            return false;
        }
        for (uint32_t t = 0u; t < episode.token_count; ++t) {
            if (!atp_reader_u32(reader, &episode.summary[t].node_index) ||
                episode.summary[t].node_index >= graph->node_count ||
                !atp_reader_f32(reader, &episode.summary[t].weight)) {
                return false;
            }
        }
        if (!atp_reader_string(reader, episode.source_id, sizeof(episode.source_id)) ||
            !atp_reader_string_opt(reader, episode.author_did, sizeof(episode.author_did))) {
            return false;
        }
        graph->episodes[graph->episode_count++] = episode;
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
    bool seen[10] = {false};
    uint32_t learning_schema = 1u;

    while (reader.size - reader.position > 8u) {
        atp_section section;
        if (!atp_reader_section(&reader, &section)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 9u && seen[section.tag]) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 9u) {
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

    if (status) {
        *status = ATP_OK;
    }
    return graph;
}
