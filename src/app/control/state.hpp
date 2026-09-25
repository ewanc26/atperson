#ifndef ATPERSON_CONTROL_STATE_HPP
#define ATPERSON_CONTROL_STATE_HPP

// Operator control state: pause, write gate, dry-run, approval gates.
//
// Owns the durable operator-facing switches that bound what the runtime may
// do before any autonomous network write exists (#22). The state file is a
// versioned JSON document saved with the same atomic write-and-rename
// contract as the ingestion checkpoint. Controls are runtime metadata only:
// they never mutate learned C23 state and never become model state.
//
// Fail-closed posture: a missing or unreadable control file means writes are
// DISABLED and dry-run is ON. Only an explicit, valid control file with
// writes_enabled=true can ever allow an outbound action.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

/* Operator control switches. All fields are runtime metadata. */
struct ControlState {
    std::uint32_t version{1};

    /* Pause: refuse new ingestion/sync work. Learned state is untouched. */
    bool paused{false};

    /* Master write gate. False means no outbound network write procedure
     * may be called, independent of learning. Fail-closed: default false. */
    bool writes_enabled{false};

    /* Dry-run: plan and evaluate policy as a write-enabled run would, but
     * never execute the outbound action. */
    bool dry_run{true};

    /* Offline mode (#154): network-bound records spool locally instead of
     * publishing. Learning, planning and journalling continue unchanged;
     * restoring online mode drains the spool through the same gates. */
    bool offline_mode{false};

    /* Require manual approval for proposed outbound actions. When true, an
     * action may only execute once its exact digest has been approved. */
    bool approval_required{true};

    /* Approved action digests, most recent first. An approval binds to the
     * exact inspected decision (context + plan + score digest); a
     * regenerated plan produces a different digest and is not covered. */
    std::vector<std::string> approved_digests;

    /* Diagnostics only. Never gates behaviour. */
    std::optional<std::string> last_sync_at;
    std::optional<std::string> shutdown_requested_at;
};

/* Thrown for malformed control files, unsupported versions, and impossible
 * field combinations. Corruption is reported, never silently reinterpreted. */
class ControlStateError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Load and validate the version-1 control file. A missing file yields the
 * fail-closed default state (writes disabled, dry-run on, approval on).
 * Anything else validates fully or throws ControlStateError. */
[[nodiscard]] ControlState load_control_state(const std::filesystem::path &path);

/* Serialise to a version-1 JSON document (deterministic field order). */
[[nodiscard]] std::string serialise_control_state(const ControlState &state);

/* Atomically persist: write <path>.tmp, flush, rename over the committed
 * file, then sync the containing directory. A crash leaves either the
 * previous or the new complete document, never half-written JSON. Throws
 * std::runtime_error on I/O failure. */
void save_control_state(const ControlState &state, const std::filesystem::path &path);

/* Fail-closed outbound gate. Throws ControlStateError unless every gate
 * passes: not paused, writes enabled, not dry-run, and (when
 * approval_required) `digest` appears in approved_digests. This is the single
 * choke point every future outbound write path must call before any network
 * procedure. */
void ensure_outbound_allowed(const ControlState &state, std::string_view digest);

/* True when `digest` is in approved_digests. */
[[nodiscard]] bool is_digest_approved(const ControlState &state,
                                      std::string_view digest);

/* RFC 3339 UTC timestamp, for diagnostics fields. */
[[nodiscard]] std::string control_now_rfc3339();

} // namespace atperson

#endif
