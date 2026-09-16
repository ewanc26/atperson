#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *atp_strdup_local(const char *value) {
    if (!value) {
        return NULL;
    }
    const size_t len = strlen(value);
    char *copy = malloc(len + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len + 1u);
    return copy;
}

atp_graph_config atp_graph_default_config(void) {
    atp_graph_config config = {
        .seed = UINT64_C(0x4154504552534f4e),
        .learning_rate = 0.025f,
        .familiarity_decay = 0.98f,
        .episode_capacity = ATPERSON_EPISODE_DEFAULT_CAPACITY,
    };
    return config;
}

atp_graph *atp_graph_create(const atp_graph_config *config) {
    atp_graph_config effective = config ? *config : atp_graph_default_config();
    if (effective.seed == 0u) {
        effective.seed = atp_graph_default_config().seed;
    }
    if (!(effective.learning_rate > 0.0f) || !isfinite(effective.learning_rate)) {
        effective.learning_rate = atp_graph_default_config().learning_rate;
    }
    if (!(effective.familiarity_decay > 0.0f) || effective.familiarity_decay >= 1.0f) {
        effective.familiarity_decay = atp_graph_default_config().familiarity_decay;
    }
    if (effective.episode_capacity == 0u) {
        effective.episode_capacity = ATPERSON_EPISODE_DEFAULT_CAPACITY;
    }

    atp_graph *graph = calloc(1u, sizeof(*graph));
    if (!graph) {
        return NULL;
    }

    graph->config = effective;
    graph->episode_max = effective.episode_capacity;
    graph->rng_state = effective.seed;
    atp_network_init(graph);
    return graph;
}

void atp_graph_destroy(atp_graph *graph) {
    if (!graph) {
        return;
    }

    for (size_t i = 0; i < graph->node_count; ++i) {
        free(graph->nodes[i].token);
    }
    free(graph->nodes);
    free(graph->edges);
    free(graph->node_index_slots);
    free(graph->edge_index_slots);
    free(graph->ledger_entries);
    free(graph->episodes);
    free(graph);
}

uint64_t atp_rng_next(atp_graph *graph) {
    uint64_t x = graph->rng_state;
    x ^= x >> 12u;
    x ^= x << 25u;
    x ^= x >> 27u;
    graph->rng_state = x;
    return x * UINT64_C(2685821657736338717);
}

float atp_rng_signed(atp_graph *graph) {
    const uint64_t value = atp_rng_next(graph) >> 40u;
    const float unit = (float)value / (float)UINT32_C(16777215);
    return (unit * 2.0f) - 1.0f;
}

uint64_t atp_hash_source(const char *source_id) {
    const unsigned char *cursor = (const unsigned char *)(source_id ? source_id : "");
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*cursor) {
        hash ^= (uint64_t)*cursor++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* -------- hash indexes (issue #9) --------
 *
 * Open-addressing indexes over the canonical node and edge arrays. The
 * arrays remain the source of truth; the indexes are derived state that
 * any loader rebuilds after restoring the arrays. Power-of-two capacity,
 * linear probing, UINT32_MAX = empty slot. */

#define ATP_INDEX_EMPTY UINT32_MAX

static uint64_t atp_hash_token(const char *token) {
    const unsigned char *cursor = (const unsigned char *)token;
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*cursor) {
        hash ^= (uint64_t)*cursor++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t atp_hash_edge_key(uint32_t source, uint32_t target) {
    /* The packed key is dense and sequential (intern indices grow
     * together), so identity hashing packs linear-probe runs into
     * contiguous spans and degrades toward O(n) probes. Run the key
     * through a splitmix64-style finalizer to spread it. */
    uint64_t hash = (uint64_t)source << 32u | (uint64_t)target;
    hash ^= hash >> 30;
    hash *= UINT64_C(0xBF58476D1CE4E5B9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94D049BB133111EB);
    hash ^= hash >> 31;
    return hash;
}

/* Grow (or create) the node index so it holds node_count entries at a load
 * factor <= 0.5, then reinsert every existing node. */
static bool atp_node_index_rebuild(atp_graph *graph, size_t needed) {
    size_t capacity = graph->node_index_capacity ? graph->node_index_capacity * 2u : 64u;
    while (capacity < needed * 2u) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    uint32_t *slots = malloc(capacity * sizeof(*slots));
    if (!slots) {
        return false;
    }
    for (size_t i = 0u; i < capacity; ++i) {
        slots[i] = ATP_INDEX_EMPTY;
    }

    for (size_t i = 0u; i < graph->node_count; ++i) {
        const uint64_t hash = atp_hash_token(graph->nodes[i].token);
        size_t slot = (size_t)(hash & (uint64_t)(capacity - 1u));
        while (slots[slot] != ATP_INDEX_EMPTY) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        slots[slot] = (uint32_t)i;
    }

    free(graph->node_index_slots);
    graph->node_index_slots = slots;
    graph->node_index_capacity = capacity;
    return true;
}

static bool atp_node_index_maybe_grow(atp_graph *graph) {
    if (graph->node_count * 2u + 2u > graph->node_index_capacity) {
        return atp_node_index_rebuild(graph, graph->node_count + 1u);
    }
    return true;
}

/* Insert a freshly appended node index. The token must not already be
 * indexed. Call only after node_count includes the new node. */
static void atp_node_index_insert(atp_graph *graph, uint32_t node_index) {
    if (!graph->node_index_slots) {
        return;
    }
    const uint64_t hash = atp_hash_token(graph->nodes[node_index].token);
    size_t slot = (size_t)(hash & (uint64_t)(graph->node_index_capacity - 1u));
    while (graph->node_index_slots[slot] != ATP_INDEX_EMPTY) {
        slot = (slot + 1u) & (graph->node_index_capacity - 1u);
    }
    graph->node_index_slots[slot] = node_index;
}

static bool atp_edge_index_rebuild(atp_graph *graph, size_t needed) {
    size_t capacity = graph->edge_index_capacity ? graph->edge_index_capacity * 2u : 128u;
    while (capacity < needed * 2u) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    uint32_t *slots = malloc(capacity * sizeof(*slots));
    if (!slots) {
        return false;
    }
    for (size_t i = 0u; i < capacity; ++i) {
        slots[i] = ATP_INDEX_EMPTY;
    }

    for (size_t i = 0u; i < graph->edge_count; ++i) {
        const uint64_t hash = atp_hash_edge_key(graph->edges[i].source, graph->edges[i].target);
        size_t slot = (size_t)(hash & (uint64_t)(capacity - 1u));
        while (slots[slot] != ATP_INDEX_EMPTY) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        slots[slot] = (uint32_t)i;
    }

    free(graph->edge_index_slots);
    graph->edge_index_slots = slots;
    graph->edge_index_capacity = capacity;
    return true;
}

static bool atp_edge_index_maybe_grow(atp_graph *graph) {
    if (graph->edge_count * 2u + 2u > graph->edge_index_capacity) {
        return atp_edge_index_rebuild(graph, graph->edge_count + 1u);
    }
    return true;
}

static void atp_edge_index_insert(atp_graph *graph, uint32_t edge_index) {
    if (!graph->edge_index_slots) {
        return;
    }
    const uint64_t hash =
        atp_hash_edge_key(graph->edges[edge_index].source, graph->edges[edge_index].target);
    size_t slot = (size_t)(hash & (uint64_t)(graph->edge_index_capacity - 1u));
    while (graph->edge_index_slots[slot] != ATP_INDEX_EMPTY) {
        slot = (slot + 1u) & (graph->edge_index_capacity - 1u);
    }
    graph->edge_index_slots[slot] = edge_index;
}

/* Rebuild both indexes from the canonical arrays. Called by snapshot
 * loaders; the arrays are complete and the indexes are empty. */
bool atp_graph_rebuild_indexes(atp_graph *graph) {
    if (!graph) {
        return false;
    }
    if (!atp_node_index_rebuild(graph, graph->node_count + 1u)) {
        return false;
    }
    if (!atp_edge_index_rebuild(graph, graph->edge_count + 1u)) {
        return false;
    }
    return true;
}

bool atp_reserve_nodes(atp_graph *graph, size_t needed) {
    if (needed <= graph->node_capacity) {
        return true;
    }

    size_t capacity = graph->node_capacity ? graph->node_capacity : 32u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    atp_node *nodes = realloc(graph->nodes, capacity * sizeof(*nodes));
    if (!nodes) {
        return false;
    }

    graph->nodes = nodes;
    graph->node_capacity = capacity;
    return true;
}

bool atp_reserve_edges(atp_graph *graph, size_t needed) {
    if (needed <= graph->edge_capacity) {
        return true;
    }

    size_t capacity = graph->edge_capacity ? graph->edge_capacity : 64u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    atp_edge *edges = realloc(graph->edges, capacity * sizeof(*edges));
    if (!edges) {
        return false;
    }

    graph->edges = edges;
    graph->edge_capacity = capacity;
    return true;
}

bool atp_reserve_ledger_entries(atp_graph *graph, size_t needed) {
    if (needed <= graph->ledger_capacity) {
        return true;
    }

    size_t capacity = graph->ledger_capacity ? graph->ledger_capacity : 16u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    atp_ledger_entry *entries = realloc(graph->ledger_entries, capacity * sizeof(*entries));
    if (!entries) {
        return false;
    }

    graph->ledger_entries = entries;
    graph->ledger_capacity = capacity;
    return true;
}

bool atp_reserve_episodes(atp_graph *graph, size_t needed) {
    if (needed > graph->episode_max) {
        return false;
    }
    if (needed <= graph->episode_capacity) {
        return true;
    }
    size_t capacity = graph->episode_capacity ? graph->episode_capacity : 16u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    atp_episode *episodes = realloc(graph->episodes, capacity * sizeof(*episodes));
    if (!episodes) {
        return false;
    }
    graph->episodes = episodes;
    graph->episode_capacity = capacity;
    return true;
}

int32_t atp_find_node(const atp_graph *graph, const char *token) {
    if (!graph || !token || !graph->node_index_slots) {
        return -1;
    }
    const uint64_t mask = (uint64_t)(graph->node_index_capacity - 1u);
    size_t slot = (size_t)(atp_hash_token(token) & mask);
    for (;;) {
        const uint32_t entry = graph->node_index_slots[slot];
        if (entry == ATP_INDEX_EMPTY) {
            return -1;
        }
        if (strcmp(graph->nodes[entry].token, token) == 0) {
            return (int32_t)entry;
        }
        slot = (slot + 1u) & (size_t)mask;
    }
}

int32_t atp_intern_node_checked(atp_graph *graph, const char *token, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (!graph || !token) {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return -1;
    }

    const int32_t existing = atp_find_node(graph, token);
    if (existing >= 0) {
        graph->nodes[existing].observations++;
        graph->nodes[existing].familiarity =
            graph->nodes[existing].familiarity * graph->config.familiarity_decay + 1.0f;
        return existing;
    }

    /* Resource ceiling: reject before any mutation. The caller rejects
     * the whole observation; nothing is half-interned. */
    if (graph->config.node_capacity_max != 0u &&
        graph->node_count >= graph->config.node_capacity_max) {
        graph->capacity_rejections++;
        if (status) {
            *status = ATP_ERR_CAPACITY;
        }
        return -1;
    }

    if (graph->node_count >= UINT32_MAX || !atp_reserve_nodes(graph, graph->node_count + 1u)) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return -1;
    }
    if (!atp_node_index_maybe_grow(graph)) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return -1;
    }

    atp_node *node = &graph->nodes[graph->node_count];
    memset(node, 0, sizeof(*node));
    node->token = atp_strdup_local(token);
    if (!node->token) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return -1;
    }
    node->observations = 1u;
    node->familiarity = 1.0f;
    for (size_t i = 0; i < ATPERSON_EMBEDDING_DIM; ++i) {
        node->embedding[i] = atp_rng_signed(graph) * 0.05f;
    }

    const int32_t index = (int32_t)graph->node_count;
    graph->node_count++;
    atp_node_index_insert(graph, (uint32_t)index);
    return index;
}

int32_t atp_intern_node(atp_graph *graph, const char *token) {
    atp_status status = ATP_OK;
    return atp_intern_node_checked(graph, token, &status);
}

int32_t atp_find_edge(const atp_graph *graph, uint32_t source, uint32_t target) {
    if (!graph || !graph->edge_index_slots) {
        return -1;
    }
    const uint64_t mask = (uint64_t)(graph->edge_index_capacity - 1u);
    size_t slot = (size_t)(atp_hash_edge_key(source, target) & mask);
    for (;;) {
        const uint32_t entry = graph->edge_index_slots[slot];
        if (entry == ATP_INDEX_EMPTY) {
            return -1;
        }
        const atp_edge *edge = &graph->edges[entry];
        if (edge->source == source && edge->target == target) {
            return (int32_t)entry;
        }
        slot = (slot + 1u) & (size_t)mask;
    }
}

atp_status atp_observe_pair(atp_graph *graph, uint32_t source, uint32_t target,
                            uint64_t source_hash) {
    if (!graph || source >= graph->node_count || target >= graph->node_count) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    int32_t edge_index = atp_find_edge(graph, source, target);
    if (edge_index < 0) {
        /* Resource ceiling: reject before any mutation. */
        if (graph->config.edge_capacity_max != 0u &&
            graph->edge_count >= graph->config.edge_capacity_max) {
            graph->capacity_rejections++;
            return ATP_ERR_CAPACITY;
        }
        if (!atp_reserve_edges(graph, graph->edge_count + 1u)) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        if (!atp_edge_index_maybe_grow(graph)) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        edge_index = (int32_t)graph->edge_count++;
        graph->edges[edge_index] = (atp_edge){
            .source = source,
            .target = target,
            .observations = 0u,
            .last_source_hash = source_hash,
            .strength = 0.0f,
        };
        atp_edge_index_insert(graph, (uint32_t)edge_index);
    }

    const float positive_loss = atp_network_train(graph, source, target, 1.0f);
    graph->loss_total += positive_loss;
    graph->training_steps++;

    if (graph->node_count > 2u) {
        uint32_t negative = 0u;
        for (unsigned attempt = 0; attempt < 8u; ++attempt) {
            negative = (uint32_t)(atp_rng_next(graph) % graph->node_count);
            if (negative != source && negative != target) {
                break;
            }
        }
        if (negative != source && negative != target) {
            const float negative_loss = atp_network_train(graph, source, negative, 0.0f);
            graph->loss_total += negative_loss;
            graph->training_steps++;
        }
    }

    atp_edge *edge = &graph->edges[edge_index];
    const float score = atp_network_score(graph, source, target);
    edge->strength = edge->observations == 0u ? score : (edge->strength * 0.90f + score * 0.10f);
    edge->observations++;
    edge->last_source_hash = source_hash;
    return ATP_OK;
}

/* Shared tokenizer state for the observe walks. emit returns false to stop
 * the scan on the first error, carried out through walk->status. */
typedef struct atp_observe_walk {
    atp_graph *graph;
    uint64_t source_hash;
    int32_t previous;
    bool saw_token;
    atp_status status;
} atp_observe_walk;

/* Dry-run walk (issue #9): counts the new nodes and edges a text would
 * create without mutating anything. Observations that would cross a
 * resource ceiling are rejected whole before any learning happens, so a
 * rejected observation never leaves half-learned state behind. */
typedef struct atp_budget_walk {
    const atp_graph *graph;
    size_t new_nodes;
    size_t new_edges;
    int32_t previous;
} atp_budget_walk;

static bool atp_budget_emit(void *userdata, const char *token) {
    atp_budget_walk *walk = userdata;
    const int32_t current = atp_find_node(walk->graph, token);
    const uint32_t current_index =
        current >= 0 ? (uint32_t)current : (uint32_t)walk->graph->node_count;
    if (current < 0) {
        walk->new_nodes++;
    }
    if (walk->previous >= 0) {
        const uint32_t previous_index = (uint32_t)walk->previous;
        bool edge_exists = false;
        if (current >= 0) {
            edge_exists = atp_find_edge(walk->graph, previous_index, current_index) >= 0;
        } else {
            /* New node: every pair touching it is a new edge. */
            edge_exists = false;
        }
        if (!edge_exists) {
            walk->new_edges++;
        }
    }
    walk->previous = (int32_t)current_index;
    return true;
}

/* Reject the observation if it would cross a configured ceiling. Returns
 * ATP_OK when it fits (or no ceiling is configured). */
static atp_status atp_check_budget(atp_graph *graph, const char *text) {
    if (graph->config.node_capacity_max == 0u && graph->config.edge_capacity_max == 0u) {
        return ATP_OK;
    }

    atp_budget_walk walk = {
        .graph = graph,
        .previous = -1,
    };
    atp_tokenize(text, ATPERSON_SCHEMA_VERSION, atp_budget_emit, &walk);

    if (graph->config.node_capacity_max != 0u &&
        graph->node_count + walk.new_nodes > graph->config.node_capacity_max) {
        graph->capacity_rejections++;
        return ATP_ERR_CAPACITY;
    }
    if (graph->config.edge_capacity_max != 0u &&
        graph->edge_count + walk.new_edges > graph->config.edge_capacity_max) {
        graph->capacity_rejections++;
        return ATP_ERR_CAPACITY;
    }
    return ATP_OK;
}

static bool atp_observe_emit(void *userdata, const char *token) {
    atp_observe_walk *walk = userdata;
    atp_status status = ATP_OK;
    const int32_t current = atp_intern_node_checked(walk->graph, token, &status);
    if (current < 0) {
        walk->status = status;
        return false;
    }
    walk->graph->token_observations++;
    walk->saw_token = true;
    if (walk->previous >= 0) {
        const atp_status status = atp_observe_pair(
            walk->graph, (uint32_t)walk->previous, (uint32_t)current, walk->source_hash);
        if (status != ATP_OK) {
            walk->status = status;
            return false;
        }
    }
    walk->previous = current;
    return true;
}

atp_status atp_graph_observe_text(atp_graph *graph, const char *text, const char *source_id) {
    if (!graph || !text) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    /* Whole-observation budget check: a text that would cross a ceiling
     * is rejected before any learning happens. */
    const atp_status budget = atp_check_budget(graph, text);
    if (budget != ATP_OK) {
        return budget;
    }

    const uint64_t source_hash = atp_hash_source(source_id);
    atp_observe_walk walk = {
        .graph = graph,
        .source_hash = source_hash,
        .previous = -1,
        .saw_token = false,
    };

    atp_tokenize(text, ATPERSON_SCHEMA_VERSION, atp_observe_emit, &walk);

    if (walk.status != ATP_OK) {
        return walk.status;
    }
    if (walk.saw_token) {
        graph->observations++;
    }
    return ATP_OK;
}

/* -------- episodic memory -------- */

typedef struct atp_token_count {
    uint32_t node_index;
    uint32_t count;
} atp_token_count;

static atp_status atp_token_counts_bump(atp_token_count **items_out, size_t *count,
                                        size_t *capacity, uint32_t node_index) {
    atp_token_count *items = *items_out;
    for (size_t i = 0u; i < *count; ++i) {
        if (items[i].node_index == node_index) {
            items[i].count++;
            return ATP_OK;
        }
    }
    if (*count == *capacity) {
        const size_t next = *capacity ? *capacity * 2u : 16u;
        if (next < *count || next > SIZE_MAX / sizeof(*items)) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        atp_token_count *grown = realloc(items, next * sizeof(*grown));
        if (!grown) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        items = grown;
        *items_out = items;
        *capacity = next;
    }
    items[*count] = (atp_token_count){.node_index = node_index, .count = 1u};
    (*count)++;
    return ATP_OK;
}

static int atp_token_count_compare(const void *left, const void *right) {
    const atp_token_count *a = left;
    const atp_token_count *b = right;
    if (a->count < b->count) {
        return 1;
    }
    if (a->count > b->count) {
        return -1;
    }
    if (a->node_index < b->node_index) {
        return -1;
    }
    if (a->node_index > b->node_index) {
        return 1;
    }
    return 0;
}

static atp_episode atp_episode_build(const atp_token_count *sorted, size_t distinct,
                                     uint64_t ledger_id, const char *source_id,
                                     const char *author_did, uint64_t observed_at,
                                     uint64_t content_digest, uint32_t schema_version) {
    atp_episode episode = {0};
    episode.ledger_id = ledger_id;
    episode.observed_at = observed_at;
    episode.content_digest = content_digest;
    episode.schema_version = schema_version;
    strncpy(episode.source_id, source_id ? source_id : "", sizeof(episode.source_id) - 1u);
    strncpy(episode.author_did, author_did ? author_did : "", sizeof(episode.author_did) - 1u);
    const size_t kept =
        distinct < ATPERSON_EPISODE_SUMMARY_SIZE ? distinct : ATPERSON_EPISODE_SUMMARY_SIZE;
    for (size_t i = 0u; i < kept; ++i) {
        episode.summary[i] = (atp_episode_token){
            .node_index = sorted[i].node_index,
            .weight = (float)sorted[i].count,
        };
    }
    episode.token_count = (uint32_t)kept;
    return episode;
}

/* Evict the least-recalled episode; ties go to the oldest, then the smallest
 * ledger id, so eviction is deterministic and inspectable. */
static void atp_evict_episode(atp_graph *graph) {
    size_t victim = 0u;
    for (size_t i = 1u; i < graph->episode_count; ++i) {
        const atp_episode *current = &graph->episodes[i];
        const atp_episode *best = &graph->episodes[victim];
        if (current->recall_count < best->recall_count ||
            (current->recall_count == best->recall_count &&
             current->observed_at < best->observed_at) ||
            (current->recall_count == best->recall_count &&
             current->observed_at == best->observed_at && current->ledger_id < best->ledger_id)) {
            victim = i;
        }
    }
    if (victim + 1u < graph->episode_count) {
        memmove(&graph->episodes[victim], &graph->episodes[victim + 1u],
                (graph->episode_count - victim - 1u) * sizeof(graph->episodes[0]));
    }
    graph->episode_count--;
    graph->episode_evictions++;
}

/* Episode-building variant of the observe walk: also counts distinct tokens
 * per observation for the episode's token histogram. */
typedef struct atp_memory_walk {
    atp_observe_walk base;
    atp_token_count *counts;
    size_t distinct;
    size_t counts_capacity;
} atp_memory_walk;

static bool atp_memory_emit(void *userdata, const char *token) {
    atp_memory_walk *walk = userdata;
    if (!atp_observe_emit(userdata, token)) {
        return false;
    }
    const atp_status bumped = atp_token_counts_bump(&walk->counts, &walk->distinct,
                                                    &walk->counts_capacity,
                                                    (uint32_t)walk->base.previous);
    if (bumped != ATP_OK) {
        walk->base.status = bumped;
        return false;
    }
    return true;
}

atp_status atp_graph_observe_with_memory(atp_graph *graph, const char *text, const char *source_id,
                                         const char *author_did, uint64_t observed_at,
                                         uint64_t content_digest, uint32_t schema_version,
                                         uint64_t ledger_id, bool *out_remembered) {
    if (out_remembered) {
        *out_remembered = false;
    }
    if (!graph || !text) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    /* Whole-observation budget check: a text that would cross a ceiling
     * is rejected before any learning happens. */
    const atp_status budget = atp_check_budget(graph, text);
    if (budget != ATP_OK) {
        return budget;
    }

    const uint64_t source_hash = atp_hash_source(source_id);
    const size_t nodes_before = graph->node_count;

    atp_memory_walk walk = {
        .base =
            {
                .graph = graph,
                .source_hash = source_hash,
                .previous = -1,
                .status = ATP_OK,
            },
    };

    /* Tokenization follows the entry's schema version so replay of a
     * schema-1 ledger reproduces schema-1 token identity exactly. */
    atp_tokenize(text, schema_version, atp_memory_emit, &walk);

    if (walk.base.status != ATP_OK) {
        free(walk.counts);
        return walk.base.status;
    }
    if (walk.base.saw_token) {
        graph->observations++;
    }

    /* Count-based selection: new vocabulary or at least two distinct tokens.
     * No hidden thresholds on meaning, sentiment, or topic. */
    const bool remembered = graph->node_count > nodes_before || walk.distinct >= 2u;
    if (!remembered) {
        free(walk.counts);
        return ATP_OK;
    }
    if (out_remembered) {
        *out_remembered = true;
    }

    qsort(walk.counts, walk.distinct, sizeof(*walk.counts), atp_token_count_compare);
    const atp_episode episode =
        atp_episode_build(walk.counts, walk.distinct, ledger_id, source_id, author_did, observed_at,
                          content_digest, schema_version);
    free(walk.counts);

    if (graph->episode_count >= graph->episode_max) {
        atp_evict_episode(graph);
    }
    if (!atp_reserve_episodes(graph, graph->episode_count + 1u)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    graph->episodes[graph->episode_count++] = episode;
    return ATP_OK;
}

typedef struct atp_recall_match {
    size_t episode_index;
    float score;
    uint64_t observed_at;
    uint64_t ledger_id;
} atp_recall_match;

static int atp_recall_match_compare(const void *left, const void *right) {
    const atp_recall_match *a = left;
    const atp_recall_match *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->observed_at < b->observed_at) {
        return 1;
    }
    if (a->observed_at > b->observed_at) {
        return -1;
    }
    if (a->ledger_id < b->ledger_id) {
        return 1;
    }
    if (a->ledger_id > b->ledger_id) {
        return -1;
    }
    return 0;
}

/* Recall query walk: collects distinct known node indices. */
typedef struct atp_query_walk {
    const atp_graph *graph;
    uint32_t *nodes;
    size_t count;
    size_t capacity;
    atp_status status;
} atp_query_walk;

static bool atp_query_emit(void *userdata, const char *token) {
    atp_query_walk *walk = userdata;
    const int32_t node = atp_find_node(walk->graph, token);
    if (node < 0) {
        return true;
    }
    for (size_t i = 0u; i < walk->count; ++i) {
        if (walk->nodes[i] == (uint32_t)node) {
            return true;
        }
    }
    if (walk->count == walk->capacity) {
        const size_t next = walk->capacity ? walk->capacity * 2u : 8u;
        if (next < walk->count || next > SIZE_MAX / sizeof(*walk->nodes)) {
            walk->status = ATP_ERR_OUT_OF_MEMORY;
            return false;
        }
        uint32_t *grown = realloc(walk->nodes, next * sizeof(*grown));
        if (!grown) {
            walk->status = ATP_ERR_OUT_OF_MEMORY;
            return false;
        }
        walk->nodes = grown;
        walk->capacity = next;
    }
    walk->nodes[walk->count++] = (uint32_t)node;
    return true;
}

/* Shared query helper: tokenize a query under the current schema and
 * collect the distinct vocabulary node indices it references. Unknown
 * tokens are ignored; the graph is not mutated. Used by recall and by
 * the semantic memory query path so both share one tokenizer. */
atp_status atp_graph_query_nodes(const atp_graph *graph, const char *query, uint32_t **out_nodes,
                                 size_t *out_count) {
    if (out_nodes) {
        *out_nodes = NULL;
    }
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !query || !out_nodes || !out_count) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_query_walk walk = {
        .graph = graph,
    };
    atp_tokenize(query, ATPERSON_SCHEMA_VERSION, atp_query_emit, &walk);
    if (walk.status != ATP_OK) {
        free(walk.nodes);
        return walk.status;
    }
    *out_nodes = walk.nodes;
    *out_count = walk.count;
    return ATP_OK;
}

atp_status atp_graph_recall(atp_graph *graph, const char *query, uint64_t at_epoch,
                            atp_episode *out, size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !query || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (capacity == 0u) {
        return ATP_OK;
    }
    if (graph->episode_count == 0u) {
        return ATP_OK;
    }

    /* Distinct query node indices; tokens unknown to the vocabulary cannot
     * match anything and do not mutate the graph. */
    uint32_t *query_nodes = NULL;
    size_t query_count = 0u;
    const atp_status queried = atp_graph_query_nodes(graph, query, &query_nodes, &query_count);
    if (queried != ATP_OK) {
        return queried;
    }

    if (query_count == 0u) {
        free(query_nodes);
        return ATP_OK;
    }

    atp_recall_match *matches = malloc(graph->episode_count * sizeof(*matches));
    if (!matches) {
        free(query_nodes);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    size_t match_count = 0u;
    for (size_t i = 0u; i < graph->episode_count; ++i) {
        const atp_episode *episode = &graph->episodes[i];
        float score = 0.0f;
        for (uint32_t s = 0u; s < episode->token_count; ++s) {
            const uint32_t node_index = episode->summary[s].node_index;
            if (node_index >= graph->node_count) {
                continue;
            }
            for (size_t q = 0u; q < query_count; ++q) {
                if (query_nodes[q] == node_index) {
                    score += episode->summary[s].weight;
                    break;
                }
            }
        }
        if (score > 0.0f) {
            matches[match_count++] = (atp_recall_match){
                .episode_index = i,
                .score = score,
                .observed_at = episode->observed_at,
                .ledger_id = episode->ledger_id,
            };
        }
    }
    free(query_nodes);

    qsort(matches, match_count, sizeof(*matches), atp_recall_match_compare);
    const size_t written = match_count < capacity ? match_count : capacity;
    for (size_t i = 0u; i < written; ++i) {
        atp_episode *episode = &graph->episodes[matches[i].episode_index];
        if (out) {
            out[i] = *episode;
        }
        episode->recall_count++;
        episode->last_recall_at = at_epoch;
    }
    free(matches);
    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}

size_t atp_graph_episode_count(const atp_graph *graph) {
    return graph ? graph->episode_count : 0u;
}

atp_status atp_graph_episode_at(const atp_graph *graph, size_t index, atp_episode *out_episode) {
    if (!graph || !out_episode) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->episode_count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_episode = graph->episodes[index];
    return ATP_OK;
}

float atp_graph_familiarity(const atp_graph *graph, const char *token) {
    if (!graph || !token) {
        return 0.0f;
    }
    const int32_t node = atp_find_node(graph, token);
    if (node < 0) {
        return 0.0f;
    }
    return graph->nodes[node].familiarity;
}

void atp_graph_set_capacity(atp_graph *graph, size_t node_capacity_max,
                            size_t edge_capacity_max) {
    if (!graph) {
        return;
    }
    graph->config.node_capacity_max = node_capacity_max;
    graph->config.edge_capacity_max = edge_capacity_max;
}

atp_graph_stats atp_graph_get_stats(const atp_graph *graph) {    if (!graph) {
        return (atp_graph_stats){0};
    }
    return (atp_graph_stats){
        .node_count = graph->node_count,
        .edge_count = graph->edge_count,
        .observations = graph->observations,
        .token_observations = graph->token_observations,
        .training_steps = graph->training_steps,
        .mean_loss =
            graph->training_steps ? graph->loss_total / (double)graph->training_steps : 0.0,
        .episode_count = graph->episode_count,
        .episode_capacity = graph->episode_max,
        .episode_evictions = graph->episode_evictions,
        .capacity_rejections = graph->capacity_rejections,
    };
}

typedef struct atp_ranked_edge {
    const atp_edge *edge;
    float score;
} atp_ranked_edge;

static int atp_ranked_edge_compare(const void *left, const void *right) {
    const atp_ranked_edge *a = left;
    const atp_ranked_edge *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    return 0;
}

/* Single-token lookup normalization. The input is one token, not a
 * sentence, so the whole normalized output is the token — but a caller
 * passing punctuation must not match vocabulary, so only token bytes are
 * kept, same rule as the scanner. */
typedef struct atp_lookup_walk {
    char output[ATPERSON_TOKEN_BYTES];
    size_t len;
} atp_lookup_walk;

static bool atp_lookup_emit(void *userdata, const char *token) {
    atp_lookup_walk *walk = userdata;
    walk->len = strlen(token);
    if (walk->len >= sizeof(walk->output)) {
        walk->len = sizeof(walk->output) - 1u;
    }
    memcpy(walk->output, token, walk->len);
    walk->output[walk->len] = '\0';
    /* First token wins; a lookup input never spans multiple tokens. */
    return false;
}

static void atp_normalize_lookup(const char *input, char output[ATPERSON_TOKEN_BYTES]) {
    atp_lookup_walk walk = {.len = 0u};
    atp_tokenize(input, ATPERSON_SCHEMA_VERSION, atp_lookup_emit, &walk);
    if (walk.len == 0u) {
        output[0] = '\0';
        return;
    }
    memcpy(output, walk.output, walk.len + 1u);
}

atp_status atp_graph_associations(const atp_graph *graph, const char *token, atp_association *out,
                                  size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !token || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    char normalized[ATPERSON_TOKEN_BYTES];
    atp_normalize_lookup(token, normalized);
    const int32_t source = atp_find_node(graph, normalized);
    if (source < 0) {
        return ATP_ERR_NOT_FOUND;
    }

    size_t candidate_count = 0u;
    for (size_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].source == (uint32_t)source) {
            candidate_count++;
        }
    }
    if (candidate_count == 0u || capacity == 0u) {
        return ATP_OK;
    }

    atp_ranked_edge *ranked = malloc(candidate_count * sizeof(*ranked));
    if (!ranked) {
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t next = 0u;
    for (size_t i = 0; i < graph->edge_count; ++i) {
        const atp_edge *edge = &graph->edges[i];
        if (edge->source != (uint32_t)source) {
            continue;
        }
        const float neural = atp_network_score(graph, edge->source, edge->target);
        ranked[next++] = (atp_ranked_edge){
            .edge = edge,
            .score = edge->strength * 0.7f + neural * 0.3f,
        };
    }

    qsort(ranked, candidate_count, sizeof(*ranked), atp_ranked_edge_compare);
    const size_t written = candidate_count < capacity ? candidate_count : capacity;
    for (size_t i = 0; i < written; ++i) {
        const atp_edge *edge = ranked[i].edge;
        const atp_node *target = &graph->nodes[edge->target];
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].token, target->token, sizeof(out[i].token) - 1u);
        out[i].score = ranked[i].score;
        out[i].observations = edge->observations;
        out[i].last_source_hash = edge->last_source_hash;
    }

    free(ranked);
    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}

atp_status atp_graph_add_ledger_entry(atp_graph *graph, const atp_ledger_entry *entry) {
    if (!graph || !entry) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const size_t source_len = strlen(entry->source_id);
    if (source_len == 0u || source_len >= ATPERSON_LEDGER_SOURCE_BYTES) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const size_t author_len = strlen(entry->author_did);
    if (author_len >= ATPERSON_LEDGER_AUTHOR_BYTES) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (!atp_reserve_ledger_entries(graph, graph->ledger_count + 1u)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    graph->ledger_entries[graph->ledger_count] = *entry;
    graph->ledger_count++;
    return ATP_OK;
}

size_t atp_graph_ledger_count(const atp_graph *graph) {
    return graph ? graph->ledger_count : 0u;
}

atp_status atp_graph_ledger_entry(const atp_graph *graph, size_t index,
                                  atp_ledger_entry *out_entry) {
    if (!graph || !out_entry) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->ledger_count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_entry = graph->ledger_entries[index];
    return ATP_OK;
}

const char *atp_status_string(atp_status status) {
    switch (status) {
    case ATP_OK:
        return "ok";
    case ATP_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ATP_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case ATP_ERR_IO:
        return "I/O error";
    case ATP_ERR_FORMAT:
        return "invalid snapshot format";
    case ATP_ERR_NOT_FOUND:
        return "not found";
    case ATP_ERR_SCHEMA:
        return "learning schema not replayable by this build";
    case ATP_ERR_CAPACITY:
        return "configured resource ceiling reached";
    }
    return "unknown error";
}
