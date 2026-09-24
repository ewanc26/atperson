#include "rules.hpp"

#include "resolve.hpp"
#include "state/time.hpp"

#include <cJSON.h>

#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>

namespace atperson {
namespace journal {
namespace {

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

[[noreturn]] void invalid(const std::string &message) {
    throw JournalError("valence rule table: " + message);
}

std::string required_string(const cJSON *object, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        invalid(std::string("rule field '") + name + "' is missing or not a string");
    }
    return field->valuestring;
}

std::optional<std::uint32_t> optional_count(const cJSON *object, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (!cJSON_IsNumber(field) || std::floor(field->valuedouble) != field->valuedouble ||
        field->valuedouble < 1.0 || field->valuedouble > 1e9) {
        invalid(std::string("rule field '") + name + "' must be a positive integer");
    }
    return static_cast<std::uint32_t>(field->valuedouble);
}

std::optional<std::uint64_t> optional_seconds(const cJSON *object, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (!cJSON_IsNumber(field) || std::floor(field->valuedouble) != field->valuedouble ||
        field->valuedouble < 1.0 ||
        field->valuedouble > static_cast<double>(kMaxWithinSeconds)) {
        invalid(std::string("rule field '") + name +
                "' must be a positive integer of seconds (max " +
                std::to_string(kMaxWithinSeconds) + ")");
    }
    return static_cast<std::uint64_t>(field->valuedouble);
}

/* The optional `when.expectation` condition (#149): endorses only the three
 * terminal resolution states, so a condition can never match an action
 * without a prediction or one still pending judgement. */
std::optional<JournalExpectationState> optional_expectation(const cJSON *object) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, "expectation");
    if (field == nullptr) {
        return std::nullopt;
    }
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        invalid("rule field 'when.expectation' must be a string");
    }
    const std::optional<JournalExpectationState> state =
        journal_expectation_state_from_name(field->valuestring);
    if (!state.has_value() || state.value() == JournalExpectationState::None ||
        state.value() == JournalExpectationState::Pending) {
        invalid("rule field 'when.expectation' must be 'met', 'unmet' or 'expired'");
    }
    return state;
}

ValenceRule parse_rule(const cJSON *rule) {
    if (!cJSON_IsObject(rule)) {
        invalid("each entry in 'rules' must be an object");
    }
    ValenceRule parsed;
    parsed.id = required_string(rule, "id");
    if (parsed.id.empty()) {
        invalid("rule field 'id' must not be empty");
    }

    const cJSON *when = cJSON_GetObjectItemCaseSensitive(rule, "when");
    if (!cJSON_IsObject(when)) {
        invalid(std::string("rule '") + parsed.id + "' is missing the 'when' object");
    }
    const std::string outcome_name = required_string(when, "outcome");
    const std::optional<JournalActionOutcome> outcome =
        journal_action_outcome_from_name(outcome_name);
    if (!outcome.has_value()) {
        invalid(std::string("rule '") + parsed.id + "' has unknown outcome '" + outcome_name +
                "'");
    }
    parsed.outcome = outcome.value();
    parsed.min_events = optional_count(when, "min_events");
    parsed.within_seconds = optional_seconds(when, "within_seconds");
    parsed.expectation = optional_expectation(when);

    const std::string kind_name = required_string(rule, "kind");
    const std::optional<atp_valence_kind> kind = valence_kind_from_name(kind_name);
    if (!kind.has_value()) {
        invalid(std::string("rule '") + parsed.id + "' has unknown valence kind '" + kind_name +
                "'");
    }
    parsed.kind = kind.value();

    const cJSON *signal = cJSON_GetObjectItemCaseSensitive(rule, "signal");
    if (!cJSON_IsNumber(signal) || !std::isfinite(signal->valuedouble) ||
        signal->valuedouble < -1.0 || signal->valuedouble > 1.0) {
        invalid(std::string("rule '") + parsed.id + "' field 'signal' must be a number in [-1, 1]");
    }
    parsed.signal = static_cast<float>(signal->valuedouble);
    return parsed;
}

} // namespace

RuleTable parse_rule_table(std::string_view json_text) {
    Json root(cJSON_ParseWithLength(json_text.data(), json_text.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        invalid("not a JSON object");
    }
    const std::string format = required_string(root.get(), "format");
    if (format != kRuleFormat) {
        invalid("unknown format '" + format + "' (expected '" + std::string(kRuleFormat) + "')");
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) ||
        version->valuedouble != static_cast<double>(kRuleFormatVersion)) {
        invalid("unsupported version (expected " + std::to_string(kRuleFormatVersion) + ")");
    }

    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(root.get(), "rules");
    if (!cJSON_IsArray(rules)) {
        invalid("field 'rules' is missing or not an array");
    }
    const int count = cJSON_GetArraySize(rules);
    if (count < 0 || static_cast<std::size_t>(count) > kMaxRules) {
        invalid("too many rules (max " + std::to_string(kMaxRules) + ")");
    }

    RuleTable table;
    table.rules.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        table.rules.push_back(parse_rule(cJSON_GetArrayItem(rules, i)));
    }
    for (std::size_t i = 0u; i < table.rules.size(); ++i) {
        for (std::size_t j = i + 1u; j < table.rules.size(); ++j) {
            if (table.rules[i].id == table.rules[j].id) {
                invalid("duplicate rule id '" + table.rules[i].id + "'");
            }
        }
    }
    return table;
}

RuleTable load_rule_table(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read valence rule table " + path.string());
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    return parse_rule_table(text);
}

const ValenceRule *first_matching_rule(const RuleTable &table, const JournalAction &action,
                                       const std::vector<JournalEvent> &events, std::int64_t now,
                                       const std::vector<JournalIntent> &intents) {
    /* Count the later events linked to this action, honouring the optional
     * within-seconds window. An action timestamp that does not parse is
     * unknown time (0), the same convention the sync engine uses; a rule
     * with a window then matches only events at or after the attempt. */
    const std::uint64_t action_epoch = parse_rfc3339_epoch(action.at).value_or(0u);
    for (const ValenceRule &rule : table.rules) {
        if (rule.outcome != action.outcome) {
            continue;
        }
        if (rule.min_events.has_value()) {
            std::uint32_t matched = 0u;
            for (const JournalEvent &event : events) {
                if (event.action_id != action.id) {
                    continue;
                }
                if (rule.within_seconds.has_value()) {
                    const std::uint64_t event_epoch = parse_rfc3339_epoch(event.at).value_or(0u);
                    if (event_epoch < action_epoch ||
                        event_epoch - action_epoch > rule.within_seconds.value()) {
                        continue;
                    }
                }
                ++matched;
            }
            if (matched < rule.min_events.value()) {
                continue;
            }
        }
        /* An expectation-conditioned rule (#149) fires only when the action's
         * derived resolution state is exactly the named terminal state. */
        if (rule.expectation.has_value() &&
            derive_expectation_state(action, events, now, intents) != rule.expectation.value()) {
            continue;
        }
        return &rule;
    }
    return nullptr;
}

} // namespace journal
} // namespace atperson
