#include "intent/config.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace atperson {
namespace {

std::uint64_t parse_positive_u64(const char *value, std::uint64_t fallback,
                                 std::uint64_t maximum) {
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0ull) {
        return fallback;
    }
    return std::min<std::uint64_t>(parsed, maximum);
}

} // namespace

IntentConfig intent_config_from_environment() {
    IntentConfig config;
    const char *enabled = std::getenv("ATPERSON_INTENTS");
    config.enabled = enabled != nullptr && std::string_view(enabled) == "1";

    config.max_active = static_cast<std::size_t>(parse_positive_u64(
        std::getenv("ATPERSON_INTENT_MAX_ACTIVE"), 3u, kMaxActiveIntents));

    config.max_continuations = static_cast<std::uint32_t>(parse_positive_u64(
        std::getenv("ATPERSON_INTENT_MAX_CONTINUATIONS"), 3u, kMaxIntentContinuations));

    config.window_seconds = static_cast<std::int64_t>(parse_positive_u64(
        std::getenv("ATPERSON_INTENT_WINDOW_DAYS"), 2u, 366u) * 24ull * 3600ull);
    return config;
}

} // namespace atperson