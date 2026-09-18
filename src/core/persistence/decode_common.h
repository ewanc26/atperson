#ifndef ATPERSON_PERSISTENCE_DECODE_COMMON_H
#define ATPERSON_PERSISTENCE_DECODE_COMMON_H

#include "internal.h"
#include "persistence/reader.h"

/*
 * Snapshot section decoders shared byte-for-byte by the v5 and v6 loaders
 * (src/core/persistence/decode_common.c). Each consumes exactly one
 * already-framed section payload from the reader; the format-specific
 * loaders own section ordering, duplicate detection, and graph creation.
 */
bool atp_decode_edges(atp_reader *reader, atp_graph *graph);
bool atp_decode_ledger(atp_reader *reader, atp_graph *graph);
bool atp_decode_context(atp_reader *reader, atp_graph *graph);
bool atp_decode_valence(atp_reader *reader, atp_graph *graph);
bool atp_decode_episodes(atp_reader *reader, atp_graph *graph);

#endif
