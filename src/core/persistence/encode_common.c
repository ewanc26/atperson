#include "persistence/encode_common.h"
#include "persistence/format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Format-agnostic snapshot sections and the atomic rename write
 * (persistence/encode_common.h). v5 and v6 share these exactly; only the
 * header/architecture/network/node encoders differ between formats.
 */

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