#include "envelope.hpp"

#include "../outbound/config.hpp"
#include "../state/time.hpp"

#include <cJSON.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

namespace atperson {
namespace {

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

[[noreturn]] void invalid(const std::string &message) {
    throw EnvelopeError("authorization envelope: " + message);
}

std::string require_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0]) {
        invalid(std::string("missing or empty string field '") + key + "'");
    }
    return value->valuestring;
}

std::optional<std::string> optional_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value || cJSON_IsNull(value)) {
        return std::nullopt;
    }
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0]) {
        invalid(std::string("field '") + key + "' must be a string or null");
    }
    return std::string(value->valuestring);
}

bool has_key(cJSON *object, const char *key) {
    return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(object, key)) != 0;
}

std::string json_string_value(const std::string &value) {
    Json printed(cJSON_CreateString(value.c_str()));
    if (!printed) {
        throw std::runtime_error("authorization envelope: string allocation failed");
    }
    char *raw = cJSON_PrintUnformatted(printed.get());
    if (!raw) {
        throw std::runtime_error("authorization envelope: string printing failed");
    }
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

/* Count budget records for `kind` inside the envelope's trailing window.
 * The records are the same ones the policy uses; the envelope only adds a
 * second, tighter ceiling over them. */
std::uint32_t count_in_window(const OutboundKindUsage &usage, std::int64_t window_seconds,
                               std::int64_t now) {
    std::uint32_t count = 0u;
    for (auto it = usage.recent_actions.rbegin(); it != usage.recent_actions.rend(); ++it) {
        if (now - *it < window_seconds) {
            ++count;
        } else {
            break;
        }
    }
    return count;
}

bool text_contains_term(std::string_view text, std::string_view term) {
    if (term.empty() || text.size() < term.size()) {
        return false;
    }
    const auto equal_case_insensitive = [](char a, char b) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return lower(a) == lower(b);
    };
    for (std::size_t i = 0u; i + term.size() <= text.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0u; j < term.size(); ++j) {
            if (!equal_case_insensitive(text[i + j], term[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_envelope_id(std::string_view id) {
    if (id.empty() || id.size() > 64u) {
        return false;
    }
    if (id.front() == '-' || id.back() == '-') {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

AuthorizationEnvelope parse_authorization_envelope(std::string_view json,
                                                   const OutboundPolicy &policy,
                                                   std::string_view source) {
    Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        invalid(std::string(source) + ": not a valid JSON object");
    }

    const std::string format = require_string(root.get(), "format");
    if (format != kAuthorizationEnvelopeFormat) {
        invalid("unexpected format '" + format + "'");
    }

    cJSON *version_value = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version_value)) {
        invalid("missing or invalid 'version'");
    }
    const auto version = static_cast<std::uint32_t>(version_value->valuedouble);
    if (version != kAuthorizationEnvelopeVersion) {
        invalid("unsupported version " + std::to_string(version));
    }

    AuthorizationEnvelope envelope;
    envelope.version = version;
    envelope.id = require_string(root.get(), "id");
    if (!is_envelope_id(envelope.id)) {
        invalid("id '" + envelope.id + "' must be 1-64 of [a-z0-9-], no edge hyphens");
    }
    envelope.expires_at = optional_string(root.get(), "expires_at");
    if (envelope.expires_at && !parse_rfc3339_epoch(*envelope.expires_at)) {
        invalid("expires_at '" + *envelope.expires_at + "' is not a valid RFC 3339 instant");
    }
    envelope.created_at = require_string(root.get(), "created_at");
    if (!parse_rfc3339_epoch(envelope.created_at)) {
        invalid("created_at '" + envelope.created_at + "' is not a valid RFC 3339 instant");
    }
    envelope.note = optional_string(root.get(), "note").value_or("");

    cJSON *scope = cJSON_GetObjectItemCaseSensitive(root.get(), "scope_terms");
    if (!cJSON_IsArray(scope)) {
        invalid("missing 'scope_terms' array");
    }
    cJSON *term = nullptr;
    cJSON_ArrayForEach(term, scope) {
        if (!cJSON_IsString(term) || !term->valuestring || !term->valuestring[0]) {
            invalid("scope_terms entries must be non-empty strings");
        }
        envelope.scope_terms.emplace_back(term->valuestring);
    }

    cJSON *kinds = cJSON_GetObjectItemCaseSensitive(root.get(), "kinds");
    if (!cJSON_IsArray(kinds)) {
        invalid("missing 'kinds' array");
    }
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, kinds) {
        if (!cJSON_IsObject(entry)) {
            invalid("kinds entries must be objects");
        }
        EnvelopeKindRule rule;
        const std::string kind_name = require_string(entry, "kind");
        const auto kind = parse_outbound_kind(kind_name);
        if (!kind) {
            invalid("unknown action kind '" + kind_name + "'");
        }
        rule.kind = *kind;

        cJSON *max_value = cJSON_GetObjectItemCaseSensitive(entry, "max_in_window");
        if (!cJSON_IsNumber(max_value) || max_value->valuedouble < 1.0) {
            invalid("field 'max_in_window' must be a number >= 1");
        }
        rule.max_in_window = static_cast<std::uint32_t>(max_value->valuedouble);

        cJSON *window_value = cJSON_GetObjectItemCaseSensitive(entry, "window_seconds");
        if (!cJSON_IsNumber(window_value) || window_value->valuedouble < 1.0) {
            invalid("field 'window_seconds' must be a number >= 1");
        }
        rule.window_seconds = static_cast<std::int64_t>(window_value->valuedouble);
        if (rule.window_seconds > kMaxOutboundWindowSeconds) {
            invalid("window_seconds exceeds the maximum of 366 days");
        }

        cJSON *plan_score = cJSON_GetObjectItemCaseSensitive(entry, "min_plan_score");
        if (cJSON_IsNumber(plan_score)) {
            rule.min_plan_score = plan_score->valuedouble;
        } else if (has_key(entry, "min_plan_score")) {
            invalid("field 'min_plan_score' must be a number");
        }
        cJSON *support_score = cJSON_GetObjectItemCaseSensitive(entry, "min_support_score");
        if (cJSON_IsNumber(support_score)) {
            rule.min_support_score = support_score->valuedouble;
        } else if (has_key(entry, "min_support_score")) {
            invalid("field 'min_support_score' must be a number");
        }

        /* The envelope narrows, never widens: every ceiling must be within
         * the policy's own limits for the kind, or the document is
         * rejected outright. */
        const ActionBudget &policy_budget = budget_for(policy, rule.kind);
        if (!policy_budget.enabled) {
            invalid("kind '" + kind_name + "' is not enabled in the outbound policy; an " +
                    "envelope cannot enable it");
        }
        if (rule.max_in_window > policy_budget.max_in_window) {
            invalid("max_in_window " + std::to_string(rule.max_in_window) +
                    " exceeds the policy ceiling " +
                    std::to_string(policy_budget.max_in_window) + " for kind '" + kind_name +
                    "'");
        }
        if (rule.window_seconds > policy_budget.window_seconds) {
            invalid("window_seconds " + std::to_string(rule.window_seconds) +
                    " exceeds the policy window " +
                    std::to_string(policy_budget.window_seconds) + " for kind '" + kind_name +
                    "'");
        }

        envelope.kinds.push_back(rule);
    }

    if (envelope.kinds.empty()) {
        invalid("an envelope must cover at least one action kind");
    }
    return envelope;
}

std::string serialise_authorization_envelope(const AuthorizationEnvelope &e) {
    std::string out;
    out.reserve(256u);
    out.append("{\"format\":\"").append(kAuthorizationEnvelopeFormat);
    out.append("\",\"version\":").append(std::to_string(e.version));
    out.append(",\"id\":").append(json_string_value(e.id));
    out.append(",\"created_at\":").append(json_string_value(e.created_at));
    out.append(",\"expires_at\":");
    out.append(e.expires_at ? json_string_value(*e.expires_at) : std::string("null"));
    out.append(",\"scope_terms\":[");
    for (std::size_t i = 0u; i < e.scope_terms.size(); ++i) {
        if (i != 0u) {
            out.push_back(',');
        }
        out.append(json_string_value(e.scope_terms[i]));
    }
    out.append("],\"kinds\":[");
    for (std::size_t i = 0u; i < e.kinds.size(); ++i) {
        const EnvelopeKindRule &rule = e.kinds[i];
        if (i != 0u) {
            out.push_back(',');
        }
        out.append("{\"kind\":\"").append(outbound_kind_name(rule.kind));
        out.append("\",\"max_in_window\":").append(std::to_string(rule.max_in_window));
        out.append(",\"window_seconds\":").append(std::to_string(rule.window_seconds));
        out.append(",\"min_plan_score\":");
        out.append(std::to_string(rule.min_plan_score));
        out.append(",\"min_support_score\":");
        out.append(std::to_string(rule.min_support_score));
        out.push_back('}');
    }
    if (e.note.empty()) {
        out.append("],\"note\":null");
    } else {
        out.append("],\"note\":").append(json_string_value(e.note));
    }
    out.append("}\n");
    return out;
}

std::optional<AuthorizationEnvelope>
load_authorization_envelope(const std::filesystem::path &dir, std::string_view id,
                            const OutboundPolicy &policy) {
    if (!is_envelope_id(id)) {
        return std::nullopt;
    }
    const std::filesystem::path path = dir / (std::string(id) + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("authorization envelope: could not open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    if (input.bad()) {
        throw std::runtime_error("authorization envelope: could not read " + path.string());
    }
    return parse_authorization_envelope(text, policy, path.string());
}

void save_authorization_envelope(const AuthorizationEnvelope &e,
                                 const std::filesystem::path &dir) {
    if (!is_envelope_id(e.id)) {
        invalid("id '" + e.id + "' is not a valid envelope id");
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / (e.id + ".json");
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("authorization envelope: could not create " + tmp.string());
        }
        const std::string payload = serialise_authorization_envelope(e);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            throw std::runtime_error("authorization envelope: could not write " + tmp.string());
        }
    }
    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        throw std::runtime_error("authorization envelope: could not commit " + path.string() +
                                 ": " + rename_ec.message());
    }
}

void revoke_authorization_envelope(const std::filesystem::path &dir, std::string_view id) {
    if (!is_envelope_id(id)) {
        invalid("id '" + std::string(id) + "' is not a valid envelope id");
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(dir / (std::string(id) + ".json"), ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("authorization envelope: could not revoke " + std::string(id) +
                                 ": " + ec.message());
    }
    (void)removed;
}

std::vector<std::string> list_authorization_envelopes(const std::filesystem::path &dir) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || it->path().extension() != ".json") {
            continue;
        }
        std::string stem = it->path().stem().string();
        if (is_envelope_id(stem)) {
            ids.push_back(std::move(stem));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

EnvelopeCoverage covers_action(const AuthorizationEnvelope &e, OutboundActionKind kind,
                               std::string_view text, const EnvelopeEvidence &evidence,
                               const OutboundKindUsage &usage, std::int64_t now) {
    EnvelopeCoverage coverage;
    coverage.envelope_id = e.id;

    if (e.expires_at) {
        const auto expires = parse_rfc3339_epoch(*e.expires_at);
        if (!expires || static_cast<std::int64_t>(*expires) <= now) {
            coverage.reason = "expired";
            return coverage;
        }
    }

    const EnvelopeKindRule *rule = nullptr;
    for (const EnvelopeKindRule &candidate : e.kinds) {
        if (candidate.kind == kind) {
            rule = &candidate;
            break;
        }
    }
    if (!rule) {
        coverage.reason = "kind_not_covered";
        return coverage;
    }

    const std::uint32_t used = count_in_window(usage, rule->window_seconds, now);
    if (used >= rule->max_in_window) {
        coverage.reason = "rate_ceiling";
        return coverage;
    }

    if (rule->min_plan_score > 0.0) {
        if (!evidence.plan_score || *evidence.plan_score < rule->min_plan_score) {
            coverage.reason = "score_floor";
            return coverage;
        }
    }
    if (rule->min_support_score > 0.0) {
        if (!evidence.support_score || *evidence.support_score < rule->min_support_score) {
            coverage.reason = "score_floor";
            return coverage;
        }
    }

    if (!e.scope_terms.empty()) {
        bool in_scope = false;
        for (const std::string &term : e.scope_terms) {
            if (text_contains_term(text, term)) {
                in_scope = true;
                break;
            }
        }
        if (!in_scope) {
            coverage.reason = "scope";
            return coverage;
        }
    }

    coverage.covered = true;
    return coverage;
}

EnvelopeCoverage find_covering_envelope(const std::filesystem::path &dir,
                                        const OutboundPolicy &policy, OutboundActionKind kind,
                                        std::string_view text,
                                        const EnvelopeEvidence &evidence,
                                        const OutboundBudgetState &budget, std::int64_t now) {
    const OutboundKindUsage &usage = budget.usage[static_cast<std::size_t>(kind)];
    const std::vector<std::string> ids = list_authorization_envelopes(dir);
    EnvelopeCoverage best;
    best.reason = ids.empty() ? "no_envelope" : "not_covered";
    for (const std::string &id : ids) {
        const auto envelope = load_authorization_envelope(dir, id, policy);
        if (!envelope) {
            /* Unreadable or corrupt: skipped as non-covering, never an
             * abort — a broken envelope must not widen authorisation. */
            continue;
        }
        const EnvelopeCoverage coverage =
            covers_action(*envelope, kind, text, evidence, usage, now);
        if (coverage.covered) {
            return coverage;
        }
        /* Prefer the most specific reason: a concrete non-expiry reason
         * wins over "expired", but a sole expired envelope reports
         * "expired" rather than the generic "not_covered". */
        if (coverage.reason != "expired" || best.reason == "no_envelope" ||
            best.reason == "not_covered") {
            best = coverage;
        }
    }
    return best;
}

} // namespace atperson
