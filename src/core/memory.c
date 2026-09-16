#include "atperson/memory.h"

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct atp_recall_match {
    size_t episode_index;
    float score;
    float exact_score;
    float association_score;
    float familiarity_score;
    float recency_score;
    float use_score;
    uint32_t exact_token_matches;
    uint32_t association_token_matches;
    uint64_t observed_at;
    uint64_t ledger_id;
} atp_recall_match;

static float atp_memory_clamp01(float value) {
    if (!(value > 0.0f)) {
        return 0.0f;
    }
    return value >= 1.0f ? 1.0f : value;
}

static float atp_memory_edge_score(const atp_graph *graph, uint32_t source, uint32_t target) {
    const int32_t edge_index = atp_find_edge(graph, source, target);
    if (edge_index < 0) {
        return 0.0f;
    }
    const atp_edge *edge = &graph->edges[edge_index];
    const float neural = atp_network_score(graph, source, target);
    return atp_memory_clamp01(edge->strength * 0.7f + neural * 0.3f);
}

static float atp_memory_association_score(const atp_graph *graph, uint32_t left, uint32_t right) {
    if (left == right) {
        return 0.0f;
    }
    const float forward = atp_memory_edge_score(graph, left, right);
    const float reverse = atp_memory_edge_score(graph, right, left);
    return forward > reverse ? forward : reverse;
}

static float atp_memory_bounded_exposure(float familiarity) {
    if (!(familiarity > 0.0f) || !isfinite(familiarity)) {
        return 0.0f;
    }
    return familiarity / (familiarity + 1.0f);
}

static float atp_memory_recency(uint64_t observed_at, uint64_t at_epoch) {
    if (observed_at == 0u || at_epoch == 0u) {
        return 0.0f;
    }
    const uint64_t age = at_epoch > observed_at ? at_epoch - observed_at : 0u;
    const double scaled =
        (double)age / (double)ATPERSON_RECALL_RECENCY_WINDOW_SECONDS;
    return (float)(1.0 / (1.0 + scaled));
}

static float atp_memory_use(uint64_t recall_count) {
    if (recall_count == 0u) {
        return 0.0f;
    }
    if (recall_count == UINT64_MAX) {
        return 1.0f;
    }
    return (float)((double)recall_count / ((double)recall_count + 1.0));
}

static int atp_recall_match_compare(const void *left, const void *right) {
    const atp_recall_match *a = left;
    const atp_recall_match *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->exact_score < b->exact_score) {
        return 1;
    }
    if (a->exact_score > b->exact_score) {
        return -1;
    }
    if (a->association_score < b->association_score) {
        return 1;
    }
    if (a->association_score > b->association_score) {
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

static atp_status atp_memory_query_nodes(const atp_graph *graph, const char *query,
                                         uint32_t **out_nodes, size_t *out_count) {
    /* Shared tokenizer path (issue #8): identical token identity to
     * observation and recall. */
    return atp_graph_query_nodes(graph, query, out_nodes, out_count);
}

atp_status atp_graph_recall_ranked(atp_graph *graph, const char *query, uint64_t at_epoch,
                                   atp_recall_result *out, size_t capacity,
                                   size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !query || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (capacity == 0u || graph->episode_count == 0u) {
        return ATP_OK;
    }

    uint32_t *query_nodes = NULL;
    size_t query_count = 0u;
    const atp_status query_status =
        atp_memory_query_nodes(graph, query, &query_nodes, &query_count);
    if (query_status != ATP_OK) {
        return query_status;
    }
    if (query_count == 0u) {
        free(query_nodes);
        return ATP_OK;
    }

    if (graph->episode_count > SIZE_MAX / sizeof(atp_recall_match)) {
        free(query_nodes);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    atp_recall_match *matches = malloc(graph->episode_count * sizeof(*matches));
    if (!matches) {
        free(query_nodes);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t match_count = 0u;
    for (size_t i = 0u; i < graph->episode_count; ++i) {
        const atp_episode *episode = &graph->episodes[i];
        float exact = 0.0f;
        float association = 0.0f;
        double familiarity_sum = 0.0;
        double familiarity_weight = 0.0;
        uint32_t exact_matches = 0u;
        uint32_t association_matches = 0u;

        for (uint32_t s = 0u; s < episode->token_count; ++s) {
            const uint32_t summary_node = episode->summary[s].node_index;
            if (summary_node >= graph->node_count) {
                continue;
            }
            const float summary_weight = episode->summary[s].weight;
            bool exact_match = false;
            for (size_t q = 0u; q < query_count; ++q) {
                if (query_nodes[q] == summary_node) {
                    exact_match = true;
                    break;
                }
            }

            float semantic_support = 0.0f;
            if (exact_match) {
                exact += summary_weight;
                exact_matches++;
            } else {
                for (size_t q = 0u; q < query_count; ++q) {
                    const float candidate =
                        atp_memory_association_score(graph, query_nodes[q], summary_node);
                    if (candidate > semantic_support) {
                        semantic_support = candidate;
                    }
                }
                if (semantic_support > 0.0f) {
                    association += summary_weight * semantic_support;
                    association_matches++;
                }
            }

            if (exact_match || semantic_support > 0.0f) {
                familiarity_sum +=
                    (double)summary_weight *
                    (double)atp_memory_bounded_exposure(graph->nodes[summary_node].familiarity);
                familiarity_weight += (double)summary_weight;
            }
        }

        if (!(exact > 0.0f) && !(association > 0.0f)) {
            continue;
        }

        const float familiarity = familiarity_weight > 0.0
                                      ? (float)(familiarity_sum / familiarity_weight)
                                      : 0.0f;
        const float recency = atp_memory_recency(episode->observed_at, at_epoch);
        const float use = atp_memory_use(episode->recall_count);
        const float score =
            exact * ATPERSON_RECALL_EXACT_WEIGHT +
            association * ATPERSON_RECALL_ASSOCIATION_WEIGHT +
            familiarity * ATPERSON_RECALL_FAMILIARITY_WEIGHT +
            recency * ATPERSON_RECALL_RECENCY_WEIGHT +
            use * ATPERSON_RECALL_USE_WEIGHT;

        matches[match_count++] = (atp_recall_match){
            .episode_index = i,
            .score = score,
            .exact_score = exact,
            .association_score = association,
            .familiarity_score = familiarity,
            .recency_score = recency,
            .use_score = use,
            .exact_token_matches = exact_matches,
            .association_token_matches = association_matches,
            .observed_at = episode->observed_at,
            .ledger_id = episode->ledger_id,
        };
    }
    free(query_nodes);

    qsort(matches, match_count, sizeof(*matches), atp_recall_match_compare);
    const size_t written = match_count < capacity ? match_count : capacity;
    for (size_t i = 0u; i < written; ++i) {
        atp_episode *episode = &graph->episodes[matches[i].episode_index];
        if (out) {
            out[i] = (atp_recall_result){
                .episode = *episode,
                .score = matches[i].score,
                .exact_score = matches[i].exact_score,
                .association_score = matches[i].association_score,
                .familiarity_score = matches[i].familiarity_score,
                .recency_score = matches[i].recency_score,
                .use_score = matches[i].use_score,
                .exact_token_matches = matches[i].exact_token_matches,
                .association_token_matches = matches[i].association_token_matches,
            };
        }
        if (episode->recall_count != UINT64_MAX) {
            episode->recall_count++;
        }
        episode->last_recall_at = at_epoch;
    }
    free(matches);

    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}
