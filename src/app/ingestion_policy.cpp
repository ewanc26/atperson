#include "ingestion_policy.hpp"

namespace atperson {

const char *policy_reason_name(PolicyReason reason) {
    switch (reason) {
    case PolicyReason::Eligible:
        return "eligible";
    case PolicyReason::Repost:
        return "repost";
    case PolicyReason::Reply:
        return "reply";
    case PolicyReason::SelfAuthored:
        return "self-authored";
    case PolicyReason::ViewerBlocked:
        return "viewer-blocked";
    case PolicyReason::ViewerBlockedBy:
        return "viewer-blocked-by";
    case PolicyReason::ViewerMuted:
        return "viewer-muted";
    case PolicyReason::EmptyText:
        return "empty-text";
    case PolicyReason::UnsupportedRecord:
        return "unsupported-record";
    case PolicyReason::NonTextOnly:
        return "non-text-only";
    case PolicyReason::ModerationFiltered:
        return "moderation-filtered";
    }
    return "unknown";
}

PolicyDecision evaluate_post(std::string_view account_did, const PolicyPost &post) {
    /*
     * Rule order is deliberate and documented:
     *
     * 1. Record type: only app.bsky.feed.post records are supported. A feed
     *    can carry other record types (e.g. generator "posts"); they are
     *    skipped as unsupported rather than mis-parsed.
     *
     * 2. Self-observation: the account's own output is never learned from.
     *    This is an explicit policy decision, not an accident: learning from
     *    self-authored records would create a feedback loop between the
     *    entity's output and its experience.
     *
     * 3. Moderation/mute/block state: the viewer's relationship to the author
     *    (blocked, blocked-by, muted) and any moderation filter on the post
     *    make the record ineligible. The account chose not to see this
     *    content; atperson respects that choice.
     *
     * 4. Text presence: empty text is skipped (nothing to learn). Image- or
     *    video-only posts with no text are skipped as non-text records.
     *
     * Replies and reposts remain eligible: a repost's feed item points at
     * the underlying post (the client extracts that post's text), and a
     * reply carries its own text. Both are tagged with their reason so the
     * ledger can distinguish them from plain timeline posts.
     */

    if (post.record_type != "app.bsky.feed.post") {
        return {false, PolicyReason::UnsupportedRecord};
    }

    if (!account_did.empty() && post.author_did == account_did) {
        return {false, PolicyReason::SelfAuthored};
    }

    if (post.viewer_blocked) {
        return {false, PolicyReason::ViewerBlocked};
    }
    if (post.viewer_blocked_by) {
        return {false, PolicyReason::ViewerBlockedBy};
    }
    if (post.viewer_muted) {
        return {false, PolicyReason::ViewerMuted};
    }
    if (post.moderation_filtered) {
        return {false, PolicyReason::ModerationFiltered};
    }

    if (post.text.empty()) {
        /* An embed with a text fallback (e.g. a quote post) still counts:
         * the client extracts the quote's text into `text`. */
        if (!post.embed_type.empty() && !post.embed_has_text_fallback) {
            return {false, PolicyReason::NonTextOnly};
        }
        return {false, PolicyReason::EmptyText};
    }

    if (post.is_repost) {
        return {true, PolicyReason::Repost};
    }
    if (post.is_reply) {
        return {true, PolicyReason::Reply};
    }
    return {true, PolicyReason::Eligible};
}

} // namespace atperson
