#ifndef ATPERSON_DAEMON_FAILURE_HPP
#define ATPERSON_DAEMON_FAILURE_HPP

// Failure classification for the ingestion daemon (issue #21).
//
// The daemon must distinguish a transient protocol/transport failure (retry
// with backoff) from a fatal local-state failure (stop rather than continue
// on uncertain durable state). The distinction is structural: only
// RetryableError is retried, and every other exception is fatal.
//
// The network boundary is responsible for raising RetryableError: the CLI
// daemon wraps Wolfram-backed transport failures (for example
// TimelineHttpError) in RetryableError before they cross into the loop. The
// offline loop therefore needs no Wolfram or network dependency.
//
// Ownership: RetryableError is a value exception; callers throw and catch by
// const reference. Nothing here mutates state.

#include <stdexcept>
#include <string>

namespace atperson {

/* A transient failure the daemon may retry: transport/protocol problems that
 * leave local durable state untouched and retryable. */
class RetryableError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* True when `error` is a transient failure the daemon should back off from
 * rather than stop on. */
[[nodiscard]] inline bool is_retryable(const std::exception &error) noexcept {
    return dynamic_cast<const RetryableError *>(&error) != nullptr;
}

} // namespace atperson

#endif
