#ifndef ATPERSON_PERSISTENCE_MIGRATION_H
#define ATPERSON_PERSISTENCE_MIGRATION_H

#include "internal.h"

#include <stddef.h>

/*
 * Historical snapshot migration. v4 is decoded into current in-memory graph
 * state; v1-v3 remain deliberately unsupported by public load dispatch.
 */
atp_graph *atp_load_v4(const unsigned char *data, size_t size, atp_status *status);

#endif
