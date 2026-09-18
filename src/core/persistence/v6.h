#ifndef ATPERSON_PERSISTENCE_V6_H
#define ATPERSON_PERSISTENCE_V6_H

#include "internal.h"
#include "persistence/reader.h"
#include "persistence/wire.h"

/*
 * Snapshot v6 (persistence/v6.c): the generalized format.
 *
 * v6 generalizes v5 to the explicit neural architecture descriptor carried
 * by ATP_SECTION_ARCH, so every supported topology and its
 * plasticity-importance values round-trip exactly (issue #72). HEADER is
 * config-only (the topology lives in ARCH); NETWORK persists the generic
 * layer stack in the kernel's index order followed by its importance
 * arrays; NODES appends the embedding-importance vector to each node.
 *
 * Legacy graphs never reach these encoders: atp_graph_save writes them as
 * byte-identical v5, keeping the historical wire shape authoritative.
 *
 * Decoding: ARCH must precede NETWORK and NODES because it creates the
 * graph at the persisted topology before any learned state is read.
 * Corruption guards mirror the v5 loader: duplicate sections, reordered
 * dependencies, truncated payloads, and counts exceeding the remaining
 * bytes all fail as ATP_ERR_FORMAT. Allocates one graph on success via
 * atp_graph_create_with_architecture; every failure path destroys
 * partial state.
 */
bool atp_encode_snapshot_v6(const atp_graph *graph, atp_buffer *buffer);
atp_graph *atp_load_v6(const unsigned char *data, size_t size, atp_status *status);

#endif
