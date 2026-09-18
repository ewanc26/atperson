#include "internal.h"
#include "persistence/v5.h"
#include "persistence/v6.h"
#include "persistence/sections.h"

#include <stdlib.h>

/*
 * Public snapshot save dispatch (atp_graph_save, declared in
 * atperson/core.h). Legacy graphs save as byte-identical v5 (persistence/v5.c);
 * every other topology saves as v6 (persistence/v6.c). Both formats share
 * their non-topology sections and the atomic rename write
 * (persistence/sections.c).
 */

atp_status atp_graph_save(const atp_graph *graph, const char *path) {
    if (!graph || !path || path[0] == '\0') {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_buffer buffer = {0};
    const bool encoded = atp_neural_architecture_is_legacy(&graph->neural_architecture)
                             ? atp_encode_snapshot_v5(graph, &buffer)
                             : atp_encode_snapshot_v6(graph, &buffer);
    if (!encoded) {
        free(buffer.data);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    return atp_snapshot_write(path, &buffer) ? ATP_OK : ATP_ERR_IO;
}
