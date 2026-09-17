#ifndef ATPERSON_OUTBOUND_ACTIONS_HPP
#define ATPERSON_OUTBOUND_ACTIONS_HPP

// Outbound action layer (#23): the runtime's own vocabulary for one network
// action, deliberately separate from any C23 plan, decision or proposal.
//
// A core plan is evidence, never permission. Only a proposal constructed here
// may enter the outbound policy, and building one executes nothing. This
// header is pure data and pure functions: no network, no Wolfram, no core
// state, no time source of its own.
//
// Ownership: callers own proposals by value; nothing here allocates beyond
// std::string. Every mapping is total and deterministic.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace atperson {

/* Known network action kinds. New kinds are added here explicitly; the
 * absence of a kind is what default-deny means, so this list is the single
 * source of truth for what the policy can even consider. */
enum class OutboundActionKind {
    Post,
    Reply,
    Like,
    Repost,
    Follow,
    Unfollow,
    Moderation,
};

inline constexpr std::size_t kOutboundActionKindCount = 7u;

/* Stable lowercase wire/storage name, e.g. "post". Never null. */
[[nodiscard]] const char *outbound_kind_name(OutboundActionKind kind) noexcept;

/* Parse a kind name. Unknown or malformed names return nullopt rather than
 * throwing, so a boundary that wants default-deny can decide. Case-sensitive
 * by design: policy config and CLI input use the canonical names. */
[[nodiscard]] std::optional<OutboundActionKind> parse_outbound_kind(std::string_view name) noexcept;

/* A runtime request to perform one network action. `target` is the stable AT
 * URI or DID the action applies to (empty for an original post); for a like,
 * repost or reply it is the subject record URI, for a follow/unfollow the
 * subject DID. `action_digest` is the #22 approval digest binding the exact
 * inspected decision; together with kind and target it is the action's
 * identity for duplicate suppression. */
struct OutboundActionProposal {
    OutboundActionKind kind{OutboundActionKind::Post};
    std::string target;
    std::string action_digest;
};

/* Stable identity for duplicate suppression: "<kind>:<target>:<digest>". */
[[nodiscard]] std::string outbound_dedup_key(const OutboundActionProposal &proposal);

/* Policy verdict. Allow permits admission, Deny is a permanent refusal for
 * this proposal, Defer is a time-based refusal that may be retried later. */
enum class OutboundOutcome { Allow, Deny, Defer };

/* Machine-readable reason for a verdict. Codes are stable and form the
 * inspectable contract; the human explanation is derived separately. */
enum class OutboundReason {
    Allow,
    KindDisabled,
    UnsupportedKind,
    DuplicateSuppressed,
    CooldownActive,
    WindowExhausted,
};

[[nodiscard]] const char *outbound_outcome_name(OutboundOutcome outcome) noexcept;
[[nodiscard]] const char *outbound_reason_code(OutboundReason reason) noexcept;

} // namespace atperson

#endif
