#ifndef ATPERSON_SCHEDULER_BREAKER_HPP
#define ATPERSON_SCHEDULER_BREAKER_HPP

// Circuit breaker and poison-proposal quarantine for unattended execution.
//
// An unattended entity has no operator watching for a failing dependency, so
// it must stop hammering one on its own. Without this, a proposal that can never
// succeed (its reply parent was deleted) is retried every cycle forever, and a
// systemic failure (the PDS is down, the session was revoked, the server is
// rate-limiting) is retried at full speed every cycle. The two mechanisms here
// are the standard remedies, kept deterministic and inspectable:
//
//   circuit breaker   after `failure_threshold` consecutive FAILED executions the
//                     breaker opens and execution pauses for a cool-down. When
//                     it elapses one attempt is allowed (half-open): success
//                     closes it and resets the cool-down; failure re-opens it
//                     with the cool-down doubled, up to `max_cooldown_seconds`.
//                     It heals on its own, so it never needs a human to reset it
//                     (though `autonomy breaker reset` can).
//   quarantine        a proposal that fails `proposal_failure_limit` times is
//                     set aside and not retried, so one poison proposal cannot
//                     starve the ones behind it.
//
// Only genuine faults count. A refusal by policy, control or the output guard
// (denied, deferred, dry-run) is the system working as designed and never trips
// the breaker. Deciding and proposing continue while the breaker is open; only
// the network write is held back.
//
// State is runtime metadata, not learned state: one small JSON file in the data
// directory, written atomically. Pure transitions with an injected clock.
//
// Failure modes: BreakerError for a malformed state file (corruption is
// reported, never silently reset); std::runtime_error for I/O failure.

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {

class BreakerError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct BreakerConfig {
    std::uint32_t failure_threshold{3};
    std::int64_t base_cooldown_seconds{15ll * 60ll};
    std::int64_t max_cooldown_seconds{6ll * 60ll * 60ll};
    std::uint32_t proposal_failure_limit{3};
};

struct BreakerState {
    std::uint32_t version{1};
    std::uint32_t consecutive_failures{0};
    /* Unix seconds; 0 = closed. Non-zero means tripped: open while `now` is
     * before it, half-open once it has passed, until a success resets it. */
    std::int64_t open_until{0};
    std::int64_t cooldown_seconds{0};
    std::uint64_t trips{0};
    std::int64_t last_failure_at{0};
    std::string last_failure_detail;
    /* Failures per proposal digest, for quarantine. */
    std::map<std::string, std::uint32_t> proposal_failures;
};

enum class BreakerGate { Closed, Open, HalfOpen };

[[nodiscard]] const char *breaker_gate_name(BreakerGate gate) noexcept;
[[nodiscard]] BreakerGate breaker_gate(const BreakerState &state, std::int64_t now) noexcept;

/* A successful execution closes the breaker and forgets the proposal. */
void record_success(BreakerState &state, std::string_view proposal_digest);

struct FailureEffect {
    /* This failure opened (or re-opened) the breaker. */
    bool tripped{false};
    /* The proposal has failed too often and should be set aside. */
    bool quarantine{false};
};

[[nodiscard]] FailureEffect record_failure(BreakerState &state, const BreakerConfig &config,
                                           std::string_view proposal_digest,
                                           std::string_view detail, std::int64_t now);

/* Forget failure counts for proposals that no longer exist, keeping the map
 * bounded by the number of queued proposals. */
void retain_proposals(BreakerState &state, const std::set<std::string> &digests);

/* Missing file = closed with no history. A malformed file throws BreakerError. */
[[nodiscard]] BreakerState load_breaker_state(const std::filesystem::path &path);
/* Atomic (temp + rename). */
void save_breaker_state(const BreakerState &state, const std::filesystem::path &path);

} // namespace atperson

#endif
