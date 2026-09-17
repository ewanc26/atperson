#ifndef ATPERSON_OUTBOUND_ACTION_HPP
#define ATPERSON_OUTBOUND_ACTION_HPP

// Outbound action document (#25): the frozen, inspectable description of one
// exact network post or reply that an operator has approved for execution.
//
// A core plan is evidence, never permission (root AGENTS.md). This document is
// the C++ runtime's own record of the *exact* action that may be submitted: the
// text is carried verbatim from what the operator inspected, never regenerated
// at execution time. `digest` is the #22 control-plane approval binding, so the
// document and the approval cannot drift.
//
// Scope for #25: only original posts and replies are executable. Other kinds
// remain non-executable here even though the #23 policy vocabulary knows them;
// a document that names one is rejected rather than silently downgraded.
//
// Ownership: `OutboundAction` is payload-by-value; nothing here allocates
// beyond std::string. This header has no network, no Wolfram and no clock.
// `build_outbound_record_json` is pure and deterministic for fixed inputs.
//
// Failure modes: `OutboundActionError` for malformed documents, unsupported
// format/version, non-executable kinds and missing reply context;
// `std::runtime_error` for I/O failures reading the file.

#include "actions.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {

/* The AT collection every executable #25 action writes to. A reply is still an
 * app.bsky.feed.post record with `reply` strongRefs. */
inline constexpr const char *kOutboundPostCollection = "app.bsky.feed.post";

inline constexpr const char *kOutboundActionFormat = "atperson-outbound-action";
inline constexpr std::uint32_t kOutboundActionVersion = 1u;

/* One exact post or reply. `rkey` is frozen up front and used with putRecord,
 * so a retry after an ambiguous failure overwrites the same record key instead
 * of creating a duplicate. */
struct OutboundAction {
    OutboundActionKind kind{OutboundActionKind::Post};
    std::string text;
    std::string rkey;
    std::string created_at;
    /* The #22 approval digest this action is bound to: exactly 16 lowercase
     * hex characters (the 64-bit control-plane digest). A malformed digest is
     * rejected at parse time, since it could never match an approval. */
    std::string digest;
    /* Reply context (#24), at-URIs of the immediate parent and the thread
     * root. Empty for an original post. The CIDs are resolved at execution
     * time, when the records are read from the network. */
    std::string reply_root;
    std::string reply_parent;
};

class OutboundActionError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] bool outbound_action_is_reply(const OutboundAction &action) noexcept;

/* The #23 proposal this document evaluates against: a reply targets its
 * immediate parent, an original post has no target. */
[[nodiscard]] OutboundActionProposal outbound_action_proposal(const OutboundAction &action);

/* Parse and fully validate one `atperson-outbound-action` v1 document. */
[[nodiscard]] OutboundAction parse_outbound_action(std::string_view json, std::string_view source);

/* Deterministic JSON with stable field order. */
[[nodiscard]] std::string serialise_outbound_action(const OutboundAction &action);

/* Load and parse an action file. I/O failures throw std::runtime_error; a
 * malformed document throws OutboundActionError. */
[[nodiscard]] OutboundAction load_outbound_action(const std::filesystem::path &path);

/* Build the app.bsky.feed.post record body for `action`. For a reply,
 * `root_cid` and `parent_cid` are required and become the strongRef CIDs;
 * for a post they are ignored. Throws OutboundActionError when a reply lacks
 * its CIDs or the record cannot be serialised. */
[[nodiscard]] std::string build_outbound_record_json(const OutboundAction &action,
                                                     std::string_view root_cid,
                                                     std::string_view parent_cid);

} // namespace atperson

#endif
