#include "selfeval/config.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
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

SelfEvalConfig self_eval_config_from_environment() {
    SelfEvalConfig config;
    const char *enabled = std::getenv("ATPERSON_SELFEVAL");
    config.enabled = enabled != nullptr && std::string_view(enabled) == "1";

    config.cadence_seconds = static_cast<std::int64_t>(parse_positive_u64(
        std::getenv("ATPERSON_SELFEVAL_CADENCE_SECONDS"), 7ull * 24ull * 3600ull,
        static_cast<std::uint64_t>(kMaxSelfEvalCadenceSeconds)));

    config.max_trace_authors = static_cast<std::size_t>(parse_positive_u64(
        std::getenv("ATPERSON_SELFEVAL_MAX_TRACE"), 8u, kMaxSelfEvalTraceItems));

    config.max_trace_groups = static_cast<std::size_t>(parse_positive_u64(
        std::getenv("ATPERSON_SELFEVAL_MAX_TRACE_GROUPS"), 8u,
        kMaxSelfEvalTraceItems));

    return config;
}

} // namespace atperson