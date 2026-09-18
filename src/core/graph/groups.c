#include "graph_internal.h"

/* -------- episode group index (issue #55) --------
 *
 * Derived, inspectable structure over the episode array. Episodes are
 * grouped by their summary-token set: the group key is the FNV-1a hash of
 * the episode's summary node indices in stored (weight-descending) order,
 * so episodes that remember the same vocabulary land in the same group.
 * The key is derived from persisted fields alone -- a graph rebuilt from
 * a snapshot reproduces identical groups.
 *
 * The index is conservative for recall prefiltering: a group's token set
 * is the union of its member summaries, so a query token that overlaps any
 * member's summary is present in the group set. A group with zero overlap
 * against the query cannot contain a scoring episode, so skipping it
 * never changes recall results -- only the scan cost. When the union
 * saturates (more distinct tokens than the per-group bound) the group is
 * treated as always-scanning, keeping the prefilter conservative.
 *
 * Storage: an open key -> group-id table (power-of-two capacity, linear
 * probing), a group array, and a per-episode group-id array parallel to
 * the episodes. Eviction compacts the episode array with memmove, which
 * invalidates the parallel array, so eviction rebuilds membership in place
 * (episodes are bounded; the rebuild is O(n) against an already-O(n)
 * eviction scan). Per-group eviction counters survive rebuilds keyed by
 * group key; they are process-local and reset on load, like the index. */

#define ATP_GROUP_EMPTY UINT32_MAX

static uint64_t atp_group_hash_key(uint64_t key) {
    /* Keys are FNV-1a outputs, already well spread; finalize anyway so
     * probe position and stored key are decorrelated. */
    key ^= key >> 33;
    key *= UINT64_C(0xFF51AFD7ED558CCD);
    key ^= key >> 33;
    return key;
}

/* Group key for one episode: FNV-1a over the summary node indices in
 * stored order. Group identity is the key value; two different token sets
 * that collide merge into one group, which is safe -- the union token set
 * is computed from actual members, so the prefilter stays conservative. */
static uint64_t atp_episode_group_key(const atp_episode *episode) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t i = 0u; i < episode->token_count; ++i) {
        const uint64_t node = episode->summary[i].node_index;
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (node >> shift) & UINT64_C(0xFF);
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

/* Grow the key table so it holds group_count entries at load <= 0.5, then
 * reinsert every existing group. */
static bool atp_group_table_rebuild(atp_graph *graph, size_t needed) {
    size_t capacity = graph->group_table_capacity ? graph->group_table_capacity * 2u : 16u;
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
        slots[i] = ATP_GROUP_EMPTY;
    }

    for (size_t g = 0u; g < graph->group_count; ++g) {
        const uint64_t hash = atp_group_hash_key(graph->groups[g].key);
        size_t slot = (size_t)(hash & (uint64_t)(capacity - 1u));
        while (slots[slot] != ATP_GROUP_EMPTY) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        slots[slot] = (uint32_t)g;
    }

    free(graph->group_table_slots);
    graph->group_table_slots = slots;
    graph->group_table_capacity = capacity;
    return true;
}

/* Look up the group id for `key`, or ATP_GROUP_EMPTY when absent. */
static uint32_t atp_group_find(const atp_graph *graph, uint64_t key) {
    if (!graph->group_table_slots) {
        return ATP_GROUP_EMPTY;
    }
    const uint64_t mask = (uint64_t)(graph->group_table_capacity - 1u);
    size_t slot = (size_t)(atp_group_hash_key(key) & mask);
    for (;;) {
        const uint32_t entry = graph->group_table_slots[slot];
        if (entry == ATP_GROUP_EMPTY) {
            return ATP_GROUP_EMPTY;
        }
        if (graph->groups[entry].key == key) {
            return entry;
        }
        slot = (slot + 1u) & (size_t)mask;
    }
}

/* Insert a new group for `key` and return its id. Call only when the key is
 * absent. On allocation failure returns ATP_GROUP_EMPTY. */
static uint32_t atp_group_create(atp_graph *graph, uint64_t key) {
    if (graph->group_count == graph->group_capacity) {
        const size_t next = graph->group_capacity ? graph->group_capacity * 2u : 8u;
        atp_episode_group *grown = realloc(graph->groups, next * sizeof(*grown));
        if (!grown) {
            return ATP_GROUP_EMPTY;
        }
        graph->groups = grown;
        graph->group_capacity = next;
    }
    if (graph->group_count * 2u + 2u > graph->group_table_capacity) {
        if (!atp_group_table_rebuild(graph, graph->group_count + 1u)) {
            return ATP_GROUP_EMPTY;
        }
    }

    atp_episode_group *group = &graph->groups[graph->group_count];
    group->key = key;
    group->token_count = 0u;
    group->evictions = 0u;

    /* Insert the new key into the open-addressing table. The load-factor
     * check above guarantees a free slot exists. */
    const uint64_t mask = (uint64_t)(graph->group_table_capacity - 1u);
    size_t slot = (size_t)(atp_group_hash_key(key) & mask);
    while (graph->group_table_slots[slot] != ATP_GROUP_EMPTY) {
        slot = (slot + 1u) & (size_t)mask;
    }
    graph->group_table_slots[slot] = (uint32_t)graph->group_count;

    return (uint32_t)graph->group_count++;
}

/* Ensure the parallel member array covers `needed` episodes. */
static bool atp_group_members_reserve(atp_graph *graph, size_t needed) {
    if (needed <= graph->group_member_capacity) {
        return true;
    }
    size_t capacity = graph->group_member_capacity ? graph->group_member_capacity : 64u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    uint32_t *grown = realloc(graph->episode_group_ids, capacity * sizeof(*grown));
    if (!grown) {
        return false;
    }
    graph->episode_group_ids = grown;
    graph->group_member_capacity = capacity;
    return true;
}

/* Index one episode (at `episode_index`) into the group structures:
 * find-or-create its group, assign the parallel member id, union the
 * summary tokens. Call only when the parallel array already covers the
 * episode. */
static bool atp_group_index_episode(atp_graph *graph, size_t episode_index) {
    const atp_episode *episode = &graph->episodes[episode_index];
    const uint64_t key = atp_episode_group_key(episode);
    uint32_t group_id = atp_group_find(graph, key);
    if (group_id == ATP_GROUP_EMPTY) {
        group_id = atp_group_create(graph, key);
        if (group_id == ATP_GROUP_EMPTY) {
            return false;
        }
    }
    graph->episode_group_ids[episode_index] = group_id;

    atp_episode_group *group = &graph->groups[group_id];
    for (uint32_t t = 0u; t < episode->token_count; ++t) {
        const uint32_t node = episode->summary[t].node_index;
        bool present = false;
        for (uint32_t g = 0u; g < group->token_count; ++g) {
            if (group->tokens[g] == node) {
                present = true;
                break;
            }
        }
        /* A saturated union is incomplete; recall treats a full token set
         * as always-scanning, so the prefilter stays conservative. */
        if (!present && group->token_count < ATPERSON_GROUP_TOKEN_MAX) {
            group->tokens[group->token_count++] = node;
        }
    }
    return true;
}

/* Rebuild all group membership from the episode array. Derived state only;
 * per-group eviction counters survive the rebuild keyed by group key. */
bool atp_episode_groups_rebuild(atp_graph *graph) {
    if (!graph) {
        return false;
    }

    /* Snapshot eviction counters keyed by group key so they survive the
     * membership reset. */
    typedef struct {
        uint64_t key;
        uint64_t evictions;
    } atp_group_eviction_carry;
    atp_group_eviction_carry *carry = NULL;
    if (graph->group_count > 0u) {
        carry = malloc(graph->group_count * sizeof(*carry));
        if (!carry) {
            return false;
        }
        for (size_t g = 0u; g < graph->group_count; ++g) {
            carry[g].key = graph->groups[g].key;
            carry[g].evictions = graph->groups[g].evictions;
        }
    }
    const size_t carry_count = graph->group_count;

    if (!atp_group_members_reserve(graph, graph->episode_count)) {
        free(carry);
        return false;
    }

    /* Reset the derived structures atomically: the hash table must be
     * empty too, or atp_group_find would match stale entries against
     * group slots that are about to be reassigned. */
    if (graph->group_table_slots) {
        for (size_t s = 0u; s < graph->group_table_capacity; ++s) {
            graph->group_table_slots[s] = ATP_GROUP_EMPTY;
        }
    }
    graph->group_count = 0u;
    for (size_t i = 0u; i < graph->episode_count; ++i) {
        const uint64_t key = atp_episode_group_key(&graph->episodes[i]);
        uint32_t group_id = atp_group_find(graph, key);
        if (group_id == ATP_GROUP_EMPTY) {
            group_id = atp_group_create(graph, key);
            if (group_id == ATP_GROUP_EMPTY) {
                free(carry);
                return false;
            }
            /* Restore the carried eviction counter for this key. */
            for (size_t c = 0u; c < carry_count; ++c) {
                if (carry[c].key == key) {
                    graph->groups[group_id].evictions = carry[c].evictions;
                    break;
                }
            }
        }
        graph->episode_group_ids[i] = group_id;

        /* Union the member's summary tokens into the group token set. */
        atp_episode_group *group = &graph->groups[group_id];
        for (uint32_t t = 0u; t < graph->episodes[i].token_count; ++t) {
            const uint32_t node = graph->episodes[i].summary[t].node_index;
            bool present = false;
            for (uint32_t g = 0u; g < group->token_count; ++g) {
                if (group->tokens[g] == node) {
                    present = true;
                    break;
                }
            }
            if (!present && group->token_count < ATPERSON_GROUP_TOKEN_MAX) {
                group->tokens[group->token_count++] = node;
            }
        }
    }
    free(carry);
    return true;
}

/* Incrementally index the most recently appended episode. Returns false on
 * allocation failure, in which case the index is left absent for the new
 * episode (episode_group_ids may be stale beyond the previous count) and
 * callers should fall back to a full rebuild or a linear scan. */
bool atp_episode_groups_append(atp_graph *graph) {
    if (!graph || graph->episode_count == 0u) {
        return false;
    }
    if (!atp_group_members_reserve(graph, graph->episode_count)) {
        return false;
    }
    return atp_group_index_episode(graph, graph->episode_count - 1u);
}

/* Record one episode eviction into its group's counter. Call before the
 * episode array is compacted, while the parallel member array is valid. */
void atp_episode_groups_note_eviction(atp_graph *graph, size_t episode_index) {
    if (!graph || !graph->episode_group_ids || episode_index >= graph->episode_count) {
        return;
    }
    const uint32_t group_id = graph->episode_group_ids[episode_index];
    if (group_id < graph->group_count) {
        graph->groups[group_id].evictions++;
    }
}

/* Group id for one episode, or ATP_GROUP_EMPTY when the index is absent
 * (e.g. allocation failed at build time). */
uint32_t atp_episode_group_of(const atp_graph *graph, size_t episode_index) {
    if (!graph || !graph->episode_group_ids || episode_index >= graph->episode_count) {
        return ATP_GROUP_EMPTY;
    }
    return graph->episode_group_ids[episode_index];
}

atp_status atp_graph_episode_groups(const atp_graph *graph, atp_episode_group *out,
                                    size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (out_count) {
        *out_count = graph->group_count;
    }
    const size_t written = graph->group_count < capacity ? graph->group_count : capacity;
    for (size_t i = 0u; i < written; ++i) {
        out[i] = graph->groups[i];
    }
    return ATP_OK;
}

atp_status atp_graph_episode_group_members(const atp_graph *graph, uint32_t group_id,
                                           uint64_t *out, size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (!graph->episode_group_ids || group_id >= graph->group_count) {
        return ATP_ERR_NOT_FOUND;
    }
    size_t member_count = 0u;
    for (size_t i = 0u; i < graph->episode_count; ++i) {
        if (graph->episode_group_ids[i] == group_id) {
            if (member_count < capacity) {
                out[member_count] = graph->episodes[i].ledger_id;
            }
            member_count++;
        }
    }
    if (out_count) {
        *out_count = member_count;
    }
    return ATP_OK;
}

void atp_episode_groups_destroy(atp_graph *graph) {
    if (!graph) {
        return;
    }
    free(graph->groups);
    free(graph->group_table_slots);
    free(graph->episode_group_ids);
    graph->groups = NULL;
    graph->group_count = 0u;
    graph->group_capacity = 0u;
    graph->group_table_slots = NULL;
    graph->group_table_capacity = 0u;
    graph->episode_group_ids = NULL;
    graph->group_member_capacity = 0u;
}
