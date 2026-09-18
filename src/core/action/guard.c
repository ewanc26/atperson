#include "action_internal.h"

/*
 * Guarded decision layer for action continuations. Owns the guard and
 * decision config defaults and the read-only pass that turns raw bounded
 * plans into a guarded outbound decision: reject weak first steps, apply
 * the score-drop/repetition/cycle stops, and either abstain with inspectable
 * evidence or pick the single best viable plan using the shared ordering.
 *
 * Collaborators: bounded plan generation (plans.c) via the public
 * atp_graph_action_plans API and the shared plan ordering (ordering.c).
 * Pure read-only; never mutates the graph, never performs network I/O.
 * Caller owns the output decision.
 *
 * Failure modes: ATP_ERR_INVALID_ARGUMENT for null/mismatched arguments and
 * invalid planner/guard config ranges; propagates planner errors. A decision
 * with abstained=true carries the concrete abstain reason and stop evidence
 * instead of a hard failure.
 *
 * Determinism: viable-plan selection uses atp_action_plan_compare so repeated
 * calls on unchanged learned state pick the same plan, including ties.
 */

typedef struct atp_guarded_plan {
    atp_action_plan plan;
    atp_action_stop_evidence evidence;
} atp_guarded_plan;

static int atp_guarded_plan_compare(const void *left, const void *right) {
    const atp_guarded_plan *a = left;
    const atp_guarded_plan *b = right;
    return atp_action_plan_compare(&a->plan, &b->plan);
}

static bool atp_plan_config_valid(const atp_action_plan_config *planner) {
    return planner->max_tokens >= 1u && planner->max_tokens <= ATPERSON_PLAN_MAX_TOKENS &&
           planner->beam_width >= 1u && planner->beam_width <= ATPERSON_PLAN_MAX_BEAM_WIDTH;
}

static bool atp_guard_config_valid(const atp_action_guard_config *guards) {
    return isfinite(guards->min_candidate_score) && guards->min_candidate_score >= 0.0f &&
           guards->min_candidate_score <= 1.0f && isfinite(guards->min_support_score) &&
           guards->min_support_score >= 0.0f && guards->min_support_score <= 1.0f &&
           isfinite(guards->max_score_drop) && guards->max_score_drop >= 0.0f &&
           guards->max_score_drop <= 1.0f && guards->max_consecutive_occurrences >= 1u &&
           guards->max_consecutive_occurrences <= ATPERSON_ACTION_MAX_CONSECUTIVE_OCCURRENCES;
}

static void atp_evidence_init(atp_action_stop_evidence *evidence,
                              const atp_action_decision_config *config) {
    memset(evidence, 0, sizeof(*evidence));
    evidence->min_candidate_score = config->guards.min_candidate_score;
    evidence->min_support_score = config->guards.min_support_score;
    evidence->max_score_drop = config->guards.max_score_drop;
    evidence->max_consecutive_occurrences = config->guards.max_consecutive_occurrences;
    evidence->max_tokens = config->planner.max_tokens;
    evidence->cycle_start_index = SIZE_MAX;
}

static void atp_evidence_candidate(atp_action_stop_evidence *evidence,
                                   atp_action_plan_stop_reason reason, size_t step_index,
                                   size_t accepted_steps, const atp_action_candidate *candidate,
                                   float previous_score) {
    evidence->reason = reason;
    evidence->step_index = step_index;
    evidence->accepted_steps = accepted_steps;
    evidence->candidate_score = candidate->score;
    evidence->support_score = candidate->support_score;
    evidence->previous_score = previous_score;
    strncpy(evidence->token, candidate->token, sizeof(evidence->token) - 1u);
}

static void atp_guard_raw_plan(const atp_action_plan *raw,
                               const atp_action_decision_config *config,
                               atp_guarded_plan *guarded) {
    memset(guarded, 0, sizeof(*guarded));
    atp_evidence_init(&guarded->evidence, config);

    double score_sum = 0.0;
    for (size_t i = 0u; i < raw->step_count; ++i) {
        const atp_action_candidate *candidate = &raw->steps[i];
        const float previous_score = guarded->plan.step_count > 0u
                                         ? guarded->plan.steps[guarded->plan.step_count - 1u].score
                                         : 0.0f;

        if (candidate->score < config->guards.min_candidate_score) {
            guarded->plan.stop_reason = ATP_ACTION_PLAN_STOP_LOW_SCORE;
            atp_evidence_candidate(&guarded->evidence, ATP_ACTION_PLAN_STOP_LOW_SCORE, i,
                                   guarded->plan.step_count, candidate, previous_score);
            return;
        }
        if (candidate->support_score < config->guards.min_support_score) {
            guarded->plan.stop_reason = ATP_ACTION_PLAN_STOP_LOW_SUPPORT;
            atp_evidence_candidate(&guarded->evidence, ATP_ACTION_PLAN_STOP_LOW_SUPPORT, i,
                                   guarded->plan.step_count, candidate, previous_score);
            return;
        }
        if (guarded->plan.step_count > 0u &&
            previous_score - candidate->score > config->guards.max_score_drop) {
            guarded->plan.stop_reason = ATP_ACTION_PLAN_STOP_SCORE_DROP;
            atp_evidence_candidate(&guarded->evidence, ATP_ACTION_PLAN_STOP_SCORE_DROP, i,
                                   guarded->plan.step_count, candidate, previous_score);
            return;
        }

        if (guarded->plan.step_count > 0u &&
            strcmp(candidate->token,
                   guarded->plan.steps[guarded->plan.step_count - 1u].token) == 0) {
            size_t consecutive = 1u;
            for (size_t j = guarded->plan.step_count; j > 0u; --j) {
                if (strcmp(candidate->token, guarded->plan.steps[j - 1u].token) != 0) {
                    break;
                }
                consecutive++;
            }
            if (consecutive > config->guards.max_consecutive_occurrences) {
                guarded->plan.stop_reason = ATP_ACTION_PLAN_STOP_REPETITION;
                atp_evidence_candidate(&guarded->evidence, ATP_ACTION_PLAN_STOP_REPETITION, i,
                                       guarded->plan.step_count, candidate, previous_score);
                guarded->evidence.consecutive_occurrences = consecutive;
                return;
            }
        } else {
            for (size_t j = 0u; j < guarded->plan.step_count; ++j) {
                if (strcmp(candidate->token, guarded->plan.steps[j].token) == 0) {
                    guarded->plan.stop_reason = ATP_ACTION_PLAN_STOP_CYCLE;
                    atp_evidence_candidate(&guarded->evidence, ATP_ACTION_PLAN_STOP_CYCLE, i,
                                           guarded->plan.step_count, candidate, previous_score);
                    guarded->evidence.cycle_start_index = j;
                    return;
                }
            }
        }

        guarded->plan.steps[guarded->plan.step_count++] = *candidate;
        score_sum += (double)candidate->score;
        guarded->plan.score = (float)(score_sum / (double)guarded->plan.step_count);
    }

    guarded->plan.stop_reason = raw->stop_reason;
    guarded->evidence.reason = raw->stop_reason;
    guarded->evidence.step_index = guarded->plan.step_count;
    guarded->evidence.accepted_steps = guarded->plan.step_count;
    if (guarded->plan.step_count > 0u) {
        const atp_action_candidate *last = &guarded->plan.steps[guarded->plan.step_count - 1u];
        guarded->evidence.candidate_score = last->score;
        guarded->evidence.support_score = last->support_score;
        if (guarded->plan.step_count > 1u) {
            guarded->evidence.previous_score =
                guarded->plan.steps[guarded->plan.step_count - 2u].score;
        }
        strncpy(guarded->evidence.token, last->token,
                sizeof(guarded->evidence.token) - 1u);
    }
}

static atp_action_abstain_reason atp_abstain_from_stop(atp_action_plan_stop_reason reason) {
    switch (reason) {
    case ATP_ACTION_PLAN_STOP_LOW_SCORE:
        return ATP_ACTION_ABSTAIN_LOW_SCORE;
    case ATP_ACTION_PLAN_STOP_LOW_SUPPORT:
        return ATP_ACTION_ABSTAIN_LOW_SUPPORT;
    default:
        return ATP_ACTION_ABSTAIN_NO_CANDIDATES;
    }
}

atp_action_guard_config atp_action_guard_default_config(void) {
    return (atp_action_guard_config){
        .min_candidate_score = 0.15f,
        .min_support_score = 0.25f,
        .max_score_drop = 0.40f,
        .max_consecutive_occurrences = 1u,
    };
}

atp_action_decision_config atp_action_decision_default_config(void) {
    return (atp_action_decision_config){
        .planner = atp_action_plan_default_config(),
        .guards = atp_action_guard_default_config(),
    };
}

atp_status atp_graph_action_decide(const atp_graph *graph, const char *context,
                                   const atp_action_decision_config *config,
                                   atp_action_decision *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->evidence.cycle_start_index = SIZE_MAX;
    }
    if (!graph || !context || !out) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const atp_action_decision_config decision =
        config ? *config : atp_action_decision_default_config();
    if (!atp_plan_config_valid(&decision.planner) || !atp_guard_config_valid(&decision.guards)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    if (context[0] == '\0') {
        out->abstained = true;
        out->abstain_reason = ATP_ACTION_ABSTAIN_EMPTY_CONTEXT;
        atp_evidence_init(&out->evidence, &decision);
        return ATP_OK;
    }

    atp_action_plan raw[ATPERSON_PLAN_MAX_BEAM_WIDTH] = {0};
    size_t raw_count = 0u;
    const atp_status plan_status =
        atp_graph_action_plans(graph, context, &decision.planner, raw,
                               decision.planner.beam_width, &raw_count);
    if (plan_status != ATP_OK) {
        return plan_status;
    }
    out->raw_plan_count = raw_count;
    if (raw_count == 0u) {
        out->abstained = true;
        out->abstain_reason = ATP_ACTION_ABSTAIN_NO_CANDIDATES;
        atp_evidence_init(&out->evidence, &decision);
        return ATP_OK;
    }

    atp_guarded_plan viable[ATPERSON_PLAN_MAX_BEAM_WIDTH] = {0};
    size_t viable_count = 0u;
    atp_action_stop_evidence strongest_rejection = {0};
    atp_evidence_init(&strongest_rejection, &decision);

    for (size_t i = 0u; i < raw_count; ++i) {
        atp_guarded_plan guarded = {0};
        atp_guard_raw_plan(&raw[i], &decision, &guarded);
        if (guarded.plan.step_count == 0u) {
            if (i == 0u) {
                strongest_rejection = guarded.evidence;
            }
            continue;
        }
        viable[viable_count++] = guarded;
    }

    out->viable_plan_count = viable_count;
    if (viable_count == 0u) {
        out->abstained = true;
        out->abstain_reason = atp_abstain_from_stop(strongest_rejection.reason);
        out->plan.stop_reason = strongest_rejection.reason;
        out->evidence = strongest_rejection;
        return ATP_OK;
    }

    if (viable_count > 1u) {
        qsort(viable, viable_count, sizeof(*viable), atp_guarded_plan_compare);
    }
    out->plan = viable[0].plan;
    out->evidence = viable[0].evidence;
    return ATP_OK;
}