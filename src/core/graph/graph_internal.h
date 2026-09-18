#ifndef ATPERSON_CORE_GRAPH_INTERNAL_H
#define ATPERSON_CORE_GRAPH_INTERNAL_H

/*
 * Shared internals of the graph scope.
 *
 * lifecycle.c owns architecture construction and neural layout; index.c owns
 * the open-addressing node/edge hash indexes; groups.c owns the derived
 * episode group index; store.c owns node/edge allocation and vocabulary
 * internment; observe.c owns the tokenization walk and pair observation;
 * query.c owns recall and association lookup; valence.c owns the
 * experience-derived valence records and event log.
 *
 * This header is scope-private and must not leak into include/atperson/.
 */

#include "../internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- Neural architecture helpers (lifecycle.c) --- */

atp_neural_architecture atp_neural_legacy_architecture(void);

/* --- Hash indexes (index.c) --- */

bool atp_node_index_maybe_grow(atp_graph *graph);
void atp_node_index_insert(atp_graph *graph, uint32_t node_index);
bool atp_edge_index_maybe_grow(atp_graph *graph);
void atp_edge_index_insert(atp_graph *graph, uint32_t edge_index);
bool atp_graph_rebuild_indexes(atp_graph *graph);

/* --- Episode group index (groups.c) --- */

bool atp_episode_groups_rebuild(atp_graph *graph);
bool atp_episode_groups_append(atp_graph *graph);
void atp_episode_groups_note_eviction(atp_graph *graph, size_t episode_index);
uint32_t atp_episode_group_of(const atp_graph *graph, size_t episode_index);
void atp_episode_groups_destroy(atp_graph *graph);

/* --- Vocabulary store (store.c) --- */

bool atp_node_allocate_vectors(const atp_graph *graph, atp_node *node);
void atp_node_destroy(atp_node *node);
bool atp_reserve_nodes(atp_graph *graph, size_t needed);
bool atp_reserve_edges(atp_graph *graph, size_t needed);
bool atp_reserve_ledger_entries(atp_graph *graph, size_t needed);
bool atp_reserve_episodes(atp_graph *graph, size_t needed);
int32_t atp_find_node(const atp_graph *graph, const char *token);
int32_t atp_intern_node(atp_graph *graph, const char *token);
int32_t atp_intern_node_checked(atp_graph *graph, const char *token, atp_status *status);
int32_t atp_find_edge(const atp_graph *graph, uint32_t source, uint32_t target);

/* --- Observation walk (observe.c) --- */

atp_status atp_observe_pair(atp_graph *graph, uint32_t source, uint32_t target,
                            uint64_t source_hash);
atp_status atp_graph_query_nodes(const atp_graph *graph, const char *query,
                                 uint32_t **out_nodes, size_t *out_count);

/* --- Neural layout (neural_layout.c) --- */

bool atp_neural_architecture_is_legacy(const atp_neural_architecture *architecture);
bool atp_neural_layout_build(const atp_neural_architecture *architecture,
                             atp_neural_layout *out_layout);

/* --- Neural network (neural.c) --- */

bool atp_network_init(atp_graph *graph);
void atp_network_destroy(atp_network *network);
atp_status atp_network_score(const atp_graph *graph, uint32_t source, uint32_t target,
                             float *out_score);
float atp_network_score_owned(atp_graph *graph, uint32_t source, uint32_t target);
float atp_network_train(atp_graph *graph, uint32_t source, uint32_t target, float expected);

/* --- RNG and hashing --- */

uint64_t atp_rng_next(atp_graph *graph);
float atp_rng_signed(atp_graph *graph);
uint64_t atp_hash_source(const char *source_id);

#endif