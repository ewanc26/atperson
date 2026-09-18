#include "persistence/persistence_internal.h"

/*
 * Snapshot section codecs shared byte-for-byte by the v5 and v6 formats
 * (persistence/sections.h). See the header contract comment for the
 * encoder/decoder invariants.
 */

/* --- Encode --- */

bool atp_encode_edges(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_EDGES)) {
        return false;
    }
    bool ok = atp_buffer_u64(buffer, (uint64_t)graph->edge_count);
    for (size_t i = 0u; ok && i < graph->edge_count; ++i) {
        const atp_edge *edge = &graph->edges[i];
        ok = atp_buffer_u32(buffer, edge->source) && atp_buffer_u32(buffer, edge->target) &&
             atp_buffer_u64(buffer, edge->observations) &&
             atp_buffer_u64(buffer, edge->last_source_hash) &&
             atp_buffer_f32(buffer, edge->strength);
    }
    atp_section_end(&section);
    return ok;
}

bool atp_encode_ledger(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_LEDGER)) {
        return false;
    }
    bool ok = atp_buffer_u64(buffer, (uint64_t)graph->ledger_count);
    for (size_t i = 0u; ok && i < graph->ledger_count; ++i) {
        const atp_ledger_entry *entry = &graph->ledger_entries[i];
        ok = atp_buffer_string(buffer, entry->source_id) &&
             atp_buffer_string_opt(buffer, entry->author_did) &&
             atp_buffer_u64(buffer, entry->observed_at) &&
             atp_buffer_u64(buffer, entry->content_digest) &&
             atp_buffer_u32(buffer, entry->schema_version) &&
             atp_buffer_u32(buffer, (uint32_t)entry->outcome) &&
             atp_buffer_u64(buffer, entry->id);
    }
    atp_section_end(&section);
    return ok;
}

/* Conversation-context section (issue #24): one row per mirrored ledger
 * entry, in mirror order. Rows with all-empty URIs are still written so the
 * row index is always the ledger mirror index — no side table. */
bool atp_encode_context(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_CONTEXT)) {
        return false;
    }
    bool ok = atp_buffer_u64(buffer, (uint64_t)graph->ledger_count);
    for (size_t i = 0u; ok && i < graph->ledger_count; ++i) {
        const atp_conversation_context *context = &graph->ledger_contexts[i];
        ok = atp_buffer_string_opt(buffer, context->reply_root_uri) &&
             atp_buffer_string_opt(buffer, context->reply_parent_uri) &&
             atp_buffer_string_opt(buffer, context->quote_uri);
    }
    atp_section_end(&section);
    return ok;
}

bool atp_encode_valence(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_VALENCE)) {
        return false;
    }
    bool ok = atp_buffer_u64(buffer, (uint64_t)graph->valence_record_count);
    for (size_t i = 0u; ok && i < graph->valence_record_count; ++i) {
        const atp_valence_record *record = &graph->valence_records[i];
        ok = atp_buffer_u32(buffer, record->node_index) &&
             atp_buffer_f32(buffer, record->valence) &&
             atp_buffer_u64(buffer, record->event_count) &&
             atp_buffer_u64(buffer, record->positive_events) &&
             atp_buffer_u64(buffer, record->negative_events) &&
             atp_buffer_u64(buffer, record->last_event_at);
    }
    ok = ok && atp_buffer_u64(buffer, (uint64_t)graph->valence_event_count) &&
         atp_buffer_u64(buffer, graph->valence_event_evictions);
    for (size_t i = 0u; ok && i < graph->valence_event_count; ++i) {
        const atp_valence_event_log_entry *entry = &graph->valence_events[i];
        ok = atp_buffer_u32(buffer, entry->node_index) && atp_buffer_u32(buffer, entry->kind) &&
             atp_buffer_f32(buffer, entry->signal) && atp_buffer_u64(buffer, entry->at_epoch) &&
             atp_buffer_string_opt(buffer, entry->source_id);
    }
    atp_section_end(&section);
    return ok;
}

bool atp_encode_schema(atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_SCHEMA)) {
        return false;
    }
    const bool ok = atp_buffer_u32(buffer, ATPERSON_SCHEMA_VERSION);
    atp_section_end(&section);
    return ok;
}

bool atp_encode_nodes(const atp_graph *graph, atp_buffer *buffer, bool with_importance) {
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
            if (ok && with_importance) {
                ok = atp_buffer_f32(buffer, node->embedding_importance[d]);
            }
        }
    }
    atp_section_end(&section);
    return ok;
}

bool atp_encode_episodes(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_EPISODES)) {
        return false;
    }
    bool ok = atp_buffer_u64(buffer, (uint64_t)graph->episode_count);
    for (size_t i = 0u; ok && i < graph->episode_count; ++i) {
        const atp_episode *episode = &graph->episodes[i];
        ok = atp_buffer_u64(buffer, episode->ledger_id) &&
             atp_buffer_u64(buffer, episode->observed_at) &&
             atp_buffer_u64(buffer, episode->content_digest) &&
             atp_buffer_u32(buffer, episode->schema_version) &&
             atp_buffer_u64(buffer, episode->recall_count) &&
             atp_buffer_u64(buffer, episode->last_recall_at) &&
             atp_buffer_u32(buffer, episode->token_count);
        for (uint32_t t = 0u; ok && t < episode->token_count; ++t) {
            ok = atp_buffer_u32(buffer, episode->summary[t].node_index) &&
                 atp_buffer_f32(buffer, episode->summary[t].weight);
        }
        ok = ok && atp_buffer_string(buffer, episode->source_id) &&
             atp_buffer_string_opt(buffer, episode->author_did);
    }
    atp_section_end(&section);
    return ok;
}

bool atp_snapshot_write(const char *path, atp_buffer *buffer) {
    const size_t path_len = strlen(path);
    char *temporary = malloc(path_len + sizeof(".tmp"));
    if (!temporary) {
        free(buffer->data);
        return false;
    }
    memcpy(temporary, path, path_len);
    memcpy(temporary + path_len, ".tmp", sizeof(".tmp"));

    FILE *file = fopen(temporary, "wb");
    if (!file) {
        free(temporary);
        free(buffer->data);
        return false;
    }
    const bool written = fwrite(buffer->data, 1u, buffer->size, file) == buffer->size;
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !flushed || !closed || rename(temporary, path) != 0) {
        remove(temporary);
        free(temporary);
        free(buffer->data);
        return false;
    }
    free(temporary);
    free(buffer->data);
    return true;
}

/* --- Decode --- */

bool atp_decode_edges(atp_reader *reader, atp_graph *graph) {
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

bool atp_decode_ledger(atp_reader *reader, atp_graph *graph) {
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
 * Conversation-context section (issue #24): one row per mirrored ledger
 * entry, in mirror order, each carrying reply root/parent and quote URIs.
 * Row indexes are validated against the already-decoded ledger mirror; the
 * loader contracts that the LEDGER section precedes CONTEXT. Absent section
 * = empty context (pre-#24 snapshots load with no thread metadata).
 */
bool atp_decode_context(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    const uint64_t min_row = 4u + 1u + 4u + 1u + 4u + 1u;
    if (!atp_reader_u64(reader, &count) || count > graph->ledger_count ||
        count > (reader->size - reader->position) / min_row) {
        return false;
    }
    for (size_t i = 0u; i < (size_t)count; ++i) {
        atp_conversation_context context = {0};
        if (!atp_reader_string_opt(reader, context.reply_root_uri,
                                   sizeof(context.reply_root_uri)) ||
            !atp_reader_string_opt(reader, context.reply_parent_uri,
                                   sizeof(context.reply_parent_uri)) ||
            !atp_reader_string_opt(reader, context.quote_uri, sizeof(context.quote_uri))) {
            return false;
        }
        graph->ledger_contexts[i] = context;
    }
    return true;
}

/*
 * Valence section (issue #13): folded per-token records plus the bounded
 * provenance log. Node indexes are validated against the already-decoded
 * node table; the loader contracts that NODES precedes VALENCE. Absent
 * section = empty valence (pre-#13 snapshots load neutral).
 */
bool atp_decode_valence(atp_reader *reader, atp_graph *graph) {
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
        graph->valence_events =
            malloc(ATPERSON_VALENCE_EVENT_CAPACITY * sizeof(*graph->valence_events));
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
             atp_reader_u32(reader, &entry->kind) &&
             entry->kind >= (uint32_t)ATP_VALENCE_ACTION &&
             entry->kind <= (uint32_t)ATP_VALENCE_AVOID && atp_reader_f32(reader, &entry->signal) &&
             entry->signal >= -1.0f && entry->signal <= 1.0f &&
             atp_reader_u64(reader, &entry->at_epoch) &&
             atp_reader_string_opt(reader, entry->source_id, sizeof(entry->source_id));
    }
    return ok;
}

bool atp_decode_episodes(atp_reader *reader, atp_graph *graph) {
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

bool atp_decode_nodes(atp_reader *reader, atp_graph *graph, bool with_importance) {
    uint64_t count = 0u;
    const uint64_t per_dimension = with_importance ? 8u : 4u;
    const uint64_t min_entry =
        4u + 8u + 4u + (uint64_t)graph->neural_architecture.embedding_dim * per_dimension + 1u;
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
            if (with_importance && !atp_reader_f32(reader, &node->embedding_importance[d])) {
                return false;
            }
        }
    }
    return true;
}
