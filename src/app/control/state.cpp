#include "state.hpp"

#include <cJSON.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace atperson {

namespace {

constexpr std::string_view kFormat = "atperson-control-state";
constexpr std::uint32_t kVersion = 1u;
constexpr std::size_t kMaxApprovedDigests = 64u;

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

[[noreturn]] void invalid(const std::string &message) {
    throw ControlStateError("control state: " + message);
}

std::string json_string(const std::string &value) {
    Json printed(cJSON_CreateString(value.c_str()));
    if (!printed) {
        throw std::runtime_error("control state: string allocation failed");
    }
    char *raw = cJSON_PrintUnformatted(printed.get());
    if (!raw) {
        throw std::runtime_error("control state: string printing failed");
    }
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

std::string json_nullable_string(const std::optional<std::string> &value) {
    return value ? json_string(*value) : "null";
}

std::optional<std::string> optional_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value || cJSON_IsNull(value)) {
        return std::nullopt;
    }
    if (!cJSON_IsString(value) || !value->valuestring) {
        invalid(std::string("field '") + key + "' must be a string or null");
    }
    if (!value->valuestring[0]) {
        invalid(std::string("field '") + key + "' must not be an empty string");
    }
    return std::string(value->valuestring);
}

std::string require_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0]) {
        invalid(std::string("missing or empty string field '") + key + "'");
    }
    return value->valuestring;
}

bool require_bool(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsBool(value)) {
        invalid(std::string("field '") + key + "' must be a boolean");
    }
    return cJSON_IsTrue(value);
}

std::uint32_t require_version(cJSON *object) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "version");
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0) {
        invalid("missing or invalid 'version'");
    }
    const auto version = static_cast<std::uint32_t>(value->valuedouble);
    if (version != kVersion) {
        invalid("unsupported version " + std::to_string(version) +
                " (supported: " + std::to_string(kVersion) + ")");
    }
    return version;
}

/* Digest format: 16 lowercase hex characters (64-bit atp_ledger_digest). */
bool is_digest_shape(std::string_view digest) {
    if (digest.size() != 16u) {
        return false;
    }
    for (const char c : digest) {
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) {
            return false;
        }
    }
    return true;
}

void validate(const ControlState &state) {
    if (state.version != kVersion) {
        invalid("unsupported version " + std::to_string(state.version));
    }
    if (state.approved_digests.size() > kMaxApprovedDigests) {
        invalid("too many approved digests (max " +
                std::to_string(kMaxApprovedDigests) + ")");
    }
    for (const std::string &digest : state.approved_digests) {
        if (!is_digest_shape(digest)) {
            invalid("approved digest '" + digest + "' is not 16 lowercase hex digits");
        }
    }
}

} // namespace

ControlState load_control_state(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        /* Fail-closed default: writes disabled, dry-run on, approval on. */
        return ControlState{};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("control state: could not open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    if (input.bad()) {
        throw std::runtime_error("control state: could not read " + path.string());
    }

    Json root(cJSON_ParseWithLength(text.data(), text.size()));
    if (!root) {
        invalid(path.string() + " is not valid JSON");
    }
    if (!cJSON_IsObject(root.get())) {
        invalid(path.string() + " is not a JSON object");
    }

    const std::string format = require_string(root.get(), "format");
    if (format != kFormat) {
        invalid("unexpected format '" + format + "'");
    }

    ControlState state;
    state.version = require_version(root.get());
    state.paused = require_bool(root.get(), "paused");
    state.writes_enabled = require_bool(root.get(), "writes_enabled");
    state.dry_run = require_bool(root.get(), "dry_run");
    state.approval_required = require_bool(root.get(), "approval_required");

    cJSON *digests = cJSON_GetObjectItemCaseSensitive(root.get(), "approved_digests");
    if (!cJSON_IsArray(digests)) {
        invalid("missing 'approved_digests' array");
    }
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, digests) {
        if (!cJSON_IsString(entry) || !entry->valuestring) {
            invalid("approved_digests entries must be strings");
        }
        state.approved_digests.emplace_back(entry->valuestring);
    }

    state.last_sync_at = optional_string(root.get(), "last_sync_at");
    state.shutdown_requested_at = optional_string(root.get(), "shutdown_requested_at");

    validate(state);
    return state;
}

std::string serialise_control_state(const ControlState &state) {
    validate(state);
    std::string out;
    out.reserve(512u);
    out.append("{\"format\":\"");
    out.append(kFormat);
    out.append("\",\"version\":");
    out.append(std::to_string(state.version));
    out.append(",\"paused\":");
    out.append(state.paused ? "true" : "false");
    out.append(",\"writes_enabled\":");
    out.append(state.writes_enabled ? "true" : "false");
    out.append(",\"dry_run\":");
    out.append(state.dry_run ? "true" : "false");
    out.append(",\"approval_required\":");
    out.append(state.approval_required ? "true" : "false");
    out.append(",\"approved_digests\":[");
    for (std::size_t i = 0; i < state.approved_digests.size(); ++i) {
        if (i != 0u) {
            out.push_back(',');
        }
        out.append(json_string(state.approved_digests[i]));
    }
    out.append("],\"last_sync_at\":");
    out.append(json_nullable_string(state.last_sync_at));
    out.append(",\"shutdown_requested_at\":");
    out.append(json_nullable_string(state.shutdown_requested_at));
    out.append("}\n");
    return out;
}

void save_control_state(const ControlState &state, const std::filesystem::path &path) {
    const std::string payload = serialise_control_state(state);

    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code parent_ec;
        std::filesystem::create_directories(parent, parent_ec);
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("control state: could not create " + tmp.string());
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            throw std::runtime_error("control state: could not write " + tmp.string());
        }
    }

    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        throw std::runtime_error("control state: could not commit " + path.string() +
                                 ": " + rename_ec.message());
    }
}

bool is_digest_approved(const ControlState &state, std::string_view digest) {
    for (const std::string &approved : state.approved_digests) {
        if (approved == digest) {
            return true;
        }
    }
    return false;
}

std::string control_now_rfc3339() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec);
    return buffer;
}

void ensure_outbound_allowed(const ControlState &state, std::string_view digest) {
    if (state.paused) {
        invalid("outbound refused: runtime is paused");
    }
    if (!state.writes_enabled) {
        invalid("outbound refused: network writes are disabled");
    }
    if (state.dry_run) {
        invalid("outbound refused: dry-run mode is on");
    }
    if (state.approval_required && !is_digest_approved(state, digest)) {
        invalid("outbound refused: action digest is not approved");
    }
}

} // namespace atperson
