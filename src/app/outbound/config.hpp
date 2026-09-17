#ifndef ATPERSON_OUTBOUND_CONFIG_HPP
#define ATPERSON_OUTBOUND_CONFIG_HPP

// Outbound action policy configuration (#23): the operator-authored rate and
// duplication rules the C++ runtime applies before any network action.
//
// This is runtime policy, never learned state. It is a versioned JSON document
// loaded from `ATPERSON_OUTBOUND_POLICY` (default
// `<data>/outbound-policy.json`). A missing file yields the fail-closed
// default: every action kind disabled, so nothing is permitted until an
// operator explicitly enables a kind. Malformed or unsupported documents
// throw rather than being silently reinterpreted.
//
// Ownership: `OutboundPolicy` is plain value state; callers own it. Parsing
// allocates only via std::string/std::vector. There is no hidden global state.
//
// Failure modes: `OutboundPolicyError` for a malformed document, an
// unsupported format/version, an unknown kind name, or impossible limits;
// `std::runtime_error` for I/O failures reading the file.

#include "actions.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace atperson {

/* Upper bound on any configured window/interval, keeping budget logs bounded
 * and rejecting nonsense values (366 days). */
inline constexpr std::int64_t kMaxOutboundWindowSeconds = 366ll * 24ll * 60ll * 60ll;

/* Per-kind budget. `enabled` is the default-deny switch: a kind with no
 * enabled budget can never be admitted, whatever its score. */
struct ActionBudget {
    bool enabled{false};

    /* Maximum admitted actions of this kind in the trailing `window_seconds`. */
    std::uint32_t max_in_window{1u};

    /* Trailing window length in whole seconds; must be >= 1. */
    std::int64_t window_seconds{24ll * 60ll * 60ll};

    /* Minimum spacing between consecutive admitted actions of this kind.
     * 0 means no spacing requirement beyond the window limit. */
    std::int64_t min_interval_seconds{0};

    /* Window within which an identical action identity (kind + target +
     * digest) is suppressed outright. 0 disables duplicate suppression. */
    std::int64_t duplicate_cooldown_seconds{0};
};

/* Full policy: one budget per known action kind, indexed by enum order. */
struct OutboundPolicy {
    std::uint32_t version{1u};
    std::array<ActionBudget, kOutboundActionKindCount> budgets{};
};

/* Thrown for malformed policy documents and impossible field values. */
class OutboundPolicyError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Fail-closed default: every kind disabled with conservative limits. */
[[nodiscard]] OutboundPolicy default_outbound_policy();

[[nodiscard]] ActionBudget &budget_for(OutboundPolicy &policy, OutboundActionKind kind);
[[nodiscard]] const ActionBudget &budget_for(const OutboundPolicy &policy, OutboundActionKind kind);

/* Deterministic JSON with stable field order. */
[[nodiscard]] std::string serialise_outbound_policy(const OutboundPolicy &policy);

/* Parse and fully validate a policy document. `source` labels errors. */
[[nodiscard]] OutboundPolicy parse_outbound_policy(std::string_view json, std::string_view source);

/* Load the policy file. A missing file returns the fail-closed default;
 * anything present must parse and validate or this throws. */
[[nodiscard]] OutboundPolicy load_outbound_policy(const std::filesystem::path &path);

} // namespace atperson

#endif
