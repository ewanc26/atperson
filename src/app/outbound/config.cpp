#include "config.hpp"

#include <cJSON.h>

#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>

namespace atperson {
namespace {

constexpr std::string_view kFormat = "atperson-outbound-policy";
constexpr std::uint32_t kVersion = 1u;
constexpr std::uint32_t kMaxActionsInWindow = 100000u;

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

[[noreturn]] void invalid(std::string_view source, const std::string &message) {
    throw OutboundPolicyError("outbound policy (" + std::string(source) + "): " + message);
}

bool integral_number(const cJSON *value) {
    return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
           std::floor(value->valuedouble) == value->valuedouble;
}

std::int64_t require_int64(const cJSON *object, const char *key, std::int64_t low,
                           std::int64_t high, std::int64_t fallback, std::string_view source) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value) {
        return fallback;
    }
    if (!integral_number(value)) {
        invalid(source, std::string("field '") + key + "' must be an integer");
    }
    const auto number = static_cast<std::int64_t>(value->valuedouble);
    if (number < low || number > high) {
        invalid(source, std::string("field '") + key + "' out of range [" + std::to_string(low) +
                            ", " + std::to_string(high) + "]");
    }
    return number;
}

ActionBudget parse_budget(const cJSON *object, std::string_view kind, std::string_view source) {
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(object, "enabled");
    if (!cJSON_IsBool(enabled)) {
        invalid(source,
                std::string("kind '") + std::string(kind) + "' is missing a boolean 'enabled'");
    }

    ActionBudget budget;
    budget.enabled = cJSON_IsTrue(enabled);
    budget.max_in_window = static_cast<std::uint32_t>(
        require_int64(object, "max_in_window", 1ll, static_cast<std::int64_t>(kMaxActionsInWindow),
                      static_cast<std::int64_t>(budget.max_in_window), source));
    budget.window_seconds = require_int64(object, "window_seconds", 1ll, kMaxOutboundWindowSeconds,
                                          budget.window_seconds, source);
    budget.min_interval_seconds =
        require_int64(object, "min_interval_seconds", 0ll, kMaxOutboundWindowSeconds,
                      budget.min_interval_seconds, source);
    budget.duplicate_cooldown_seconds =
        require_int64(object, "duplicate_cooldown_seconds", 0ll, kMaxOutboundWindowSeconds,
                      budget.duplicate_cooldown_seconds, source);
    return budget;
}

void append_budget(std::string &out, const ActionBudget &budget) {
    out.append("{\"enabled\":");
    out.append(budget.enabled ? "true" : "false");
    out.append(",\"max_in_window\":");
    out.append(std::to_string(budget.max_in_window));
    out.append(",\"window_seconds\":");
    out.append(std::to_string(budget.window_seconds));
    out.append(",\"min_interval_seconds\":");
    out.append(std::to_string(budget.min_interval_seconds));
    out.append(",\"duplicate_cooldown_seconds\":");
    out.append(std::to_string(budget.duplicate_cooldown_seconds));
    out.push_back('}');
}

void validate(const OutboundPolicy &policy, std::string_view source) {
    if (policy.version != kVersion) {
        invalid(source, "unsupported version " + std::to_string(policy.version));
    }
    for (std::size_t index = 0; index < kOutboundActionKindCount; ++index) {
        const ActionBudget &budget = policy.budgets[index];
        const auto kind = static_cast<OutboundActionKind>(index);
        if (budget.max_in_window == 0u || budget.max_in_window > kMaxActionsInWindow) {
            invalid(source, std::string("kind '") + outbound_kind_name(kind) +
                                "' has an impossible max_in_window");
        }
        if (budget.window_seconds < 1 || budget.window_seconds > kMaxOutboundWindowSeconds) {
            invalid(source, std::string("kind '") + outbound_kind_name(kind) +
                                "' has an impossible window_seconds");
        }
        if (budget.min_interval_seconds < 0 ||
            budget.min_interval_seconds > kMaxOutboundWindowSeconds) {
            invalid(source, std::string("kind '") + outbound_kind_name(kind) +
                                "' has an impossible min_interval_seconds");
        }
        if (budget.duplicate_cooldown_seconds < 0 ||
            budget.duplicate_cooldown_seconds > kMaxOutboundWindowSeconds) {
            invalid(source, std::string("kind '") + outbound_kind_name(kind) +
                                "' has an impossible duplicate_cooldown_seconds");
        }
    }
}

} // namespace

OutboundPolicy default_outbound_policy() {
    return OutboundPolicy{};
}

ActionBudget &budget_for(OutboundPolicy &policy, OutboundActionKind kind) {
    return policy.budgets[static_cast<std::size_t>(kind)];
}

const ActionBudget &budget_for(const OutboundPolicy &policy, OutboundActionKind kind) {
    return policy.budgets[static_cast<std::size_t>(kind)];
}

std::string serialise_outbound_policy(const OutboundPolicy &policy) {
    validate(policy, "<serialise>");
    std::string out;
    out.reserve(512u);
    out.append("{\"format\":\"");
    out.append(kFormat);
    out.append("\",\"version\":");
    out.append(std::to_string(policy.version));
    out.append(",\"kinds\":{");
    for (std::size_t index = 0; index < kOutboundActionKindCount; ++index) {
        if (index != 0u) {
            out.push_back(',');
        }
        out.push_back('"');
        out.append(outbound_kind_name(static_cast<OutboundActionKind>(index)));
        out.append("\":");
        append_budget(out, policy.budgets[index]);
    }
    out.append("}}\n");
    return out;
}

OutboundPolicy parse_outbound_policy(std::string_view json, std::string_view source) {
    Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root) {
        invalid(source, "not valid JSON");
    }
    if (!cJSON_IsObject(root.get())) {
        invalid(source, "not a JSON object");
    }

    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root.get(), "format");
    if (!cJSON_IsString(format) || !format->valuestring) {
        invalid(source, "missing string field 'format'");
    }
    if (std::string_view(format->valuestring) != kFormat) {
        invalid(source, std::string("unexpected format '") + format->valuestring + "'");
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!integral_number(version)) {
        invalid(source, "missing or invalid 'version'");
    }
    if (static_cast<std::uint32_t>(version->valuedouble) != kVersion) {
        invalid(source, "unsupported version " + std::to_string(version->valuedouble));
    }

    const cJSON *kinds = cJSON_GetObjectItemCaseSensitive(root.get(), "kinds");
    if (!cJSON_IsObject(kinds)) {
        invalid(source, "missing 'kinds' object");
    }

    OutboundPolicy policy;
    bool seen[kOutboundActionKindCount] = {};
    const cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, kinds) {
        if (!cJSON_IsObject(entry) || !entry->string) {
            invalid(source, "'kinds' entries must be named objects");
        }
        const auto kind = parse_outbound_kind(entry->string);
        if (!kind) {
            invalid(source, std::string("unknown action kind '") + entry->string + "'");
        }
        const auto index = static_cast<std::size_t>(*kind);
        if (seen[index]) {
            invalid(source, std::string("duplicate kind '") + entry->string + "'");
        }
        seen[index] = true;
        policy.budgets[index] = parse_budget(entry, entry->string, source);
    }

    validate(policy, source);
    return policy;
}

OutboundPolicy load_outbound_policy(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return default_outbound_policy();
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("outbound policy: could not open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        throw std::runtime_error("outbound policy: could not read " + path.string());
    }
    return parse_outbound_policy(buffer.str(), path.string());
}

} // namespace atperson
