#include "audit/command.hpp"

#include "audit/evidence.hpp"
#include "audit/questions.hpp"
#include "audit/transport.hpp"
#include "audit/verdict.hpp"

#include <cJSON.h>

#include <charconv>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace atperson {
namespace audit {

namespace {

constexpr const char *kEnvApiKey = "ATPERSON_TYPESAFE_API_KEY";
constexpr const char *kEnvEndpoint = "ATPERSON_TYPESAFE_ENDPOINT";
constexpr const char *kDefaultEndpoint = "https://api.typesafe.ai/v1/systemone";
constexpr const char *kModel = "jev-latest";

std::size_t parse_bounded(std::string_view value, std::size_t minimum, std::size_t maximum,
                          std::string_view name) {
    std::size_t parsed = 0u;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum) {
        throw std::runtime_error(std::string(name) + " must be between " +
                                 std::to_string(minimum) + " and " +
                                 std::to_string(maximum));
    }
    return parsed;
}

atp_action_decision_config decision_config_from_arguments(
    const std::vector<std::string_view> &arguments) {
    atp_action_decision_config config = atp_action_decision_default_config();
    if (arguments.size() >= 2u) {
        config.planner.max_tokens = parse_bounded(arguments[1], 1u, ATPERSON_PLAN_MAX_TOKENS,
                                                  "max-tokens");
    }
    if (arguments.size() >= 3u) {
        config.planner.beam_width = parse_bounded(arguments[2], 1u,
                                                  ATPERSON_PLAN_MAX_BEAM_WIDTH, "beam-width");
    }
    return config;
}

std::string env_or(const char *name, const char *fallback) {
    if (const char *value = std::getenv(name); value && value[0] != '\0') {
        return value;
    }
    return fallback ? fallback : "";
}

using Payload = AuditJson;

} // namespace

int run_audit_command(std::ostream &out, const LanguageGraph &graph,
                      std::string_view command,
                      const std::vector<std::string_view> &arguments) {
    (void)command;
    if (arguments.empty() || arguments.size() > 3u) {
        throw std::runtime_error("audit usage: audit <context> [max-tokens] [beam-width]");
    }

    const std::string api_key = env_or(kEnvApiKey, nullptr);
    if (api_key.empty()) {
        throw std::runtime_error(
            std::string("missing required environment variable ") + kEnvApiKey +
            " (audit sends the bounded decision trace to " + kDefaultEndpoint +
            " for an advisory judgment); set the key to opt in");
    }
    const std::string endpoint = env_or(kEnvEndpoint, kDefaultEndpoint);

    const atp_action_decision_config config = decision_config_from_arguments(arguments);
    const atp_action_decision decision = graph.action_decide(arguments[0], config);

    const AuditJson state = build_audit_state(config, decision, arguments[0]);
    const AuditJson questions = build_audit_questions(!decision.abstained);

    cJSON *payload_json = cJSON_CreateObject();
    if (!payload_json) {
        throw std::runtime_error("audit: failed to allocate request JSON");
    }
    Payload payload(payload_json);
    cJSON_AddStringToObject(payload_json, "model", kModel);
    if (!cJSON_AddItemToObject(payload_json, "state", cJSON_Duplicate(state.get(), 1))) {
        throw std::runtime_error("audit: failed to embed state JSON");
    }
    if (!cJSON_AddItemToObject(payload_json, "questions", cJSON_Duplicate(questions.get(), 1))) {
        throw std::runtime_error("audit: failed to embed questions JSON");
    }

    const std::string body = json_to_string(payload);
    const std::string response = post_typesafe_evaluation(endpoint, api_key, body);

    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) {
        throw std::runtime_error("audit: TypeSafe response was not valid JSON");
    }
    Payload response_owned(root);
    const AuditVerdict verdict = parse_audit_response(root);

    render_audit_report(out, decision, verdict);
    return 0;
}

} // namespace audit
} // namespace atperson