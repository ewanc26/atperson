#ifndef ATPERSON_PERSISTENCE_DECODE_H
#define ATPERSON_PERSISTENCE_DECODE_H

#include "persistence/reader.h"

/*
 * Current-format snapshot decoding. The decoder consumes caller-owned bytes,
 * allocates one graph on success, and destroys partial graph state on failure.
 */
bool atp_decode_network(atp_reader *reader, atp_graph *graph);
atp_graph *atp_load_v5(const unsigned char *data, size_t size, atp_status *status);

#endif
