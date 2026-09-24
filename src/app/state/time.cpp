#include "time.hpp"

#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>

namespace atperson {

std::optional<std::uint64_t> parse_rfc3339_epoch(std::string_view value) {
    if (value.size() < 19u) {
        return std::nullopt;
    }
    for (std::size_t i = 0u; i < 19u; ++i) {
        const bool digit = value[i] >= '0' && value[i] <= '9';
        /* Fixed positions: YYYY-MM-DD (dashes at 4 and 7), THH:MM:SS
         * (colons at 13 and 16). Anything else makes the instant invalid. */
        const bool dash = (i == 4u || i == 7u) && value[i] == '-';
        const bool colon = (i == 13u || i == 16u) && value[i] == ':';
        const bool tee = i == 10u && value[i] == 'T';
        if (!digit && !dash && !colon && !tee) {
            return std::nullopt;
        }
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(std::string(value.substr(0u, 19u)).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year,
                    &month, &day, &hour, &minute, &second) != 6) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60) {
        return std::nullopt;
    }

    std::size_t position = 19u;
    if (position < value.size() && value[position] == '.') {
        ++position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            ++position;
        }
    }

    long offset_seconds = 0;
    if (position < value.size()) {
        const char zone = value[position];
        if (zone == 'Z' || zone == 'z') {
            ++position;
        } else if (zone == '+' || zone == '-') {
            if (position + 6u > value.size() || value[position + 3u] != ':') {
                return std::nullopt;
            }
            long hours = 0;
            long minutes = 0;
            try {
                hours = std::stol(std::string(value.substr(position + 1u, 2u)));
                minutes = std::stol(std::string(value.substr(position + 4u, 2u)));
            } catch (const std::exception &) {
                return std::nullopt;
            }
            if (hours > 23 || minutes > 59) {
                return std::nullopt;
            }
            offset_seconds = hours * 3600 + minutes * 60;
            if (zone == '-') {
                offset_seconds = -offset_seconds;
            }
            position += 6u;
        } else {
            return std::nullopt;
        }
    }
    if (position != value.size()) {
        return std::nullopt;
    }

    const std::int64_t adjusted_month =
        month > 2 ? static_cast<std::int64_t>(month) : static_cast<std::int64_t>(month + 12);
    const std::int64_t adjusted_year = year - (month > 2 ? 0 : 1);
    const std::int64_t era = adjusted_year >= 0 ? adjusted_year / 400 : (adjusted_year - 399) / 400;
    const std::int64_t year_of_era = adjusted_year - era * 400;
    const std::int64_t day_of_year =
        (153 * (adjusted_month > 2 ? adjusted_month - 3 : adjusted_month + 9) + 2) / 5 + day - 1;
    const std::int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const std::int64_t epoch_days = era * 146097 + day_of_era - 719468;
    const std::int64_t local_seconds = epoch_days * 86400 +
                                       static_cast<std::int64_t>(hour) * 3600 +
                                       static_cast<std::int64_t>(minute) * 60 + second;
    const std::int64_t epoch = local_seconds - offset_seconds;
    if (epoch < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(epoch);
}

std::string rfc3339_from_unix(std::int64_t unix_seconds) {
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

} // namespace atperson
