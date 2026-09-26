#ifndef ATPERSON_CONTROL_REMOTE_POLL_HPP
#define ATPERSON_CONTROL_REMOTE_POLL_HPP

// Network side of the remote operator channel (#143): read the operator's
// control collection, apply what is admissible, persist the result.
//
// This is the only part of the channel that touches a service. Everything
// about trust — provenance, sequencing, argument rules — lives in
// control/remote.hpp and is decided offline; this file only fetches
// candidate records and hands them to that decision.
//
// One poll is one bounded pass:
//
//   - read the durable cursor; a missing cursor starts at zero, so the
//     first admissible request is seq 1;
//   - list the operator's control collection newest-first and stop as soon
//     as a record at or below the watermark is seen (records are ordered,
//     so everything past that point has already been applied);
//   - parse and apply in ascending seq order;
//   - commit the control state and the cursor together, cursor last.
//
// Fail-closed throughout: a fetch failure leaves both files untouched, a
// refused record advances nothing, and an unreadable cursor aborts the
// pass rather than restarting the sequence from zero.
//
// Ownership: borrows a WolframSession; the caller keeps it alive.
// Not thread-safe (one session, one thread).
//
// Failure modes: std::runtime_error (including wolfram_error) for a
// transport failure or an unusable cursor. A single malformed or
// inadmissible record is reported in RemotePollReport, not thrown — one
// bad record must not stop the daemon from serving the next one.

#include "atproto/session.hpp"
#include "control/remote.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace atperson {

/* How the poller is configured. `operator_did` empty disables the channel
 * entirely: no DID, no commands, and the pass reports why. */
struct RemotePollConfig {
    std::string operator_did;
    std::size_t max_records{32u};
    std::size_t max_applies{8u};
};

/* One record the poller refused, with the reason. Diagnostics only. */
struct RemoteRefusal {
    std::string rkey;
    std::string reason;
};

struct RemotePollReport {
    bool enabled{false};
    std::size_t examined{0};
    std::size_t applied{0};
    std::uint64_t watermark{}; /* after the pass */
    std::string last_op;       /* canonical op name, when anything applied */
    std::vector<RemoteRefusal> refusals;
};

class RemoteControlChannel final {
  public:
    RemoteControlChannel(WolframSession &session, RemotePollConfig config,
                         std::filesystem::path control_file, std::filesystem::path cursor_file)
        : session_(session), config_(std::move(config)), control_file_(std::move(control_file)),
          cursor_file_(std::move(cursor_file)) {}

    /* Run one bounded pass. Throws std::runtime_error on a transport
     * failure or an unusable cursor, having changed nothing on disk. */
    [[nodiscard]] RemotePollReport poll();

  private:
    WolframSession &session_;
    RemotePollConfig config_;
    std::filesystem::path control_file_;
    std::filesystem::path cursor_file_;
};

} // namespace atperson

#endif
