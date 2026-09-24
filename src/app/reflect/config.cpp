#include "reflect/config.hpp"

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

double parse_unit_fraction(const char *value, double fallback, double maximum) {
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    errno = 0;
    char *end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || !(parsed > 0.0) || parsed > 1.0) {
        return fallback;
    }
    return std::min<double>(parsed, maximum);
}

} // namespace

ReflectionConfig reflection_config_from_environment() {
    ReflectionConfig config;
    const char *enabled = std::getenv("ATPERSON_REFLECTION");
    config.enabled = enabled != nullptr && std::string_view(enabled) == "1";

    config.cadence_seconds = static_cast<std::int64_t>(parse_positive_u64(
        std::getenv("ATPERSON_REFLECTION_CADENCE_SECONDS"), 24ull * 3600ull,
        static_cast<std::uint64_t>(kMaxReflectionCadenceSeconds)));

    config.max_thoughts = static_cast<std::size_t>(
        parse_positive_u64(std::getenv("ATPERSON_REFLECTION_MAX_THOUGHTS"), 8u,
                           kMaxReflectionThoughts));

    config.window_seconds = static_cast<std::int64_t>(parse_positive_u64(
        std::getenv("ATPERSON_REFLECTION_WINDOW_SECONDS"), 7ull * 24ull * 3600ull,
        static_cast<std::uint64_t>(kMaxReflectionWindowSeconds)));

    config.valence_delta_min = static_cast<float>(parse_unit_fraction(
        std::getenv("ATPERSON_REFLECTION_VALENCE_DELTA_MIN"), 0.25, 1.0));

    config.unfamiliar_authors_min = static_cast<std::size_t>(parse_positive_u64(
        std::getenv("ATPERSON_REFLECTION_UNFAMILIAR_MIN"), 2u, kMaxReflectionThoughts));

    config.reply_ratio_shift_min = static_cast<float>(parse_unit_fraction(
        std::getenv("ATPERSON_REFLECTION_REPLY_SHIFT_MIN"), 0.25, 1.0));

    return config;
}

} // namespace atperson