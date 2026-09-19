#ifndef ATPERSON_CORE_PERSISTENCE_INTERNAL_H
#define ATPERSON_CORE_PERSISTENCE_INTERNAL_H

/*
 * Shared internals of the persistence scope.
 *
 * wire.c owns the growable buffer and section framing primitives;
 * reader.c owns bounded byte reads and the shared loader epilogue;
 * sections.c owns the format-agnostic section encoders/decoders;
 * v5.c owns the legacy one-hidden-layer format;
 * v6.c owns the explicit neural architecture format;
 * migration.c owns the historical v4 format decoder;
 * load.c owns public load dispatch and file I/O;
 * save.c owns public save dispatch.
 *
 * This header is scope-private and must not leak into include/atperson/.
 */

#include "internal.h"
#include "io/portable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Wire-format identifiers (formerly format.h) ---
 *
 * This header owns identifiers only; changing these values is a format
 * change, not a refactor.
 *
 * v6 carries the explicit neural architecture descriptor and
 * variable-length network/node payloads. v5 remains the legacy fragment
 * format with its historical one-hidden-layer wire shape; both load as
 * current graphs, v5 at the named legacy architecture. */

#define ATP_SNAPSHOT_MAGIC_V4 "ATPERSN1"
#define ATP_SNAPSHOT_MAGIC_V5 "ATPERSN5"
#define ATP_SNAPSHOT_MAGIC_V6 "ATPERSN6"
#define ATP_SNAPSHOT_MAGIC_V7 "ATPERSN7"
#define ATPERSON_SNAPSHOT_VERSION_V4 4u

#define ATP_SECTION_HEADER 1u
#define ATP_SECTION_NETWORK 2u
#define ATP_SECTION_NODES 3u
#define ATP_SECTION_EDGES 4u
#define ATP_SECTION_LEDGER 5u
#define ATP_SECTION_EPISODES 6u
#define ATP_SECTION_FAMILIARITY 7u
#define ATP_SECTION_SCHEMA 8u
#define ATP_SECTION_VALENCE 9u
#define ATP_SECTION_CONTEXT 10u
/* v6: explicit neural architecture descriptor (issue #72). */
#define ATP_SECTION_ARCH 11u
/* v7: ordered deterministic neural migration history (issue #66). */
#define ATP_SECTION_MIGRATIONS 12u
#define ATP_SECTION_TRACKED_MAX 12u

/* --- Wire buffer (wire.h) --- */

typedef struct atp_buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
} atp_buffer;

typedef struct atp_section_writer {
    atp_buffer *buffer;
    size_t length_offset;
} atp_section_writer;

bool atp_buffer_put(atp_buffer *buffer, const void *data, size_t size);
bool atp_buffer_u32(atp_buffer *buffer, uint32_t value);
bool atp_buffer_u64(atp_buffer *buffer, uint64_t value);
bool atp_buffer_f32(atp_buffer *buffer, float value);
bool atp_buffer_f64(atp_buffer *buffer, double value);
bool atp_buffer_string(atp_buffer *buffer, const char *text);
bool atp_buffer_string_opt(atp_buffer *buffer, const char *text);

bool atp_section_begin(atp_buffer *buffer, atp_section_writer *section, uint32_t tag);
void atp_section_end(const atp_section_writer *section);

/* --- Bounded reader (reader.h) --- */

typedef struct atp_reader {
    const unsigned char *data;
    size_t size;
    size_t position;
} atp_reader;

/* atp_section is defined by io/portable.h and shared by the reader. */

bool atp_reader_take(atp_reader *reader, size_t count, const unsigned char **out);
bool atp_reader_u32(atp_reader *reader, uint32_t *value);
bool atp_reader_u64(atp_reader *reader, uint64_t *value);
bool atp_reader_f32(atp_reader *reader, float *value);
bool atp_reader_f64(atp_reader *reader, double *value);
bool atp_reader_string(atp_reader *reader, char *out, size_t capacity);
bool atp_reader_string_opt(atp_reader *reader, char *out, size_t capacity);
bool atp_reader_section(atp_reader *reader, atp_section *section);

bool atp_reader_next_section(atp_reader *reader, bool seen[ATP_SECTION_TRACKED_MAX + 1u], uint32_t max_tag,
                             atp_section *section, size_t *payload_end);

atp_graph *atp_load_finish(atp_graph *graph, const bool seen[ATP_SECTION_TRACKED_MAX + 1u], uint32_t required_mask,
                           uint32_t learning_schema, atp_reader *reader, atp_status *status);

atp_graph *atp_load_failure(atp_graph *graph, atp_status *status, atp_status failure);

/* --- Shared section codecs (sections.c) --- */

bool atp_encode_edges(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_ledger(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_context(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_valence(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_schema(atp_buffer *buffer);
bool atp_encode_episodes(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_nodes(const atp_graph *graph, atp_buffer *buffer, bool with_importance);

bool atp_snapshot_write(const char *path, atp_buffer *buffer);

bool atp_decode_edges(atp_reader *reader, atp_graph *graph);
bool atp_decode_ledger(atp_reader *reader, atp_graph *graph);
bool atp_decode_context(atp_reader *reader, atp_graph *graph);
bool atp_decode_valence(atp_reader *reader, atp_graph *graph);
bool atp_decode_episodes(atp_reader *reader, atp_graph *graph);
bool atp_decode_nodes(atp_reader *reader, atp_graph *graph, bool with_importance);

/* --- Legacy format (v5.c) --- */

bool atp_encode_snapshot_v5(const atp_graph *graph, atp_buffer *buffer);
atp_graph *atp_load_v5(const unsigned char *data, size_t size, atp_status *status);
bool atp_decode_network(atp_reader *reader, atp_graph *graph);

/* --- Explicit architecture format (v6.c) --- */

bool atp_encode_snapshot_v6(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_snapshot_v7(const atp_graph *graph, atp_buffer *buffer);
atp_graph *atp_load_v6(const unsigned char *data, size_t size, atp_status *status);
atp_graph *atp_load_v7(const unsigned char *data, size_t size, atp_status *status);

/* --- Historical migration (migration.c) --- */

atp_graph *atp_load_v4(const unsigned char *data, size_t size, atp_status *status);

/* --- Little-endian helpers are in io/portable.h --- */

#endif