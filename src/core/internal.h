#ifndef ATPERSON_CORE_INTERNAL_H
#define ATPERSON_CORE_INTERNAL_H

#include "atperson/core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATPERSON_HIDDEN_DIM 16u
#define ATPERSON_INPUT_DIM (ATPERSON_EMBEDDING_DIM * 2u)

/*
 * Shared tokenizer (src/core/tokenize.c). Calls emit once per token; emit
 * returning false stops the scan early. schema_version selects the
 * contract: 1 = legacy byte tokenizer, >= 2 = Unicode contract (see
 * tokenize.c header comment). All token-producing paths — observation,
 * action context, recall, lookup — must go through this one entry point.
 */
void atp_tokenize(const char *text, uint32_t schema_version,
                  bool (*emit)(void *userdata, const char *token), void *userdata);

/*
 * Shared query helper (src/core/graph.c): tokenize a query under the
 * current schema and collect distinct known node indices. Caller frees
 * *out_nodes. Unknown tokens are ignored; the graph is not mutated.
 */
atp_status atp_graph_query_nodes(const atp_graph *graph, const char *query, uint32_t **out_nodes,
                                 size_t *out_count);

/*
 * Rebuild the node and edge hash indexes from the canonical arrays.
 * Snapshot loaders call this after restoring nodes and edges; the
 * indexes are derived state and never persisted.
 */
bool atp_node_index_maybe_grow(atp_graph *graph);
void atp_node_index_insert(atp_graph *graph, uint32_t node_index);
bool atp_edge_index_maybe_grow(atp_graph *graph);
void atp_edge_index_insert(atp_graph *graph, uint32_t edge_index);
bool atp_graph_rebuild_indexes(atp_graph *graph);

typedef struct atp_node {
    char *token;
    uint64_t observations;
    /* Exponentially weighted exposure score; updated on every observation. */
    float familiarity;
    float embedding[ATPERSON_EMBEDDING_DIM];
} atp_node;

typedef struct atp_edge {
    uint32_t source;
    uint32_t target;
    uint64_t observations;
    uint64_t last_source_hash;
    float strength;
} atp_edge;

typedef struct atp_network {
    float input_hidden[ATPERSON_HIDDEN_DIM][ATPERSON_INPUT_DIM];
    float hidden_bias[ATPERSON_HIDDEN_DIM];
    float hidden_output[ATPERSON_HIDDEN_DIM];
    float output_bias;
} atp_network;

struct atp_graph {
    atp_graph_config config;
    uint64_t rng_state;

    atp_node *nodes;
    size_t node_count;
    size_t node_capacity;

    atp_edge *edges;
    size_t edge_count;
    size_t edge_capacity;

    /*
     * Hash indexes over the canonical arrays (issue #9). The arrays stay
     * the source of truth — snapshots and replay never see the indexes —
     * so these are rebuildable derived state. Open addressing, power-of-two
     * capacity, slot value UINT32_MAX means empty. Node index maps token
     * hash -> node index; edge index maps (source, target) -> edge index.
     */
    uint32_t *node_index_slots;
    size_t node_index_capacity; /* power of two, or 0 */

    uint32_t *edge_index_slots;
    size_t edge_index_capacity; /* power of two, or 0 */

    atp_network network;

    atp_ledger_entry *ledger_entries;
    size_t ledger_count;
    size_t ledger_capacity;

    atp_episode *episodes;
    size_t episode_count;
    size_t episode_capacity; /* allocated buffer capacity */
    size_t episode_max;      /* configured upper bound */
    uint64_t episode_evictions;

    uint64_t observations;
    uint64_t token_observations;
    uint64_t training_steps;
    double loss_total;
    uint64_t capacity_rejections;
};

uint64_t atp_rng_next(atp_graph *graph);
float atp_rng_signed(atp_graph *graph);
uint64_t atp_hash_source(const char *source_id);

void atp_network_init(atp_graph *graph);
float atp_network_score(const atp_graph *graph, uint32_t source, uint32_t target);
float atp_network_train(atp_graph *graph, uint32_t source, uint32_t target, float expected);

bool atp_reserve_nodes(atp_graph *graph, size_t needed);
bool atp_reserve_edges(atp_graph *graph, size_t needed);
bool atp_reserve_ledger_entries(atp_graph *graph, size_t needed);
bool atp_reserve_episodes(atp_graph *graph, size_t needed);
int32_t atp_find_node(const atp_graph *graph, const char *token);
int32_t atp_intern_node(atp_graph *graph, const char *token);
/*
 * Intern with explicit failure status. Returns the node index, or -1 with
 * *status set to ATP_ERR_CAPACITY (ceiling), ATP_ERR_OUT_OF_MEMORY, or
 * ATP_ERR_INVALID_ARGUMENT. The plain atp_intern_node cannot distinguish
 * capacity rejection from allocation failure.
 */
int32_t atp_intern_node_checked(atp_graph *graph, const char *token, atp_status *status);
int32_t atp_find_edge(const atp_graph *graph, uint32_t source, uint32_t target);
atp_status atp_observe_pair(atp_graph *graph, uint32_t source, uint32_t target,
                            uint64_t source_hash);

#endif
