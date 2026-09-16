#include "internal.h"
#include "persistence/format.h"
#include "portable_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Current snapshot encoding and atomic save.
 *
 * Snapshot v5 is encoded fully into a growable caller-local buffer before any
 * destination file is replaced. Allocation failure aborts before I/O; write,
 * flush, close, or rename failure removes the temporary file and leaves the
 * previous snapshot untouched.
 */
typedef struct atp_buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
} atp_buffer;

typedef struct atp_section_writer {
    atp_buffer *buffer;
    size_t length_offset;
} atp_section_writer;

static bool atp_buffer_reserve(atp_buffer *buffer, size_t needed) {
    if (needed <= buffer->capacity) {
        return true;
    }
    size_t capacity = buffer->capacity == 0u ? 4096u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    unsigned char *grown = realloc(buffer->data, capacity);
    if (!grown) {
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static bool atp_buffer_put(atp_buffer *buffer, const void *data, size_t size) {
    if (!atp_buffer_reserve(buffer, buffer->size + size)) {
        return false;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return true;
}

static bool atp_buffer_u32(atp_buffer *buffer, uint32_t value) {
    unsigned char encoded[4];
    atp_store_u32le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

static bool atp_buffer_u64(atp_buffer *buffer, uint64_t value) {
    unsigned char encoded[8];
    atp_store_u64le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

static bool atp_buffer_f32(atp_buffer *buffer, float value) {
    unsigned char encoded[4];
    atp_store_f32le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

static bool atp_buffer_f64(atp_buffer *buffer, double value) {
    unsigned char encoded[8];
    atp_store_f64le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

static bool atp_buffer_string(atp_buffer *buffer, const char *text) {
    const size_t length = strlen(text);
    if (length == 0u || length > UINT32_MAX) {
        return false;
    }
    return atp_buffer_u32(buffer, (uint32_t)length) && atp_buffer_put(buffer, text, length);
}

static bool atp_buffer_string_opt(atp_buffer *buffer, const char *text) {
    const size_t length = strlen(text);
    if (length > UINT32_MAX) {
        return false;
    }
    return atp_buffer_u32(buffer, (uint32_t)length) && atp_buffer_put(buffer, text, length);
}

static bool atp_section_begin(atp_buffer *buffer, atp_section_writer *section, uint32_t tag) {
    unsigned char header[12];
    section->buffer = buffer;
    section->length_offset = buffer->size + 4u;
    atp_store_section(header, tag, 0u);
    return atp_buffer_put(buffer, header, sizeof(header));
}

static void atp_section_end(const atp_section_writer *section) {
    unsigned char encoded[8];
    const uint64_t length = (uint64_t)(section->buffer->size - section->length_offset - 8u);
    atp_store_u64le(encoded, length);
    memcpy(section->buffer->data + section->length_offset, encoded, sizeof(encoded));
}

static bool atp_encode_header(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_HEADER)) {
        return false;
    }
    const bool ok = atp_buffer_u32(buffer, ATPERSON_EMBEDDING_DIM) &&
                    atp_buffer_u32(buffer, ATPERSON_HIDDEN_DIM) &&
                    atp_buffer_u32(buffer, ATPERSON_INPUT_DIM) &&
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
    bool ok = true;
    for (size_t h = 0u; ok && h < ATPERSON_HIDDEN_DIM; ++h) {
        for (size_t i = 0u; ok && i < ATPERSON_INPUT_DIM; ++i) {
            ok = atp_buffer_f32(buffer, graph->network.input_hidden[h][i]);
        }
    }
    for (size_t h = 0u; ok && h < ATPERSON_HIDDEN_DIM; ++h) {
        ok = atp_buffer_f32(buffer, graph->network.hidden_bias[h]);
    }
    for (size_t h = 0u; ok && h < ATPERSON_HIDDEN_DIM; ++h) {
        ok = atp_buffer_f32(buffer, graph->network.hidden_output[h]);
    }
    ok = ok && atp_buffer_f32(buffer, graph->network.output_bias);
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
        for (size_t d = 0u; ok && d < ATPERSON_EMBEDDING_DIM; ++d) {
            ok = atp_buffer_f32(buffer, node->embedding[d]);
        }
    }
    atp_section_end(&section);
    return ok;
}

static bool atp_encode_edges(const atp_graph *graph, atp_buffer *buffer) {
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

static bool atp_encode_ledger(const atp_graph *graph, atp_buffer *buffer) {
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

static bool atp_encode_schema(atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_SCHEMA)) {
        return false;
    }
    const bool ok = atp_buffer_u32(buffer, ATPERSON_SCHEMA_VERSION);
    atp_section_end(&section);
    return ok;
}

static bool atp_encode_episodes(const atp_graph *graph, atp_buffer *buffer) {
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

atp_status atp_graph_save(const atp_graph *graph, const char *path) {
    if (!graph || !path || path[0] == '\0') {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_buffer buffer = {0};
    const bool encoded =
        atp_buffer_put(&buffer, ATP_SNAPSHOT_MAGIC, sizeof(ATP_SNAPSHOT_MAGIC) - 1u) &&
        atp_buffer_u32(&buffer, ATPERSON_SNAPSHOT_VERSION) &&
        atp_encode_header(graph, &buffer) && atp_encode_network(graph, &buffer) &&
        atp_encode_nodes(graph, &buffer) && atp_encode_edges(graph, &buffer) &&
        atp_encode_ledger(graph, &buffer) && atp_encode_episodes(graph, &buffer) &&
        atp_encode_schema(&buffer) &&
        atp_buffer_u64(&buffer, atp_fnv1a64(buffer.data, buffer.size));
    if (!encoded) {
        free(buffer.data);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    const size_t path_len = strlen(path);
    char *temporary = malloc(path_len + sizeof(".tmp"));
    if (!temporary) {
        free(buffer.data);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    memcpy(temporary, path, path_len);
    memcpy(temporary + path_len, ".tmp", sizeof(".tmp"));

    FILE *file = fopen(temporary, "wb");
    if (!file) {
        free(temporary);
        free(buffer.data);
        return ATP_ERR_IO;
    }
    const bool written = fwrite(buffer.data, 1u, buffer.size, file) == buffer.size;
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !flushed || !closed || rename(temporary, path) != 0) {
        remove(temporary);
        free(temporary);
        free(buffer.data);
        return ATP_ERR_IO;
    }
    free(temporary);
    free(buffer.data);
    return ATP_OK;
}
