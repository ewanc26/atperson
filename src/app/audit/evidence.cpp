#include "audit/evidence.hpp"

#include <cstdlib>
#include <stdexcept>

namespace atperson {
namespace audit {

namespace {

void attach_string(cJSON *object, const char *name, const char *value) {
    cJSON_AddStringToObject(object, name, value);
}

void attach_finite(cJSON *object, const char *name, float value) {
    cJSON_AddNumberToObject(object, name, static_cast<double>(value));
}

cJSON *attach_object(cJSON *object, const char *name) {
    return cJSON_AddObjectToObject(object, name);
}

cJSON *attach_array(cJSON *object, const char *name) {
    return cJSON_AddArrayToObject(object, name);
}

cJSON *build_candidate(const atp_action_candidate &candidate) {
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        throw std::runtime_error("audit: failed to allocate candidate JSON");
    }
    attach_string(item, "token", candidate.token);
    attach_finite(item, "score", candidate.score);
    attach_finite(item, "association", candidate.association_score);
    attach_finite(item, "familiarity", candidate.familiarity_score);
    attach_finite(item, "support", candidate.support_score);
    cJSON_AddNumberToObject(item, "supporting_observations",
                            static_cast<double>(candidate.supporting_observations));
    cJSON_AddNumberToObject(item, "context_matches",
                            static_cast<double>(candidate.context_matches));
    return item;
}

void build_plan(cJSON *parent, const atp_action_plan &plan) {
    cJSON *plan_object = attach_object(parent, "plan");
    cJSON_AddNumberToObject(plan_object, "step_count",
                            static_cast<double>(plan.step_count));
    attach_finite(plan_object, "score", plan.score);
    const char *stop = stop_reason_name(plan.stop_reason);
    attach_string(plan_object, "stop", stop);
    cJSON *steps = attach_array(plan_object, "steps");
    for (std::size_t i = 0u; i < plan.step_count; ++i) {
        cJSON *step = build_candidate(plan.steps[i]);
        if (!cJSON_AddItemToArray(steps, step)) {
            cJSON_Delete(step);
            throw std::runtime_error("audit: failed to append step JSON");
        }
    }
}

void build_stop(cJSON *parent, const atp_action_stop_evidence &stop) {
    cJSON *object = attach_object(parent, "stop");
    const char *reason = stop_reason_name(stop.reason);
    attach_string(object, "reason", reason);
    cJSON_AddNumberToObject(object, "step_index",
                            static_cast<double>(stop.step_index));
    cJSON_AddNumberToObject(object, "accepted_steps",
                            static_cast<double>(stop.accepted_steps));
    cJSON_AddNumberToObject(object, "max_tokens",
                            static_cast<double>(stop.max_tokens));
    attach_string(object, "token", stop.token);
    attach_finite(object, "candidate_score", stop.candidate_score);
    attach_finite(object, "min_candidate_score", stop.min_candidate_score);
    attach_finite(object, "support_score", stop.support_score);
    attach_finite(object, "min_support_score", stop.min_support_score);
    attach_finite(object, "previous_score", stop.previous_score);
    attach_finite(object, "max_score_drop", stop.max_score_drop);
    cJSON_AddNumberToObject(object, "consecutive_occurrences",
                            static_cast<double>(stop.consecutive_occurrences));
    cJSON_AddNumberToObject(object, "max_consecutive_occurrences",
                            static_cast<double>(stop.max_consecutive_occurrences));
    if (stop.cycle_start_index == SIZE_MAX) {
        cJSON_AddNullToObject(object, "cycle_start_index");
    } else {
        cJSON_AddNumberToObject(object, "cycle_start_index",
                                static_cast<double>(stop.cycle_start_index));
    }
}

void build_config(cJSON *parent, const atp_action_decision_config &config) {
    cJSON *root = attach_object(parent, "config");
    cJSON *planner = attach_object(root, "planner");
    cJSON_AddNumberToObject(planner, "max_tokens",
                            static_cast<double>(config.planner.max_tokens));
    cJSON_AddNumberToObject(planner, "beam_width",
                            static_cast<double>(config.planner.beam_width));
    cJSON *guards = attach_object(root, "guards");
    attach_finite(guards, "min_candidate_score", config.guards.min_candidate_score);
    attach_finite(guards, "min_support_score", config.guards.min_support_score);
    attach_finite(guards, "max_score_drop", config.guards.max_score_drop);
    cJSON_AddNumberToObject(guards, "max_consecutive_occurrences",
                            static_cast<double>(config.guards.max_consecutive_occurrences));
}

} // namespace

std::string json_to_string(const AuditJson &json) {
    if (!json) {
        return {};
    }
    char *text = cJSON_PrintBuffered(json.get(), 0, 0);
    if (!text) {
        throw std::runtime_error("audit: failed to serialize JSON");
    }
    std::unique_ptr<char, void (*)(void *)> owned(text, &std::free);
    return std::string(owned.get());
}

const char *stop_reason_name(atp_action_plan_stop_reason reason) noexcept {
    switch (reason) {
    case ATP_ACTION_PLAN_STOP_NONE:
        return "none";
    case ATP_ACTION_PLAN_STOP_DEAD_END:
        return "dead-end";
    case ATP_ACTION_PLAN_STOP_MAX_TOKENS:
        return "max-tokens";
    case ATP_ACTION_PLAN_STOP_LOW_SCORE:
        return "low-score";
    case ATP_ACTION_PLAN_STOP_LOW_SUPPORT:
        return "low-support";
    case ATP_ACTION_PLAN_STOP_SCORE_DROP:
        return "score-drop";
    case ATP_ACTION_PLAN_STOP_REPETITION:
        return "repetition";
    case ATP_ACTION_PLAN_STOP_CYCLE:
        return "cycle";
    }
    return "unknown";
}

const char *abstain_reason_name(atp_action_abstain_reason reason) noexcept {
    switch (reason) {
    case ATP_ACTION_ABSTAIN_NONE:
        return "none";
    case ATP_ACTION_ABSTAIN_EMPTY_CONTEXT:
        return "empty-context";
    case ATP_ACTION_ABSTAIN_NO_CANDIDATES:
        return "no-candidates";
    case ATP_ACTION_ABSTAIN_LOW_SCORE:
        return "low-score";
    case ATP_ACTION_ABSTAIN_LOW_SUPPORT:
        return "low-support";
    }
    return "unknown";
}

AuditJson build_audit_state(const atp_action_decision_config &config,
                            const atp_action_decision &decision,
                            std::string_view context) {
    cJSON *state = cJSON_CreateObject();
    if (!state) {
        throw std::runtime_error("audit: failed to allocate state JSON");
    }
    AuditJson owned(state);

    attach_string(state, "kind", "atperson-action-decision");
    attach_string(state, "context", std::string(context).c_str());
    attach_string(state, "outcome", decision.abstained ? "abstain" : "plan");
    if (decision.abstained) {
        attach_string(state, "abstain_reason", abstain_reason_name(decision.abstain_reason));
    }
    cJSON_AddNumberToObject(state, "raw_plan_count",
                            static_cast<double>(decision.raw_plan_count));
    cJSON_AddNumberToObject(state, "viable_plan_count",
                            static_cast<double>(decision.viable_plan_count));

    build_stop(state, decision.evidence);
    if (!decision.abstained) {
        build_plan(state, decision.plan);
    }
    build_config(state, config);
    return owned;
}

} // namespace audit
} // namespace atperson