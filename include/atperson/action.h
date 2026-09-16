#ifndef ATPERSON_ACTION_H
#define ATPERSON_ACTION_H

#include "atperson/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_PLAN_MAX_TOKENS 16u
#define ATPERSON_PLAN_MAX_BEAM_WIDTH 8u
#define ATPERSON_PLAN_MAX_CONTEXT_BYTES 4096u
#define ATPERSON_ACTION_MAX_CONSECUTIVE_OCCURRENCES 2u

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
    ATP_ACTION_PLAN_STOP_MAX_TOKENS = 2,
    ATP_ACTION_PLAN_STOP_LOW_SCORE = 3,
    ATP_ACTION_PLAN_STOP_LOW_SUPPORT = 4,
    ATP_ACTION_PLAN_STOP_SCORE_DROP = 5,
    ATP_ACTION_PLAN_STOP_REPETITION = 6,
    ATP_ACTION_PLAN_STOP_CYCLE = 7
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

/** Guard thresholds applied to raw bounded plans by the decision layer. */
typedef struct atp_action_guard_config {
    float min_candidate_score;
    float min_support_score;
    float max_score_drop;
    /* Maximum consecutive occurrences of one generated token: 1 or 2. */
    size_t max_consecutive_occurrences;
} atp_action_guard_config;

typedef struct atp_action_decision_config {
    atp_action_plan_config planner;
    atp_action_guard_config guards;
} atp_action_decision_config;

typedef enum atp_action_abstain_reason {
    ATP_ACTION_ABSTAIN_NONE = 0,
    ATP_ACTION_ABSTAIN_EMPTY_CONTEXT = 1,
    ATP_ACTION_ABSTAIN_NO_CANDIDATES = 2,
    ATP_ACTION_ABSTAIN_LOW_SCORE = 3,
    ATP_ACTION_ABSTAIN_LOW_SUPPORT = 4
} atp_action_abstain_reason;

/**
 * Inspectable evidence for why a guarded plan stopped.
 *
 * `step_index` identifies the raw candidate that triggered a guard. For
 * DEAD_END/MAX_TOKENS it equals the accepted plan length because no rejected
 * candidate exists. `cycle_start_index` is SIZE_MAX unless CYCLE triggered.
 * Threshold fields always echo the configured values so callers never need to
 * infer why a decision was made.
 */
typedef struct atp_action_stop_evidence {
    atp_action_plan_stop_reason reason;
    size_t step_index;
    size_t accepted_steps;
    size_t max_tokens;
    char token[ATPERSON_TOKEN_BYTES];
    float candidate_score;
    float min_candidate_score;
    float support_score;
    float min_support_score;
    float previous_score;
    float max_score_drop;
    size_t consecutive_occurrences;
    size_t max_consecutive_occurrences;
    size_t cycle_start_index;
} atp_action_stop_evidence;

/**
 * One guarded planning decision. A decision is either an explicit abstention
 * or one accepted (possibly deliberately truncated) plan. No network action is
 * implied by either result.
 */
typedef struct atp_action_decision {
    bool abstained;
    atp_action_abstain_reason abstain_reason;
    atp_action_plan plan;
    atp_action_stop_evidence evidence;
    size_t raw_plan_count;
    size_t viable_plan_count;
} atp_action_decision;

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
 * the previously generated token as its context. Every frontier node expands
 * at most `beam_width` candidates and only the strongest `beam_width` partial
 * plans survive to the next depth.
 *
 * This is the raw bounded planner used for inspection and by the guarded
 * decision layer. It deliberately does not apply abstention/repetition/cycle
 * policy itself.
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

/** Conservative defaults for evidence-based guarded planning. */
atp_action_guard_config atp_action_guard_default_config(void);
atp_action_decision_config atp_action_decision_default_config(void);

/**
 * Produce one explicit guarded decision from the raw bounded planner.
 *
 * Raw plans are generated by `atp_graph_action_plans`, then inspected from the
 * first step onward. A candidate is rejected before it enters the accepted
 * plan when:
 *
 * - candidate score is below `min_candidate_score`;
 * - support score is below `min_support_score`;
 * - its score falls by more than `max_score_drop` from the previous accepted
 *   step;
 * - it would exceed `max_consecutive_occurrences` for one token; or
 * - it revisits an earlier non-consecutive generated token (a cycle).
 *
 * A guard reached after at least one accepted token deliberately terminates
 * that plan and records the triggering evidence. If every raw plan is rejected
 * before its first token, the result explicitly abstains. Empty input and a
 * context with no learned continuations have distinct abstain reasons.
 *
 * Defaults are intentionally permissive enough for sparse learned state while
 * still refusing unsupported candidates and repeated/cyclic continuation:
 * minimum candidate score 0.15, minimum support 0.25, maximum score drop 0.40,
 * one consecutive occurrence. Scores/drop must be finite and in [0,1];
 * consecutive occurrences must be 1..ATPERSON_ACTION_MAX_CONSECUTIVE_OCCURRENCES.
 *
 * The operation is deterministic and read-only. It contains no AT Protocol or
 * outbound-network policy; an accepted plan is evidence for later policy, not
 * permission to publish.
 */
atp_status atp_graph_action_decide(const atp_graph *graph, const char *context,
                                   const atp_action_decision_config *config,
                                   atp_action_decision *out);

#ifdef __cplusplus
}
#endif

#endif
