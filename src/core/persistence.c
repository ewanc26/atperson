#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char ATP_SNAPSHOT_MAGIC[8] = {
    'A', 'T', 'P', 'E', 'R', 'S', 'N', '1',
};
static const uint32_t ATP_SNAPSHOT_VERSION = 1u;

static bool atp_write(FILE *file, const void *data, size_t size) {
    return fwrite(data, 1u, size, file) == size;
}

static bool atp_read(FILE *file, void *data, size_t size) {
    return fread(data, 1u, size, file) == size;
}

static bool atp_write_u32(FILE *file, uint32_t value) {
    return atp_write(file, &value, sizeof(value));
}

static bool atp_write_u64(FILE *file, uint64_t value) {
    return atp_write(file, &value, sizeof(value));
}

static bool atp_read_u32(FILE *file, uint32_t *value) {
    return atp_read(file, value, sizeof(*value));
}

static bool atp_read_u64(FILE *file, uint64_t *value) {
    return atp_read(file, value, sizeof(*value));
}

atp_status atp_graph_save(const atp_graph *graph, const char *path) {
    if (!graph || !path || path[0] == '\0') {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const size_t path_len = strlen(path);
    char *temporary = malloc(path_len + 5u);
    if (!temporary) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    memcpy(temporary, path, path_len);
    memcpy(temporary + path_len, ".tmp", 5u);

    FILE *file = fopen(temporary, "wb");
    if (!file) {
        free(temporary);
        return ATP_ERR_IO;
    }

    const uint64_t node_count = (uint64_t)graph->node_count;
    const uint64_t edge_count = (uint64_t)graph->edge_count;
    bool ok =
        atp_write(file, ATP_SNAPSHOT_MAGIC, sizeof(ATP_SNAPSHOT_MAGIC)) &&
        atp_write_u32(file, ATP_SNAPSHOT_VERSION) &&
        atp_write_u32(file, ATPERSON_EMBEDDING_DIM) &&
        atp_write_u32(file, ATPERSON_HIDDEN_DIM) &&
        atp_write_u64(file, graph->config.seed) &&
        atp_write(file, &graph->config.learning_rate,
                  sizeof(graph->config.learning_rate)) &&
        atp_write_u64(file, graph->rng_state) &&
        atp_write_u64(file, graph->observations) &&
        atp_write_u64(file, graph->token_observations) &&
        atp_write_u64(file, graph->training_steps) &&
        atp_write(file, &graph->loss_total, sizeof(graph->loss_total)) &&
        atp_write_u64(file, node_count) && atp_write_u64(file, edge_count) &&
        atp_write(file, &graph->network, sizeof(graph->network));

    for (size_t i = 0; ok && i < graph->node_count; ++i) {
        const atp_node *node = &graph->nodes[i];
        const size_t length = strlen(node->token);
        if (length >= ATPERSON_TOKEN_BYTES) {
            ok = false;
            break;
        }
        ok = atp_write_u32(file, (uint32_t)length) &&
             atp_write_u64(file, node->observations) &&
             atp_write(file, node->embedding, sizeof(node->embedding)) &&
             atp_write(file, node->token, length);
    }

    for (size_t i = 0; ok && i < graph->edge_count; ++i) {
        const atp_edge *edge = &graph->edges[i];
        ok = atp_write_u32(file, edge->source) &&
             atp_write_u32(file, edge->target) &&
             atp_write_u64(file, edge->observations) &&
             atp_write_u64(file, edge->last_source_hash) &&
             atp_write(file, &edge->strength, sizeof(edge->strength));
    }

    if (fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(temporary);
        free(temporary);
        return ATP_ERR_IO;
    }

    if (rename(temporary, path) != 0) {
        remove(temporary);
        free(temporary);
        return ATP_ERR_IO;
    }

    free(temporary);
    return ATP_OK;
}

static atp_graph *atp_load_failure(FILE *file, atp_graph *graph,
                                   atp_status *status, atp_status failure) {
    if (file) {
        fclose(file);
    }
    atp_graph_destroy(graph);
    if (status) {
        *status = failure;
    }
    return NULL;
}

atp_graph *atp_graph_load(const char *path, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (!path || path[0] == '\0') {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }

    unsigned char magic[sizeof(ATP_SNAPSHOT_MAGIC)];
    uint32_t version = 0u;
    uint32_t embedding_dim = 0u;
    uint32_t hidden_dim = 0u;
    atp_graph_config config = {0};

    if (!atp_read(file, magic, sizeof(magic)) ||
        memcmp(magic, ATP_SNAPSHOT_MAGIC, sizeof(magic)) != 0 ||
        !atp_read_u32(file, &version) || version != ATP_SNAPSHOT_VERSION ||
        !atp_read_u32(file, &embedding_dim) ||
        embedding_dim != ATPERSON_EMBEDDING_DIM ||
        !atp_read_u32(file, &hidden_dim) ||
        hidden_dim != ATPERSON_HIDDEN_DIM ||
        !atp_read_u64(file, &config.seed) ||
        !atp_read(file, &config.learning_rate, sizeof(config.learning_rate))) {
        return atp_load_failure(file, NULL, status, ATP_ERR_FORMAT);
    }

    atp_graph *graph = atp_graph_create(&config);
    if (!graph) {
        return atp_load_failure(file, NULL, status, ATP_ERR_OUT_OF_MEMORY);
    }

    uint64_t node_count_u64 = 0u;
    uint64_t edge_count_u64 = 0u;
    if (!atp_read_u64(file, &graph->rng_state) ||
        !atp_read_u64(file, &graph->observations) ||
        !atp_read_u64(file, &graph->token_observations) ||
        !atp_read_u64(file, &graph->training_steps) ||
        !atp_read(file, &graph->loss_total, sizeof(graph->loss_total)) ||
        !atp_read_u64(file, &node_count_u64) ||
        !atp_read_u64(file, &edge_count_u64) ||
        !atp_read(file, &graph->network, sizeof(graph->network))) {
        return atp_load_failure(file, graph, status, ATP_ERR_FORMAT);
    }

    if (node_count_u64 > UINT32_MAX || node_count_u64 > SIZE_MAX ||
        edge_count_u64 > SIZE_MAX) {
        return atp_load_failure(file, graph, status, ATP_ERR_FORMAT);
    }

    const size_t node_count = (size_t)node_count_u64;
    const size_t edge_count = (size_t)edge_count_u64;
    if (!atp_reserve_nodes(graph, node_count) ||
        !atp_reserve_edges(graph, edge_count)) {
        return atp_load_failure(file, graph, status, ATP_ERR_OUT_OF_MEMORY);
    }

    for (size_t i = 0; i < node_count; ++i) {
        uint32_t length = 0u;
        atp_node *node = &graph->nodes[i];
        memset(node, 0, sizeof(*node));

        if (!atp_read_u32(file, &length) || length == 0u ||
            length >= ATPERSON_TOKEN_BYTES ||
            !atp_read_u64(file, &node->observations) ||
            !atp_read(file, node->embedding, sizeof(node->embedding))) {
            return atp_load_failure(file, graph, status, ATP_ERR_FORMAT);
        }

        node->token = malloc((size_t)length + 1u);
        if (!node->token) {
            return atp_load_failure(file, graph, status, ATP_ERR_OUT_OF_MEMORY);
        }
        if (!atp_read(file, node->token, length)) {
            return atp_load_failure(file, graph, status, ATP_ERR_FORMAT);
        }
        node->token[length] = '\0';
        graph->node_count++;
    }

    for (size_t i = 0; i < edge_count; ++i) {
        atp_edge *edge = &graph->edges[i];
        if (!atp_read_u32(file, &edge->source) ||
            !atp_read_u32(file, &edge->target) ||
            !atp_read_u64(file, &edge->observations) ||
            !atp_read_u64(file, &edge->last_source_hash) ||
            !atp_read(file, &edge->strength, sizeof(edge->strength)) ||
            edge->source >= graph->node_count ||
            edge->target >= graph->node_count) {
            return atp_load_failure(file, graph, status, ATP_ERR_FORMAT);
        }
        graph->edge_count++;
    }

    if (fclose(file) != 0) {
        atp_graph_destroy(graph);
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }

    if (status) {
        *status = ATP_OK;
    }
    return graph;
}
