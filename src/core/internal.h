#ifndef ATPERSON_CORE_INTERNAL_H
#define ATPERSON_CORE_INTERNAL_H

#include "atperson/core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATPERSON_HIDDEN_DIM 16u
#define ATPERSON_INPUT_DIM (ATPERSON_EMBEDDING_DIM * 2u)

typedef struct atp_node {
    char *token;
    uint64_t observations;
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

    atp_network network;

    uint64_t observations;
    uint64_t token_observations;
    uint64_t training_steps;
    double loss_total;
};

uint64_t atp_rng_next(atp_graph *graph);
float atp_rng_signed(atp_graph *graph);
uint64_t atp_hash_source(const char *source_id);

void atp_network_init(atp_graph *graph);
float atp_network_score(const atp_graph *graph, uint32_t source,
                        uint32_t target);
float atp_network_train(atp_graph *graph, uint32_t source, uint32_t target,
                        float expected);

bool atp_reserve_nodes(atp_graph *graph, size_t needed);
bool atp_reserve_edges(atp_graph *graph, size_t needed);
int32_t atp_find_node(const atp_graph *graph, const char *token);
int32_t atp_intern_node(atp_graph *graph, const char *token);
int32_t atp_find_edge(const atp_graph *graph, uint32_t source, uint32_t target);
atp_status atp_observe_pair(atp_graph *graph, uint32_t source, uint32_t target,
                            uint64_t source_hash);

#endif
