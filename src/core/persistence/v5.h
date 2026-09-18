#ifndef ATPERSON_PERSISTENCE_V5_H
#define ATPERSON_PERSISTENCE_V5_H

#include "internal.h"
#include "persistence/reader.h"
#include "persistence/wire.h"

/*
 * Snapshot v5 (persistence/v5.c): the legacy fragment format.
 *
 * v5 is the fixed one-hidden-layer wire shape at the named legacy
 * architecture. atp_graph_save writes legacy graphs as byte-identical v5;
 * v5 and v6 share their non-topology sections (persistence/sections.c) and
 * the atomic rename write.
 *
 * Encoding runs fully into a growable caller-local buffer before any
 * destination file is replaced. Allocation failure aborts before I/O; the
 * atomic write removes the temporary file on failure and always leaves the
 * previous snapshot untouched.
 *
 * Decoding consumes caller-owned bytes, allocates one graph on success,
 * and destroys partial state on every failure.
 */
bool atp_encode_snapshot_v5(const atp_graph *graph, atp_buffer *buffer);
atp_graph *atp_load_v5(const unsigned char *data, size_t size, atp_status *status);

/* Legacy one-hidden-layer NETWORK section decoder. Also used by the v4
 * migration (persistence/migration.c), which shares the legacy wire shape. */
bool atp_decode_network(atp_reader *reader, atp_graph *graph);

#endif
