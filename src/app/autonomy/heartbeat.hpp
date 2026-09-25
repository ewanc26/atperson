/* Supervisor health surface (#143): a heartbeat document the daemon
 * refreshes every cycle, and a health check a supervisor (systemd,
 * compose healthcheck) can poll with machine-readable output and
 * exit-code semantics.
 *
 * The heartbeat is local runtime metadata: it lives in the central data
 * directory, never on the PDS, and never in learned state. It is
 * advisory liveness evidence, not authority — a stale heartbeat must
 * never authorise anything. */
#ifndef ATPERSON_AUTONOMY_HEARTBEAT_HPP
#define ATPERSON_AUTONOMY_HEARTBEAT_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace atperson {

/* What the daemon last did, written atomically every cycle. */
struct AutonomyHeartbeat {
    std::uint32_t version{1};
    std::string run_id;
    std::uint64_t cycle{0};
    std::string beat_at;      /* RFC 3339, wall clock at write time */
    std::string phase;       /* autonomy phase name at write time */
};

/* Parse/serialise. Parse throws std::runtime_error on malformed input. */
[[nodiscard]] AutonomyHeartbeat parse_autonomy_heartbeat(const std::string &json);
[[nodiscard]] std::string serialise_autonomy_heartbeat(const AutonomyHeartbeat &beat);

/* Write the heartbeat atomically (temp file + rename). */
void save_autonomy_heartbeat(const AutonomyHeartbeat &beat,
                             const std::filesystem::path &path);

/* Read a heartbeat; nullopt when the file does not exist. Throws on a
 * corrupt (present but malformed) file — a supervisor must distinguish
 * "never started" from "running but writing garbage". */
[[nodiscard]] std::optional<AutonomyHeartbeat>
load_autonomy_heartbeat(const std::filesystem::path &path);

/* Health verdict for a supervisor poll. */
enum class AutonomyHealth { Healthy, Stale, Unreadable };

struct AutonomyHealthReport {
    AutonomyHealth health{AutonomyHealth::Unreadable};
    std::string detail;
    std::optional<AutonomyHeartbeat> heartbeat;
};

/* Evaluate liveness against a staleness threshold: a heartbeat older
 * than `max_age_seconds` (wall clock vs beat_at) is Stale. A missing
 * heartbeat is Unreadable (fail-closed: no liveness evidence). A
 * corrupt heartbeat is Unreadable. */
[[nodiscard]] AutonomyHealthReport
autonomy_health(const std::filesystem::path &path, std::int64_t now,
                std::int64_t max_age_seconds);

const char *autonomy_health_name(AutonomyHealth health) noexcept;

} // namespace atperson

#endif
