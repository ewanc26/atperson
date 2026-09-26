#ifndef ATPERSON_CONTROL_REMOTE_HPP
#define ATPERSON_CONTROL_REMOTE_HPP

// Remote operator channel (#143): an operator-authored AT Protocol record
// that the daemon applies to local control state, so a headless host can
// be paused from any client instead of only from a local shell.
//
// The record lives in a dedicated collection under the OPERATOR's own DID,
// never the entity's:
//
//   click.croft.atperson.control#request
//     seq   decimal string, strictly increasing per operator
//     op    "pause" | "resume" | "writes-on" | "writes-off"
//           | "dry-run-on" | "dry-run-off" | "offline-on" | "offline-off"
//           | "approval-on" | "approval-off"
//           | "approve" | "revoke" | "shutdown" | "cancel-shutdown"
//     arg   required for "approve"/"revoke" (the action digest),
//           forbidden for every other op
//     at    RFC 3339 UTC, diagnostics only
//
// Trust model, in the order it is checked:
//
//   0. Separation. The operator DID must NOT be the account DID. This is a
//      hard requirement, not a recommendation: if they match, the entity
//      could command itself, and a channel that can be driven by the thing
//      it governs is not a control channel. See
//      validate_operator_channel.
//   1. Provenance. A record counts only if its at-URI authority is the
//      configured operator DID. Anything else is not a command; it is
//      another account's data that happens to be visible.
//   2. Freshness. `seq` must exceed the durably persisted watermark by
//      exactly one. A gap means a command was lost or reordered, and
//      applying later ones would silently skip a pause: refuse.
//   3. Shape. Unknown op, missing/forbidden `arg`, malformed document:
//      refuse.
//
// Refusal is total — a rejected record leaves control state untouched and
// the watermark unmoved, so a corrected re-send at the same seq still
// works. Nothing here ever widens authority: every op maps onto the same
// ControlState fields the local `atperson control` CLI writes, so remote
// and local control compose instead of layering. The durable
// ATPERSON_ALLOW_EXTERNAL_PUBLISHING master switch and the outbound
// policy are downstream of this module and unaffected by it.
//
// This module is pure: no I/O, no clock, no network. `now` and the
// operator DID are always injected, so every path below is offline-
// testable and the network reader is the only part that needs a service.
//
// Failure modes: ControlRemoteError for a malformed request document, an
// unknown op, an argument that is missing or not allowed, or a sequence
// that is not the next one.

#include "state.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

/* Collection and record type for remote operator requests. */
inline constexpr std::string_view kControlCollection = "click.croft.atperson.control";
inline constexpr std::string_view kControlRequestType = "click.croft.atperson.control#request";

/* Document format marker, kept distinct from the state's own so a request
 * can never be mistaken for a control file (or the reverse). */
inline constexpr std::string_view kControlRequestFormat = "atperson-control-request";
inline constexpr std::uint32_t kControlRequestVersion = 1u;

class ControlRemoteError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* The operations a remote operator may request. Every one of these already
 * exists as a local `atperson control` subcommand; this is the same
 * surface, delivered over the network. */
enum class ControlOp {
    Pause,
    Resume,
    WritesOn,
    WritesOff,
    DryRunOn,
    DryRunOff,
    OfflineOn,
    OfflineOff,
    ApprovalOn,
    ApprovalOff,
    Approve,
    Revoke,
    Shutdown,
    CancelShutdown,
};

/* Canonical wire name. Throws ControlRemoteError for an unknown name. */
[[nodiscard]] std::string_view control_op_name(ControlOp op);

/* Inverse of control_op_name. */
[[nodiscard]] std::optional<ControlOp> control_op_from_name(std::string_view name);

/* True for the two ops that carry a digest in `arg`. */
[[nodiscard]] bool control_op_takes_argument(ControlOp op) noexcept;

/* Whether the channel may run at all, given the runtime's own account DID
 * and the configured operator DID.
 *
 * `account_did` is the DID the daemon authenticates as; `operator_did` is
 * the DID whose records are commands. Three outcomes:
 *
 *   Disabled   operator_did is empty. The channel is inert: no DID, no
 *              commands, and no session should be opened.
 *   Ready      the two DIDs are both present and different.
 *   Conflict   they are the same. Refused, because an entity that can
 *              command itself has no operator: anyone who can write to the
 *              entity's own repo could drive it, which is exactly the
 *              authority the channel exists to keep outside the runtime.
 *
 * A DID is a case-sensitive, method-specific identifier; comparison is
 * therefore exact. */
enum class OperatorChannelStatus { Disabled, Ready, Conflict };

[[nodiscard]] OperatorChannelStatus check_operator_channel(
    std::string_view account_did, std::string_view operator_did);

/* The reason a channel is not Ready, in the words an operator needs.
 * Empty for Ready. For Disabled it explains that the channel is off by
 * choice; for Conflict it names both DIDs and says what to do. */
[[nodiscard]] std::string operator_channel_denial(
    std::string_view account_did, std::string_view operator_did);

struct ControlRequest {
    std::uint64_t seq{};
    ControlOp op{ControlOp::Pause};
    std::string arg;               /* digest, for approve/revoke only */
    std::optional<std::string> at; /* RFC 3339, diagnostics only */
};

/* Serialise to the canonical request document. Rejects a missing argument
 * on approve/revoke and an argument on any other op, so an operator cannot
 * publish a record that would be refused on read. */
[[nodiscard]] std::string serialise_control_request(const ControlRequest &request);

/* Parse a request document. Validates format, version, op name, and the
 * argument rule; throws ControlRemoteError on anything else. */
[[nodiscard]] ControlRequest parse_control_request(std::string_view json);

/* Record key for a request, derived from its sequence. Fixed-width base-32
 * in the TID alphabet, listed in ASCII order, so lexicographic record order
 * matches sequence order: `reverse` listRecords really is newest-first, and
 * a daemon reading the collection can stop at the watermark. Deterministic
 * too, so re-emitting a sequence is idempotent under putRecord retry. */
[[nodiscard]] std::string control_request_rkey(std::uint64_t seq);

/* An applied request together with the watermark it advanced. */
struct RemoteApplyReport {
    bool applied{false};
    std::uint64_t seq{}; /* the request's seq, when accepted */
    std::string op;      /* canonical op name, for the audit line */
    std::string reason;  /* why it was refused, when it was */
};

/* Apply one request to `state`, gated by the persisted `watermark`.
 *
 * `operator_did` is the configured authority. A request is accepted only
 * when `request_did` equals it and `request.seq` is exactly
 * `watermark + 1`; the returned report says which check failed otherwise.
 *
 * `state` is mutated in place ONLY on acceptance. On any refusal it is
 * left byte-identical, so a caller can retry the same document after
 * fixing the cause. */
[[nodiscard]] RemoteApplyReport apply_control_request(ControlState &state, std::uint64_t &watermark,
                                                      const ControlRequest &request,
                                                      std::string_view operator_did,
                                                      std::string_view request_did);

/* The durable watermark. Separate from ControlState because it is
 * channel bookkeeping, not an operator switch: it must survive a restart
 * so a pause cannot be replayed past a resume. */
struct RemoteControlCursor {
    std::uint32_t version{1};
    std::uint64_t last_seq{0};
};

[[nodiscard]] RemoteControlCursor load_remote_control_cursor(const std::filesystem::path &path);
void save_remote_control_cursor(const RemoteControlCursor &cursor,
                                const std::filesystem::path &path);

/* Parse the authority (DID) out of an at-URI. Returns nullopt for
 * anything that is not a well-formed at:// URI with a DID authority, so
 * a malformed URI is a refusal rather than an empty-string match. */
[[nodiscard]] std::optional<std::string> aturi_authority(std::string_view uri);

/* True when `uri` is a record in `did`'s repo, in the control collection.
 * This is the provenance check apply_control_request assumes the caller
 * has already made. */
[[nodiscard]] bool is_operator_control_uri(std::string_view uri, std::string_view did,
                                           std::string_view collection = kControlCollection);

} // namespace atperson

#endif
