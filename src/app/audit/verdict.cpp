#include "audit/verdict.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <stdexcept>
#include <string_view>

namespace atperson {
namespace audit {

namespace {

constexpr double kHalf = 0.5;

std::runtime_error malformed(std::string detail) {
    return std::runtime_error("audit: malformed TypeSafe response (" + detail + ")");
}

double require_answer_number(cJSON *object, const char *name, const char *context) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(value)) {
        throw malformed(std::string(context) + " missing " + name);
    }
    return value->valuedouble;
}

/** Numeric level order, then key order, keeps rendering deterministic. */
bool score_level_less(const ScoreLevel &a, const ScoreLevel &b) {
    const long long a_key = std::strtoll(a.key.c_str(), nullptr, 10);
    const long long b_key = std::strtoll(b.key.c_str(), nullptr, 10);
    if (a_key != b_key) {
        return a_key < b_key;
    }
    return a.key < b.key;
}

std::optional<double> parse_noul_answer(cJSON *answers, const char *id) {
    cJSON *answer = cJSON_GetObjectItemCaseSensitive(answers, id);
    if (!answer) {
        return std::nullopt;
    }
    cJSON *noul = cJSON_GetObjectItemCaseSensitive(answer, "noul");
    if (!cJSON_IsNumber(noul)) {
        throw malformed(std::string(id) + " answer missing noul");
    }
    return noul->valuedouble;
}

std::optional<ScoreAnswer> parse_score_answer(cJSON *answers, const char *id) {
    cJSON *answer = cJSON_GetObjectItemCaseSensitive(answers, id);
    if (!answer) {
        return std::nullopt;
    }
    ScoreAnswer result;
    result.score = require_answer_number(answer, "score", id);
    result.confidence = require_answer_number(answer, "confidence", id);

    cJSON *probabilities = cJSON_GetObjectItemCaseSensitive(answer, "probabilities");
    cJSON *legend = cJSON_GetObjectItemCaseSensitive(answer, "legend");
    if (!cJSON_IsObject(probabilities) || !cJSON_IsObject(legend)) {
        throw malformed(std::string(id) + " score missing probabilities/legend");
    }
    for (cJSON *item = probabilities->child; item; item = item->next) {
        if (!item->string || !cJSON_IsNumber(item)) {
            throw malformed(std::string(id) + " malformed probability");
        }
        ScoreLevel level;
        level.key = item->string;
        level.probability = item->valuedouble;
        cJSON *legend_item = cJSON_GetObjectItemCaseSensitive(legend, item->string);
        level.label = cJSON_IsString(legend_item) && legend_item->valuestring
                          ? legend_item->valuestring
                          : std::string(item->string);
        result.levels.push_back(std::move(level));
    }
    std::sort(result.levels.begin(), result.levels.end(), score_level_less);
    return result;
}

std::int64_t require_usage_tokens(cJSON *usage, const char *name) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(usage, name);
    if (!cJSON_IsNumber(value)) {
        throw malformed(std::string("usage missing ") + name);
    }
    return static_cast<std::int64_t>(value->valuedouble);
}

void print_candidate(std::ostream &out, const atp_action_candidate &candidate) {
    out << "  step token=" << candidate.token << " score=" << std::fixed
        << std::setprecision(4) << candidate.score
        << " association=" << candidate.association_score
        << " familiarity=" << candidate.familiarity_score
        << " support=" << candidate.support_score
        << " observations=" << candidate.supporting_observations
        << " context-matches=" << candidate.context_matches << '\n';
}

void render_stop(std::ostream &out, const atp_action_stop_evidence &stop) {
    out << "stop-evidence reason=" << stop_reason_name(stop.reason)
        << " step=" << stop.step_index << " accepted=" << stop.accepted_steps
        << " max-tokens=" << stop.max_tokens;
    if (stop.token[0] != '\0') {
        out << " token=" << stop.token;
    }
    out << " candidate-score=" << std::fixed << std::setprecision(4) << stop.candidate_score
        << " min-score=" << stop.min_candidate_score
        << " support=" << stop.support_score << " min-support=" << stop.min_support_score
        << " previous-score=" << stop.previous_score
        << " max-drop=" << stop.max_score_drop
        << " consecutive=" << stop.consecutive_occurrences
        << " max-consecutive=" << stop.max_consecutive_occurrences;
    if (stop.cycle_start_index != SIZE_MAX) {
        out << " cycle-start=" << stop.cycle_start_index;
    }
    out << '\n';
}

std::string derived_plan_summary(const AuditVerdict &verdict) {
    std::string summary;
    if (verdict.first_step_credible && *verdict.first_step_credible < kHalf) {
        summary = "unsupported";
    } else if (verdict.evidence_quality && verdict.evidence_quality->score >= 1.0) {
        summary = "supported";
    } else {
        summary = "weak";
    }
    if (verdict.plan_coherent && *verdict.plan_coherent < kHalf) {
        summary += ", coherence-concern";
    }
    if (verdict.evidence_quality && verdict.evidence_quality->confidence < kHalf) {
        summary += ", low-confidence";
    }
    if (summary.empty()) {
        summary = "unsupported";
    }
    return summary;
}

std::string derived_abstain_summary(const AuditVerdict &verdict) {
    std::string summary;
    if (verdict.abstain_expected && *verdict.abstain_expected >= kHalf) {
        summary = "abstention-corroborated";
    } else {
        summary = "abstention-questioned";
    }
    if (verdict.evidence_quality && verdict.evidence_quality->confidence < kHalf) {
        summary += ", low-confidence";
    }
    return summary;
}

} // namespace

AuditVerdict parse_audit_response(cJSON *response) {
    if (!cJSON_IsObject(response)) {
        throw malformed("response is not an object");
    }
    AuditVerdict verdict;

    cJSON *model = cJSON_GetObjectItemCaseSensitive(response, "model");
    if (cJSON_IsString(model) && model->valuestring) {
        verdict.model = model->valuestring;
    }
    cJSON *answers = cJSON_GetObjectItemCaseSensitive(response, "answers");
    if (!cJSON_IsObject(answers)) {
        throw malformed("missing answers");
    }
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(response, "usage");
    if (!cJSON_IsObject(usage)) {
        throw malformed("missing usage");
    }

    verdict.first_step_credible = parse_noul_answer(answers, "first-step-credible");
    verdict.plan_coherent = parse_noul_answer(answers, "plan-coherent");
    verdict.abstain_expected = parse_noul_answer(answers, "abstain-expected");
    verdict.evidence_quality = parse_score_answer(answers, "evidence-quality");
    verdict.plan_outcome = verdict.first_step_credible.has_value();
    verdict.input_tokens = require_usage_tokens(usage, "input_tokens");
    verdict.output_tokens = require_usage_tokens(usage, "output_tokens");
    return verdict;
}

void render_decision_trace(std::ostream &out, const atp_action_decision &decision) {
    out << "layer: learned-core\nnetwork-policy: not-evaluated\n"
        << "outcome: " << (decision.abstained ? "abstain" : "plan") << '\n'
        << "abstain-reason: " << abstain_reason_name(decision.abstain_reason) << '\n'
        << "raw-plans: " << decision.raw_plan_count << '\n'
        << "viable-plans: " << decision.viable_plan_count << '\n';
    render_stop(out, decision.evidence);
    if (!decision.abstained) {
        const atp_action_plan &plan = decision.plan;
        out << "plan score=" << std::fixed << std::setprecision(4) << plan.score
            << " stop=" << stop_reason_name(plan.stop_reason) << " steps=" << plan.step_count
            << '\n';
        for (std::size_t i = 0u; i < plan.step_count; ++i) {
            print_candidate(out, plan.steps[i]);
        }
    }
}

void render_audit_verdict(std::ostream &out, const AuditVerdict &verdict) {
    out << "audit-agent: typesafe\n"
        << "model: " << (verdict.model.empty() ? "unknown" : verdict.model) << '\n'
        << "authority: advisory (non-authoritative; learned state, planning, and network "
           "policy are unchanged)\n";
    if (verdict.first_step_credible) {
        out << "first-step-credible: " << std::fixed << std::setprecision(3)
            << *verdict.first_step_credible << '\n';
    }
    if (verdict.plan_coherent) {
        out << "plan-coherent: " << std::fixed << std::setprecision(3)
            << *verdict.plan_coherent << '\n';
    }
    if (verdict.abstain_expected) {
        out << "abstain-expected: " << std::fixed << std::setprecision(3)
            << *verdict.abstain_expected << '\n';
    }
    if (verdict.evidence_quality) {
        const ScoreAnswer &score = *verdict.evidence_quality;
        out << "evidence-quality: score " << std::fixed << std::setprecision(3) << score.score
            << " confidence " << score.confidence << " levels:";
        for (const ScoreLevel &level : score.levels) {
            out << " " << level.key << "=" << level.label << "(" << std::setprecision(3)
                << level.probability << ")";
        }
        out << '\n';
    }
    out << "usage-tokens: " << verdict.input_tokens << " in / " << verdict.output_tokens
        << " out\n"
        << "audit-summary: "
        << (verdict.plan_outcome ? derived_plan_summary(verdict)
                                 : derived_abstain_summary(verdict))
        << '\n';
}

void render_audit_report(std::ostream &out, const atp_action_decision &decision,
                         const AuditVerdict &verdict) {
    render_decision_trace(out, decision);
    render_audit_verdict(out, verdict);
}

} // namespace audit
} // namespace atperson