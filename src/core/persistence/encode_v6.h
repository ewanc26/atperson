#ifndef ATPERSON_PERSISTENCE_ENCODE_V6_H
#define ATPERSON_PERSISTENCE_ENCODE_V6_H

#include "internal.h"
#include "persistence/wire.h"

/*
 * v6 snapshot encoding (src/core/persistence/encode_v6.c): the explicit
 * neural-architecture descriptor plus variable-length generic network and
 * node payloads with plasticity-importance values (issue #72). Called by
 * atp_graph_save for graphs whose topology is not the fixed legacy shape;
 * legacy graphs continue to save byte-identical v5 snapshots.
 */
bool atp_encode_snapshot_v6(const atp_graph *graph, atp_buffer *buffer);

#endif