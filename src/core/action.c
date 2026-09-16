#include "atperson/action.h"
#include "internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Context walk: collects distinct known node indices from the context
 * string, using the shared tokenizer so action-context tokenisation
 * matches observation token identity (issue #8). */
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

static bool atp_action_context_within_limit(const char *context) {
    for (size_t i = 0u; i <= ATPERSON_PLAN_MAX_CONTEXT_BYTES; ++i) {
        if (context[i] == '\0') {
            return true;
        }
    }
    return false;
}

typedef struct atp_action_plan_work {
    atp_action_plan plan;
    double score_sum;
} atp_action_plan_work;

static int atp_action_plan_value_compare(const atp_action_plan *a, const atp_action_plan *b) {
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->step_count < b->step_count) {
        return 1;
    }
    if (a->step_count > b->step_count) {
        return -1;
    }

    for (size_t i = 0u; i < a->step_count; ++i) {
        const int token_order = strcmp(a->steps[i].token, b->steps[i].token);
        if (token_order != 0) {
            return token_order;
        }
    }

    if (a->stop_reason < b->stop_reason) {
        return -1;
    }
    if (a->stop_reason > b->stop_reason) {
        return 1;
    }
    return 0;
}

static int atp_action_plan_work_compare(const void *left, const void *right) {
    const atp_action_plan_work *a = left;
    const atp_action_plan_work *b = right;
    return atp_action_plan_value_compare(&a->plan, &b->plan);
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

        const float neural = atp_network_score(graph, edge->source, edge->target);
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

atp_action_plan_config atp_action_plan_default_config(void) {
    return (atp_action_plan_config){.max_tokens = 8u, .beam_width = 4u};
}

atp_status atp_graph_action_plans(const atp_graph *graph, const char *context,
                                  const atp_action_plan_config *config, atp_action_plan *out,
                                  size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !context || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const atp_action_plan_config planner = config ? *config : atp_action_plan_default_config();
    if (planner.max_tokens == 0u || planner.max_tokens > ATPERSON_PLAN_MAX_TOKENS ||
        planner.beam_width == 0u || planner.beam_width > ATPERSON_PLAN_MAX_BEAM_WIDTH ||
        !atp_action_context_within_limit(context)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (capacity == 0u || graph->node_count == 0u || graph->edge_count == 0u) {
        return ATP_OK;
    }

    const size_t proposal_capacity = planner.beam_width * planner.beam_width;
    const size_t completed_capacity = planner.beam_width * (planner.max_tokens + 1u);
    atp_action_plan_work *frontier = calloc(planner.beam_width, sizeof(*frontier));
    atp_action_plan_work *proposals = calloc(proposal_capacity, sizeof(*proposals));
    atp_action_plan_work *completed = calloc(completed_capacity, sizeof(*completed));
    if (!frontier || !proposals || !completed) {
        free(frontier);
        free(proposals);
        free(completed);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t frontier_count = 1u;
    size_t completed_count = 0u;
    atp_status status = ATP_OK;

    for (size_t depth = 0u; depth < planner.max_tokens && frontier_count > 0u; ++depth) {
        size_t proposal_count = 0u;

        for (size_t i = 0u; i < frontier_count; ++i) {
            atp_action_plan_work *current = &frontier[i];
            const char *step_context = current->plan.step_count == 0u
                                           ? context
                                           : current->plan.steps[current->plan.step_count - 1u].token;
            atp_action_candidate candidates[ATPERSON_PLAN_MAX_BEAM_WIDTH] = {0};
            size_t candidate_count = 0u;
            status = atp_graph_action_candidates(graph, step_context, candidates,
                                                 planner.beam_width, &candidate_count);
            if (status != ATP_OK) {
                goto cleanup;
            }

            if (candidate_count == 0u) {
                if (current->plan.step_count > 0u) {
                    current->plan.stop_reason = ATP_ACTION_PLAN_STOP_DEAD_END;
                    completed[completed_count++] = *current;
                }
                continue;
            }

            for (size_t j = 0u; j < candidate_count; ++j) {
                if (proposal_count >= proposal_capacity) {
                    status = ATP_ERR_FORMAT;
                    goto cleanup;
                }

                atp_action_plan_work next_plan = *current;
                next_plan.plan.steps[next_plan.plan.step_count] = candidates[j];
                next_plan.plan.step_count++;
                next_plan.score_sum += (double)candidates[j].score;
                next_plan.plan.score =
                    (float)(next_plan.score_sum / (double)next_plan.plan.step_count);
                next_plan.plan.stop_reason = ATP_ACTION_PLAN_STOP_NONE;
                proposals[proposal_count++] = next_plan;
            }
        }

        if (proposal_count == 0u) {
            frontier_count = 0u;
            break;
        }

        qsort(proposals, proposal_count, sizeof(*proposals), atp_action_plan_work_compare);
        frontier_count = proposal_count < planner.beam_width ? proposal_count : planner.beam_width;
        memcpy(frontier, proposals, frontier_count * sizeof(*frontier));
    }

    for (size_t i = 0u; i < frontier_count; ++i) {
        frontier[i].plan.stop_reason = ATP_ACTION_PLAN_STOP_MAX_TOKENS;
        completed[completed_count++] = frontier[i];
    }

    if (completed_count > 0u) {
        qsort(completed, completed_count, sizeof(*completed), atp_action_plan_work_compare);
        const size_t written = completed_count < capacity ? completed_count : capacity;
        for (size_t i = 0u; i < written; ++i) {
            out[i] = completed[i].plan;
        }
        if (out_count) {
            *out_count = written;
        }
    }

cleanup:
    free(frontier);
    free(proposals);
    free(completed);
    return status;
}
