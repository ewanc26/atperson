#ifndef ATPERSON_CONTROL_ENVELOPE_HPP
#define ATPERSON_CONTROL_ENVELOPE_HPP

// Standing authorization envelopes (#141): bounded operator pre-approval
// within which the runtime may act without a per-action digest approval.
//
// An envelope is an operator-authored, versioned JSON document stored at
// `<data>/envelopes/<id>.json`. It never widens what is possible: it can
// only narrow the existing outbound policy. An action is envelope-covered
// only when:
//   - the envelope is not expired and not revoked (revocation is deletion
//     of the file — immediate, durable, and seen by the next attempt);
//   - the action's kind is enabled in the envelope;
//   - the envelope's own rate ceiling for that kind has room against the
//     same shared budget records the policy uses (the ceiling must be <=
//     the policy ceiling; the tighter always wins);
//   - the decision evidence (plan score / support score) meets the
//     envelope's floors, when floors are configured. An action with no
//     recorded evidence fails the floor: fail-closed;
//   - the action's text is within the term scope, when a scope is
//     configured (at least one scope term must appear in the text).
//
// Coverage is evaluated at execution time, never at decision time, and the
// executing path re-reads the envelope files on every attempt, so a
// revocation between cycles always stops the next execution. An expired or
// narrowed envelope never retroactively invalidates an already-executed
// action: authorisation is a fact about the moment of execution, recorded
// in the audit log.
//
// No envelope exists by default and bootstrap does not create one. No
// envelope, or no covering envelope, means per-digest approval as today.
// The envelope cannot bypass: pause, dry-run mode, the write gate,
// ATPERSON_ALLOW_EXTERNAL_PUBLISHING, or the outbound policy's own limits.
//
// Ownership: `AuthorizationEnvelope` is plain value state. Load/save is
// the caller's; `save_authorization_envelope` is atomic (temp + rename).
// This module performs no I/O beyond the explicit load/save calls, holds
// no locks and consults no clock: `now` is always injected.
//
// Failure modes: `EnvelopeError` for malformed documents, unsupported
// versions, unknown kind names, impossible ceilings and scope/term
// contradictions; `std::runtime_error` for I/O failures.

#include "../outbound/actions.hpp"
#include "../outbound/budget.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

inline constexpr const char *kAuthorizationEnvelopeFormat =
    "atperson-authorization-envelope";
inline constexpr std::uint32_t kAuthorizationEnvelopeVersion = 1u;

/* Operator-chosen identifier: 1-64 characters, [a-z0-9-], no leading or
 * trailing hyphen. The id is the file name, so it must stay boring. */
[[nodiscard]] bool is_envelope_id(std::string_view id);

/* Per-kind standing limits. All optional; an absent kind is not covered. */
struct EnvelopeKindRule {
    OutboundActionKind kind{OutboundActionKind::Post};

    /* Ceiling on admitted actions of this kind in the trailing window,
     * counted against the same budget records the policy uses. Must be
     * <= the policy's max_in_window for the kind, or the document is
     * rejected at parse time: an envelope narrows, never widens. */
    std::uint32_t max_in_window{1u};

    /* Trailing window length in seconds, 1..366 days. */
    std::int64_t window_seconds{24ll * 60ll * 60ll};

    /* Decision-evidence floors. <= 0 means "no floor for this component".
     * An action with no recorded evidence fails any configured floor. */
    double min_plan_score{0.0};
    double min_support_score{0.0};
};

struct AuthorizationEnvelope {
    std::uint32_t version{kAuthorizationEnvelopeVersion};
    std::string id;
    /* RFC 3339 UTC. Absent means no expiry. */
    std::optional<std::string> expires_at;
    /* Scope restriction: when non-empty, the action text must contain at
     * least one term (case-insensitive). Empty means unrestricted. */
    std::vector<std::string> scope_terms;
    std::vector<EnvelopeKindRule> kinds;
    /* Operator note, shown by `control envelope show`. Never gates. */
    std::string note;
    std::string created_at;
};

class EnvelopeError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Parse and fully validate one envelope document. `policy` is the outbound
 * policy the envelope must not widen; `source` labels errors. */
[[nodiscard]] AuthorizationEnvelope
parse_authorization_envelope(std::string_view json, const OutboundPolicy &policy,
                             std::string_view source);

/* Deterministic JSON with stable field order. */
[[nodiscard]] std::string serialise_authorization_envelope(const AuthorizationEnvelope &e);

/* Load <dir>/<id>.json. A missing file returns nullopt (no envelope with
 * that id); a malformed one throws. */
[[nodiscard]] std::optional<AuthorizationEnvelope>
load_authorization_envelope(const std::filesystem::path &dir, std::string_view id,
                            const OutboundPolicy &policy);

/* Atomically persist the envelope as <dir>/<id>.json (temp + rename). */
void save_authorization_envelope(const AuthorizationEnvelope &e,
                                 const std::filesystem::path &dir);

/* Immediate, durable revocation: delete <dir>/<id>.json. Idempotent; a
 * missing file is not an error. Throws on other I/O failure. */
void revoke_authorization_envelope(const std::filesystem::path &dir, std::string_view id);

/* All envelope ids present in `dir`, sorted. A directory that does not
 * exist yields an empty list (no envelopes: per-digest approval). */
[[nodiscard]] std::vector<std::string>
list_authorization_envelopes(const std::filesystem::path &dir);

/* Inspectable coverage decision. */
struct EnvelopeCoverage {
    bool covered{false};
    /* The covering envelope's id; empty when not covered. */
    std::string envelope_id;
    /* Stable reason when not covered: "no_envelope", "expired",
     * "kind_not_covered", "rate_ceiling", "score_floor", "scope". */
    std::string reason;
};

/* The evidence an envelope's floors are evaluated against. Both optional:
 * a proposal may predate floors. An absent component fails a configured
 * floor for that component (fail-closed). */
struct EnvelopeEvidence {
    std::optional<double> plan_score;
    std::optional<double> support_score;
};

/* Pure coverage evaluation for one action against one envelope. `now` is
 * injected Unix seconds; `usage` is the shared budget record for the
 * action's kind. Deterministic for fixed inputs. */
[[nodiscard]] EnvelopeCoverage
covers_action(const AuthorizationEnvelope &e, OutboundActionKind kind,
              std::string_view text, const EnvelopeEvidence &evidence,
              const OutboundKindUsage &usage, std::int64_t now);

/* Evaluate every envelope in `dir` (loaded fresh from disk) and return the
 * first covering one, or the not-covered result. Envelope files that fail
 * to parse are skipped as non-covering rather than aborting the attempt:
 * a corrupt envelope must never widen authorisation. */
[[nodiscard]] EnvelopeCoverage
find_covering_envelope(const std::filesystem::path &dir, const OutboundPolicy &policy,
                       OutboundActionKind kind, std::string_view text,
                       const EnvelopeEvidence &evidence, const OutboundBudgetState &budget,
                       std::int64_t now);

} // namespace atperson

#endif
