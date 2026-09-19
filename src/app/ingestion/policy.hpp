#ifndef ATPERSON_INGESTION_POLICY_HPP
#define ATPERSON_INGESTION_POLICY_HPP

#include <string>
#include <string_view>

namespace atperson {

/*
 * Explicit ingestion policy for public AT Protocol data.
 *
 * The policy is the single reviewable decision point for which fetched
 * records may become observations. It consumes already-extracted fields
 * (no protocol mechanics — those stay in Wolfram and the client) and
 * produces a decision: eligible, or skipped with a machine-readable reason.
 *
 * Every skipped class is recorded in the ledger as SKIPPED rather than
 * silently disappearing, so "observed but not learned" stays distinguishable
 * from "never fetched".
 *
 * This is intentionally a public-post policy. Only app.bsky.feed.post is
 * eligible; conversation/private-message payloads must remain unsupported and
 * never become graph learning input (#61).
 */

enum class PolicyReason {
    Eligible,
    Repost,             /* eligible; the text is a reposted record */
    Reply,              /* eligible; replies carry their own text */
    Quote,              /* eligible; quotes carry own text; quoted text is never merged (issue #24) */
    SelfAuthored,       /* the account's own output: never learned from */
    ViewerBlocked,      /* the viewer blocks the author */
    ViewerBlockedBy,    /* the author blocks the viewer */
    ViewerMuted,       /* the viewer muted the author */
    EmptyText,          /* no text to learn from */
    UnsupportedRecord,  /* not an app.bsky.feed.post record */
    NonTextOnly,        /* images/video only; no text content */
    ModerationFiltered, /* a moderation cause filtered the post */
};

const char *policy_reason_name(PolicyReason reason);

/* Single authority for whether a policy reason may train learned state.
 * Keep all ingestion engines on this helper so newly-added eligible reasons
 * cannot be accidentally ledgered as skipped. */
[[nodiscard]] bool policy_reason_is_eligible(PolicyReason reason) noexcept;

/* Fields the policy needs, extracted by the client. Mirrors the subset of
 * app.bsky.feed.defs#feedViewPost the policy decides on. */
struct PolicyPost {
    std::string author_did;
    std::string record_type; /* the record's $type, e.g. app.bsky.feed.post */
    std::string_view text;
    std::string embed_type;         /* primary embed $type, empty when none */
    bool embed_has_text_fallback{}; /* e.g. a quote post carries text */
    bool viewer_blocked{};          /* viewer blocks the author */
    bool viewer_blocked_by{};       /* author blocks the viewer */
    bool viewer_muted{};            /* viewer muted the author */
    bool moderation_filtered{};     /* a moderation decision filtered it */
    bool is_repost{};               /* feed item reason is a repost */
    bool is_reply{};                /* feed item is a reply */
    bool is_quote{};                /* record embeds a quoted post (issue #24) */
};

struct PolicyDecision {
    bool eligible{};
    PolicyReason reason{PolicyReason::Eligible};
};

/*
 * Evaluate one fetched post. account_did is the authenticated session's DID
 * (never the login handle) so self-observation is explicit.
 */
PolicyDecision evaluate_post(std::string_view account_did, const PolicyPost &post);

} // namespace atperson

#endif
