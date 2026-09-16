#ifndef ATPERSON_ACTION_H
#define ATPERSON_ACTION_H

#include "atperson/core.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif
