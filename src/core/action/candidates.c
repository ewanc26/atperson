#include "action_internal.h"

/*
 * Candidate aggregation and ranking for action continuation. Owns the
 * read-only context walk (distinct known nodes from the context via the
 * shared tokenizer) and the aggregation of association, familiarity and
 * support evidence into ranked atp_action_candidate values.
 *
 * Collaborators: graph/tokenizer machinery via the public
 * atp_graph_action_candidates API. Pure read-only: never mutates the graph.
 * Caller owns the output buffer; nothing is allocated for the caller.
 *
 * Failure modes: ATP_ERR_INVALID_ARGUMENT for null/mismatched arguments,
 * ATP_ERR_OUT_OF_MEMORY on internal scratch failure (state untouched), and
 * ATP_OK with *out_count unset predecessor values on empty context.
 *
 * Determinism: candidate ordering is the documented full tie-break (score,
 * association, familiarity, support observations, then token) so repeated
 * calls on unchanged learned state return identical orderings.
 */

typedef struct atp_context_walk {
    const atp_graph *graph;
    bool *context_nodes;
    size_t context_count;
} atp_context_walk;

static bool atp_context_emit(void *userdata, const char *token) {
    atp_context_walk *walk = userdata;
    const int32_t node = atp_find_node(walk->graph, token);
    if (node >= 0 && !walk->context_nodes[node]) {
        walk->context_nodes[node] = true;
        walk->context_count++;
    }
    return true;
}

static float atp_action_clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static int atp_action_candidate_compare(const void *left, const void *right) {
    const atp_action_candidate *a = left;
    const atp_action_candidate *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->association_score < b->association_score) {
        return 1;
    }
    if (a->association_score > b->association_score) {
        return -1;
    }
    if (a->familiarity_score < b->familiarity_score) {
        return 1;
    }
    if (a->familiarity_score > b->familiarity_score) {
        return -1;
    }
    if (a->supporting_observations < b->supporting_observations) {
        return 1;
    }
    if (a->supporting_observations > b->supporting_observations) {
        return -1;
    }
    return strcmp(a->token, b->token);
}

atp_status atp_graph_action_candidates(const atp_graph *graph, const char *context,
                                       atp_action_candidate *out, size_t capacity,
                                       size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !context || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (capacity == 0u || graph->node_count == 0u || graph->edge_count == 0u) {
        return ATP_OK;
    }

    bool *context_nodes = calloc(graph->node_count, sizeof(*context_nodes));
    if (!context_nodes) {
        return ATP_ERR_OUT_OF_MEMORY;
    }

    atp_context_walk walk = {
        .graph = graph,
        .context_nodes = context_nodes,
        .context_count = 0u,
    };
    atp_tokenize(context, ATPERSON_SCHEMA_VERSION, atp_context_emit, &walk);
    const size_t context_count = walk.context_count;

    if (context_count == 0u) {
        free(context_nodes);
        return ATP_OK;
    }

    float *association_sums = calloc(graph->node_count, sizeof(*association_sums));
    uint64_t *supporting_observations = calloc(graph->node_count, sizeof(*supporting_observations));
    uint32_t *context_matches = calloc(graph->node_count, sizeof(*context_matches));
    if (!association_sums || !supporting_observations || !context_matches) {
        free(context_nodes);
        free(association_sums);
        free(supporting_observations);
        free(context_matches);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0u; i < graph->edge_count; ++i) {
        const atp_edge *edge = &graph->edges[i];
        if (edge->source >= graph->node_count || edge->target >= graph->node_count ||
            !context_nodes[edge->source]) {
            continue;
        }

        float neural = 0.0f;
        const atp_status score_status =
            atp_network_score(graph, edge->source, edge->target, &neural);
        if (score_status != ATP_OK) {
            free(context_nodes);
            free(association_sums);
            free(supporting_observations);
            free(context_matches);
            return score_status;
        }
        const float association = atp_action_clamp01(edge->strength * 0.7f + neural * 0.3f);
        association_sums[edge->target] += association;
        supporting_observations[edge->target] += edge->observations;
        context_matches[edge->target]++;
    }
    free(context_nodes);

    size_t candidate_count = 0u;
    for (size_t i = 0u; i < graph->node_count; ++i) {
        if (context_matches[i] > 0u) {
            candidate_count++;
        }
    }
    if (candidate_count == 0u) {
        free(association_sums);
        free(supporting_observations);
        free(context_matches);
        return ATP_OK;
    }

    atp_action_candidate *ranked = calloc(candidate_count, sizeof(*ranked));
    if (!ranked) {
        free(association_sums);
        free(supporting_observations);
        free(context_matches);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t next = 0u;
    for (size_t i = 0u; i < graph->node_count; ++i) {
        if (context_matches[i] == 0u) {
            continue;
        }

        const float association_score =
            atp_action_clamp01(association_sums[i] / (float)context_matches[i]);
        const float familiarity_score = atp_action_clamp01(
            graph->nodes[i].familiarity * (1.0f - graph->config.familiarity_decay));
        const uint64_t support = supporting_observations[i];
        const float support_score = support == UINT64_MAX
                                        ? 1.0f
                                        : (float)((double)support / ((double)support + 1.0));

        atp_action_candidate *candidate = &ranked[next++];
        strncpy(candidate->token, graph->nodes[i].token, sizeof(candidate->token) - 1u);
        candidate->association_score = association_score;
        candidate->familiarity_score = familiarity_score;
        candidate->support_score = support_score;
        candidate->score = (association_score + familiarity_score + support_score) / 3.0f;
        candidate->supporting_observations = support;
        candidate->context_matches = context_matches[i];
    }

    free(association_sums);
    free(supporting_observations);
    free(context_matches);

    qsort(ranked, candidate_count, sizeof(*ranked), atp_action_candidate_compare);
    const size_t written = candidate_count < capacity ? candidate_count : capacity;
    memcpy(out, ranked, written * sizeof(*out));
    free(ranked);

    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}