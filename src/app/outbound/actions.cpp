#include "actions.hpp"

#include <array>
#include <string>

namespace atperson {
namespace {

struct KindEntry {
    OutboundActionKind kind;
    const char *name;
};

/* Ordered to match the enum so indexing stays obvious and auditable. */
constexpr std::array<KindEntry, kOutboundActionKindCount> kKinds{{
    {OutboundActionKind::Post, "post"},
    {OutboundActionKind::Reply, "reply"},
    {OutboundActionKind::Like, "like"},
    {OutboundActionKind::Repost, "repost"},
    {OutboundActionKind::Follow, "follow"},
    {OutboundActionKind::Unfollow, "unfollow"},
    {OutboundActionKind::Moderation, "moderation"},
}};

} // namespace

const char *outbound_kind_name(OutboundActionKind kind) noexcept {
    for (const KindEntry &entry : kKinds) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<OutboundActionKind> parse_outbound_kind(std::string_view name) noexcept {
    for (const KindEntry &entry : kKinds) {
        if (name == entry.name) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

std::string outbound_dedup_key(const OutboundActionProposal &proposal) {
    std::string key = outbound_kind_name(proposal.kind);
    key.push_back(':');
    key.append(proposal.target);
    key.push_back(':');
    key.append(proposal.action_digest);
    return key;
}

const char *outbound_outcome_name(OutboundOutcome outcome) noexcept {
    switch (outcome) {
    case OutboundOutcome::Allow:
        return "allow";
    case OutboundOutcome::Deny:
        return "deny";
    case OutboundOutcome::Defer:
        return "defer";
    }
    return "deny";
}

const char *outbound_reason_code(OutboundReason reason) noexcept {
    switch (reason) {
    case OutboundReason::Allow:
        return "allow";
    case OutboundReason::KindDisabled:
        return "kind_disabled";
    case OutboundReason::UnsupportedKind:
        return "unsupported_kind";
    case OutboundReason::DuplicateSuppressed:
        return "duplicate_suppressed";
    case OutboundReason::CooldownActive:
        return "cooldown_active";
    case OutboundReason::WindowExhausted:
        return "window_exhausted";
    }
    return "unsupported_kind";
}

} // namespace atperson
