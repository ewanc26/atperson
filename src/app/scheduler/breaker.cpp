#include "breaker.hpp"

#include <cJSON.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

namespace atperson {
namespace {

struct JsonDeleter {
    void operator()(cJSON *value) const noexcept { cJSON_Delete(value); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

[[noreturn]] void invalid(const std::string &what) {
    throw BreakerError("scheduler breaker state: " + what);
}

std::int64_t number_field(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 9.0e15) {
        invalid(std::string("field '") + name + "' is missing or not a non-negative number");
    }
    return static_cast<std::int64_t>(item->valuedouble);
}

} // namespace

const char *breaker_gate_name(BreakerGate gate) noexcept {
    switch (gate) {
    case BreakerGate::Closed: return "closed";
    case BreakerGate::Open: return "open";
    case BreakerGate::HalfOpen: return "half-open";
    }
    return "closed";
}

BreakerGate breaker_gate(const BreakerState &state, std::int64_t now) noexcept {
    if (state.open_until == 0) {
        return BreakerGate::Closed;
    }
    return now < state.open_until ? BreakerGate::Open : BreakerGate::HalfOpen;
}

void record_success(BreakerState &state, std::string_view proposal_digest) {
    state.consecutive_failures = 0;
    state.open_until = 0;
    state.cooldown_seconds = 0;
    state.proposal_failures.erase(std::string(proposal_digest));
}

FailureEffect record_failure(BreakerState &state, const BreakerConfig &config,
                             std::string_view proposal_digest, std::string_view detail,
                             std::int64_t now) {
    FailureEffect effect;
    state.last_failure_at = now;
    state.last_failure_detail = std::string(detail.substr(0, 200));
    ++state.consecutive_failures;

    /* A failure while half-open (a trip is still recorded) re-opens at once; a
     * closed breaker opens when the streak reaches the threshold. */
    const bool half_open_failure = state.open_until != 0;
    if (half_open_failure || state.consecutive_failures >= config.failure_threshold) {
        state.cooldown_seconds = state.cooldown_seconds == 0
                                     ? config.base_cooldown_seconds
                                     : std::min(state.cooldown_seconds * 2,
                                                config.max_cooldown_seconds);
        state.cooldown_seconds = std::min(state.cooldown_seconds, config.max_cooldown_seconds);
        state.open_until = now + state.cooldown_seconds;
        ++state.trips;
        effect.tripped = true;
    }

    std::uint32_t &failures = state.proposal_failures[std::string(proposal_digest)];
    ++failures;
    if (failures >= config.proposal_failure_limit) {
        effect.quarantine = true;
        state.proposal_failures.erase(std::string(proposal_digest));
    }
    return effect;
}

void retain_proposals(BreakerState &state, const std::set<std::string> &digests) {
    for (auto it = state.proposal_failures.begin(); it != state.proposal_failures.end();) {
        it = digests.contains(it->first) ? std::next(it) : state.proposal_failures.erase(it);
    }
}

BreakerState load_breaker_state(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    const std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json root(cJSON_ParseWithLength(raw.data(), raw.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        invalid("not a JSON object");
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) || version->valueint != 1) {
        invalid("unsupported version");
    }
    BreakerState state;
    state.consecutive_failures = static_cast<std::uint32_t>(
        std::min<std::int64_t>(number_field(root.get(), "consecutive_failures"), 1000000));
    state.open_until = number_field(root.get(), "open_until");
    state.cooldown_seconds = number_field(root.get(), "cooldown_seconds");
    state.trips = static_cast<std::uint64_t>(number_field(root.get(), "trips"));
    state.last_failure_at = number_field(root.get(), "last_failure_at");
    const cJSON *detail = cJSON_GetObjectItemCaseSensitive(root.get(), "last_failure_detail");
    if (cJSON_IsString(detail) && detail->valuestring != nullptr) {
        state.last_failure_detail = detail->valuestring;
    }
    const cJSON *failures = cJSON_GetObjectItemCaseSensitive(root.get(), "proposal_failures");
    if (!cJSON_IsObject(failures)) {
        invalid("'proposal_failures' must be an object");
    }
    for (const cJSON *entry = failures->child; entry != nullptr; entry = entry->next) {
        if (!cJSON_IsNumber(entry) || entry->string == nullptr || entry->valuedouble < 0) {
            invalid("'proposal_failures' entries must be non-negative numbers");
        }
        state.proposal_failures[entry->string] = static_cast<std::uint32_t>(entry->valuedouble);
    }
    return state;
}

void save_breaker_state(const BreakerState &state, const std::filesystem::path &path) {
    Json root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.get(), "version", 1);
    cJSON_AddNumberToObject(root.get(), "consecutive_failures", state.consecutive_failures);
    cJSON_AddNumberToObject(root.get(), "open_until", static_cast<double>(state.open_until));
    cJSON_AddNumberToObject(root.get(), "cooldown_seconds",
                            static_cast<double>(state.cooldown_seconds));
    cJSON_AddNumberToObject(root.get(), "trips", static_cast<double>(state.trips));
    cJSON_AddNumberToObject(root.get(), "last_failure_at",
                            static_cast<double>(state.last_failure_at));
    cJSON_AddStringToObject(root.get(), "last_failure_detail", state.last_failure_detail.c_str());
    cJSON *failures = cJSON_AddObjectToObject(root.get(), "proposal_failures");
    for (const auto &[digest, count] : state.proposal_failures) {
        cJSON_AddNumberToObject(failures, digest.c_str(), count);
    }
    char *printed = cJSON_PrintUnformatted(root.get());
    if (printed == nullptr) {
        throw std::runtime_error("failed to serialise breaker state");
    }
    const std::string text(printed);
    cJSON_free(printed);

    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write " + tmp.string());
        }
        output << text << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("failed while writing " + tmp.string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        throw std::runtime_error("cannot replace " + path.string() + ": " + ec.message());
    }
}

} // namespace atperson
