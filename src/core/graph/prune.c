/*
 * Vocabulary pruning.
 *
 * Bounds a graph's size by dropping its least-observed tokens. Everything
 * that holds a node index is remapped in one pass so the graph stays
 * self-consistent: edges (dropped when either end goes), valence records and
 * events (their tokens are never dropped), and episode summaries (dropped
 * tokens leave the summary; an episode left with none is evicted). The hash
 * indexes and the derived episode groups are rebuilt from the result.
 *
 * The choice is deterministic: fewest observations, then lowest familiarity,
 * then lowest index. Pruning is deliberate forgetting, not compaction: a
 * replay of the ledger regrows the full vocabulary.
 */

#include "graph_internal.h"

typedef struct atp_prune_candidate {
    uint64_t observations;
    float familiarity;
    uint32_t index;
} atp_prune_candidate;

static int atp_prune_candidate_compare(const void *left, const void *right) {
    const atp_prune_candidate *a = left;
    const atp_prune_candidate *b = right;
    if (a->observations != b->observations) {
        return a->observations < b->observations ? -1 : 1;
    }
    if (a->familiarity != b->familiarity) {
        return a->familiarity < b->familiarity ? -1 : 1;
    }
    if (a->index != b->index) {
        return a->index < b->index ? -1 : 1;
    }
    return 0;
}

#define ATP_PRUNED UINT32_MAX

atp_status atp_graph_prune_vocabulary(atp_graph *graph, size_t max_nodes,
                                      atp_prune_report *report) {
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!graph || max_nodes == 0u || graph->node_count > (size_t)ATP_PRUNED) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const size_t node_count = graph->node_count;
    if (report) {
        report->nodes_before = node_count;
        report->edges_before = graph->edge_count;
        report->episodes_before = graph->episode_count;
        report->nodes_after = node_count;
        report->edges_after = graph->edge_count;
        report->episodes_after = graph->episode_count;
    }
    if (node_count <= max_nodes) {
        return ATP_OK;
    }

    uint32_t *remap = malloc(node_count * sizeof(*remap));
    uint8_t *protect = calloc(node_count, 1u);
    atp_prune_candidate *candidates = malloc(node_count * sizeof(*candidates));
    if (!remap || !protect || !candidates) {
        free(remap);
        free(protect);
        free(candidates);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0u; i < graph->valence_record_count; ++i) {
        const uint32_t node = graph->valence_records[i].node_index;
        if (node < node_count) {
            protect[node] = 1u;
        }
    }
    for (size_t i = 0u; i < graph->valence_event_count; ++i) {
        const uint32_t node = graph->valence_events[i].node_index;
        if (node < node_count) {
            protect[node] = 1u;
        }
    }

    size_t candidate_count = 0u;
    for (size_t i = 0u; i < node_count; ++i) {
        if (!protect[i]) {
            candidates[candidate_count++] = (atp_prune_candidate){
                .observations = graph->nodes[i].observations,
                .familiarity = graph->nodes[i].familiarity,
                .index = (uint32_t)i,
            };
        }
    }
    qsort(candidates, candidate_count, sizeof(*candidates), atp_prune_candidate_compare);
    size_t drop = node_count - max_nodes;
    if (drop > candidate_count) {
        drop = candidate_count; /* protected tokens alone exceed the bound */
    }

    for (size_t i = 0u; i < node_count; ++i) {
        remap[i] = 0u;
    }
    for (size_t i = 0u; i < drop; ++i) {
        remap[candidates[i].index] = ATP_PRUNED;
    }
    free(candidates);
    free(protect);

    size_t next = 0u;
    for (size_t i = 0u; i < node_count; ++i) {
        if (remap[i] == ATP_PRUNED) {
            atp_node_destroy(&graph->nodes[i]);
            continue;
        }
        remap[i] = (uint32_t)next;
        if (next != i) {
            graph->nodes[next] = graph->nodes[i];
            memset(&graph->nodes[i], 0, sizeof(graph->nodes[i]));
        }
        next++;
    }
    graph->node_count = next;

    size_t edge_out = 0u;
    for (size_t i = 0u; i < graph->edge_count; ++i) {
        atp_edge edge = graph->edges[i];
        const uint32_t source = edge.source < node_count ? remap[edge.source] : ATP_PRUNED;
        const uint32_t target = edge.target < node_count ? remap[edge.target] : ATP_PRUNED;
        if (source == ATP_PRUNED || target == ATP_PRUNED) {
            continue;
        }
        edge.source = source;
        edge.target = target;
        graph->edges[edge_out++] = edge;
    }
    graph->edge_count = edge_out;

    for (size_t i = 0u; i < graph->valence_record_count; ++i) {
        graph->valence_records[i].node_index = remap[graph->valence_records[i].node_index];
    }
    for (size_t i = 0u; i < graph->valence_event_count; ++i) {
        graph->valence_events[i].node_index = remap[graph->valence_events[i].node_index];
    }

    size_t episode_out = 0u;
    for (size_t i = 0u; i < graph->episode_count; ++i) {
        atp_episode episode = graph->episodes[i];
        uint32_t kept = 0u;
        for (uint32_t t = 0u; t < episode.token_count; ++t) {
            const uint32_t node = episode.summary[t].node_index;
            if (node < node_count && remap[node] != ATP_PRUNED) {
                episode.summary[kept] = episode.summary[t];
                episode.summary[kept].node_index = remap[node];
                kept++;
            }
        }
        if (kept == 0u) {
            graph->episode_evictions++;
            continue;
        }
        episode.token_count = kept;
        graph->episodes[episode_out++] = episode;
    }
    graph->episode_count = episode_out;
    free(remap);

    if (!atp_graph_rebuild_indexes(graph) || !atp_episode_groups_rebuild(graph)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    if (report) {
        report->nodes_after = graph->node_count;
        report->edges_after = graph->edge_count;
        report->episodes_after = graph->episode_count;
    }
    return ATP_OK;
}
