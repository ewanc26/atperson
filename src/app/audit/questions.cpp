#include "audit/questions.hpp"

#include <stdexcept>

namespace atperson {
namespace audit {

namespace {

const char *const kEvidenceLevelsStart = "absent-or-contradicting";
const char *const kEvidenceLevelsThin = "thin-single-source";
const char *const kEvidenceLevelsSolid = "solid-multisource";

cJSON *make_noul(const char *instructions, const char *yes, const char *no) {
    cJSON *question = cJSON_CreateObject();
    if (!question) {
        throw std::runtime_error("audit: failed to allocate question JSON");
    }
    cJSON_AddStringToObject(question, "type", "noul");
    cJSON_AddStringToObject(question, "instructions", instructions);
    cJSON *criteria = cJSON_AddObjectToObject(question, "criteria");
    cJSON_AddStringToObject(criteria, "true", yes);
    cJSON_AddStringToObject(criteria, "false", no);
    return question;
}

cJSON *make_evidence_score() {
    cJSON *question = cJSON_CreateObject();
    if (!question) {
        throw std::runtime_error("audit: failed to allocate question JSON");
    }
    cJSON_AddStringToObject(question, "type", "score");
    cJSON_AddStringToObject(
        question, "instructions",
        "How solid is the learned-graph evidence behind the decision?");
    cJSON *criteria = cJSON_AddArrayToObject(question, "criteria");
    cJSON_AddItemToArray(criteria, cJSON_CreateString(kEvidenceLevelsStart));
    cJSON_AddItemToArray(criteria, cJSON_CreateString(kEvidenceLevelsThin));
    cJSON_AddItemToArray(criteria, cJSON_CreateString(kEvidenceLevelsSolid));
    return question;
}

void attach_question(cJSON *questions, const char *id, cJSON *question) {
    if (!cJSON_AddItemToObject(questions, id, question)) {
        cJSON_Delete(question);
        throw std::runtime_error("audit: failed to append question JSON");
    }
}

AuditJson build_questions_impl(bool plan_outcome) {
    cJSON *questions = cJSON_CreateObject();
    if (!questions) {
        throw std::runtime_error("audit: failed to allocate questions JSON");
    }
    AuditJson owned(questions);

    if (plan_outcome) {
        attach_question(
            questions, "first-step-credible",
            make_noul(
                "Is the first accepted step of the guarded plan supported by the learned "
                "evidence?",
                "`plan.steps[0].score` and `plan.steps[0].support` sit comfortably above "
                "the configured floors `stop.min_candidate_score` and "
                "`stop.min_support_score`, and `plan.steps[0].supporting_observations` "
                "shows real exposure.",
                "The first accepted step barely clears the score/support floors or relies "
                "on a single weak observation."));
        attach_question(
            questions, "plan-coherent",
            make_noul(
                "Does the accepted step sequence form a coherent continuation of "
                "`context`, rather than a repetitive, cycling, or fragmentary artifact?",
                "The steps are distinct tokens that plausibly continue the context as a "
                "multi-token phrase.",
                "The steps repeat, cycle back, or read as an accidental collection."));
        attach_question(questions, "evidence-quality", make_evidence_score());
    } else {
        attach_question(
            questions, "abstain-expected",
            make_noul(
                "Is abstention the expected outcome given the recorded stop evidence?",
                "`stop.reason` and the recorded `stop.*` values explain why no step met "
                "the guard floors or why every raw plan was rejected before its first "
                "token (`raw_plan_count` > 0, `viable_plan_count` = 0).",
                "The evidence would support a viable plan, or the abstention contradicts "
                "the recorded candidate scores."));
        attach_question(questions, "evidence-quality", make_evidence_score());
    }

    return owned;
}

} // namespace

AuditJson build_audit_questions(bool plan_outcome) {
    return build_questions_impl(plan_outcome);
}

} // namespace audit
} // namespace atperson