#ifndef ATPERSON_PERSISTENCE_ENCODE_COMMON_H
#define ATPERSON_PERSISTENCE_ENCODE_COMMON_H

#include "internal.h"
#include "persistence/wire.h"

/*
 * Snapshot sections shared byte-for-byte by the v5 and v6 encoders
 * (src/core/persistence/encode_common.c), plus the atomic file write used by
 * both. Each encoder contracts that the graph state is committed before any
 * destination file is touched.
 */
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

#endif