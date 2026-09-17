#ifndef ATPERSON_DAEMON_SIGNALS_HPP
#define ATPERSON_DAEMON_SIGNALS_HPP

// Process shutdown signalling for the ingestion daemon (#21).
//
// The daemon must stop promptly on SIGINT/SIGTERM and flush durable state.
// This module owns the process-global signal flag and an interruptible sleep
// so a long poll/catch-up wait ends as soon as a signal arrives instead of
// being cut short mid-flush.
//
// Contract: install_shutdown_signals() must be called once, on the thread
// that will observe shutdown_requested(). The flag is a lock-free
// sig_atomic_t written by the handler; it is never cleared implicitly, so a
// caller that installs handlers starts from a clean flag.
//
// Not thread-safe to install concurrently with running workers; the daemon is
// single-threaded.

#include <chrono>

namespace atperson {

/* Install SIGINT/SIGTERM handlers that set the shutdown flag. */
void install_shutdown_signals();

/* True once a shutdown signal has been observed. Async-signal-safe read. */
[[nodiscard]] bool shutdown_requested() noexcept;

/* Sleep for `duration`, returning early (true) when a shutdown signal
 * arrives or one was already pending. Returns false only after sleeping the
 * full duration with no pending shutdown. */
[[nodiscard]] bool sleep_until_interrupted(std::chrono::milliseconds duration);

} // namespace atperson

#endif
