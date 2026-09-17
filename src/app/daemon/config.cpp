#include "config.hpp"

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace atperson {
namespace {

std::optional<std::string> env_value(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

[[noreturn]] void invalid(const char *name, const std::string &value) {
    throw std::runtime_error(std::string("invalid ") + name + " value: " + value);
}

std::uint64_t parse_u64(const char *name, const std::string &value) {
    if (value.empty() || value[0] == '-') {
        invalid(name, value);
    }
    try {
        std::size_t consumed = 0u;
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            invalid(name, value);
        }
        return static_cast<std::uint64_t>(parsed);
    } catch (const std::invalid_argument &) {
        invalid(name, value);
    } catch (const std::out_of_range &) {
        invalid(name, value);
    }
}

int parse_positive_int(const char *name, const std::string &value) {
    if (value.empty() || value[0] == '-') {
        invalid(name, value);
    }
    try {
        std::size_t consumed = 0u;
        const long long parsed = std::stoll(value, &consumed);
        if (consumed != value.size() || parsed <= 0 || parsed > 2147483647LL) {
            invalid(name, value);
        }
        return static_cast<int>(parsed);
    } catch (const std::invalid_argument &) {
        invalid(name, value);
    } catch (const std::out_of_range &) {
        invalid(name, value);
    }
}

double parse_double(const char *name, const std::string &value) {
    try {
        std::size_t consumed = 0u;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            invalid(name, value);
        }
        return parsed;
    } catch (const std::invalid_argument &) {
        invalid(name, value);
    } catch (const std::out_of_range &) {
        invalid(name, value);
    }
}

std::chrono::milliseconds parse_milliseconds(const char *name, const std::string &value) {
    const std::uint64_t millis = parse_u64(name, value);
    return std::chrono::milliseconds(static_cast<long long>(millis));
}

} // namespace

DaemonConfig daemon_config_from_environment() {
    DaemonConfig config;

    if (const auto value = env_value("ATPERSON_DAEMON_PAGES_PER_CYCLE")) {
        config.pages_per_cycle = parse_positive_int("ATPERSON_DAEMON_PAGES_PER_CYCLE", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_POLL_MS")) {
        config.poll_interval = parse_milliseconds("ATPERSON_DAEMON_POLL_MS", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_CATCHUP_MS")) {
        config.catchup_interval = parse_milliseconds("ATPERSON_DAEMON_CATCHUP_MS", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_MAX_CYCLES")) {
        config.max_cycles = parse_u64("ATPERSON_DAEMON_MAX_CYCLES", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_SNAPSHOT_EVERY")) {
        config.snapshot_every_cycles = parse_u64("ATPERSON_DAEMON_SNAPSHOT_EVERY", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_BACKOFF_INITIAL_MS")) {
        config.backoff.initial = parse_milliseconds("ATPERSON_DAEMON_BACKOFF_INITIAL_MS", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_BACKOFF_MAX_MS")) {
        config.backoff.maximum = parse_milliseconds("ATPERSON_DAEMON_BACKOFF_MAX_MS", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_BACKOFF_FACTOR")) {
        config.backoff.factor = parse_double("ATPERSON_DAEMON_BACKOFF_FACTOR", *value);
    }
    if (const auto value = env_value("ATPERSON_DAEMON_BACKOFF_JITTER")) {
        config.backoff.jitter = parse_double("ATPERSON_DAEMON_BACKOFF_JITTER", *value);
    }

    validate_daemon_config(config);
    return config;
}

void validate_daemon_config(const DaemonConfig &config) {
    if (config.pages_per_cycle <= 0) {
        throw std::runtime_error("daemon pages_per_cycle must be positive");
    }
    if (config.poll_interval.count() < 0 || config.catchup_interval.count() < 0) {
        throw std::runtime_error("daemon intervals must not be negative");
    }
    if (config.snapshot_every_cycles == 0u) {
        throw std::runtime_error("daemon snapshot_every_cycles must be positive");
    }
    if (config.backoff.initial.count() <= 0) {
        throw std::runtime_error("daemon backoff initial delay must be positive");
    }
    if (config.backoff.maximum < config.backoff.initial) {
        throw std::runtime_error("daemon backoff maximum must be >= initial");
    }
    if (config.backoff.factor < 1.0) {
        throw std::runtime_error("daemon backoff factor must be >= 1.0");
    }
    if (config.backoff.jitter < 0.0 || config.backoff.jitter > 1.0) {
        throw std::runtime_error("daemon backoff jitter must be in [0, 1]");
    }
}

} // namespace atperson
