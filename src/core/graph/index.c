#include "graph_internal.h"

/* -------- hash indexes (issue #9) --------
 *
 * Open-addressing indexes over the canonical node and edge arrays. The
 * arrays remain the source of truth; the indexes are derived state that
 * any loader rebuilds after restoring the arrays. Power-of-two capacity,
 * linear probing, slot value UINT32_MAX means empty. Node index maps token
 * hash -> node index; edge index maps (source, target) -> edge index. */
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

bool atp_node_index_maybe_grow(atp_graph *graph) {
    if (graph->node_count * 2u + 2u > graph->node_index_capacity) {
        return atp_node_index_rebuild(graph, graph->node_count + 1u);
    }
    return true;
}

/* Insert a freshly appended node index. The token must not already be
 * indexed. Call only after node_count includes the new node. */
void atp_node_index_insert(atp_graph *graph, uint32_t node_index) {
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

bool atp_edge_index_maybe_grow(atp_graph *graph) {
    if (graph->edge_count * 2u + 2u > graph->edge_index_capacity) {
        return atp_edge_index_rebuild(graph, graph->edge_count + 1u);
    }
    return true;
}

void atp_edge_index_insert(atp_graph *graph, uint32_t edge_index) {
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
