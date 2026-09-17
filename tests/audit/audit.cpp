#include "audit/evidence.hpp"
#include "audit/questions.hpp"
#include "audit/verdict.hpp"

#include "atperson/action.h"
#include "atperson/core.h"
#include "atperson/graph.hpp"

#include <cJSON.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

namespace {

using atperson::LanguageGraph;
using atperson::audit::AuditJson;
using atperson::audit::AuditVerdict;
using atperson::audit::build_audit_questions;
using atperson::audit::build_audit_state;
using atperson::audit::parse_audit_response;
using atperson::audit::render_audit_report;

using JsonRoot = AuditJson;

JsonRoot parse_json(const char *text) {
    JsonRoot root(cJSON_Parse(text));
    assert(root);
    return root;
}

const char *object_string(cJSON *root, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    assert(cJSON_IsString(value));
    return value->valuestring;
}

double object_number(cJSON *root, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    assert(cJSON_IsNumber(value));
    return value->valuedouble;
}

cJSON *object_item(cJSON *root, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    assert(value);
    return value;
}

void record_learned(LanguageGraph &graph, std::uint64_t id, std::string_view text,
                    std::string_view source, std::string_view author,
                    std::uint64_t observed_at) {
    const std::uint64_t digest = atp_ledger_digest(text.data(), text.size());
    const bool remembered =
        graph.remember(text, source, author, observed_at, digest, ATPERSON_SCHEMA_VERSION, id);
    assert(remembered);

    atp_ledger_entry entry{};
    entry.id = id;
    entry.observed_at = observed_at;
    entry.content_digest = digest;
    entry.schema_version = ATPERSON_SCHEMA_VERSION;
    entry.outcome = ATP_LEDGER_OUTCOME_LEARNED;
    assert(source.size() < sizeof(entry.source_id));
    assert(author.size() < sizeof(entry.author_did));
    source.copy(entry.source_id, source.size());
    entry.source_id[source.size()] = '\0';
    author.copy(entry.author_did, author.size());
    entry.author_did[author.size()] = '\0';
    graph.record_ledger_entry(entry);
}

void test_plan_decision_state_marshals_full_evidence() {
    LanguageGraph graph;
    record_learned(graph, 1u, "start mid", "at://audit/one", "did:plc:alice", 100u);
    record_learned(graph, 2u, "mid tail", "at://audit/two", "did:plc:alice", 200u);

    const atp_graph_stats before = graph.stats();
    const atp_action_decision_config config = atp_action_decision_default_config();
    const atp_action_decision decision = graph.action_decide("start", config);
    AuditJson state = build_audit_state(config, decision, "start");
    assert(std::strcmp(object_string(state.get(), "kind"), "atperson-action-decision") == 0);
    assert(std::strcmp(object_string(state.get(), "context"), "start") == 0);
    assert(std::strcmp(object_string(state.get(), "outcome"), "plan") == 0);
    assert(object_number(state.get(), "raw_plan_count") >= 1.0);
    assert(object_number(state.get(), "viable_plan_count") >= 1.0);

    cJSON *stop = object_item(state.get(), "stop");
    assert(object_number(stop, "accepted_steps") >= 1.0);
    assert(object_number(stop, "max_tokens") == 8.0);

    cJSON *plan = object_item(state.get(), "plan");
    assert(object_number(plan, "step_count") >= 1.0);
    cJSON *steps = object_item(plan, "steps");
    assert(cJSON_IsArray(steps));
    cJSON *first = cJSON_GetArrayItem(steps, 0);
    assert(cJSON_IsObject(first));
    assert(std::strcmp(object_string(first, "token"), "mid") == 0);
    assert(object_number(first, "association") >= 0.0);
    assert(object_number(first, "support") >= 0.0);
    assert(object_number(first, "supporting_observations") >= 1.0);
    assert(object_number(first, "context_matches") >= 1.0);

    cJSON *config_json = object_item(state.get(), "config");
    cJSON *planner = object_item(config_json, "planner");
    assert(object_number(planner, "max_tokens") == 8.0);
    assert(object_number(planner, "beam_width") == 4.0);
    cJSON *guards = object_item(config_json, "guards");
    assert(object_number(guards, "min_candidate_score") == static_cast<double>(0.15f));
    assert(object_number(guards, "min_support_score") == static_cast<double>(0.25f));

    const atp_graph_stats after = graph.stats();
    assert(after.node_count == before.node_count);
    assert(after.training_steps == before.training_steps);
}

void test_abstain_decision_state_has_no_plan() {
    LanguageGraph graph;
    graph.observe("alpha beta", "at://audit/alpha");

    const atp_action_decision_config config = atp_action_decision_default_config();
    const atp_action_decision decision = graph.action_decide("zzz unknown", config);
    assert(decision.abstained);
    AuditJson state = build_audit_state(config, decision, "zzz unknown");
    assert(std::strcmp(object_string(state.get(), "outcome"), "abstain") == 0);
    assert(std::strcmp(object_string(state.get(), "abstain_reason"), "no-candidates") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(state.get(), "plan") == nullptr);
    cJSON *stop = object_item(state.get(), "stop");
    assert(std::strcmp(object_string(stop, "reason"), "none") == 0);
    assert(object_number(state.get(), "raw_plan_count") == 0.0);
}

void test_state_serializes_and_roundtrips() {
    LanguageGraph graph;
    graph.observe("start mid", "at://audit/rt");

    const atp_action_decision_config config = atp_action_decision_default_config();
    const atp_action_decision decision = graph.action_decide("start", config);
    const AuditJson state = build_audit_state(config, decision, "start");
    const std::string text = atperson::audit::json_to_string(state);
    assert(!text.empty());

    JsonRoot reparsed = parse_json(text.c_str());
    const std::string again = atperson::audit::json_to_string(reparsed);
    assert(again == text);
}

void test_questions_schema_by_outcome() {
    AuditJson plan_questions = build_audit_questions(true);
    assert(object_item(plan_questions.get(), "first-step-credible"));
    assert(object_item(plan_questions.get(), "plan-coherent"));
    assert(object_item(plan_questions.get(), "evidence-quality"));

    cJSON *noul = object_item(plan_questions.get(), "first-step-credible");
    assert(std::strcmp(object_string(noul, "type"), "noul") == 0);
    assert(cJSON_IsObject(object_item(noul, "criteria")));

    cJSON *score = object_item(plan_questions.get(), "evidence-quality");
    assert(std::strcmp(object_string(score, "type"), "score") == 0);
    cJSON *levels = object_item(score, "criteria");
    assert(cJSON_IsArray(levels));
    assert(cJSON_GetArraySize(levels) == 3);

    AuditJson abstain_questions = build_audit_questions(false);
    assert(object_item(abstain_questions.get(), "abstain-expected"));
    assert(object_item(abstain_questions.get(), "evidence-quality"));
    assert(cJSON_GetObjectItemCaseSensitive(abstain_questions.get(), "plan-coherent") == nullptr);
    assert(cJSON_GetObjectItemCaseSensitive(abstain_questions.get(), "first-step-credible") ==
           nullptr);
}

void test_verdict_parses_fixture_and_renders_deterministically() {
    const char *fixture =
        "{"
        "  \"model\": \"jev-latest\","
        "  \"answers\": {"
        "    \"first-step-credible\": { \"type\": \"noul\", \"noul\": 0.87 },"
        "    \"plan-coherent\": { \"type\": \"noul\", \"noul\": 0.71 },"
        "    \"evidence-quality\": {"
        "      \"type\": \"score\","
        "      \"score\": 1.6,"
        "      \"confidence\": 0.78,"
        "      \"legend\": { \"0\": \"absent-or-contradicting\", \"1\": "
        "\"thin-single-source\", \"2\": \"solid-multisource\" },"
        "      \"probabilities\": { \"0\": 0.05, \"2\": 0.65, \"1\": 0.30 }"
        "    }"
        "  },"
        "  \"usage\": { \"input_tokens\": 312, \"output_tokens\": 48 }"
        "}";

    JsonRoot root = parse_json(fixture);
    const AuditVerdict verdict = parse_audit_response(root.get());
    assert(verdict.model == "jev-latest");
    assert(verdict.plan_outcome);
    assert(verdict.first_step_credible.has_value());
    assert(*verdict.first_step_credible == 0.87);
    assert(verdict.plan_coherent.has_value());
    assert(*verdict.plan_coherent == 0.71);
    assert(!verdict.abstain_expected.has_value());
    assert(verdict.evidence_quality.has_value());
    assert(verdict.evidence_quality->score == 1.6);
    assert(verdict.evidence_quality->confidence == 0.78);
    assert(verdict.evidence_quality->levels.size() == 3u);
    assert(verdict.evidence_quality->levels[0].key == "0");
    assert(verdict.evidence_quality->levels[0].label == "absent-or-contradicting");
    assert(verdict.evidence_quality->levels[0].probability == 0.05);
    assert(verdict.evidence_quality->levels[2].key == "2");
    assert(verdict.evidence_quality->levels[2].label == "solid-multisource");
    assert(verdict.evidence_quality->levels[2].probability == 0.65);
    assert(verdict.input_tokens == 312);
    assert(verdict.output_tokens == 48);

    LanguageGraph graph;
    record_learned(graph, 1u, "start mid", "at://audit/one", "did:plc:alice", 100u);
    record_learned(graph, 2u, "mid tail", "at://audit/two", "did:plc:alice", 200u);
    const atp_action_decision_config config = atp_action_decision_default_config();
    const atp_action_decision decision = graph.action_decide("start", config);

    std::ostringstream first;
    render_audit_report(first, decision, verdict);
    const std::string report = first.str();
    assert(report.find("layer: learned-core") != std::string::npos);
    assert(report.find("audit-agent: typesafe") != std::string::npos);
    assert(report.find("authority: advisory") != std::string::npos);
    assert(report.find("first-step-credible: 0.870") != std::string::npos);
    assert(report.find("plan-coherent: 0.710") != std::string::npos);
    assert(report.find("evidence-quality: score 1.600") != std::string::npos);
    assert(report.find("usage-tokens: 312 in / 48 out") != std::string::npos);
    assert(report.find("audit-summary: supported") != std::string::npos);

    std::ostringstream second;
    render_audit_report(second, decision, verdict);
    assert(second.str() == report);
}

} // namespace

int main() {
    test_plan_decision_state_marshals_full_evidence();
    test_abstain_decision_state_has_no_plan();
    test_state_serializes_and_roundtrips();
    test_questions_schema_by_outcome();
    test_verdict_parses_fixture_and_renders_deterministically();
    return 0;
}