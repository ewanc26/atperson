#ifndef ATPERSON_ACTION_H
#define ATPERSON_ACTION_H

#include "atperson/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_PLAN_MAX_TOKENS 16u
#define ATPERSON_PLAN_MAX_BEAM_WIDTH 8u
#define ATPERSON_PLAN_MAX_CONTEXT_BYTES 4096u

/*
 * Read-only action-model primitive.
 *
 * The first action-model pass does not publish anything. It ranks learned
 * token continuations that are supported by outgoing associations from the
 * supplied context. Every score component is returned so later policy can be
 * inspected rather than hidden behind an opaque decision.
 */
typedef struct atp_action_candidate {
    char token[ATPERSON_TOKEN_BYTES];
    float score;
    float association_score;
    float familiarity_score;
    float support_score;
    uint64_t supporting_observations;
    uint32_t context_matches;
} atp_action_candidate;

typedef enum atp_action_plan_stop_reason {
    ATP_ACTION_PLAN_STOP_NONE = 0,
    ATP_ACTION_PLAN_STOP_DEAD_END = 1,
    ATP_ACTION_PLAN_STOP_MAX_TOKENS = 2
} atp_action_plan_stop_reason;

typedef struct atp_action_plan_config {
    size_t max_tokens;
    size_t beam_width;
} atp_action_plan_config;

typedef struct atp_action_plan {
    atp_action_candidate steps[ATPERSON_PLAN_MAX_TOKENS];
    size_t step_count;
    float score;
    atp_action_plan_stop_reason stop_reason;
} atp_action_plan;

/**
 * Rank learned continuation candidates for `context` without mutating the
 * graph. Unknown context tokens are ignored and an entirely unknown context
 * produces zero candidates.
 *
 * Candidate generation uses only existing outgoing graph edges. For each
 * target token:
 *
 *   association_score = mean(0.7 * edge_strength + 0.3 * neural_score)
 *   familiarity_score = familiarity * (1 - familiarity_decay), clamped 0..1
 *   support_score     = observations / (observations + 1)
 *   score             = mean(association_score, familiarity_score,
 *                            support_score)
 *
 * `context_matches` is the number of distinct known context tokens that
 * support the target; `supporting_observations` is the summed edge exposure.
 * Results are deterministic and ordered strongest-first.
 */
atp_status atp_graph_action_candidates(const atp_graph *graph, const char *context,
                                       atp_action_candidate *out, size_t capacity,
                                       size_t *out_count);

/** Return conservative defaults for bounded sequence planning. */
atp_action_plan_config atp_action_plan_default_config(void);

/**
 * Build bounded deterministic token sequences over the one-step candidate
 * scorer without mutating learned state.
 *
 * The first step is scored from the supplied `context`. Each later step uses
 * the previously generated token as its context, keeping this first planner
 * deliberately narrow until structured context assembly is introduced. Every
 * frontier node expands at most `beam_width` candidates and only the strongest
 * `beam_width` partial plans survive to the next depth.
 *
 * `max_tokens` must be in [1, ATPERSON_PLAN_MAX_TOKENS], `beam_width` in
 * [1, ATPERSON_PLAN_MAX_BEAM_WIDTH], and the initial context must terminate
 * within ATPERSON_PLAN_MAX_CONTEXT_BYTES bytes. Values outside those bounds
 * are rejected with ATP_ERR_INVALID_ARGUMENT rather than silently clamped.
 *
 * A plan score is the mean of its per-step candidate scores. Plans ending
 * because their last token has no outgoing candidates report
 * ATP_ACTION_PLAN_STOP_DEAD_END; plans that reach `max_tokens` report
 * ATP_ACTION_PLAN_STOP_MAX_TOKENS. An unknown initial context produces zero
 * plans. Results are ordered strongest-first with deterministic tie-breaking.
 * `capacity` limits only copied results; it does not widen the search.
 */
atp_status atp_graph_action_plans(const atp_graph *graph, const char *context,
                                  const atp_action_plan_config *config, atp_action_plan *out,
                                  size_t capacity, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
