/* Supervisor health surface (#143): heartbeat implementation. See
 * heartbeat.hpp for the contract. */
#include "heartbeat.hpp"

#include "state/time.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace atperson {
namespace {

/* Extract a JSON string value for a key from the heartbeat document the
 * serialiser writes. Throws when missing or malformed. */
std::string json_string_value(const std::string &json, const std::string &key) {
    const std::size_t key_at = json.find("\"" + key + "\":");
    if (key_at == std::string::npos) {
        throw std::runtime_error("heartbeat: missing " + key);
    }
    const std::size_t open = json.find('"', key_at + key.size() + 3);
    if (open == std::string::npos) {
        throw std::runtime_error("heartbeat: " + key + " not a string");
    }
    const std::size_t close = json.find('"', open + 1);
    if (close == std::string::npos) {
        throw std::runtime_error("heartbeat: " + key + " unterminated");
    }
    return json.substr(open + 1, close - open - 1);
}

} // namespace

AutonomyHeartbeat parse_autonomy_heartbeat(const std::string &json) {
    const std::size_t version_at = json.find("\"version\":");
    if (version_at == std::string::npos) {
        throw std::runtime_error("heartbeat: missing version");
    }
    AutonomyHeartbeat beat;
    beat.version = static_cast<std::uint32_t>(std::stoul(json.substr(version_at + 10)));
    beat.run_id = json_string_value(json, "run_id");
    const std::size_t cycle_at = json.find("\"cycle\":");
    if (cycle_at == std::string::npos) {
        throw std::runtime_error("heartbeat: missing cycle");
    }
    beat.cycle = std::stoull(json.substr(cycle_at + 8));
    beat.beat_at = json_string_value(json, "beat_at");
    beat.phase = json_string_value(json, "phase");
    return beat;
}

std::string serialise_autonomy_heartbeat(const AutonomyHeartbeat &beat) {
    return "{\"version\":" + std::to_string(beat.version) + ",\"run_id\":\"" + beat.run_id +
           "\",\"cycle\":" + std::to_string(beat.cycle) + ",\"beat_at\":\"" + beat.beat_at +
           "\",\"phase\":\"" + beat.phase + "\"}";
}

void save_autonomy_heartbeat(const AutonomyHeartbeat &beat,
                             const std::filesystem::path &path) {
    const std::filesystem::path target = path;
    std::filesystem::path temp_file = target;
    temp_file += ".tmp";
    {
        std::ofstream stream(temp_file, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("heartbeat: cannot write " + temp_file.string());
        }
        stream << serialise_autonomy_heartbeat(beat);
        stream.flush();
        if (!stream) {
            throw std::runtime_error("heartbeat: write failed " + temp_file.string());
        }
    }
    std::filesystem::rename(temp_file, target);
}

std::optional<AutonomyHeartbeat> load_autonomy_heartbeat(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
    }
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("heartbeat: cannot read " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return parse_autonomy_heartbeat(buffer.str());
}

AutonomyHealthReport autonomy_health(const std::filesystem::path &path, std::int64_t now,
                                     std::int64_t max_age_seconds) {
    AutonomyHealthReport report;
    std::optional<AutonomyHeartbeat> beat;
    try {
        beat = load_autonomy_heartbeat(path);
    } catch (const std::exception &error) {
        report.health = AutonomyHealth::Unreadable;
        report.detail = std::string("corrupt heartbeat: ") + error.what();
        return report;
    }
    if (!beat) {
        report.health = AutonomyHealth::Unreadable;
        report.detail = "no heartbeat: daemon has not completed a cycle";
        return report;
    }
    const auto beat_epoch = parse_rfc3339_epoch(beat->beat_at);
    if (!beat_epoch) {
        report.health = AutonomyHealth::Unreadable;
        report.detail = "heartbeat timestamp not RFC 3339";
        report.heartbeat = beat;
        return report;
    }
    const std::int64_t age = now - static_cast<std::int64_t>(*beat_epoch);
    report.heartbeat = beat;
    if (age > max_age_seconds) {
        report.health = AutonomyHealth::Stale;
        report.detail = "heartbeat " + std::to_string(age) + "s old (max " +
                         std::to_string(max_age_seconds) + "s)";
        return report;
    }
    report.health = AutonomyHealth::Healthy;
    report.detail = "heartbeat " + std::to_string(age) + "s old";
    return report;
}

const char *autonomy_health_name(AutonomyHealth health) noexcept {
    switch (health) {
    case AutonomyHealth::Healthy: return "healthy";
    case AutonomyHealth::Stale: return "stale";
    case AutonomyHealth::Unreadable: return "unreadable";
    }
    return "unreadable";
}

} // namespace atperson
