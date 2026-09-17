#ifndef ATPERSON_DAEMON_BACKOFF_HPP
#define ATPERSON_DAEMON_BACKOFF_HPP

// Deterministic bounded exponential backoff for the ingestion daemon (#21).
//
// Owns the delay policy applied between retries of a transient failure. The
// sequence is deterministic for a fixed config and seed, so restart/failure
// behaviour is reproducible in tests without waiting on real time.
//
// Design: the first failure returns `initial`; each further consecutive
// failure multiplies the previous delay by `factor`, clamped to `maximum`.
// A non-zero `jitter` fraction is added on top from a deterministic PRNG and
// the result is clamped to `maximum`, so the delay is never unbounded. Any
// success resets the sequence via reset().
//
// No global state, no threads, no I/O.

#include <chrono>
#include <cstdint>

namespace atperson {

/* Backoff tuning. All fields are validated by validate_daemon_config. */
struct BackoffConfig {
    std::chrono::milliseconds initial{1000};
    std::chrono::milliseconds maximum{300000};
    double factor{2.0};
    double jitter{0.2}; /* fraction of the computed delay, in [0, 1] */
};

class Backoff {
  public:
    /* `seed` seeds the jitter PRNG; 0 selects a fixed default so the
     * unseeded sequence is still deterministic. */
    explicit Backoff(const BackoffConfig &config, std::uint64_t seed = 0u);

    /* Advance the sequence and return the delay to wait before the next
     * retry. */
    [[nodiscard]] std::chrono::milliseconds next();

    /* Return to the start of the sequence (after a success). */
    void reset() noexcept;

    /* Consecutive failures observed since the last reset(). */
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept {
        return failures_;
    }

  private:
    BackoffConfig config_;
    std::chrono::milliseconds current_{};
    std::uint64_t rng_;
    std::uint32_t failures_{};
};

} // namespace atperson

#endif
