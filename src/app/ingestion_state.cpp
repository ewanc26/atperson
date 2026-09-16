#include "ingestion_state.hpp"

#include <cJSON.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace atperson {

namespace {

constexpr std::string_view kFormat = "atperson-ingestion-state";
constexpr std::uint32_t kVersion = 1u;
constexpr std::string_view kSourceKind = "atproto-timeline";
constexpr std::string_view kEndpoint = "app.bsky.feed.getTimeline";

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

[[noreturn]] void invalid(const std::string &message) {
    throw IngestionStateError("ingestion state: " + message);
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
    if (!cJSON_IsString(value) || !value->valuestring) {
        invalid(std::string("field '") + key + "' must be a string or null");
    }
    if (!value->valuestring[0]) {
        invalid(std::string("field '") + key + "' must not be an empty string");
    }
    return std::string(value->valuestring);
}

std::uint64_t require_u64(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 ||
        value->valuedouble > 9.007199254740992e15 /* 2^53 */) {
        invalid(std::string("field '") + key + "' must be a non-negative integer");
    }
    return static_cast<std::uint64_t>(value->valuedouble);
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

std::string json_string(const std::string &value) {
    Json printed(cJSON_CreateString(value.c_str()));
    if (!printed) {
        throw std::runtime_error("ingestion state: string allocation failed");
    }
    char *raw = cJSON_PrintUnformatted(printed.get());
    if (!raw) {
        throw std::runtime_error("ingestion state: string printing failed");
    }
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

std::string json_nullable_string(const std::optional<std::string> &value) {
    return value ? json_string(*value) : "null";
}

/* RFC 3339 UTC timestamp for checkpoint.saved_at, diagnostic only. */
std::string utc_now_rfc3339() {
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

void validate(const IngestionState &state) {
    if (state.version != kVersion) {
        invalid("unsupported version " + std::to_string(state.version));
    }
    if (state.source.kind != kSourceKind) {
        invalid("unsupported source kind '" + state.source.kind + "'");
    }
    if (state.source.service.empty()) {
        invalid("source.service must be non-empty");
    }
    if (state.source.account_did.rfind("did:", 0u) != 0u) {
        invalid("source.account_did must be a DID");
    }
    if (state.source.endpoint != kEndpoint) {
        invalid("unsupported source endpoint '" + state.source.endpoint + "'");
    }
    if (state.catchup.active) {
        if (!state.catchup.cursor || state.catchup.cursor->empty()) {
            invalid("active catch-up requires a non-empty cursor");
        }
    } else if (state.catchup.cursor) {
        invalid("inactive catch-up must not carry a cursor");
    }
}

} // namespace

std::string normalise_service(std::string_view service) {
    std::string result(service);
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

IngestionState initial_ingestion_state(std::string_view service,
                                       std::string_view account_did) {
    IngestionState state;
    state.source.kind = kSourceKind;
    state.source.service = normalise_service(service);
    state.source.account_did = std::string(account_did);
    state.source.endpoint = kEndpoint;
    /* algorithm: nullopt — the default timeline algorithm. */
    return state;
}

bool source_matches(const IngestionState &state, std::string_view service,
                    std::string_view account_did) {
    if (state.source.kind != kSourceKind || state.source.endpoint != kEndpoint) {
        return false;
    }
    if (state.source.service != normalise_service(service)) {
        return false;
    }
    if (state.source.account_did != account_did) {
        return false;
    }
    /* Both nullopt means the default algorithm on both sides; an explicitly
     * selected algorithm is never interchangeable with the default. */
    return !state.source.algorithm.has_value();
}

std::string serialise_ingestion_state(const IngestionState &state) {
    validate(state);
    std::string out;
    out.reserve(512u);
    out.append("{\"format\":\"");
    out.append(kFormat);
    out.append("\",\"version\":");
    out.append(std::to_string(state.version));
    out.append(",\"source\":{\"kind\":\"");
    out.append(state.source.kind);
    out.append("\",\"service\":");
    out.append(json_string(state.source.service));
    out.append(",\"account_did\":");
    out.append(json_string(state.source.account_did));
    out.append(",\"endpoint\":\"");
    out.append(state.source.endpoint);
    out.append("\",\"algorithm\":");
    out.append(json_nullable_string(state.source.algorithm));
    out.append("},\"catchup\":{\"active\":");
    out.append(state.catchup.active ? "true" : "false");
    out.append(",\"cursor\":");
    out.append(json_nullable_string(state.catchup.cursor));
    out.append("},\"checkpoint\":{\"generation\":");
    out.append(std::to_string(state.checkpoint.generation));
    out.append(",\"saved_at\":");
    out.append(json_nullable_string(state.checkpoint.saved_at));
    out.append(",\"pages_completed\":");
    out.append(std::to_string(state.checkpoint.pages_completed));
    out.append(",\"observations_seen\":");
    out.append(std::to_string(state.checkpoint.observations_seen));
    out.append("}}\n");
    return out;
}

IngestionState load_ingestion_state(const std::filesystem::path &path,
                                    std::string_view service,
                                    std::string_view account_did) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return initial_ingestion_state(service, account_did);
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("ingestion state: could not open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    if (input.bad()) {
        throw std::runtime_error("ingestion state: could not read " + path.string());
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

    IngestionState state;
    state.version = require_version(root.get());

    cJSON *source = cJSON_GetObjectItemCaseSensitive(root.get(), "source");
    if (!cJSON_IsObject(source)) {
        invalid("missing 'source' object");
    }
    state.source.kind = require_string(source, "kind");
    state.source.service = require_string(source, "service");
    state.source.account_did = require_string(source, "account_did");
    state.source.endpoint = require_string(source, "endpoint");
    state.source.algorithm = optional_string(source, "algorithm");

    cJSON *catchup = cJSON_GetObjectItemCaseSensitive(root.get(), "catchup");
    if (!cJSON_IsObject(catchup)) {
        invalid("missing 'catchup' object");
    }
    cJSON *active = cJSON_GetObjectItemCaseSensitive(catchup, "active");
    if (!cJSON_IsBool(active)) {
        invalid("catchup.active must be a boolean");
    }
    state.catchup.active = cJSON_IsTrue(active);
    state.catchup.cursor = optional_string(catchup, "cursor");

    cJSON *checkpoint = cJSON_GetObjectItemCaseSensitive(root.get(), "checkpoint");
    if (!cJSON_IsObject(checkpoint)) {
        invalid("missing 'checkpoint' object");
    }
    state.checkpoint.generation = require_u64(checkpoint, "generation");
    state.checkpoint.saved_at = optional_string(checkpoint, "saved_at");
    state.checkpoint.pages_completed = require_u64(checkpoint, "pages_completed");
    state.checkpoint.observations_seen = require_u64(checkpoint, "observations_seen");

    validate(state);

    if (!source_matches(state, service, account_did)) {
        /* The cursor belongs to a different account, service, or feed: it is
         * not reusable. Report the mismatch and start from the current
         * session's clean state. The ledger still suppresses anything
         * already committed. */
        std::fprintf(stderr,
                     "atperson: ingestion cursor belongs to a different source "
                     "(stored service '%s', account '%s'; current service '%s', "
                     "account '%s'); starting a fresh traversal from the timeline "
                     "head\n",
                     state.source.service.c_str(),
                     state.source.account_did.c_str(),
                     normalise_service(service).c_str(), std::string(account_did).c_str());
        return initial_ingestion_state(service, account_did);
    }
    return state;
}

void save_ingestion_state(const IngestionState &state,
                          const std::filesystem::path &path) {
    /* Every persisted checkpoint carries the time it was written; the
     * caller's in-memory copy is not mutated. */
    IngestionState stamped = state;
    stamped.checkpoint.saved_at = utc_now_rfc3339();
    const std::string payload = serialise_ingestion_state(stamped);

    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("ingestion state: could not create " + tmp.string());
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            throw std::runtime_error("ingestion state: could not write " + tmp.string());
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        throw std::runtime_error("ingestion state: could not commit " + path.string() +
                                 ": " + ec.message());
    }
}

void reset_ingestion_state(IngestionState &state) {
    state.catchup = CatchupCursor{};
    state.checkpoint.pages_completed = 0u;
    state.checkpoint.observations_seen = 0u;
}

} // namespace atperson
