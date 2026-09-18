#include "persistence/migration.h"
#include "persistence/decode.h"
#include "persistence/reader.h"
#include "io/portable.h"

#include <stdlib.h>
#include <string.h>

/*
 * Snapshot v4 compatibility decoder.
 *
 * v4 was host-oriented but all historical writers used the little-endian
 * layout decoded here. Successful migration produces current in-memory state;
 * the next save writes v5. Partial graphs are destroyed on every failure.
 */
atp_graph *atp_load_v4(const unsigned char *data, size_t size, atp_status *status) {
    atp_reader reader = {data, size, 12u};
    uint32_t embedding_dim = 0u;
    uint32_t hidden_dim = 0u;
    atp_graph_config config = {0};
    atp_graph *graph = NULL;

    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    if (!atp_reader_u32(&reader, &embedding_dim) || embedding_dim != legacy.embedding_dim ||
        !atp_reader_u32(&reader, &hidden_dim) || hidden_dim != legacy.hidden_widths[0] ||
        !atp_reader_u64(&reader, &config.seed) ||
        !atp_reader_f32(&reader, &config.learning_rate) ||
        !atp_reader_f32(&reader, &config.familiarity_decay) ||
        !(config.familiarity_decay > 0.0f) || !(config.familiarity_decay < 1.0f)) {
        return atp_load_failure(NULL, status, ATP_ERR_FORMAT);
    }
    graph = atp_graph_create(&config);
    if (!graph) {
        return atp_load_failure(NULL, status, ATP_ERR_OUT_OF_MEMORY);
    }

    uint64_t node_count = 0u;
    uint64_t edge_count = 0u;
    const uint64_t v4_min_node =
        4u + 8u + (uint64_t)graph->neural_architecture.embedding_dim * 4u + 1u;
    if (!atp_reader_u64(&reader, &graph->rng_state) ||
        !atp_reader_u64(&reader, &graph->observations) ||
        !atp_reader_u64(&reader, &graph->token_observations) ||
        !atp_reader_u64(&reader, &graph->training_steps) ||
        !atp_reader_f64(&reader, &graph->loss_total) ||
        !atp_reader_u64(&reader, &node_count) || !atp_reader_u64(&reader, &edge_count) ||
        node_count > SIZE_MAX / sizeof(atp_node) || edge_count > SIZE_MAX / sizeof(atp_edge) ||
        node_count > (reader.size - reader.position) / v4_min_node ||
        edge_count > (reader.size - reader.position) / 28u) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }

    if (!atp_decode_network(&reader, graph)) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    if (!atp_reserve_nodes(graph, (size_t)node_count) ||
        !atp_reserve_edges(graph, (size_t)edge_count)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }

    for (size_t i = 0u; i < (size_t)node_count; ++i) {
        atp_node *node = &graph->nodes[graph->node_count];
        memset(node, 0, sizeof(*node));
        uint32_t length = 0u;
        const unsigned char *bytes;
        if (!atp_reader_u32(&reader, &length) || length == 0u ||
            length >= ATPERSON_TOKEN_BYTES ||
            !atp_reader_u64(&reader, &node->observations)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (!atp_node_allocate_vectors(graph, node)) {
            return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
        }
        graph->node_count++;
        const size_t embedding_bytes =
            (size_t)graph->neural_architecture.embedding_dim * sizeof(float);
        if (!atp_reader_take(&reader, embedding_bytes, &bytes)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        for (size_t d = 0u; d < graph->neural_architecture.embedding_dim; ++d) {
            node->embedding[d] = atp_load_f32le(bytes + 4u * d);
        }
        if (!atp_reader_take(&reader, length, &bytes)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        node->token = malloc((size_t)length + 1u);
        if (!node->token) {
            return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
        }
        memcpy(node->token, bytes, length);
        node->token[length] = '\0';
    }

    for (size_t i = 0u; i < (size_t)edge_count; ++i) {
        atp_edge *edge = &graph->edges[graph->edge_count];
        if (!atp_reader_u32(&reader, &edge->source) || !atp_reader_u32(&reader, &edge->target) ||
            !atp_reader_u64(&reader, &edge->observations) ||
            !atp_reader_u64(&reader, &edge->last_source_hash) ||
            !atp_reader_f32(&reader, &edge->strength) || edge->source >= graph->node_count ||
            edge->target >= graph->node_count) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        graph->edge_count++;
    }

    uint64_t ledger_count = 0u;
    const uint64_t v4_min_ledger = (4u + 1u) + (4u + 1u) + 8u + 8u + 4u + 4u + 8u;
    if (!atp_reader_u64(&reader, &ledger_count) ||
        ledger_count > SIZE_MAX / sizeof(atp_ledger_entry) ||
        ledger_count > (reader.size - reader.position) / v4_min_ledger ||
        !atp_reserve_ledger_entries(graph, (size_t)ledger_count)) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    for (size_t i = 0u; i < (size_t)ledger_count; ++i) {
        atp_ledger_entry entry = {0};
        uint32_t outcome = 0u;
        const unsigned char *bytes;
        uint32_t source_len = 0u;
        uint32_t author_len = 0u;
        if (!atp_reader_u32(&reader, &source_len) || source_len == 0u ||
            source_len >= ATPERSON_LEDGER_SOURCE_BYTES ||
            !atp_reader_take(&reader, source_len, &bytes)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        memcpy(entry.source_id, bytes, source_len);
        entry.source_id[source_len] = '\0';
        if (!atp_reader_u32(&reader, &author_len) || author_len >= ATPERSON_LEDGER_AUTHOR_BYTES ||
            !atp_reader_take(&reader, author_len, &bytes)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        memcpy(entry.author_did, bytes, author_len);
        entry.author_did[author_len] = '\0';
        if (!atp_reader_u64(&reader, &entry.observed_at) ||
            !atp_reader_u64(&reader, &entry.content_digest) ||
            !atp_reader_u32(&reader, &entry.schema_version) ||
            !atp_reader_u32(&reader, &outcome) ||
            outcome > (uint32_t)ATP_LEDGER_OUTCOME_WITHDRAWN ||
            !atp_reader_u64(&reader, &entry.id)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        entry.outcome = (atp_ledger_outcome)outcome;
        graph->ledger_entries[graph->ledger_count] = entry;
        graph->ledger_count++;
    }

    uint64_t episode_count = 0u;
    const uint64_t v4_min_episode = 8u + 8u + 8u + 4u + 8u + 8u + 4u + (4u + 1u) + (4u + 1u);
    if (!atp_reader_u64(&reader, &episode_count) ||
        episode_count > SIZE_MAX / sizeof(atp_episode) ||
        episode_count > (reader.size - reader.position) / v4_min_episode ||
        !atp_reserve_episodes(graph, (size_t)episode_count)) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    for (size_t i = 0u; i < (size_t)episode_count; ++i) {
        atp_episode episode = {0};
        const unsigned char *bytes;
        uint32_t source_len = 0u;
        uint32_t author_len = 0u;
        if (!atp_reader_u64(&reader, &episode.ledger_id) ||
            !atp_reader_u64(&reader, &episode.observed_at) ||
            !atp_reader_u64(&reader, &episode.content_digest) ||
            !atp_reader_u32(&reader, &episode.schema_version) ||
            !atp_reader_u64(&reader, &episode.recall_count) ||
            !atp_reader_u64(&reader, &episode.last_recall_at) ||
            !atp_reader_u32(&reader, &episode.token_count) ||
            episode.token_count > ATPERSON_EPISODE_SUMMARY_SIZE) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        for (uint32_t t = 0u; t < episode.token_count; ++t) {
            if (!atp_reader_u32(&reader, &episode.summary[t].node_index) ||
                episode.summary[t].node_index >= graph->node_count ||
                !atp_reader_f32(&reader, &episode.summary[t].weight)) {
                return atp_load_failure(graph, status, ATP_ERR_FORMAT);
            }
        }
        if (!atp_reader_u32(&reader, &source_len) || source_len == 0u ||
            source_len >= ATPERSON_LEDGER_SOURCE_BYTES ||
            !atp_reader_take(&reader, source_len, &bytes)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        memcpy(episode.source_id, bytes, source_len);
        episode.source_id[source_len] = '\0';
        if (!atp_reader_u32(&reader, &author_len) || author_len >= ATPERSON_LEDGER_AUTHOR_BYTES ||
            !atp_reader_take(&reader, author_len, &bytes)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        memcpy(episode.author_did, bytes, author_len);
        episode.author_did[author_len] = '\0';
        graph->episodes[graph->episode_count++] = episode;
    }

    uint32_t familiarity_count = 0u;
    if (!atp_reader_u32(&reader, &familiarity_count) ||
        (size_t)familiarity_count != graph->node_count) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    for (size_t i = 0u; i < graph->node_count; ++i) {
        if (!atp_reader_f32(&reader, &graph->nodes[i].familiarity)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
    }

    if (reader.position != reader.size) {
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
