#ifndef ATPERSON_PERSISTENCE_DECODE_V6_H
#define ATPERSON_PERSISTENCE_DECODE_V6_H

#include "persistence/reader.h"

/*
 * Snapshot v6 decoding (src/core/persistence/decode_v6.c): the explicit
 * neural-architecture descriptor plus variable-length generic network and
 * node payloads with plasticity-importance values (issue #72). Allocates
 * one graph on success via atp_graph_create_with_architecture; every
 * failure path destroys partial state.
 */
atp_graph *atp_load_v6(const unsigned char *data, size_t size, atp_status *status);

#endif
