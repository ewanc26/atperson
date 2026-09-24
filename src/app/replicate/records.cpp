#include "records.hpp"

#include "thought/store.hpp"

#include <cJSON.h>

#include <cstring>
#include <memory>
#include <string>

namespace atperson {
namespace {

struct JsonDeleter {
    void operator()(cJSON *json) const noexcept { cJSON_Delete(json); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

[[noreturn]] void invalid(const std::string &message) {
    throw RecordError("atperson record: " + message);
}

std::string require_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring) {
        invalid(std::string("missing or invalid string field '") + key + "'");
    }
    return value->valuestring;
}

std::string optional_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value || cJSON_IsNull(value)) {
        return {};
    }
    if (!cJSON_IsString(value) || !value->valuestring) {
        invalid(std::string("field '") + key + "' must be a string or null");
    }
    return value->valuestring;
}

std::uint64_t require_u64(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring) {
        /* u64 values are serialised as decimal strings (see header). */
        const std::string text(value->valuestring);
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
            invalid(std::string("field '") + key + "' is not a decimal string");
        }
        try {
            return std::stoull(text);
        } catch (const std::exception &) {
            invalid(std::string("field '") + key + "' overflows u64");
        }
    }
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0) {
        invalid(std::string("missing or invalid number field '") + key + "'");
    }
    return static_cast<std::uint64_t>(value->valuedouble);
}

/* u64 as a JSON string, so the full 64 bits survive the round-trip. */
void add_u64_string(cJSON *object, const char *key, std::uint64_t value) {
    cJSON_AddStringToObject(object, key, std::to_string(value).c_str());
}

std::uint32_t require_u32(cJSON *object, const char *key) {
    const std::uint64_t value = require_u64(object, key);
    if (value > 0xFFFFFFFFull) {
        invalid(std::string("field '") + key + "' exceeds u32 range");
    }
    return static_cast<std::uint32_t>(value);
}

double require_number(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value)) {
        invalid(std::string("missing or invalid number field '") + key + "'");
    }
    return value->valuedouble;
}

Json parse_root(std::string_view json) {
    Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        invalid("not a JSON object");
    }
    return root;
}

void require_format(cJSON *root) {
    const std::string format = require_string(root, "format");
    if (format != kRecordFormat) {
        invalid("unknown format '" + format + "'");
    }
    const std::uint32_t version = require_u32(root, "version");
    if (version != kRecordFormatVersion) {
        invalid("unsupported version " + std::to_string(version));
    }
}

std::string print_json(const cJSON *record) {
    char *raw = cJSON_PrintUnformatted(record);
    if (!raw) {
        throw std::runtime_error("atperson record: JSON printing failed");
    }
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

} // namespace

std::string_view ledger_outcome_name(atp_ledger_outcome outcome) {
    switch (outcome) {
    case ATP_LEDGER_OUTCOME_PENDING:
        return "pending";
    case ATP_LEDGER_OUTCOME_LEARNED:
        return "learned";
    case ATP_LEDGER_OUTCOME_SKIPPED:
        return "skipped";
    case ATP_LEDGER_OUTCOME_FAILED:
        return "failed";
    case ATP_LEDGER_OUTCOME_WITHDRAWN:
        return "withdrawn";
    }
    return "pending";
}

std::optional<atp_ledger_outcome> ledger_outcome_from_name(std::string_view name) {
    if (name == "pending") {
        return ATP_LEDGER_OUTCOME_PENDING;
    }
    if (name == "learned") {
        return ATP_LEDGER_OUTCOME_LEARNED;
    }
    if (name == "skipped") {
        return ATP_LEDGER_OUTCOME_SKIPPED;
    }
    if (name == "failed") {
        return ATP_LEDGER_OUTCOME_FAILED;
    }
    if (name == "withdrawn") {
        return ATP_LEDGER_OUTCOME_WITHDRAWN;
    }
    return std::nullopt;
}

std::string serialise_observation_record(const ObservationRecord &record) {
    Json root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "format", kRecordFormat.data());
    cJSON_AddNumberToObject(root.get(), "version", kRecordFormatVersion);
    add_u64_string(root.get(), "id", record.id);
    cJSON_AddStringToObject(root.get(), "sourceId", record.source_id.c_str());
    cJSON_AddStringToObject(root.get(), "authorDid", record.author_did.c_str());
    add_u64_string(root.get(), "observedAt", record.observed_at);
    add_u64_string(root.get(), "contentDigest", record.content_digest);
    cJSON_AddNumberToObject(root.get(), "schemaVersion",
                             static_cast<double>(record.schema_version));
    cJSON_AddStringToObject(root.get(), "outcome", record.outcome.c_str());
    cJSON *context = cJSON_CreateObject();
    cJSON_AddStringToObject(context, "replyRoot", record.context.reply_root_uri.c_str());
    cJSON_AddStringToObject(context, "replyParent", record.context.reply_parent_uri.c_str());
    cJSON_AddStringToObject(context, "quote", record.context.quote_uri.c_str());
    cJSON_AddItemToObject(root.get(), "context", context);
    return print_json(root.get());
}

ObservationRecord parse_observation_record(std::string_view json) {
    Json root = parse_root(json);
    require_format(root.get());

    ObservationRecord record;
    record.id = require_u64(root.get(), "id");
    if (record.id == 0u) {
        invalid("id must be >= 1");
    }
    record.source_id = require_string(root.get(), "sourceId");
    record.author_did = optional_string(root.get(), "authorDid");
    record.observed_at = require_u64(root.get(), "observedAt");
    record.content_digest = require_u64(root.get(), "contentDigest");
    record.schema_version = require_u32(root.get(), "schemaVersion");
    record.outcome = require_string(root.get(), "outcome");
    if (!ledger_outcome_from_name(record.outcome)) {
        invalid("unknown outcome '" + record.outcome + "'");
    }

    cJSON *context = cJSON_GetObjectItemCaseSensitive(root.get(), "context");
    if (!context || !cJSON_IsObject(context)) {
        invalid("missing context object");
    }
    record.context.reply_root_uri = optional_string(context, "replyRoot");
    record.context.reply_parent_uri = optional_string(context, "replyParent");
    record.context.quote_uri = optional_string(context, "quote");
    return record;
}

std::string serialise_action_record(const ActionRecord &record) {
    Json root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "format", kRecordFormat.data());
    cJSON_AddNumberToObject(root.get(), "version", kRecordFormatVersion);
    cJSON_AddStringToObject(root.get(), "id", record.id.c_str());
    cJSON_AddStringToObject(root.get(), "kind", record.kind.c_str());
    cJSON_AddStringToObject(root.get(), "text", record.text.c_str());
    cJSON_AddStringToObject(root.get(), "digest", record.digest.c_str());
    cJSON_AddStringToObject(root.get(), "outcome", record.outcome.c_str());
    cJSON_AddStringToObject(root.get(), "reason", record.reason.c_str());
    cJSON_AddStringToObject(root.get(), "uri", record.uri.c_str());
    cJSON_AddStringToObject(root.get(), "cid", record.cid.c_str());
    cJSON_AddStringToObject(root.get(), "at", record.at.c_str());
    return print_json(root.get());
}

ActionRecord parse_action_record(std::string_view json) {
    Json root = parse_root(json);
    require_format(root.get());

    ActionRecord record;
    record.id = require_string(root.get(), "id");
    record.kind = require_string(root.get(), "kind");
    record.text = require_string(root.get(), "text");
    record.digest = require_string(root.get(), "digest");
    record.outcome = require_string(root.get(), "outcome");
    if (!journal_action_outcome_from_name(record.outcome)) {
        invalid("unknown action outcome '" + record.outcome + "'");
    }
    record.reason = optional_string(root.get(), "reason");
    record.uri = optional_string(root.get(), "uri");
    record.cid = optional_string(root.get(), "cid");
    record.at = require_string(root.get(), "at");
    return record;
}

std::string serialise_valence_record(const ValenceRecord &record) {
    Json root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "format", kRecordFormat.data());
    cJSON_AddNumberToObject(root.get(), "version", kRecordFormatVersion);
    cJSON_AddStringToObject(root.get(), "token", record.token.c_str());
    cJSON_AddStringToObject(root.get(), "kind", record.kind.c_str());
    cJSON_AddNumberToObject(root.get(), "signal", record.signal);
    cJSON_AddStringToObject(root.get(), "source", record.source.c_str());
    add_u64_string(root.get(), "atEpoch", record.at_epoch);
    cJSON_AddStringToObject(root.get(), "at", record.at.c_str());
    cJSON_AddStringToObject(root.get(), "provenance", record.provenance.c_str());
    return print_json(root.get());
}

ValenceRecord parse_valence_record(std::string_view json) {
    Json root = parse_root(json);
    require_format(root.get());

    ValenceRecord record;
    record.token = require_string(root.get(), "token");
    record.kind = require_string(root.get(), "kind");
    if (!valence_kind_from_name(record.kind)) {
        invalid("unknown valence kind '" + record.kind + "'");
    }
    record.signal = static_cast<float>(require_number(root.get(), "signal"));
    record.source = optional_string(root.get(), "source");
    record.at_epoch = require_u64(root.get(), "atEpoch");
    record.at = require_string(root.get(), "at");
    record.provenance = optional_string(root.get(), "provenance");
    return record;
}

std::string serialise_thought_record(const ThoughtRecord &record) {
    if (!thought_kind_is_valid(record.kind)) {
        invalid("unknown thought kind '" + record.kind + "'");
    }
    Json root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "format", kRecordFormat.data());
    cJSON_AddNumberToObject(root.get(), "version", kRecordFormatVersion);
    cJSON_AddStringToObject(root.get(), "id", record.id.c_str());
    cJSON_AddStringToObject(root.get(), "kind", record.kind.c_str());
    cJSON_AddStringToObject(root.get(), "text", record.text.c_str());
    cJSON_AddStringToObject(root.get(), "about", record.about_uri.c_str());
    cJSON_AddStringToObject(root.get(), "at", record.at.c_str());
    return print_json(root.get());
}

ThoughtRecord parse_thought_record(std::string_view json) {
    Json root = parse_root(json);
    require_format(root.get());

    ThoughtRecord record;
    record.id = require_string(root.get(), "id");
    record.kind = require_string(root.get(), "kind");
    if (!thought_kind_is_valid(record.kind)) {
        invalid("unknown thought kind '" + record.kind + "'");
    }
    record.text = require_string(root.get(), "text");
    record.about_uri = optional_string(root.get(), "about");
    record.at = require_string(root.get(), "at");
    return record;
}

std::string serialise_intent_record(const IntentRecord &record) {
    const std::optional<IntentState> state = intent_state_from_name(record.state);
    if (!state.has_value()) {
        invalid("unknown intent state '" + record.state + "'");
    }
    Json root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "format", kRecordFormat.data());
    cJSON_AddNumberToObject(root.get(), "version", kRecordFormatVersion);
    cJSON_AddStringToObject(root.get(), "id", record.id.c_str());
    cJSON *actions = cJSON_CreateArray();
    if (!actions) {
        throw std::runtime_error("failed to allocate intent record actions");
    }
    for (const std::string &action : record.actions) {
        cJSON *item = cJSON_CreateString(action.c_str());
        if (!item) {
            cJSON_Delete(actions);
            throw std::runtime_error("failed to allocate intent record action");
        }
        cJSON_AddItemToArray(actions, item);
    }
    cJSON_AddItemToObject(root.get(), "actions", actions);
    cJSON_AddStringToObject(root.get(), "responder", record.responder.c_str());
    add_u64_string(root.get(), "expires_at_epoch", record.expires_at_epoch);
    cJSON_AddStringToObject(root.get(), "expires_at", record.expires_at.c_str());
    cJSON_AddNumberToObject(root.get(), "max_continuations",
                             static_cast<double>(record.max_continuations));
    cJSON_AddStringToObject(root.get(), "state", record.state.c_str());
    add_u64_string(root.get(), "at_epoch", record.at_epoch);
    cJSON_AddStringToObject(root.get(), "at", record.at.c_str());
    return print_json(root.get());
}

IntentRecord parse_intent_record(std::string_view json) {
    Json root = parse_root(json);
    require_format(root.get());

    IntentRecord record;
    record.id = require_string(root.get(), "id");
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(root.get(), "actions");
    if (!cJSON_IsArray(actions)) {
        invalid("missing or invalid actions array");
    }
    cJSON *action = nullptr;
    cJSON_ArrayForEach(action, actions) {
        if (!cJSON_IsString(action) || !action->valuestring) {
            invalid("actions array contains a non-string entry");
        }
        record.actions.emplace_back(action->valuestring);
    }
    record.responder = require_string(root.get(), "responder");
    record.expires_at_epoch = require_u64(root.get(), "expires_at_epoch");
    record.expires_at = require_string(root.get(), "expires_at");
    record.max_continuations = require_u32(root.get(), "max_continuations");
    record.state = require_string(root.get(), "state");
    if (!intent_state_from_name(record.state).has_value()) {
        invalid("unknown intent state '" + record.state + "'");
    }
    record.at_epoch = require_u64(root.get(), "at_epoch");
    record.at = require_string(root.get(), "at");
    return record;
}

} // namespace atperson
