#include "../internal.h"
#include "atperson/action.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * Bounded plan generation for action continuations. Owns the planner
 * defaults and the deterministic beam search that expands the best raw
 * candidate continuations into finished atp_action_plan values.
 *
 * Collaborators: candidate aggregation (candidates.c), the shared plan
 * ordering (ordering.c), and internal.h graph/tokenizer machinery via the
 * public atp_graph_action_candidates API. Pure read-only; never mutates
 * the graph. Caller owns the output array.
 *
 * Failure modes: ATP_ERR_INVALID_ARGUMENT for null arguments, empty or
 * out-of-range planner config and over-long contexts; ATP_ERR_OUT_OF_MEMORY
 * or ATP_ERR_FORMAT (proposal overflow, unreachable given hard limits) while
 * leaving the graph untouched. ATP_OK with *out_count zero means bounded
 * exhaustion or empty learned state.
 *
 * Determinism: beam pruning and final ranking use atp_action_plan_compare so
 * repeated calls on unchanged learned state are identical, including ties.
 */

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

static int atp_action_plan_work_compare(const void *left, const void *right) {
    const atp_action_plan_work *a = left;
    const atp_action_plan_work *b = right;
    return atp_action_plan_compare(&a->plan, &b->plan);
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
