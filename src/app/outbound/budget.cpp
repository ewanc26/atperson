#include "budget.hpp"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>

namespace atperson {
namespace {

constexpr std::string_view kFormat = "atperson-outbound-budget";
constexpr std::uint32_t kVersion = 1u;

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

[[noreturn]] void invalid(const std::string &message) {
    throw OutboundBudgetError("outbound budget: " + message);
}

bool integral_number(const cJSON *value) {
    return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
           std::floor(value->valuedouble) == value->valuedouble;
}

std::string json_string(const std::string &value) {
    Json printed(cJSON_CreateString(value.c_str()));
    if (!printed) {
        throw std::runtime_error("outbound budget: string allocation failed");
    }
    char *raw = cJSON_PrintUnformatted(printed.get());
    if (!raw) {
        throw std::runtime_error("outbound budget: string printing failed");
    }
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

std::int64_t require_timestamp(const cJSON *value, const char *what) {
    if (!integral_number(value)) {
        invalid(std::string(what) + " must be an integer timestamp");
    }
    const auto number = static_cast<std::int64_t>(value->valuedouble);
    if (number < 0) {
        invalid(std::string(what) + " must not be negative");
    }
    return number;
}

std::size_t keep_last(std::size_t size, std::size_t cap) {
    return size > cap ? size - cap : 0u;
}

OutboundKindUsage parse_usage(const cJSON *object, const std::string &kind) {
    OutboundKindUsage usage;

    const cJSON *last = cJSON_GetObjectItemCaseSensitive(object, "last_action_at");
    if (last && !cJSON_IsNull(last)) {
        usage.last_action_at = require_timestamp(last, "last_action_at");
    }

    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(object, "recent_actions");
    if (actions) {
        if (!cJSON_IsArray(actions)) {
            invalid("kind '" + kind + "' recent_actions must be an array");
        }
        const cJSON *entry = nullptr;
        cJSON_ArrayForEach(entry, actions) {
            usage.recent_actions.push_back(require_timestamp(entry, "recent_actions entry"));
        }
    }

    const cJSON *duplicates = cJSON_GetObjectItemCaseSensitive(object, "recent_duplicates");
    if (duplicates) {
        if (!cJSON_IsArray(duplicates)) {
            invalid("kind '" + kind + "' recent_duplicates must be an array");
        }
        const cJSON *entry = nullptr;
        cJSON_ArrayForEach(entry, duplicates) {
            if (!cJSON_IsObject(entry)) {
                invalid("kind '" + kind + "' recent_duplicates entries must be objects");
            }
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(entry, "key");
            if (!cJSON_IsString(key) || !key->valuestring || !key->valuestring[0]) {
                invalid("kind '" + kind + "' duplicate record is missing a key");
            }
            const cJSON *at = cJSON_GetObjectItemCaseSensitive(entry, "at");
            usage.recent_duplicates.push_back(
                OutboundDuplicateRecord{key->valuestring, require_timestamp(at, "duplicate at")});
        }
    }

    if (usage.recent_actions.size() > kMaxOutboundRecordsPerKind) {
        invalid("kind '" + kind + "' has too many recent_actions");
    }
    if (usage.recent_duplicates.size() > kMaxOutboundRecordsPerKind) {
        invalid("kind '" + kind + "' has too many recent_duplicates");
    }
    for (std::size_t i = 1; i < usage.recent_actions.size(); ++i) {
        if (usage.recent_actions[i] < usage.recent_actions[i - 1]) {
            invalid("kind '" + kind + "' recent_actions must be non-decreasing");
        }
    }
    return usage;
}

void prune_kind(OutboundKindUsage &usage, const ActionBudget &budget, std::int64_t now) {
    const std::int64_t horizon =
        std::max<std::int64_t>({budget.window_seconds, budget.min_interval_seconds, 1ll});
    const std::int64_t action_cutoff = now - horizon;
    std::erase_if(usage.recent_actions,
                  [action_cutoff](std::int64_t at) { return at < action_cutoff; });

    if (budget.duplicate_cooldown_seconds > 0) {
        const std::int64_t duplicate_cutoff = now - budget.duplicate_cooldown_seconds;
        std::erase_if(usage.recent_duplicates,
                      [duplicate_cutoff](const OutboundDuplicateRecord &record) {
                          return record.at < duplicate_cutoff;
                      });
    } else {
        usage.recent_duplicates.clear();
    }

    if (const std::size_t drop = keep_last(usage.recent_actions.size(), kMaxOutboundRecordsPerKind);
        drop != 0u) {
        usage.recent_actions.erase(usage.recent_actions.begin(),
                                   usage.recent_actions.begin() +
                                       static_cast<std::ptrdiff_t>(drop));
    }
    if (const std::size_t drop =
            keep_last(usage.recent_duplicates.size(), kMaxOutboundRecordsPerKind);
        drop != 0u) {
        usage.recent_duplicates.erase(usage.recent_duplicates.begin(),
                                      usage.recent_duplicates.begin() +
                                          static_cast<std::ptrdiff_t>(drop));
    }
}

void append_usage(std::string &out, const OutboundKindUsage &usage) {
    out.append("{\"last_action_at\":");
    out.append(usage.last_action_at ? std::to_string(*usage.last_action_at) : "null");
    out.append(",\"recent_actions\":[");
    for (std::size_t i = 0; i < usage.recent_actions.size(); ++i) {
        if (i != 0u) {
            out.push_back(',');
        }
        out.append(std::to_string(usage.recent_actions[i]));
    }
    out.append("],\"recent_duplicates\":[");
    for (std::size_t i = 0; i < usage.recent_duplicates.size(); ++i) {
        if (i != 0u) {
            out.push_back(',');
        }
        out.append("{\"key\":");
        out.append(json_string(usage.recent_duplicates[i].key));
        out.append(",\"at\":");
        out.append(std::to_string(usage.recent_duplicates[i].at));
        out.push_back('}');
    }
    out.append("]}");
}

} // namespace

OutboundBudgetState load_outbound_budget_state(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return OutboundBudgetState{};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("outbound budget: could not open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        throw std::runtime_error("outbound budget: could not read " + path.string());
    }
    const std::string text = buffer.str();

    Json root(cJSON_ParseWithLength(text.data(), text.size()));
    if (!root) {
        invalid(path.string() + " is not valid JSON");
    }
    if (!cJSON_IsObject(root.get())) {
        invalid(path.string() + " is not a JSON object");
    }

    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root.get(), "format");
    if (!cJSON_IsString(format) || !format->valuestring) {
        invalid("missing string field 'format'");
    }
    if (std::string_view(format->valuestring) != kFormat) {
        invalid(std::string("unexpected format '") + format->valuestring + "'");
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!integral_number(version)) {
        invalid("missing or invalid 'version'");
    }
    if (static_cast<std::uint32_t>(version->valuedouble) != kVersion) {
        invalid("unsupported version " + std::to_string(version->valuedouble));
    }

    OutboundBudgetState state;

    const cJSON *saved = cJSON_GetObjectItemCaseSensitive(root.get(), "saved_at");
    if (saved && !cJSON_IsNull(saved)) {
        state.saved_at = require_timestamp(saved, "saved_at");
    }

    const cJSON *kinds = cJSON_GetObjectItemCaseSensitive(root.get(), "kinds");
    if (kinds) {
        if (!cJSON_IsObject(kinds)) {
            invalid("'kinds' must be an object");
        }
        bool seen[kOutboundActionKindCount] = {};
        const cJSON *entry = nullptr;
        cJSON_ArrayForEach(entry, kinds) {
            if (!cJSON_IsObject(entry) || !entry->string) {
                invalid("'kinds' entries must be named objects");
            }
            const auto kind = parse_outbound_kind(entry->string);
            if (!kind) {
                invalid(std::string("unknown action kind '") + entry->string + "'");
            }
            const auto index = static_cast<std::size_t>(*kind);
            if (seen[index]) {
                invalid(std::string("duplicate kind '") + entry->string + "'");
            }
            seen[index] = true;
            state.usage[index] = parse_usage(entry, entry->string);
        }
    }

    return state;
}

std::string serialise_outbound_budget_state(const OutboundBudgetState &state) {
    if (state.version != kVersion) {
        invalid("unsupported version " + std::to_string(state.version));
    }
    std::string out;
    out.reserve(1024u);
    out.append("{\"format\":\"");
    out.append(kFormat);
    out.append("\",\"version\":");
    out.append(std::to_string(state.version));
    out.append(",\"saved_at\":");
    out.append(state.saved_at ? std::to_string(*state.saved_at) : "null");
    out.append(",\"kinds\":{");
    for (std::size_t index = 0; index < kOutboundActionKindCount; ++index) {
        if (index != 0u) {
            out.push_back(',');
        }
        out.push_back('"');
        out.append(outbound_kind_name(static_cast<OutboundActionKind>(index)));
        out.append("\":");
        append_usage(out, state.usage[index]);
    }
    out.append("}}\n");
    return out;
}

void save_outbound_budget_state(const OutboundBudgetState &state,
                                const std::filesystem::path &path) {
    const std::string payload = serialise_outbound_budget_state(state);

    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code parent_ec;
        std::filesystem::create_directories(parent, parent_ec);
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("outbound budget: could not create " + tmp.string());
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            throw std::runtime_error("outbound budget: could not write " + tmp.string());
        }
    }

    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        throw std::runtime_error("outbound budget: could not commit " + path.string() + ": " +
                                 rename_ec.message());
    }
}

void prune_outbound_budget_state(OutboundBudgetState &state, const OutboundPolicy &policy,
                                 std::int64_t now) {
    for (std::size_t index = 0; index < kOutboundActionKindCount; ++index) {
        prune_kind(state.usage[index], budget_for(policy, static_cast<OutboundActionKind>(index)),
                   now);
    }
}

void record_outbound_action(OutboundBudgetState &state, const OutboundActionProposal &proposal,
                            const ActionBudget &budget, std::int64_t now) {
    OutboundKindUsage &usage = state.usage[static_cast<std::size_t>(proposal.kind)];

    /* Conservative clock handling: a backwards clock must never shrink a
     * window, so record at no earlier than the last admitted action. */
    if (usage.last_action_at && now < *usage.last_action_at) {
        now = *usage.last_action_at;
    }
    usage.last_action_at = now;
    usage.recent_actions.push_back(now);

    const std::string key = outbound_dedup_key(proposal);
    std::erase_if(usage.recent_duplicates,
                  [&key](const OutboundDuplicateRecord &record) { return record.key == key; });
    usage.recent_duplicates.push_back(OutboundDuplicateRecord{key, now});

    prune_kind(usage, budget, now);
}

std::uint32_t count_outbound_actions_in_window(const OutboundBudgetState &state,
                                               OutboundActionKind kind, std::int64_t window_seconds,
                                               std::int64_t now) {
    const OutboundKindUsage &usage = state.usage[static_cast<std::size_t>(kind)];
    const std::int64_t cutoff = now - window_seconds;
    std::uint32_t count = 0u;
    for (const std::int64_t at : usage.recent_actions) {
        if (at >= cutoff) {
            ++count;
        }
    }
    return count;
}

std::optional<std::int64_t> last_outbound_action_at(const OutboundBudgetState &state,
                                                    OutboundActionKind kind) {
    return state.usage[static_cast<std::size_t>(kind)].last_action_at;
}

std::optional<std::int64_t> earliest_outbound_action_in_window(const OutboundBudgetState &state,
                                                               OutboundActionKind kind,
                                                               std::int64_t window_seconds,
                                                               std::int64_t now) {
    const OutboundKindUsage &usage = state.usage[static_cast<std::size_t>(kind)];
    const std::int64_t cutoff = now - window_seconds;
    for (const std::int64_t at : usage.recent_actions) {
        if (at >= cutoff) {
            return at;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> duplicate_outbound_recorded_at(const OutboundBudgetState &state,
                                                           const OutboundActionProposal &proposal) {
    const OutboundKindUsage &usage = state.usage[static_cast<std::size_t>(proposal.kind)];
    const std::string key = outbound_dedup_key(proposal);
    for (const OutboundDuplicateRecord &record : usage.recent_duplicates) {
        if (record.key == key) {
            return record.at;
        }
    }
    return std::nullopt;
}

} // namespace atperson
