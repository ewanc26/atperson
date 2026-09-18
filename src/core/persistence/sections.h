#ifndef ATPERSON_PERSISTENCE_SECTIONS_H
#define ATPERSON_PERSISTENCE_SECTIONS_H

#include "internal.h"
#include "persistence/format.h"
#include "persistence/wire.h"
#include "persistence/reader.h"

/*
 * Snapshot section codecs shared byte-for-byte by the v5 and v6 formats
 * (src/core/persistence/sections.c). Only the header, architecture, and
 * network sections differ between formats; those live in v5.c and v6.c.
 *
 * Encoder contract: the graph state is committed before any destination
 * file is touched. Decoder contract: each decoder consumes exactly one
 * already-framed section payload; the format-specific loaders in v5.c and
 * v6.c own section ordering, duplicate detection, and graph creation.
 * Validation mirrors the encoders: counts are bounded against the
 * remaining payload before any allocation, node indexes are checked
 * against the already-decoded node table, and every failure leaves the
 * caller to destroy partial state.
 */

/* --- Encode --- */

bool atp_encode_edges(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_ledger(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_context(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_valence(const atp_graph *graph, atp_buffer *buffer);
bool atp_encode_schema(atp_buffer *buffer);
bool atp_encode_episodes(const atp_graph *graph, atp_buffer *buffer);
/* NODES section: token/observations/familiarity then the embedding, with the
 * per-dimension plasticity-importance value interleaved when the format
 * carries it (v6; issue #59). */
bool atp_encode_nodes(const atp_graph *graph, atp_buffer *buffer, bool with_importance);

/*
 * Write buffer to `path` through a sibling temporary file and rename, so a
 * crash never leaves a partial snapshot at `path`. On any failure the
 * temporary is removed and the previous snapshot stays intact. Always frees
 * buffer->data, including on failure.
 */
bool atp_snapshot_write(const char *path, atp_buffer *buffer);

/* --- Decode --- */

bool atp_decode_edges(atp_reader *reader, atp_graph *graph);
bool atp_decode_ledger(atp_reader *reader, atp_graph *graph);
bool atp_decode_context(atp_reader *reader, atp_graph *graph);
bool atp_decode_valence(atp_reader *reader, atp_graph *graph);
bool atp_decode_episodes(atp_reader *reader, atp_graph *graph);
/* NODES section: token/observations/familiarity then the embedding, with the
 * per-dimension plasticity-importance value interleaved when the format
 * carries it (v6). */
bool atp_decode_nodes(atp_reader *reader, atp_graph *graph, bool with_importance);

#endif
