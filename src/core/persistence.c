#include "internal.h"
#include "portable_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Snapshot format v5: portable, framed, digest-checked.
 *
 * Wire format:
 *   magic "ATPERSN5" (8 bytes) | version u32le (= 5)
 *   then a sequence of sections, each: tag u32le | length u64le | payload
 *   then a trailing u64le: FNV-1a 64 digest over every byte before it.
 *
 * Sections are bounds-checked against the remaining file size before any
 * allocation, and unknown tags are skipped, so newer writers can add
 * sections without breaking older readers. Integers are little-endian and
 * floats are IEEE 754 bit patterns regardless of host architecture.
 *
 * Historical versions: v1-v4 were host-oriented. v4 files load portably
 * (every v4 writer in practice ran on a little-endian host) and migrate
 * to v5 on the next save; v1-v3 are refused with ATP_ERR_FORMAT rather
 * than silently reinterpreted.
 *
 * Saves remain atomic: encode fully in memory, write to <path>.tmp once,
 * flush, rename. A failure at any point leaves the old file untouched.
 */

#define ATP_SNAPSHOT_MAGIC "ATPERSN5"
#define ATP_SNAPSHOT_MAGIC_V4 "ATPERSN1"

/* Section tags. 1-8 are the v5 baseline; unknown tags are skipped. */
#define ATP_SECTION_HEADER 1u        /* dims, config, rng state, counters */
#define ATP_SECTION_NETWORK 2u       /* neural parameters (fixed layout) */
#define ATP_SECTION_NODES 3u        /* vocabulary nodes + familiarity */
#define ATP_SECTION_EDGES 4u        /* directed associations */
#define ATP_SECTION_LEDGER 5u       /* mirrored observation ledger */
#define ATP_SECTION_EPISODES 6u    /* episodic memory */
#define ATP_SECTION_FAMILIARITY 7u /* per-node familiarity (split out) */
#define ATP_SECTION_SCHEMA 8u      /* learning schema that produced this state */

/* ---- Growable in-memory buffer; encoded fully, then written once. ---- */

typedef struct atp_buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
} atp_buffer;

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

/* Length-prefixed string that may be empty (author fields are optional). */
static bool atp_buffer_string_opt(atp_buffer *buffer, const char *text) {
    const size_t length = strlen(text);
    if (length > UINT32_MAX) {
        return false;
    }
    return atp_buffer_u32(buffer, (uint32_t)length) && atp_buffer_put(buffer, text, length);
}

/* Section framing with back-patched length. */
typedef struct atp_section_writer {
    atp_buffer *buffer;
    size_t length_offset;
} atp_section_writer;

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

/* ---- v5 encode ---- */

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
    atp_section_writer writer_section;
    if (!atp_section_begin(buffer, &writer_section, ATP_SECTION_EDGES)) {
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
    atp_section_end(&writer_section);
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

static bool atp_encode_schema(const atp_graph *graph, atp_buffer *buffer) {
    atp_section_writer section;
    if (!atp_section_begin(buffer, &section, ATP_SECTION_SCHEMA)) {
        return false;
    }
    /* The learning schema that produced this state. A graph trained under
     * schema N must not be extended by schema M != N code: the section is
     * absent only in v5 snapshots written before section 8 existed, which
     * are all schema 1. */
    const bool ok = atp_buffer_u32(buffer, ATPERSON_SCHEMA_VERSION);
    atp_section_end(&section);
    (void)graph;
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
        atp_encode_schema(graph, &buffer) &&
        atp_buffer_u64(&buffer, atp_fnv1a64(buffer.data, buffer.size));
    if (!encoded) {
        free(buffer.data);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    /* Atomic save: the fully encoded image is written to <path>.tmp, then
     * renamed over the destination, so a crash mid-save never produces a
     * half-written snapshot. */
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

/* ---- v5 decode ---- */

typedef struct atp_reader {
    const unsigned char *data;
    size_t size;
    size_t position;
} atp_reader;

static bool atp_reader_take(atp_reader *reader, size_t count, const unsigned char **out) {
    if (count > reader->size - reader->position) {
        return false;
    }
    *out = reader->data + reader->position;
    reader->position += count;
    return true;
}

static bool atp_reader_u32(atp_reader *reader, uint32_t *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 4u, &bytes)) {
        return false;
    }
    *value = atp_load_u32le(bytes);
    return true;
}

static bool atp_reader_u64(atp_reader *reader, uint64_t *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 8u, &bytes)) {
        return false;
    }
    *value = atp_load_u64le(bytes);
    return true;
}

static bool atp_reader_f32(atp_reader *reader, float *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 4u, &bytes)) {
        return false;
    }
    *value = atp_load_f32le(bytes);
    return true;
}

static bool atp_reader_f64(atp_reader *reader, double *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 8u, &bytes)) {
        return false;
    }
    *value = atp_load_f64le(bytes);
    return true;
}

static bool atp_reader_string(atp_reader *reader, char *out, size_t capacity) {
    uint32_t length = 0u;
    const unsigned char *bytes;
    if (!atp_reader_u32(reader, &length) || length == 0u || length >= capacity ||
        !atp_reader_take(reader, length, &bytes)) {
        return false;
    }
    memcpy(out, bytes, length);
    out[length] = '\0';
    return true;
}

/* Length-prefixed string that may be empty (author fields are optional). */
static bool atp_reader_string_opt(atp_reader *reader, char *out, size_t capacity) {
    uint32_t length = 0u;
    const unsigned char *bytes;
    if (!atp_reader_u32(reader, &length) || length >= capacity ||
        !atp_reader_take(reader, length, &bytes)) {
        return false;
    }
    memcpy(out, bytes, length);
    out[length] = '\0';
    return true;
}

/* Read a section header and bound its length against the remaining bytes.
 * Returns false on truncation or an over-long section. */
static bool atp_reader_section(atp_reader *reader, atp_section *section) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 12u, &bytes) || !atp_load_section(bytes, 12u, section)) {
        return false;
    }
    return section->length <= reader->size - reader->position;
}

static atp_graph *atp_load_failure(atp_graph *graph, atp_status *status, atp_status failure) {
    atp_graph_destroy(graph);
    if (status) {
        *status = failure;
    }
    return NULL;
}

/* Decode the HEADER section before the graph exists. */
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

static bool atp_decode_network(atp_reader *reader, atp_graph *graph) {
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
    /* Each node needs at least: len u32 + observations u64 + familiarity f32
     * + embedding (DIM * f32) + one token byte. Corrupt counts must be
     * refused before the reserve call, not discovered on a huge malloc. */
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
    /* Each edge is exactly 28 bytes on the wire. */
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
    /* Each ledger entry needs at least: two length-prefixed strings with
     * one byte each, plus 8+8+4+4+8 fixed fields. */
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

static bool atp_decode_episodes(atp_reader *reader, atp_graph *graph) {
    uint64_t count = 0u;
    /* Each episode needs at least the fixed fields plus two length-
     * prefixed strings with one byte each. */
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

static atp_graph *atp_load_v5(const unsigned char *data, size_t size, atp_status *status) {
    atp_reader reader = {data, size, 12u}; /* past magic + version */
    atp_graph *graph = NULL;
    bool seen[9] = {false};
    uint32_t learning_schema = 1u; /* absent section means pre-section-8 v5 */

    /* Sections may appear in any order; unknown tags are skipped. */
    while (reader.size - reader.position > 8u) {
        atp_section section;
        if (!atp_reader_section(&reader, &section)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 8u && seen[section.tag]) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (section.tag != 0u && section.tag <= 8u) {
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
                break; /* duplicate header */
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
        case ATP_SECTION_SCHEMA: {
            /* The learning schema that produced this state. Refuse foreign
             * schemas: extending a graph trained under one algorithm with
             * another would ambiguously mix models. Rebuild from the
             * ledger (or start a new generation) instead. */
            uint32_t schema = 0u;
            ok = atp_reader_u32(&reader, &schema) && schema > 0u;
            if (ok) {
                learning_schema = schema;
            }
            break;
        }
        case ATP_SECTION_FAMILIARITY: {
            /* v5 kept familiarity inside NODES; a separate section would be
             * a later extension. Skip it. */
            reader.position = payload_end;
            ok = true;
            break;
        }
        default:
            /* Unknown tag from a newer writer: skip, bounds already checked. */
            reader.position = payload_end;
            ok = true;
            break;
        }
        if (!ok) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        if (reader.position != payload_end) {
            /* Known sections must consume their payload exactly. */
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
    }

    if (!graph || !seen[ATP_SECTION_HEADER] || !seen[ATP_SECTION_NETWORK] ||
        !seen[ATP_SECTION_NODES] || !seen[ATP_SECTION_EDGES] || !seen[ATP_SECTION_LEDGER] ||
        !seen[ATP_SECTION_EPISODES]) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }

    /* A snapshot trained under a learning schema this core cannot replay
     * must not be extended by the current algorithm: the state is intact,
     * the model generation is the mismatch. */
    if (!atp_schema_can_replay(learning_schema)) {
        return atp_load_failure(graph, status, ATP_ERR_SCHEMA);
    }

    /* Trailing digest over everything before it. */
    uint64_t stored_digest = 0u;
    if (reader.size - reader.position != 8u || !atp_reader_u64(&reader, &stored_digest) ||
        stored_digest != atp_fnv1a64(data, size - 8u)) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }

    /* Indexes are derived state; rebuild after the arrays are complete. */
    if (!atp_graph_rebuild_indexes(graph)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }

    if (status) {
        *status = ATP_OK;
    }
    return graph;
}

/* ---- v4 decode (host-oriented, read as little-endian) ---- */

static atp_graph *atp_load_v4(const unsigned char *data, size_t size, atp_status *status) {
    atp_reader reader = {data, size, 12u}; /* past magic + version */
    uint32_t embedding_dim = 0u;
    uint32_t hidden_dim = 0u;
    atp_graph_config config = {0};
    atp_graph *graph = NULL;

    /* v4 header: dims, seed, lr, decay, then counters and counts. */
    if (!atp_reader_u32(&reader, &embedding_dim) || embedding_dim != ATPERSON_EMBEDDING_DIM ||
        !atp_reader_u32(&reader, &hidden_dim) || hidden_dim != ATPERSON_HIDDEN_DIM ||
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
    /* v4 node: len u32 + observations u64 + embedding raw + token byte;
     * v4 edge: 28 bytes. Bound counts before reserving. */
    const uint64_t v4_min_node = 4u + 8u + (uint64_t)ATPERSON_EMBEDDING_DIM * 4u + 1u;
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
    /* v4 wrote the network struct raw; v5 decodes it field by field. The
     * struct layout is floats only, so the byte count matches. */
    if (!atp_decode_network(&reader, graph)) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }

    if (!atp_reserve_nodes(graph, (size_t)node_count) ||
        !atp_reserve_edges(graph, (size_t)edge_count)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }

    /* v4 node layout: len u32, observations u64, embedding raw, token bytes. */
    for (size_t i = 0u; i < (size_t)node_count; ++i) {
        atp_node *node = &graph->nodes[graph->node_count];
        memset(node, 0, sizeof(*node));
        uint32_t length = 0u;
        const unsigned char *bytes;
        if (!atp_reader_u32(&reader, &length) || length == 0u ||
            length >= ATPERSON_TOKEN_BYTES ||
            !atp_reader_u64(&reader, &node->observations) ||
            !atp_reader_take(&reader, sizeof(node->embedding), &bytes)) {
            return atp_load_failure(graph, status, ATP_ERR_FORMAT);
        }
        for (size_t d = 0u; d < ATPERSON_EMBEDDING_DIM; ++d) {
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
        graph->node_count++;
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

    /* v4 ledger mirror. */
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

    /* v4 episodes. */
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

    /* v4 trailing familiarity block (one float per node). */
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

    /* v4 did not persist episode_evictions; it starts at zero and resumes
     * counting on the migrated graph. */

    /* Indexes are derived state; rebuild after the arrays are complete. */
    if (!atp_graph_rebuild_indexes(graph)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }

    if (status) {
        *status = ATP_OK;
    }
    return graph;
}

/* ---- Public load ---- */

atp_graph *atp_graph_load(const char *path, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (!path || path[0] == '\0') {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    const long file_size_long = ftell(file);
    if (file_size_long < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    const size_t file_size = (size_t)file_size_long;

    /* Refuse implausible files before allocating: magic + version + one
     * header section + digest is the theoretical minimum. */
    if (file_size < 8u + 4u + 12u + 8u) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    unsigned char *data = malloc(file_size);
    if (!data) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }
    if (fread(data, 1u, file_size, file) != file_size) {
        free(data);
        fclose(file);
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    fclose(file);

    atp_graph *graph = NULL;
    if (memcmp(data, ATP_SNAPSHOT_MAGIC, 8u) == 0) {
        const uint32_t version = atp_load_u32le(data + 8u);
        if (version != ATPERSON_SNAPSHOT_VERSION) {
            free(data);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
        graph = atp_load_v5(data, file_size, status);
    } else if (memcmp(data, ATP_SNAPSHOT_MAGIC_V4, 8u) == 0) {
        const uint32_t version = atp_load_u32le(data + 8u);
        if (version != 4u) {
            /* v1-v3 shared the v1 magic but different layouts; refuse. */
            free(data);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
        graph = atp_load_v4(data, file_size, status);
    } else {
        free(data);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    free(data);
    return graph;
}
