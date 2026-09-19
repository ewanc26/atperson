#ifndef ATPERSON_CORE_INTERNAL_H
#define ATPERSON_CORE_INTERNAL_H

#include "atperson/core.h"
#include "atperson/tokenize.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATPERSON_HIDDEN_DIM 16u
#define ATPERSON_INPUT_DIM (ATPERSON_EMBEDDING_DIM * 2u)
#define ATPERSON_NEURAL_DENSE_LAYER_MAX (ATPERSON_NEURAL_MAX_HIDDEN_LAYERS + 1u)

/* atp_tokenize is declared publicly in atperson/tokenize.h. */

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

/* Derived episode group index (issue #55), src/core/graph/groups.c. */
bool atp_episode_groups_rebuild(atp_graph *graph);
bool atp_episode_groups_append(atp_graph *graph);
void atp_episode_groups_note_eviction(atp_graph *graph, size_t episode_index);
uint32_t atp_episode_group_of(const atp_graph *graph, size_t episode_index);
void atp_episode_groups_destroy(atp_graph *graph);

typedef struct atp_node {
    char *token;
    uint64_t observations;
    /* Exponentially weighted exposure score; updated on every observation. */
    float familiarity;
    float *embedding;
    float *embedding_importance;
} atp_node;

typedef struct atp_edge {
    uint32_t source;
    uint32_t target;
    uint64_t observations;
    uint64_t last_source_hash;
    float strength;
} atp_edge;

/*
 * Experience-derived valence (issue #13). One record per token that has
 * received at least one explicit valence event — records are created
 * lazily by events, never by observation, so a fresh graph has none and
 * the empty-start invariant holds. Kept sorted by node_index (binary
 * search; insertion is a memmove, which is fine for event-scale rates).
 */
typedef struct atp_valence_record {
    uint32_t node_index;
    float valence; /* in [-1, 1] */
    uint64_t event_count;
    uint64_t positive_events;
    uint64_t negative_events;
    uint64_t last_event_at;
} atp_valence_record;

/*
 * Bounded provenance log for valence updates: one entry per event, oldest
 * evicted when the log is full. Persisted in the snapshot so the folded
 * state can be explained after restart.
 */
typedef struct atp_valence_event_log_entry {
    uint32_t node_index;
    uint32_t kind; /* atp_valence_kind */
    float signal; /* in [-1, 1] */
    uint64_t at_epoch;
    char source_id[ATPERSON_LEDGER_SOURCE_BYTES];
} atp_valence_event_log_entry;

typedef struct atp_neural_layout {
    size_t layer_count;
    size_t input_widths[ATPERSON_NEURAL_DENSE_LAYER_MAX];
    size_t output_widths[ATPERSON_NEURAL_DENSE_LAYER_MAX];
    size_t weight_offsets[ATPERSON_NEURAL_DENSE_LAYER_MAX];
    size_t bias_offsets[ATPERSON_NEURAL_DENSE_LAYER_MAX];
    size_t activation_offsets[ATPERSON_NEURAL_DENSE_LAYER_MAX + 1u];
    size_t weight_count;
    size_t bias_count;
    size_t activation_count;
} atp_neural_layout;

typedef struct atp_network {
    atp_neural_layout layout;
    float *weights;
    float *biases;
    float *weight_importance;
    float *bias_importance;
    /* Mutating training owns these scratch buffers; they are derived state. */
    float *training_activations;
    float *training_deltas;
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

    /* Explicit topology metadata; legacy-fixed until issue #65 variable shape lands. */
    atp_neural_architecture neural_architecture;
    atp_network network;

    atp_ledger_entry *ledger_entries;
    atp_conversation_context *ledger_contexts; /* parallel to ledger_entries */
    size_t ledger_count;
    size_t ledger_capacity;

    atp_episode *episodes;
    size_t episode_count;
    size_t episode_capacity; /* allocated buffer capacity */
    size_t episode_max;      /* configured upper bound */
    uint64_t episode_evictions;

    /* Derived episode group index (issue #55). Membership is rebuilt from
     * the episode array on load and on eviction; per-group eviction
     * counters survive rebuilds keyed by group key. */
    atp_episode_group *groups;
    size_t group_count;
    size_t group_capacity;
    uint32_t *group_table_slots; /* key hash -> group id, linear probing */
    size_t group_table_capacity; /* power of two, or 0 */
    uint32_t *episode_group_ids;  /* parallel to episodes */
    size_t group_member_capacity;

    atp_valence_record *valence_records;
    size_t valence_record_count;
    size_t valence_record_capacity;

    atp_valence_event_log_entry *valence_events;
    size_t valence_event_count;   /* live entries, oldest first */
    size_t valence_event_capacity;/* fixed at ATPERSON_VALENCE_EVENT_CAPACITY */
    uint64_t valence_event_evictions;

    uint64_t observations;
    uint64_t token_observations;
    uint64_t training_steps;
    double loss_total;
    uint64_t capacity_rejections;

    /* Plasticity statistics (issue #59). */
    uint64_t plasticity_steps_total;
    uint64_t plasticity_steps_protected;
    uint64_t plasticity_parameters_protected;
};

uint64_t atp_rng_next(atp_graph *graph);
float atp_rng_signed(atp_graph *graph);
uint64_t atp_hash_source(const char *source_id);

bool atp_neural_architecture_is_legacy(const atp_neural_architecture *architecture);
bool atp_neural_layout_build(const atp_neural_architecture *architecture,
                             atp_neural_layout *out_layout);

static inline size_t atp_network_weight_index(const atp_network *network, size_t layer,
                                              size_t output, size_t input) {
    return network->layout.weight_offsets[layer] +
           output * network->layout.input_widths[layer] + input;
}

static inline size_t atp_network_bias_index(const atp_network *network, size_t layer,
                                            size_t output) {
    return network->layout.bias_offsets[layer] + output;
}

bool atp_network_init(atp_graph *graph);
void atp_network_destroy(atp_network *network);
atp_status atp_network_score(const atp_graph *graph, uint32_t source, uint32_t target,
                             float *out_score);
float atp_network_score_owned(atp_graph *graph, uint32_t source, uint32_t target);
float atp_network_train(atp_graph *graph, uint32_t source, uint32_t target, float expected);

bool atp_node_allocate_vectors(const atp_graph *graph, atp_node *node);
void atp_node_destroy(atp_node *node);
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
