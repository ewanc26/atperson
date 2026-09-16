/* Ingestion policy: every skip class produces its documented reason, and
 * eligible classes (plain posts, reposts, replies) are tagged. Everything
 * here runs offline — the policy consumes already-extracted fields. */
#include "ingestion_policy.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

using atperson::PolicyReason;
using atperson::PolicyPost;

constexpr std::string_view ME = "did:plc:me";
constexpr std::string_view OTHER = "did:plc:other";

/* A plain timeline post from another account: the baseline eligible case. */
PolicyPost plain_post() {
    PolicyPost post;
    post.author_did = std::string(OTHER);
    post.record_type = "app.bsky.feed.post";
    post.text = "a perfectly ordinary post";
    return post;
}

void expect(PolicyReason reason, bool eligible, const PolicyPost &post,
            const char *label) {
    const auto decision = atperson::evaluate_post(ME, post);
    if (decision.reason != reason || decision.eligible != eligible) {
        std::fprintf(stderr, "FAIL %s: got %s/%d\n", label,
                     atperson::policy_reason_name(decision.reason),
                     decision.eligible);
        std::exit(1);
    }
    std::printf("ok %s -> %s%s\n", label,
                atperson::policy_reason_name(decision.reason),
                decision.eligible ? "" : " (skipped)");
}

/* ---------------------------------------------------------------- */
/* Eligible classes                                                  */
/* ---------------------------------------------------------------- */

void test_plain_post_is_eligible() {
    expect(PolicyReason::Eligible, true, plain_post(), "plain post");
}

void test_repost_is_eligible_and_tagged() {
    PolicyPost post = plain_post();
    post.is_repost = true;
    expect(PolicyReason::Repost, true, post, "repost");
}

void test_reply_is_eligible_and_tagged() {
    PolicyPost post = plain_post();
    post.is_reply = true;
    expect(PolicyReason::Reply, true, post, "reply");
}

/* ---------------------------------------------------------------- */
/* Skip classes                                                      */
/* ---------------------------------------------------------------- */

void test_self_authored_is_skipped() {
    PolicyPost post = plain_post();
    post.author_did = std::string(ME);
    expect(PolicyReason::SelfAuthored, false, post, "self-authored");
}

void test_viewer_blocked_is_skipped() {
    PolicyPost post = plain_post();
    post.viewer_blocked = true;
    expect(PolicyReason::ViewerBlocked, false, post, "viewer blocked");
}

void test_viewer_blocked_by_is_skipped() {
    PolicyPost post = plain_post();
    post.viewer_blocked_by = true;
    expect(PolicyReason::ViewerBlockedBy, false, post, "blocked by author");
}

void test_viewer_muted_is_skipped() {
    PolicyPost post = plain_post();
    post.viewer_muted = true;
    expect(PolicyReason::ViewerMuted, false, post, "viewer muted");
}

void test_moderation_filtered_is_skipped() {
    PolicyPost post = plain_post();
    post.moderation_filtered = true;
    expect(PolicyReason::ModerationFiltered, false, post, "moderation filtered");
}

void test_empty_text_is_skipped() {
    PolicyPost post = plain_post();
    post.text = {};
    expect(PolicyReason::EmptyText, false, post, "empty text");
}

void test_image_only_post_is_non_text() {
    PolicyPost post = plain_post();
    post.text = {};
    post.embed_type = "app.bsky.embed.images";
    expect(PolicyReason::NonTextOnly, false, post, "image-only post");
}

void test_video_only_post_is_non_text() {
    PolicyPost post = plain_post();
    post.text = {};
    post.embed_type = "app.bsky.embed.video";
    expect(PolicyReason::NonTextOnly, false, post, "video-only post");
}

void test_unsupported_record_type_is_skipped() {
    PolicyPost post = plain_post();
    post.record_type = "com.example.custom.record";
    expect(PolicyReason::UnsupportedRecord, false, post, "unsupported record");
}

/* ---------------------------------------------------------------- */
/* Precedence                                                        */
/* ---------------------------------------------------------------- */

void test_self_authored_beats_viewer_state() {
    /* The account's own output is skipped even when viewer state is set:
     * the self-observation rule is the identity-level decision. */
    PolicyPost post = plain_post();
    post.author_did = std::string(ME);
    post.viewer_muted = true;
    expect(PolicyReason::SelfAuthored, false, post, "self-authored + muted");
}

void test_record_type_beats_everything() {
    /* A non-post record never reaches the author checks. */
    PolicyPost post = plain_post();
    post.record_type = "com.example.custom.record";
    post.author_did = std::string(ME);
    post.viewer_blocked = true;
    expect(PolicyReason::UnsupportedRecord, false, post,
           "unsupported record + self + blocked");
}

void test_empty_account_did_never_self_skips() {
    /* Degenerate input: no authenticated DID means the self-observation
     * rule cannot fire; the post is judged on its own merits. */
    PolicyPost post = plain_post();
    const auto decision = atperson::evaluate_post("", post);
    assert(decision.eligible);
    assert(decision.reason == PolicyReason::Eligible);
    std::printf("ok empty account did -> eligible\n");
}

} // namespace

int main() {
    test_plain_post_is_eligible();
    test_repost_is_eligible_and_tagged();
    test_reply_is_eligible_and_tagged();
    test_self_authored_is_skipped();
    test_viewer_blocked_is_skipped();
    test_viewer_blocked_by_is_skipped();
    test_viewer_muted_is_skipped();
    test_moderation_filtered_is_skipped();
    test_empty_text_is_skipped();
    test_image_only_post_is_non_text();
    test_video_only_post_is_non_text();
    test_unsupported_record_type_is_skipped();
    test_self_authored_beats_viewer_state();
    test_record_type_beats_everything();
    test_empty_account_did_never_self_skips();
    std::printf("all policy tests passed\n");
    return 0;
}
