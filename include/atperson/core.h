#ifndef ATPERSON_CORE_H
#define ATPERSON_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_EMBEDDING_DIM 16u
#define ATPERSON_TOKEN_BYTES 96u

typedef enum atp_status {
    ATP_OK = 0,
    ATP_ERR_INVALID_ARGUMENT = 1,
    ATP_ERR_OUT_OF_MEMORY = 2,
    ATP_ERR_IO = 3,
    ATP_ERR_FORMAT = 4,
    ATP_ERR_NOT_FOUND = 5
} atp_status;

typedef struct atp_graph atp_graph;

typedef struct atp_graph_config {
    uint64_t seed;
    float learning_rate;
} atp_graph_config;

typedef struct atp_graph_stats {
    size_t node_count;
    size_t edge_count;
    uint64_t observations;
    uint64_t token_observations;
    uint64_t training_steps;
    double mean_loss;
} atp_graph_stats;

typedef struct atp_association {
    char token[ATPERSON_TOKEN_BYTES];
    float score;
    uint64_t observations;
    uint64_t last_source_hash;
} atp_association;

/**
 * Return a stable default configuration. The graph starts with no vocabulary,
 * no edges, and randomly initialised neural parameters derived from `seed`.
 */
atp_graph_config atp_graph_default_config(void);

/** Create an empty language graph. */
atp_graph *atp_graph_create(const atp_graph_config *config);

/** Release a graph returned by atp_graph_create/atp_graph_load. */
void atp_graph_destroy(atp_graph *graph);

/**
 * Learn from one textual observation.
 *
 * `source_id` should be a stable provenance identifier such as an AT URI.
 * atperson hashes it into learned edges; it is never interpreted by the core.
 */
atp_status atp_graph_observe_text(atp_graph *graph, const char *text,
                                  const char *source_id);

/** Read aggregate graph/training statistics. */
atp_graph_stats atp_graph_get_stats(const atp_graph *graph);

/**
 * Return the strongest outgoing associations for `token`.
 *
 * Results are ordered strongest-first. `out_count` is always set when non-null.
 */
atp_status atp_graph_associations(const atp_graph *graph, const char *token,
                                  atp_association *out, size_t capacity,
                                  size_t *out_count);

/** Save the complete mutable learning state to a versioned binary snapshot. */
atp_status atp_graph_save(const atp_graph *graph, const char *path);

/** Load a graph snapshot. Returns NULL and writes status on failure. */
atp_graph *atp_graph_load(const char *path, atp_status *status);

const char *atp_status_string(atp_status status);

#ifdef __cplusplus
}
#endif

#endif
